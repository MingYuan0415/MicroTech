/**
 * @brief Development-only display stress benchmark lifecycle.
 */

#ifndef __DISPLAY_BENCHMARK_H__
#define __DISPLAY_BENCHMARK_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Arm the benchmark task.
 *
 * Profiles with a TCP workload wait for a Wi-Fi address before sampling.
 */
esp_err_t display_benchmark_start(void);

/** @brief Stop the benchmark before its runtime service dependencies. */
esp_err_t display_benchmark_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __DISPLAY_BENCHMARK_H__ */
