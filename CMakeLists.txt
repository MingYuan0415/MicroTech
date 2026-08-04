cmake_minimum_required(VERSION 3.22)
if(NOT IDF_TARGET)
    set(IDF_TARGET "esp32s3")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/layers/bsp/boards.cmake)
mt_resolve_board("${IDF_TARGET}")

set(EXTRA_COMPONENT_DIRS
    layers/bsp
    layers/middleware/components
    layers/app_manager
    layers/apps
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(microtech)

if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE AND NOT CONFIG_LV_USE_CLIB_MALLOC)
    message(FATAL_ERROR
        "LVGL FreeType requires the libc allocator for fonts larger than "
        "the fixed LVGL pool")
endif()

if(NOT CONFIG_LV_USE_CLIB_STRING OR
        NOT CONFIG_LV_USE_CLIB_SPRINTF OR
        NOT CONFIG_LV_OBJ_STYLE_CACHE)
    message(FATAL_ERROR "The production LVGL profile requires libc string/sprintf and style cache")
endif()

if(MT_BOARD_ID STREQUAL "waveshare_esp32_s3_touch_amoled_1_8")
    if(NOT CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_8)
        message(FATAL_ERROR
            "The selected target and MicroTech board Kconfig do not match")
    endif()
    set(_project_task_affinity_enabled FALSE)
    if(CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 OR
            CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1)
        set(_project_task_affinity_enabled TRUE)
    endif()
    set(_lvgl_task_affinity_enabled FALSE)
    if(CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU0 OR
            CONFIG_APP_MANAGER_LVGL_WORKER_AFFINITY_CPU1)
        set(_lvgl_task_affinity_enabled TRUE)
    endif()
    if((_project_task_affinity_enabled OR _lvgl_task_affinity_enabled) AND
            (CONFIG_FREERTOS_UNICORE OR CONFIG_FREERTOS_SMP OR
             NOT CONFIG_FREERTOS_NUMBER_OF_CORES EQUAL 2))
        message(FATAL_ERROR
            "Task-core isolation requires ESP32-S3 legacy dual-core FreeRTOS")
    endif()
    if(_project_task_affinity_enabled AND _lvgl_task_affinity_enabled AND
            CONFIG_MAIN_PROJECT_TASK_CORE_ID EQUAL
            CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID)
        message(FATAL_ERROR
            "Project-owned tasks and the LVGL worker must use different cores")
    endif()
    if(NOT CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB OR
            NOT CONFIG_ESP32S3_DATA_CACHE_64KB OR
            NOT CONFIG_ESP32S3_DATA_CACHE_LINE_64B)
        message(FATAL_ERROR
            "The ESP32-S3 LVGL profile requires 32K I-cache and 64K/64B D-cache")
    endif()
    if(NOT CONFIG_LV_COLOR_DEPTH_16)
        message(FATAL_ERROR
            "The Waveshare display pipeline requires LVGL RGB565")
    endif()
    if(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL LESS 131072)
        message(FATAL_ERROR
            "Wi-Fi, I2S, and LCD DMA staging require a 128 KiB internal reserve")
    endif()
    if(NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
        message(FATAL_ERROR
            "The Waveshare ESP32-S3 profile requires external BSS in PSRAM")
    endif()
else()
    message(FATAL_ERROR "The selected MicroTech board has no root build profile")
endif()
