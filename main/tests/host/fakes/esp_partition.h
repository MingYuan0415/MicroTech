#ifndef __ESP_PARTITION_H__
#define __ESP_PARTITION_H__

#include "esp_err.h"

#include <stddef.h>

/** @brief Host representation of an ESP-IDF partition type. */
typedef int esp_partition_type_t;
/** @brief Host representation of an ESP-IDF partition subtype. */
typedef int esp_partition_subtype_t;

#define ESP_PARTITION_TYPE_DATA             1
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS 0x83

/** @brief Minimal partition descriptor used by storage tests. */
typedef struct esp_partition
{
    size_t size; /**< Partition size in bytes. */
} esp_partition_t;

/**
 * @brief Find the scripted fake partition.
 *
 * @return Fake partition descriptor when present, otherwise NULL.
 */
const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype,
    const char *label);
/**
 * @brief Read bytes from the scripted fake partition.
 *
 * @return Scripted ESP-IDF result.
 */
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *destination, size_t size);

#endif /* __ESP_PARTITION_H__ */
