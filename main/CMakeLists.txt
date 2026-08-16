idf_component_register(
    SRCS "app_main.c" "app_product_config.c" "app_runtime.c" "app_runtime_pm.c"
         "network_runtime.c"
         "display_benchmark.c"
         "fs_storage/fs_storage.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        apps
        bsp
        chore_service
        mt_log
        fatfs
        littlefs
        esp_event
        esp_netif
        esp_partition
        event_bus
        nv_storage
        time_service
        weather_service
        system_pm
        power_service
        imu_service
        audio_service
        sd_storage_service
        connectivity_manager
        device_link_service
        factory_reset_service
        app_core
        app_theme
        esp_timer
        esp_hw_support
        freertos
        heap
        lwip
)

# Contract pin: the Device Link contract commit this firmware is built
# against (see contracts/provisioning and tooling.contractcheck.pin). The
# configuration governance test verifies this value against contract.lock.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    EXPECTED_CONTRACT_COMMIT="fc651f9ebf313e7a1d7602012eec99c0b27af3e4")

include("${CMAKE_CURRENT_LIST_DIR}/display_benchmark_profile.cmake")
if(CONFIG_MAIN_DISPLAY_BENCHMARK)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${DISPLAY_BENCHMARK_PROFILE_DIR}")
endif()

# 资源分区打包规则
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    set(APP_WEATHER_IMAGE_FILES
        weather_app.png
        weather_clear_day_112.png weather_clear_day_40.png
        weather_clear_night_112.png weather_clear_night_40.png
        weather_partly_day_112.png weather_partly_day_40.png
        weather_partly_night_112.png weather_partly_night_40.png
        weather_cloudy_112.png weather_cloudy_40.png
        weather_overcast_112.png weather_overcast_40.png
        weather_drizzle_112.png weather_drizzle_40.png
        weather_rain_112.png weather_rain_40.png
        weather_heavy_rain_112.png weather_heavy_rain_40.png
        weather_thunder_112.png weather_thunder_40.png
        weather_hail_112.png weather_hail_40.png
        weather_freezing_rain_112.png weather_freezing_rain_40.png
        weather_snow_112.png weather_snow_40.png
        weather_heavy_snow_112.png weather_heavy_snow_40.png
        weather_sleet_112.png weather_sleet_40.png
        weather_fog_112.png weather_fog_40.png
        weather_haze_112.png weather_haze_40.png
        weather_hot_112.png weather_hot_40.png
        weather_cold_112.png weather_cold_40.png
        weather_unknown_112.png weather_unknown_40.png)
    set(APP_WEATHER_IMAGE_COUNT 0)
    foreach(APP_WEATHER_IMAGE_FILE IN LISTS APP_WEATHER_IMAGE_FILES)
        if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/res_fs/${APP_WEATHER_IMAGE_FILE}")
            math(EXPR APP_WEATHER_IMAGE_COUNT "${APP_WEATHER_IMAGE_COUNT} + 1")
        endif()
    endforeach()
    list(LENGTH APP_WEATHER_IMAGE_FILES APP_WEATHER_IMAGE_EXPECTED_COUNT)
    if(APP_WEATHER_IMAGE_COUNT GREATER 0 AND
            NOT APP_WEATHER_IMAGE_COUNT EQUAL APP_WEATHER_IMAGE_EXPECTED_COUNT)
        message(FATAL_ERROR
            "Weather image set is incomplete: found ${APP_WEATHER_IMAGE_COUNT} of ${APP_WEATHER_IMAGE_EXPECTED_COUNT}")
    endif()

    if(APP_WEATHER_IMAGE_COUNT EQUAL APP_WEATHER_IMAGE_EXPECTED_COUNT)
        idf_build_get_property(APP_WEATHER_PYTHON PYTHON)
        idf_component_get_property(APP_WEATHER_LVGL_DIR lvgl__lvgl COMPONENT_DIR)
        set(APP_WEATHER_CONVERTER
            "${APP_WEATHER_LVGL_DIR}/scripts/LVGLImage.py")
        if(NOT EXISTS "${APP_WEATHER_CONVERTER}")
            message(FATAL_ERROR
                "The locked LVGL component does not provide scripts/LVGLImage.py")
        endif()

        set(APP_WEATHER_GENERATED_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/weather_res_fs")
        set(APP_WEATHER_GENERATED_FILES "")
        foreach(APP_WEATHER_IMAGE_FILE IN LISTS APP_WEATHER_IMAGE_FILES)
            get_filename_component(APP_WEATHER_IMAGE_STEM
                "${APP_WEATHER_IMAGE_FILE}" NAME_WE)
            set(APP_WEATHER_IMAGE_SOURCE
                "${CMAKE_CURRENT_LIST_DIR}/res_fs/${APP_WEATHER_IMAGE_FILE}")
            set(APP_WEATHER_IMAGE_BIN
                "${APP_WEATHER_GENERATED_DIR}/${APP_WEATHER_IMAGE_STEM}.bin")
            add_custom_command(
                OUTPUT "${APP_WEATHER_IMAGE_BIN}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${APP_WEATHER_GENERATED_DIR}"
                COMMAND "${APP_WEATHER_PYTHON}" "${APP_WEATHER_CONVERTER}"
                    --ofmt BIN
                    --cf RGB565A8
                    --compress NONE
                    --output "${APP_WEATHER_GENERATED_DIR}"
                    "${APP_WEATHER_IMAGE_SOURCE}"
                DEPENDS
                    "${APP_WEATHER_IMAGE_SOURCE}"
                    "${APP_WEATHER_CONVERTER}"
                VERBATIM)
            list(APPEND APP_WEATHER_GENERATED_FILES
                "${APP_WEATHER_IMAGE_BIN}")
        endforeach()

        set(APP_WEATHER_FONT_SOURCE
            "${CMAKE_CURRENT_LIST_DIR}/res_fs/font.ttf")
        set(APP_WEATHER_FONT_OUTPUT
            "${APP_WEATHER_GENERATED_DIR}/font.ttf")
        add_custom_command(
            OUTPUT "${APP_WEATHER_FONT_OUTPUT}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${APP_WEATHER_GENERATED_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APP_WEATHER_FONT_SOURCE}"
                "${APP_WEATHER_FONT_OUTPUT}"
            DEPENDS "${APP_WEATHER_FONT_SOURCE}"
            VERBATIM)
        list(APPEND APP_WEATHER_GENERATED_FILES
            "${APP_WEATHER_FONT_OUTPUT}")

        spiffs_create_partition_assets(
            res "${APP_WEATHER_GENERATED_DIR}"
            FLASH_IN_PROJECT
            MMAP_FILE_SUPPORT_FORMAT ".ttf,.bin"
            DEPENDS ${APP_WEATHER_GENERATED_FILES})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            APP_WEATHER_IMAGES_ENABLED=1)
    else()
        spiffs_create_partition_assets(
            res res_fs
            FLASH_IN_PROJECT
            MMAP_FILE_SUPPORT_FORMAT ".ttf")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            APP_WEATHER_IMAGES_ENABLED=0)
    endif()
    add_dependencies(${COMPONENT_LIB} assets_res_bin)
else()
    fatfs_create_rawflash_image(res res_fs FLASH_IN_PROJECT)
endif()
