#ifndef __ESP_HEAP_CAPS_H__
#define __ESP_HEAP_CAPS_H__

#include <stddef.h>

#define MALLOC_CAP_8BIT     (1U << 2)
#define MALLOC_CAP_SPIRAM   (1U << 10)
#define MALLOC_CAP_INTERNAL (1U << 11)

/**
 * @brief Allocate host memory while accepting ESP-IDF capability choices.
 *
 * @param size is the allocation size in bytes.
 * @param preference_count is the number of following capability masks.
 *
 * @return Allocated memory, or NULL when the scripted allocation fails.
 */
void *heap_caps_malloc_prefer(size_t size, size_t preference_count, ...);

/**
 * @brief Release memory returned by heap_caps_malloc_prefer().
 *
 * @param memory is the allocation to release and may be NULL.
 */
void heap_caps_free(void *memory);

#endif /* __ESP_HEAP_CAPS_H__ */
