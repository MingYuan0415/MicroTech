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
    if(NOT CONFIG_BT_NIMBLE_SECURITY_ENABLE OR
            CONFIG_BT_NIMBLE_SM_LEGACY OR
            NOT CONFIG_BT_NIMBLE_SM_SC OR
            NOT CONFIG_BT_NIMBLE_SM_SC_ONLY EQUAL 1 OR
            NOT CONFIG_BT_NIMBLE_NVS_PERSIST OR
            NOT CONFIG_BT_NIMBLE_HS_PVCY OR
            CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY OR
            CONFIG_BT_NIMBLE_ENC_ADV_DATA OR
            NOT CONFIG_BT_NIMBLE_MAX_BONDS EQUAL 1 OR
            NOT CONFIG_BT_NIMBLE_MAX_CONNECTIONS EQUAL 1)
        message(FATAL_ERROR
            "Device Link requires SC-only NimBLE security, controller privacy, the audited peer-store schema, one bond, and one connection")
    endif()
    if(NOT CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC OR
            CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC OR
            NOT CONFIG_MBEDTLS_DYNAMIC_BUFFER)
        message(FATAL_ERROR
            "The production TLS profile requires default allocation and dynamic buffers")
    endif()
    if(NOT CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL OR
            CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL OR
            CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT)
        message(FATAL_ERROR
            "The production NimBLE profile requires external allocation")
    endif()
    if(NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
        message(FATAL_ERROR
            "The Waveshare ESP32-S3 profile requires external BSS in PSRAM")
    endif()
    if(CONFIG_BT_NIMBLE_MAX_BONDS EQUAL 1)
        # NimBLE's one-bond NVS ordering loop is unreachable but GCC 15 still
        # diagnoses its guarded i + 1 expressions as out of bounds. Keep this
        # ESP-IDF 6.0.2 workaround inside the upstream BT component.
        idf_component_get_property(_bt_component bt COMPONENT_LIB)
        target_compile_options(${_bt_component} PRIVATE -Wno-array-bounds)
        # ESP-IDF 6.0.2 otherwise starts a fresh SMP procedure when an LTK
        # lookup fails. Device Link admits pairing only inside its local
        # window, so keep the project-owned build fail-closed outside it.
        target_compile_definitions(${_bt_component} PRIVATE
            MYNEWT_VAL_BLE_RESTART_PAIR=0)
    endif()
else()
    message(FATAL_ERROR "The selected MicroTech board has no root build profile")
endif()
