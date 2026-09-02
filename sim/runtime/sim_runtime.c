/** @file Simulator runtime assembly.
 *
 * Mirrors main/app_runtime.c:788-952 with sim-owned wiring (the real
 * app_runtime is not compiled: PM/Wi-Fi assembly is too IDF-coupled):
 * BSP port -> event_bus -> faked services -> app_manager_config ->
 * app_manager_init -> UI dispatch registration -> initial navigation ->
 * display_commit_initial -> startup_commit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_manager.h"
#include "app_manager_config.h"
#include "app_product_config.h"
#include "connectivity_manager.h"
#include "network_runtime.h"
#include "chore_service.h"
#include "factory_reset_service.h"
#include "app_theme.h"
#include "esp_timer.h"
#include "sim_res_meta.h"
#include "app_resources_generated.h"
#include "event_bus.h"
#include "imu_service.h"
#include "nv_storage.h"
#include "power_service.h"
#include "time_service.h"
#include "lvgl.h"
#include "mt_log.h"
#include "onboarding_service.h"
#include "weather_service.h"
#include "timer_service.h"

#include "sim_backends.h"
#include "sim_bsp.h"
#include "sim_runtime.h"

static const uint16_t s_font_sizes[] = APP_THEME_FONT_SIZES;

static esp_err_t _standby_request(void)
{
    return ESP_OK;
}

static esp_err_t _standby_cancel(void)
{
    return ESP_OK;
}

static esp_err_t _input_register(void (*cb)(app_manager_key_t,
                                 app_manager_key_event_t,
                                 void *),
                                 void *user_data)
{
    return sim_bsp_input_register(cb, user_data);
}

static esp_err_t _input_unregister(void)
{
    return ESP_OK;
}

esp_err_t sim_runtime_boot(void)
{
    app_manager_ui_dispatch_fn dispatch = NULL;
    esp_err_t result;

    if (sim_bsp_init() != ESP_OK)
    {
        fprintf(stderr, "sim_bsp_init failed\n");
        return ESP_FAIL;
    }
    if ((sim_bsp_capabilities() &
            (BSP_CAPABILITY_DISPLAY | BSP_CAPABILITY_TOUCH |
             BSP_CAPABILITY_INPUT)) !=
            (BSP_CAPABILITY_DISPLAY | BSP_CAPABILITY_TOUCH |
             BSP_CAPABILITY_INPUT))
    {
        return ESP_ERR_INVALID_STATE;
    }

    result = event_bus_init();
    if (result != ESP_OK)
    {
        return result;
    }
    result = nv_storage_init();
    if (result != ESP_OK)
    {
        return result;
    }
    const factory_reset_service_config_t reset_config =
    {
        .restart = sim_backends_restart,
        .restart_context = NULL,
    };
    result = factory_reset_service_init(&reset_config);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: factory_reset_service_init failed %s\n", esp_err_to_name(result));
        return result;
    }
    result = onboarding_service_init();
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: onboarding_service_init failed %s\n", esp_err_to_name(result));
        return result;
    }
    static const char *s_sim_cache_dir;
    static app_product_config_t product_copy;
    const app_product_config_t *product = app_product_config_get();

    /* Device uses /data for the weather cache; the host session gets a
     * sim-owned directory instead (simulated storage root). */
    if (s_sim_cache_dir == NULL)
    {
        s_sim_cache_dir = getenv("SIM_DATA_DIR") != NULL ?
                          getenv("SIM_DATA_DIR") : "sim_data";
    }
    product_copy = *product;
    product_copy.weather.cache_directory = s_sim_cache_dir;
    product = &product_copy;
    result = time_service_init(&product->time);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: time_service_init failed %s\n", esp_err_to_name(result));
        return result;
    }
    const timer_service_config_t timer_config =
    {
        .monotonic_time_us = esp_timer_get_time,
    };
    result = timer_service_init(&timer_config);
    if (result != ESP_OK)
    {
        return result;
    }
    result = chore_service_init(&product->chore);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: chore_service_init failed %s\n", esp_err_to_name(result));
        return result;
    }
    result = power_service_register_power_ops(&sim_power_ops);
    if (result == ESP_OK)
    {
        result = power_service_init(&product->power);
    }
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: power_service_init failed %s\n", esp_err_to_name(result));
        return result;
    }
    /* No BSP_CAPABILITY_IMU on the sim board, but level_app reads the
     * service snapshot: init the real service over the fake ops. */
    result = imu_service_register_imu_ops(&sim_imu_ops);
    if (result == ESP_OK)
    {
        result = imu_service_init(&product->imu);
    }
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: imu_service_init failed %s\n", esp_err_to_name(result));
        return result;
    }

    const bsp_screen_ops_t *screen = sim_bsp_screen_ops();
    const app_manager_config_t app_config =
    {
        .disp_port = sim_bsp_display_port(),
        .font_sizes = s_font_sizes,
        .font_count = sizeof(s_font_sizes) / sizeof(s_font_sizes[0]),
        .res_fs_letter = 'F',
        .resource_file_count = APP_RESOURCES_FILE_COUNT,
        .resource_checksum = APP_RESOURCES_CHECKSUM,
        .image_resources = APP_RESOURCES_IMAGE_TABLE,
        .image_resource_count = APP_RESOURCES_IMAGE_COUNT,
        .screen_ops = {
            .suspend = screen->suspend,
            .resume_prepare = screen->resume_prepare,
            .resume_commit = screen->resume_commit,
            .is_suspended = screen->is_suspended,
            .is_suspend_committed = screen->is_suspend_committed,
            .set_brightness = screen->set_brightness,
            .set_brightness_temp = screen->set_brightness_temp,
            .get_brightness = screen->get_brightness,
        },
        .input_ops = {
            .register_handler = _input_register,
            .unregister_handler = _input_unregister,
        },
        .standby_ops = {
            .request_standby = _standby_request,
            .cancel_standby = _standby_cancel,
            .is_standby_allowed = NULL,
        },
        .max_resident_apps = product->app_max_resident_apps,
        .resident_policy = product->app_resident_policy,
        .app_forward_transition = {
            .effect = APP_MANAGER_TRANSITION_FADE,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
        .app_back_transition = {
            .effect = APP_MANAGER_TRANSITION_FADE,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
        .page_forward_transition = {
            .effect = APP_MANAGER_TRANSITION_PUSH_LEFT,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
        .page_back_transition = {
            .effect = APP_MANAGER_TRANSITION_PUSH_RIGHT,
            .duration_ms = APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS,
        },
        .control_task_priority = product->app_control_task_priority,
    };

    result = app_manager_init(&app_config);
    if (result != ESP_OK)
    {
        fprintf(stderr, "app_manager_init failed: %s\n",
                esp_err_to_name(result));
        return result;
    }

    /* Mailbox is ready only after app_manager_init; the device registers
     * the UI dispatcher afterwards for exactly this reason. */
    result = app_manager_get_ui_dispatch_fn(&dispatch);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: app_manager_get_ui_dispatch_fn failed %s\n", esp_err_to_name(result));
        return result;
    }
    result = event_bus_register_ui_dispatch(dispatch);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: event_bus_register_ui_dispatch failed %s\n", esp_err_to_name(result));
        return result;
    }

    result = weather_service_init(&product->weather);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: weather_service_init failed %s\n",
                esp_err_to_name(result));
        return result;
    }
    result = network_runtime_init();
    if (result == ESP_OK && network_runtime_is_ready())
    {
        result = connectivity_manager_init(&product->connectivity);
    }
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: connectivity_manager_init failed %s\n",
                esp_err_to_name(result));
        return result;
    }

    int discovered = app_manager_builtin_discover();
    if (discovered <= 0)
    {
        fprintf(stderr, "builtin app registry empty (%d)\n", discovered);
        return ESP_ERR_NOT_FOUND;
    }

    onboarding_service_state_t onboarding = ONBOARDING_SERVICE_PENDING;
    if (onboarding_service_get_state(&onboarding) != ESP_OK)
    {
        onboarding = ONBOARDING_SERVICE_COMPLETED;
    }
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_RUN,
        .app_id = onboarding == ONBOARDING_SERVICE_PENDING ?
        APP_MANAGER_ID_SETUP : APP_MANAGER_ID_HOME,
        .transition = {
            .effect = APP_MANAGER_TRANSITION_NONE,
        },
    };
    result = app_manager_navigate(&request, UINT32_MAX);
    if (result != ESP_OK)
    {
        return result;
    }
    result = app_manager_display_commit_initial();
    if (result != ESP_OK)
    {
        return result;
    }
    result = app_manager_startup_commit();
    if (result != ESP_OK)
    {
        return result;
    }

    /* Simulator policy: keep the panel awake (the firmware "never" tier).
     * PM otherwise runs on wall-clock and would suspend mid-scenario:
     * the paused worker stops draining the mailbox and synchronous UI
     * calls would block forever. Scenarios opt back in via sim.pm. */
    result = app_manager_pm_set_timeout_ms(-1);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: pm timeout disable failed %s\n",
                esp_err_to_name(result));
        return result;
    }
    result = app_manager_pm_set_standby_delay_ms(-1);
    if (result != ESP_OK)
    {
        fprintf(stderr, "sim boot: pm standby disable failed %s\n",
                esp_err_to_name(result));
        return result;
    }

    printf("sim runtime up: builtin apps=%d\n", discovered);
    return ESP_OK;
}
