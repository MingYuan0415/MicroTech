cmake_minimum_required(VERSION 3.22)
if(NOT IDF_TARGET)
    set(IDF_TARGET "esp32s3")
endif()
set(MT_BOARD "" CACHE STRING
    "MicroTech board id; empty selects the target's only registered board")
set(MT_PRODUCT "" CACHE STRING
    "MicroTech product id; empty selects the board's default product")

include(${CMAKE_CURRENT_SOURCE_DIR}/layers/bsp/boards.cmake)
mt_resolve_board("${IDF_TARGET}" "${MT_BOARD}")
include(${CMAKE_CURRENT_SOURCE_DIR}/products/products.cmake)
set(_mt_requested_product "${MT_PRODUCT}")
if(_mt_requested_product STREQUAL "")
    set(_mt_requested_product "${MT_BOARD_DEFAULT_PRODUCT}")
endif()
mt_resolve_product("${MT_BOARD_ID}" "${_mt_requested_product}")
mt_validate_product_capabilities(
    "${MT_BOARD_HAL_CAPABILITIES}" "${MT_PRODUCT_HAL_CAPABILITIES}")
mt_product_expand_apps()
set(MT_SELECTED_PRODUCT "${MT_PRODUCT_ID}")
set(MT_PRODUCT_HEADER "${CMAKE_BINARY_DIR}/mt_product_profile.h")
mt_product_write_header("${MT_PRODUCT_HEADER}")
mt_product_write_cmake("${CMAKE_BINARY_DIR}/mt_product_expansion.cmake")
message(STATUS
    "MicroTech board=${MT_BOARD_ID} product=${MT_SELECTED_PRODUCT} "
    "apps=${MT_PRODUCT_SELECTED_APP_IDS}")

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
    if(NOT ${MT_BOARD_KCONFIG_SYMBOL})
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
    if(NOT CONFIG_LV_COLOR_FORMAT_RGB565)
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
    # NimBLE host buffers must survive cache-disabled flash operations, so the
    # production defaults keep them in internal RAM; no build gate enforces it.
    if(NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
        message(FATAL_ERROR
            "The Waveshare ESP32-S3 profile requires external BSS in PSRAM")
    endif()
    if(CONFIG_BT_NIMBLE_MAX_BONDS EQUAL 1)
        # NimBLE's one-bond NVS ordering loop is unreachable but GCC 15 still
        # diagnoses its guarded i + 1 expressions as out of bounds. Keep this
        # ESP-IDF 6.1 workaround inside the upstream BT component.
        idf_component_get_property(_bt_component bt COMPONENT_LIB)
        target_compile_options(${_bt_component} PRIVATE -Wno-array-bounds)
        # ESP-IDF 6.1 otherwise starts a fresh SMP procedure when an LTK
        # lookup fails. Device Link admits pairing only inside its local
        # window, so keep the project-owned build fail-closed outside it.
        target_compile_definitions(${_bt_component} PRIVATE
            MYNEWT_VAL_BLE_RESTART_PAIR=0)
    endif()
elseif(MT_BOARD_ID STREQUAL "waveshare_esp32_p4_wifi6_touch_lcd_3_5")
    if(NOT ${MT_BOARD_KCONFIG_SYMBOL})
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
            (CONFIG_FREERTOS_UNICORE OR
             NOT CONFIG_FREERTOS_NUMBER_OF_CORES EQUAL 2))
        message(FATAL_ERROR
            "Task-core isolation requires ESP32-P4 dual-core FreeRTOS")
    endif()
    if(_project_task_affinity_enabled AND _lvgl_task_affinity_enabled AND
            CONFIG_MAIN_PROJECT_TASK_CORE_ID EQUAL
            CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID)
        message(FATAL_ERROR
            "Project-owned tasks and the LVGL worker must use different cores")
    endif()
    if(NOT CONFIG_CACHE_L2_CACHE_256KB OR
            NOT CONFIG_CACHE_L2_CACHE_LINE_128B)
        message(FATAL_ERROR
            "The ESP32-P4 LVGL profile requires 256K/128B L2 cache")
    endif()
    if(NOT CONFIG_LV_COLOR_FORMAT_RGB565)
        message(FATAL_ERROR
            "The Waveshare display pipeline requires LVGL RGB565")
    endif()
    if(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL LESS 131072)
        message(FATAL_ERROR
            "Hosted SDIO, I2S, and LCD DMA staging require a 128 KiB internal reserve")
    endif()
    if(NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
        message(FATAL_ERROR
            "The Waveshare ESP32-P4 profile requires external BSS in PSRAM")
    endif()
    if(NOT CONFIG_BT_CONTROLLER_DISABLED OR
            NOT CONFIG_ESP_HOSTED_HOST_FEAT_BT)
        message(FATAL_ERROR
            "The ESP32-P4 BLE profile requires the hosted controller HCI")
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
    if(CONFIG_BT_NIMBLE_MAX_BONDS EQUAL 1)
        idf_component_get_property(_bt_component bt COMPONENT_LIB)
        target_compile_options(${_bt_component} PRIVATE -Wno-array-bounds)
        target_compile_definitions(${_bt_component} PRIVATE
            MYNEWT_VAL_BLE_RESTART_PAIR=0)
    endif()
else()
    message(FATAL_ERROR "The selected MicroTech board has no root build profile")
endif()
