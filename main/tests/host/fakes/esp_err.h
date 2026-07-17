#ifndef __ESP_ERR_H__
#define __ESP_ERR_H__

#include <stdint.h>

/** @brief Host-test representation of an ESP-IDF error code. */
typedef int32_t esp_err_t;

#define ESP_OK                0
#define ESP_FAIL             -1
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_TIMEOUT       0x107
#define ESP_ERR_INVALID_RESPONSE 0x108

/**
 * @brief Return a stable host-test name for an error code.
 *
 * @param code is the error code to describe.
 *
 * @return Static error name.
 */
const char *esp_err_to_name(esp_err_t code);

#endif /* __ESP_ERR_H__ */
