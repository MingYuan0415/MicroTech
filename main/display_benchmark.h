/**
 * @brief Development-only display stress benchmark lifecycle.
 */

#ifndef __DISPLAY_BENCHMARK_H__
#define __DISPLAY_BENCHMARK_H__

#include "esp_err.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Benchmark campaign mode selected by a generated test profile. */
typedef enum display_benchmark_mode
{
    DISPLAY_BENCHMARK_MODE_STRESS = 0,
    DISPLAY_BENCHMARK_MODE_CHARACTERIZATION,
} display_benchmark_mode_t;

/** @brief Concurrent workload used during the measured benchmark stage. */
typedef enum display_benchmark_load
{
    DISPLAY_BENCHMARK_LOAD_FULL = 1,
    DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY,
    DISPLAY_BENCHMARK_LOAD_TCP_ONLY,
} display_benchmark_load_t;

/** @brief Complete generated benchmark campaign configuration. */
typedef struct display_benchmark_config
{
    display_benchmark_mode_t mode; /**< Stress or characterization campaign. */
    uint32_t stress_duration_sec;  /**< Stress runtime, 10 through 28800 seconds. */
    uint32_t effect_duration_sec;  /**< Per-effect runtime, 5 through 300 seconds. */
    display_benchmark_load_t load; /**< Concurrent characterization workload. */
    const char *ipv4_host;         /**< Numeric IPv4 echo-server address. */
    uint16_t port;                 /**< Echo-server TCP port. */
    uint32_t rate_kbit_s;          /**< Per-direction TCP traffic rate. */
} display_benchmark_config_t;

/**
 * @brief Arm the benchmark task.
 *
 * Profiles with a TCP workload wait for a Wi-Fi address before sampling.
 * The complete configuration and host string are copied before return.
 */
esp_err_t display_benchmark_start(const display_benchmark_config_t *config);

/** @brief Stop the benchmark before its runtime service dependencies. */
esp_err_t display_benchmark_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __DISPLAY_BENCHMARK_H__ */
