#ifndef __MAIN_HOST_DISPLAY_BENCHMARK_PROFILE_H__
#define __MAIN_HOST_DISPLAY_BENCHMARK_PROFILE_H__

static const display_benchmark_config_t g_display_benchmark_profile =
{
    .mode = DISPLAY_BENCHMARK_MODE_STRESS,
    .stress_duration_sec = 1800U,
    .effect_duration_sec = 30U,
    .load = DISPLAY_BENCHMARK_LOAD_FULL,
#if APP_RUNTIME_TEST_SECURITY2
    .ble_mode = DISPLAY_BENCHMARK_BLE_SECURITY2_CONNECTED,
    .app_workload = DISPLAY_BENCHMARK_APP_WORKLOAD_SYSTEM_ROUTES,
    .audio_volume_percent = 5U,
#else
    .ble_mode = DISPLAY_BENCHMARK_BLE_OFF,
    .app_workload = DISPLAY_BENCHMARK_APP_WORKLOAD_DISPLAY_ROUTES,
    .audio_volume_percent = 60U,
#endif
    .ipv4_host = "127.0.0.1",
    .port = 5001U,
    .rate_kbit_s = 2048U,
};

#endif /* __MAIN_HOST_DISPLAY_BENCHMARK_PROFILE_H__ */
