/** @file LVGL filesystem driver for the 'F:' resource volume.
 *
 * Mirrors the firmware model: the resource FS is served from mmap asset
 * memory (never host POSIX), so FreeType with LV_FREETYPE_USE_LVGL_PORT=1
 * reads "F:font.ttf" through the same buffers the adapter images use.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_mmap_assets.h"
#include "lvgl.h"

typedef struct sim_fs_file
{
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
} sim_fs_file_t;

static lv_fs_drv_t s_fs_drv;
static char s_fs_assets_owner_seen;

static void *_fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    mmap_assets_handle_t assets = drv->user_data;
    const char *name = ((path[0] != '\0') && (path[1] == ':'))
                       ? (path + 2U)
                       : path;

    if ((mode & LV_FS_MODE_WR) != 0U)
    {
        return NULL;
    }
    for (int i = 0; mmap_assets_get_name(assets, i) != NULL; i++)
    {
        if (strcmp(mmap_assets_get_name(assets, i), name) == 0)
        {
            sim_fs_file_t *file = calloc(1, sizeof(*file));
            if (file == NULL)
            {
                return NULL;
            }
            file->data = mmap_assets_get_mem(assets, i);
            file->size = (uint32_t)mmap_assets_get_size(assets, i);
            return file;
        }
    }
    return NULL;
}

static lv_fs_res_t _fs_close(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    free(file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t _fs_read(lv_fs_drv_t *drv, void *file_p, void *buf,
                            uint32_t btr, uint32_t *br)
{
    sim_fs_file_t *file = file_p;
    uint32_t avail;

    (void)drv;
    if (file == NULL)
    {
        return LV_FS_RES_NOT_EX;
    }
    avail = file->size - file->pos;
    *br = (btr < avail) ? btr : avail;
    memcpy(buf, file->data + file->pos, *br);
    file->pos += *br;
    return LV_FS_RES_OK;
}

static lv_fs_res_t _fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                            lv_fs_whence_t whence)
{
    sim_fs_file_t *file = file_p;

    (void)drv;
    if (file == NULL)
    {
        return LV_FS_RES_NOT_EX;
    }
    switch (whence)
    {
    case LV_FS_SEEK_SET:
        if (pos > file->size)
        {
            fprintf(stderr, "sim_fs: SEEK_SET beyond eof pos=%u size=%u\n",
                    pos, file->size);
        }
        file->pos = pos;
        break;
    case LV_FS_SEEK_CUR:
        file->pos += pos;
        break;
    case LV_FS_SEEK_END:
        file->pos = file->size + pos;
        break;
    default:
        return LV_FS_RES_INV_PARAM;
    }
    if (file->pos > file->size)
    {
        file->pos = file->size;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t _fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    if (file_p == NULL)
    {
        return LV_FS_RES_NOT_EX;
    }
    *pos_p = ((sim_fs_file_t *)file_p)->pos;
    return LV_FS_RES_OK;
}

esp_err_t sim_fs_mount(char letter, mmap_assets_handle_t assets)
{
    if (s_fs_assets_owner_seen != '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    lv_fs_drv_init(&s_fs_drv);
    s_fs_drv.letter = letter;
    s_fs_drv.ready_cb = NULL;
    s_fs_drv.open_cb = _fs_open;
    s_fs_drv.close_cb = _fs_close;
    s_fs_drv.read_cb = _fs_read;
    s_fs_drv.seek_cb = _fs_seek;
    s_fs_drv.tell_cb = _fs_tell;
    s_fs_drv.user_data = assets;
    lv_fs_drv_register(&s_fs_drv);
    s_fs_assets_owner_seen = 'F';
    return ESP_OK;
}

void sim_fs_unmount(void)
{
    s_fs_assets_owner_seen = '\0';
}
