#include "storage_demo_adapter.h"

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "host_freertos.h"
#include "sd_storage_service.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_MOUNT_PATH          "/sdcard"
#define TEST_FILE_CAPACITY       6U
#define TEST_DESCRIPTOR_CAPACITY 6U
#define TEST_FILE_BYTES          4096U
#define TEST_PATH_BYTES          64U
#define TEST_RANDOM_CAPACITY     20U
#define TEST_FAKE_FD_BASE        100
#define TEST_WAIT_ATTEMPTS       2000U

typedef struct test_virtual_file
{
    char path[TEST_PATH_BYTES];
    uint8_t data[TEST_FILE_BYTES];
    size_t size;
    bool exists;
    bool user_owned;
} test_virtual_file_t;

typedef struct test_virtual_descriptor
{
    size_t file_index;
    size_t offset;
    int access_mode;
    bool used;
} test_virtual_descriptor_t;

typedef struct test_unlinked_file
{
    char path[TEST_PATH_BYTES];
    uint8_t data[TEST_FILE_BYTES];
    size_t size;
    bool valid;
} test_unlinked_file_t;

static pthread_mutex_t s_filesystem_lock = PTHREAD_MUTEX_INITIALIZER;
static test_virtual_file_t s_files[TEST_FILE_CAPACITY];
static test_virtual_descriptor_t s_descriptors[TEST_DESCRIPTOR_CAPACITY];
static test_unlinked_file_t s_last_unlinked;
static uint32_t s_random_values[TEST_RANDOM_CAPACITY];
static size_t s_random_count;
static size_t s_random_index;
static size_t s_total_written;
static size_t s_total_read;
static size_t s_fail_write_after;
static size_t s_corrupt_read_offset;
static unsigned s_create_count;
static unsigned s_read_only_open_count;
static unsigned s_exclusive_conflict_count;
static unsigned s_unlink_call_count;
static unsigned s_unlink_failures_remaining;
static bool s_all_creates_exclusive;

static void _test_sleep_one_ms(void)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static void _test_reset(void)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    memset(s_files, 0, sizeof(s_files));
    memset(s_descriptors, 0, sizeof(s_descriptors));
    memset(&s_last_unlinked, 0, sizeof(s_last_unlinked));
    memset(s_random_values, 0, sizeof(s_random_values));
    s_random_count = 0U;
    s_random_index = 0U;
    s_total_written = 0U;
    s_total_read = 0U;
    s_fail_write_after = SIZE_MAX;
    s_corrupt_read_offset = SIZE_MAX;
    s_create_count = 0U;
    s_read_only_open_count = 0U;
    s_exclusive_conflict_count = 0U;
    s_unlink_call_count = 0U;
    s_unlink_failures_remaining = 0U;
    s_all_creates_exclusive = true;
    (void)pthread_mutex_unlock(&s_filesystem_lock);
}

static int _test_find_file_locked(const char *path)
{
    for (size_t index = 0; index < TEST_FILE_CAPACITY; ++index)
    {
        if (s_files[index].exists && strcmp(s_files[index].path, path) == 0)
        {
            return (int)index;
        }
    }
    return -1;
}

static int _test_alloc_file_locked(const char *path, bool user_owned)
{
    for (size_t index = 0; index < TEST_FILE_CAPACITY; ++index)
    {
        if (!s_files[index].exists)
        {
            memset(&s_files[index], 0, sizeof(s_files[index]));
            int length = snprintf(s_files[index].path,
                                  sizeof(s_files[index].path), "%s", path);
            assert(length > 0 &&
                   (size_t)length < sizeof(s_files[index].path));
            s_files[index].exists = true;
            s_files[index].user_owned = user_owned;
            return (int)index;
        }
    }
    return -1;
}

static int _test_alloc_descriptor_locked(size_t file_index, int access_mode)
{
    for (size_t index = 0; index < TEST_DESCRIPTOR_CAPACITY; ++index)
    {
        if (!s_descriptors[index].used)
        {
            s_descriptors[index] = (test_virtual_descriptor_t)
            {
                .file_index = file_index,
                .access_mode = access_mode,
                .used = true,
            };
            return TEST_FAKE_FD_BASE + (int)index;
        }
    }
    return -1;
}

static test_virtual_descriptor_t *_test_descriptor_locked(int descriptor)
{
    const int index = descriptor - TEST_FAKE_FD_BASE;
    if (index < 0 || index >= (int)TEST_DESCRIPTOR_CAPACITY ||
            !s_descriptors[index].used)
    {
        return NULL;
    }
    return &s_descriptors[index];
}

static void _test_add_random(uint32_t value)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(s_random_count < TEST_RANDOM_CAPACITY);
    s_random_values[s_random_count++] = value;
    (void)pthread_mutex_unlock(&s_filesystem_lock);
}

static void _test_add_user_file(const char *path, const void *data,
                                size_t size)
{
    assert(size <= TEST_FILE_BYTES);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    const int index = _test_alloc_file_locked(path, true);
    assert(index >= 0);
    memcpy(s_files[index].data, data, size);
    s_files[index].size = size;
    (void)pthread_mutex_unlock(&s_filesystem_lock);
}

static storage_demo_snapshot_t _test_wait_for_refresh(
    storage_demo_adapter_t *adapter)
{
    storage_demo_snapshot_t snapshot = {0};
    for (unsigned attempt = 0; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        if (storage_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.ready &&
                snapshot.operation == STORAGE_DEMO_OPERATION_NONE)
        {
            return snapshot;
        }
        _test_sleep_one_ms();
    }
    assert(!"storage refresh timed out");
    return snapshot;
}

static storage_demo_snapshot_t _test_wait_for_self_test(
    storage_demo_adapter_t *adapter,
    storage_demo_self_test_result_t expected)
{
    storage_demo_snapshot_t snapshot = {0};
    for (unsigned attempt = 0; attempt < TEST_WAIT_ATTEMPTS; ++attempt)
    {
        if (storage_demo_adapter_get_snapshot(adapter, &snapshot) == ESP_OK &&
                snapshot.operation == STORAGE_DEMO_OPERATION_NONE &&
                snapshot.self_test == expected)
        {
            return snapshot;
        }
        _test_sleep_one_ms();
    }
    assert(!"storage self-test timed out");
    return snapshot;
}

static void _test_assert_no_invalid_worker_delete(void)
{
    assert(host_caps_task_wrong_delete_count() == 0U);
    assert(host_caps_task_self_delete_count() == 0U);
}

static void _test_assert_worker_retained(size_t owner_delete_count)
{
    assert(host_caps_task_count() == 1U);
    assert(host_caps_task_owner_delete_count() == owner_delete_count);
    _test_assert_no_invalid_worker_delete();
}

static void _test_assert_worker_released(size_t owner_delete_count)
{
    assert(host_caps_task_count() == 0U);
    assert(host_caps_task_owner_delete_count() == owner_delete_count + 1U);
    _test_assert_no_invalid_worker_delete();
}

static storage_demo_adapter_t *_test_open_ready(
    size_t *owner_delete_count)
{
    storage_demo_adapter_t *adapter = NULL;
    assert(storage_demo_adapter_open(&adapter) == ESP_OK);
    assert(adapter != NULL);
    const UBaseType_t required = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    assert(host_caps_task_count() == 1U);
    assert((host_last_task_stack_caps() & required) == required);
    _test_assert_no_invalid_worker_delete();
    *owner_delete_count = host_caps_task_owner_delete_count();
    const storage_demo_snapshot_t snapshot =
        _test_wait_for_refresh(adapter);
    assert(snapshot.mounted);
    assert(snapshot.capacity_valid);
    assert(snapshot.last_error == ESP_OK);
    return adapter;
}

static void _test_assert_no_descriptors(void)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    for (size_t index = 0; index < TEST_DESCRIPTOR_CAPACITY; ++index)
    {
        assert(!s_descriptors[index].used);
    }
    (void)pthread_mutex_unlock(&s_filesystem_lock);
}

static void _test_exclusive_collision_preserves_existing_file(void)
{
    static const char conflict_path[] =
        TEST_MOUNT_PATH "/.mt-storage-11111111.tmp";
    static const char test_path[] =
        TEST_MOUNT_PATH "/.mt-storage-22222222.tmp";
    static const uint8_t existing_data[] = {0xC1U, 0xA5U, 0x5AU, 0x7EU};

    _test_reset();
    _test_add_random(0x11111111U);
    _test_add_random(0x22222222U);
    _test_add_user_file(conflict_path, existing_data, sizeof(existing_data));

    size_t owner_delete_count;
    storage_demo_adapter_t *adapter = _test_open_ready(&owner_delete_count);
    assert(storage_demo_adapter_run_self_test(adapter) == ESP_OK);
    const storage_demo_snapshot_t snapshot = _test_wait_for_self_test(
            adapter,
            STORAGE_DEMO_SELF_TEST_PASSED);
    assert(snapshot.self_test_count == 1U);

    (void)pthread_mutex_lock(&s_filesystem_lock);
    const int conflict_index = _test_find_file_locked(conflict_path);
    assert(conflict_index >= 0);
    assert(s_files[conflict_index].user_owned);
    assert(s_files[conflict_index].size == sizeof(existing_data));
    assert(memcmp(s_files[conflict_index].data, existing_data,
                  sizeof(existing_data)) == 0);
    assert(s_create_count == 1U);
    assert(s_read_only_open_count == 1U);
    assert(s_total_read == TEST_FILE_BYTES);
    assert(s_exclusive_conflict_count == 1U);
    assert(s_unlink_call_count == 1U);
    assert(s_all_creates_exclusive);
    assert(s_last_unlinked.valid);
    assert(strcmp(s_last_unlinked.path, test_path) == 0);
    assert(s_last_unlinked.size == TEST_FILE_BYTES);
    for (size_t index = 0; index < TEST_FILE_BYTES; ++index)
    {
        const uint8_t expected =
            (uint8_t)((index * 33U + 0x5AU) & 0xFFU);
        assert(s_last_unlinked.data[index] == expected);
    }
    (void)pthread_mutex_unlock(&s_filesystem_lock);

    assert(storage_demo_adapter_close(&adapter) == ESP_OK);
    assert(adapter == NULL);
    _test_assert_worker_released(owner_delete_count);
    _test_assert_no_descriptors();
}

static void _test_write_failure_removes_partial_file(void)
{
    static const char test_path[] =
        TEST_MOUNT_PATH "/.mt-storage-33333333.tmp";

    _test_reset();
    _test_add_random(0x33333333U);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    s_fail_write_after = 512U;
    (void)pthread_mutex_unlock(&s_filesystem_lock);

    size_t owner_delete_count;
    storage_demo_adapter_t *adapter = _test_open_ready(&owner_delete_count);
    assert(storage_demo_adapter_run_self_test(adapter) == ESP_OK);
    const storage_demo_snapshot_t snapshot = _test_wait_for_self_test(
            adapter,
            STORAGE_DEMO_SELF_TEST_FAILED);
    assert(snapshot.last_error == ESP_FAIL);
    assert(snapshot.filesystem_errno == EIO);

    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(_test_find_file_locked(test_path) < 0);
    assert(s_last_unlinked.valid);
    assert(strcmp(s_last_unlinked.path, test_path) == 0);
    assert(s_last_unlinked.size == 512U);
    assert(s_unlink_call_count == 1U);
    (void)pthread_mutex_unlock(&s_filesystem_lock);

    assert(storage_demo_adapter_close(&adapter) == ESP_OK);
    assert(adapter == NULL);
    _test_assert_worker_released(owner_delete_count);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(s_unlink_call_count == 1U);
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    _test_assert_no_descriptors();
}

static void _test_readback_corruption_fails_and_removes_file(void)
{
    static const char test_path[] =
        TEST_MOUNT_PATH "/.mt-storage-55555555.tmp";
    static const size_t corrupt_offset = 73U;

    _test_reset();
    _test_add_random(0x55555555U);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    s_corrupt_read_offset = corrupt_offset;
    (void)pthread_mutex_unlock(&s_filesystem_lock);

    size_t owner_delete_count;
    storage_demo_adapter_t *adapter = _test_open_ready(&owner_delete_count);
    assert(storage_demo_adapter_run_self_test(adapter) == ESP_OK);
    const storage_demo_snapshot_t snapshot = _test_wait_for_self_test(
            adapter,
            STORAGE_DEMO_SELF_TEST_FAILED);
    assert(snapshot.last_error == ESP_ERR_INVALID_RESPONSE);
    assert(snapshot.filesystem_errno == EILSEQ);
    assert(snapshot.self_test_count == 0U);

    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(s_read_only_open_count == 1U);
    assert(s_total_read > corrupt_offset);
    assert(s_total_read <= TEST_FILE_BYTES);
    assert(_test_find_file_locked(test_path) < 0);
    assert(s_last_unlinked.valid);
    assert(strcmp(s_last_unlinked.path, test_path) == 0);
    assert(s_last_unlinked.size == TEST_FILE_BYTES);
    assert(s_unlink_call_count == 1U);
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    _test_assert_no_descriptors();

    assert(storage_demo_adapter_close(&adapter) == ESP_OK);
    assert(adapter == NULL);
    _test_assert_worker_released(owner_delete_count);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(s_unlink_call_count == 1U);
    (void)pthread_mutex_unlock(&s_filesystem_lock);
}

static void _test_unlink_failure_retains_close_ownership(void)
{
    static const char test_path[] =
        TEST_MOUNT_PATH "/.mt-storage-44444444.tmp";

    _test_reset();
    _test_add_random(0x44444444U);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    s_unlink_failures_remaining = 2U;
    (void)pthread_mutex_unlock(&s_filesystem_lock);

    size_t owner_delete_count;
    storage_demo_adapter_t *adapter = _test_open_ready(&owner_delete_count);
    assert(storage_demo_adapter_run_self_test(adapter) == ESP_OK);
    const storage_demo_snapshot_t snapshot = _test_wait_for_self_test(
            adapter,
            STORAGE_DEMO_SELF_TEST_FAILED);
    assert(snapshot.last_error == ESP_FAIL);
    assert(snapshot.filesystem_errno == EACCES);

    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(_test_find_file_locked(test_path) >= 0);
    assert(s_unlink_call_count == 1U);
    (void)pthread_mutex_unlock(&s_filesystem_lock);

    storage_demo_adapter_t *owned = adapter;
    assert(storage_demo_adapter_close(&adapter) == ESP_FAIL);
    assert(adapter == owned);
    _test_assert_worker_retained(owner_delete_count);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(_test_find_file_locked(test_path) >= 0);
    assert(s_unlink_call_count == 2U);
    (void)pthread_mutex_unlock(&s_filesystem_lock);

    assert(storage_demo_adapter_close(&adapter) == ESP_OK);
    assert(adapter == NULL);
    _test_assert_worker_released(owner_delete_count);
    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(_test_find_file_locked(test_path) < 0);
    assert(s_unlink_call_count == 3U);
    assert(s_last_unlinked.valid);
    assert(strcmp(s_last_unlinked.path, test_path) == 0);
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    _test_assert_no_descriptors();
}

bool sd_storage_service_is_mounted(void)
{
    return true;
}

const char *sd_storage_service_get_mount_path(void)
{
    return TEST_MOUNT_PATH;
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total_bytes,
                           uint64_t *free_bytes)
{
    if (base_path == NULL || strcmp(base_path, TEST_MOUNT_PATH) != 0 ||
            total_bytes == NULL || free_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *total_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
    *free_bytes = 24ULL * 1024ULL * 1024ULL * 1024ULL;
    return ESP_OK;
}

uint32_t esp_random(void)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    assert(s_random_index < s_random_count);
    const uint32_t value = s_random_values[s_random_index++];
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    return value;
}

int __wrap_open(const char *path, int flags, ...)
{
    if ((flags & O_CREAT) != 0)
    {
        va_list arguments;
        va_start(arguments, flags);
        (void)va_arg(arguments, int);
        va_end(arguments);
    }

    (void)pthread_mutex_lock(&s_filesystem_lock);
    int file_index = _test_find_file_locked(path);
    if ((flags & O_CREAT) != 0)
    {
        if ((flags & O_EXCL) == 0)
        {
            s_all_creates_exclusive = false;
        }
        if (file_index >= 0 && (flags & O_EXCL) != 0)
        {
            ++s_exclusive_conflict_count;
            (void)pthread_mutex_unlock(&s_filesystem_lock);
            errno = EEXIST;
            return -1;
        }
        if (file_index < 0)
        {
            file_index = _test_alloc_file_locked(path, false);
            if (file_index < 0)
            {
                (void)pthread_mutex_unlock(&s_filesystem_lock);
                errno = ENOSPC;
                return -1;
            }
            ++s_create_count;
        }
    }
    else if (file_index < 0)
    {
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = ENOENT;
        return -1;
    }

    const int descriptor = _test_alloc_descriptor_locked(
                               (size_t)file_index, flags & O_ACCMODE);
    if (descriptor >= 0 && (flags & O_ACCMODE) == O_RDONLY)
    {
        ++s_read_only_open_count;
    }
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    if (descriptor < 0)
    {
        errno = EMFILE;
    }
    return descriptor;
}

ssize_t __wrap_write(int descriptor, const void *data, size_t size)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    test_virtual_descriptor_t *open_file =
        _test_descriptor_locked(descriptor);
    if (open_file == NULL || open_file->access_mode == O_RDONLY)
    {
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = EBADF;
        return -1;
    }
    if (s_total_written >= s_fail_write_after)
    {
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = EIO;
        return -1;
    }

    test_virtual_file_t *file = &s_files[open_file->file_index];
    if (size > sizeof(file->data) - open_file->offset)
    {
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = ENOSPC;
        return -1;
    }
    memcpy(file->data + open_file->offset, data, size);
    open_file->offset += size;
    if (open_file->offset > file->size)
    {
        file->size = open_file->offset;
    }
    s_total_written += size;
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    return (ssize_t)size;
}

ssize_t __wrap_read(int descriptor, void *data, size_t size)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    test_virtual_descriptor_t *open_file =
        _test_descriptor_locked(descriptor);
    if (open_file == NULL || open_file->access_mode == O_WRONLY)
    {
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = EBADF;
        return -1;
    }

    const test_virtual_file_t *file = &s_files[open_file->file_index];
    const size_t read_offset = open_file->offset;
    const size_t available = file->size - open_file->offset;
    const size_t copied = size < available ? size : available;
    memcpy(data, file->data + open_file->offset, copied);
    if (s_corrupt_read_offset >= read_offset &&
            s_corrupt_read_offset - read_offset < copied)
    {
        uint8_t *read_data = data;
        read_data[s_corrupt_read_offset - read_offset] ^= UINT8_C(0xFF);
        s_corrupt_read_offset = SIZE_MAX;
    }
    open_file->offset += copied;
    s_total_read += copied;
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    return (ssize_t)copied;
}

int __wrap_close(int descriptor)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    test_virtual_descriptor_t *open_file =
        _test_descriptor_locked(descriptor);
    if (open_file == NULL)
    {
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = EBADF;
        return -1;
    }
    memset(open_file, 0, sizeof(*open_file));
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    return 0;
}

int __wrap_fsync(int descriptor)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    const bool valid = _test_descriptor_locked(descriptor) != NULL;
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    if (!valid)
    {
        errno = EBADF;
        return -1;
    }
    return 0;
}

int __wrap_unlink(const char *path)
{
    (void)pthread_mutex_lock(&s_filesystem_lock);
    ++s_unlink_call_count;
    const int file_index = _test_find_file_locked(path);
    if (file_index < 0)
    {
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = ENOENT;
        return -1;
    }
    if (s_unlink_failures_remaining > 0U)
    {
        --s_unlink_failures_remaining;
        (void)pthread_mutex_unlock(&s_filesystem_lock);
        errno = EACCES;
        return -1;
    }

    const test_virtual_file_t *file = &s_files[file_index];
    memset(&s_last_unlinked, 0, sizeof(s_last_unlinked));
    memcpy(s_last_unlinked.path, file->path, sizeof(s_last_unlinked.path));
    memcpy(s_last_unlinked.data, file->data, file->size);
    s_last_unlinked.size = file->size;
    s_last_unlinked.valid = true;
    memset(&s_files[file_index], 0, sizeof(s_files[file_index]));
    (void)pthread_mutex_unlock(&s_filesystem_lock);
    return 0;
}

int main(void)
{
    _test_exclusive_collision_preserves_existing_file();
    _test_write_failure_removes_partial_file();
    _test_readback_corruption_fails_and_removes_file();
    _test_unlink_failure_retains_close_ownership();
    return 0;
}
