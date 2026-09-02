/** @file esp_log shim printing to stdout for the simulator. */
#ifndef __SIM_ESP_LOG_H__
#define __SIM_ESP_LOG_H__

#include <stdio.h>

#define ESP_LOG_LEVEL_LOCAL(level, tag, fmt, ...) \
    printf("%c (%llu) %s: " fmt "\n", (char)(level), \
           (unsigned long long)sim_log_uptime_ms(), (tag), ##__VA_ARGS__)

/** @brief Monotonic milliseconds helper shared by log and timer code. */
unsigned long long sim_log_uptime_ms(void);

#define ESP_LOGE(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_LEVEL_LOCAL('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)

#endif /* __SIM_ESP_LOG_H__ */
