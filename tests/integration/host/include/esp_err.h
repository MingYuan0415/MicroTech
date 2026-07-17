/** @file ESP-IDF error compatibility declarations for integration tests. */
#ifndef __CROSS_LAYER_ESP_ERR_H__
#define __CROSS_LAYER_ESP_ERR_H__

/** @brief Host representation of an ESP-IDF error code. */
typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                 (-1)
#define ESP_ERR_NO_MEM           0x101
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_STATE    0x103
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_NOT_FOUND        0x105
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_TIMEOUT          0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NOT_FINISHED     0x10C

/** @brief Return a stable fake name for an error code. */
const char *esp_err_to_name(esp_err_t error);

#endif /* __CROSS_LAYER_ESP_ERR_H__ */
