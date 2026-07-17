#ifndef __ESP_LOG_H__
#define __ESP_LOG_H__

/**
 * @brief Consume one formatted log call during host tests.
 *
 * @param level identifies the log level.
 * @param tag identifies the source module.
 * @param format is the printf-compatible format string.
 */
void test_log_write(const char *level, const char *tag, const char *format, ...);

#define ESP_LOGE(tag, format, ...) \
    test_log_write("E", tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) \
    test_log_write("W", tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) \
    test_log_write("I", tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) \
    test_log_write("D", tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) \
    test_log_write("V", tag, format, ##__VA_ARGS__)

#endif /* __ESP_LOG_H__ */
