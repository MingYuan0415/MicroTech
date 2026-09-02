/** @file ESP-IDF error compatibility for the simulator. */
#ifndef __SIM_ESP_ERR_H__
#define __SIM_ESP_ERR_H__

/** @brief Host representation of an ESP-IDF error code. */
typedef int esp_err_t;

#define ESP_OK               0
#define ESP_FAIL            -1
#define ESP_ERR_NO_MEM      -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_NOT_SUPPORTED -4
#define ESP_ERR_INVALID_STATE -5
#define ESP_ERR_NOT_FOUND     -6
#define ESP_ERR_TIMEOUT       -7
#define ESP_ERR_INVALID_SIZE  -8
#define ESP_ERR_INVALID_VERSION -9
#define ESP_ERR_INVALID_RESPONSE -10
#define ESP_ERR_INVALID_CRC   -11
#define ESP_ERR_INVALID_MAC   -12
#define ESP_ERR_NOT_FINISHED  -13

#define ESP_ERR_NVS_BASE                    0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED         (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND               (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH           (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY               (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE        (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_NAME            (ESP_ERR_NVS_BASE + 0x06)
#define ESP_ERR_NVS_INVALID_HANDLE          (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_KEY_TOO_LONG            (ESP_ERR_NVS_BASE + 0x09)
#define ESP_ERR_NVS_INVALID_STATE           (ESP_ERR_NVS_BASE + 0x0b)
#define ESP_ERR_NVS_INVALID_LENGTH          (ESP_ERR_NVS_BASE + 0x0c)
#define ESP_ERR_NVS_NO_FREE_PAGES           (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_VALUE_TOO_LONG          (ESP_ERR_NVS_BASE + 0x0e)
#define ESP_ERR_NVS_NEW_VERSION_FOUND       (ESP_ERR_NVS_BASE + 0x10)

/** @brief Map an error code to its name (mirror of IDF's helper). */
const char *esp_err_to_name(esp_err_t code);

#define ESP_ERROR_CHECK(x) do {                             \
        esp_err_t _err_rc = (x);                            \
        (void) _err_rc;                                     \
    } while (0)

#endif /* __SIM_ESP_ERR_H__ */
