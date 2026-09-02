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
        timer_service
        recorder_service
        onboarding_service
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

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    EXPECTED_CONTRACT_COMMIT="ec245e4e1bbf85ef73b003132679227d6ef4591b")

include("${CMAKE_CURRENT_LIST_DIR}/display_benchmark_profile.cmake")
if(CONFIG_MAIN_DISPLAY_BENCHMARK)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${DISPLAY_BENCHMARK_PROFILE_DIR}")
endif()

# 统一资源清单和分区打包规则。资源由 Apps/App Theme 所有，main 只负责聚合。
# 清单解析与暂存规则与 sim 共用：见 ../cmake/mt_app_resources.cmake。
include("${CMAKE_CURRENT_LIST_DIR}/../layers/apps/resource_manifest.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../layers/app_manager/app_theme/resource_manifest.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/mt_app_resources.cmake")

set(MT_APP_RESOURCE_RECORDS ${MICROTECH_APP_RESOURCE_RECORDS}
    ${MICROTECH_THEME_RESOURCE_RECORDS})
set(MT_APP_RESOURCE_RASTERIZER
    "${CMAKE_CURRENT_LIST_DIR}/../tools/asset_pipeline/svg2png.py")
idf_build_get_property(MT_APP_RESOURCE_PYTHON PYTHON)
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    idf_component_get_property(MT_APP_RESOURCE_LVGL_DIR lvgl__lvgl COMPONENT_DIR)
    set(MT_APP_RESOURCE_CONVERTER "${MT_APP_RESOURCE_LVGL_DIR}/scripts/LVGLImage.py")
    if(NOT EXISTS "${MT_APP_RESOURCE_CONVERTER}")
        message(FATAL_ERROR "The locked LVGL component does not provide scripts/LVGLImage.py")
    endif()
    set(MT_APP_RESOURCE_ENABLE_FS 1)
    set(MT_APP_RESOURCE_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/app_res_fs")
else()
    set(MT_APP_RESOURCE_CONVERTER "")
    set(MT_APP_RESOURCE_ENABLE_FS 0)
    set(MT_APP_RESOURCE_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/app_res_raw")
endif()
set(MT_APP_RESOURCE_TMP_ROOT "${CMAKE_CURRENT_BINARY_DIR}")

mt_stage_app_resources()

set(APP_RESOURCE_ENABLED 0)
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS AND MT_APP_RESOURCE_RECORD_COUNT GREATER 0)
    set(APP_RESOURCE_ENABLED 1)
endif()

set(APP_RESOURCE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/app_resources_generated.h")
set(APP_RESOURCE_FILE_COUNT_VALUE 0)
set(APP_RESOURCE_CHECKSUM_VALUE 0)
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    set(APP_RESOURCE_FILE_COUNT_VALUE MMAP_RES_FILES)
    set(APP_RESOURCE_CHECKSUM_VALUE MMAP_RES_CHECKSUM)
endif()
set(APP_RESOURCE_IMAGE_ENTRIES "${MT_APP_RESOURCE_IMAGE_ENTRIES}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/app_resources_generated.h.in"
    "${APP_RESOURCE_HEADER}" @ONLY)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    APP_RESOURCES_ENABLED=${APP_RESOURCE_ENABLED})

add_custom_target(app_resources_stage DEPENDS ${MT_APP_RESOURCE_STAGED_FILES})
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    spiffs_create_partition_assets(res "${MT_APP_RESOURCE_STAGE_DIR}"
        FLASH_IN_PROJECT MMAP_FILE_SUPPORT_FORMAT ".ttf,.bin"
        DEPENDS ${MT_APP_RESOURCE_STAGED_FILES})
    add_dependencies(${COMPONENT_LIB} assets_res_bin)
else()
    fatfs_create_rawflash_image(res "${MT_APP_RESOURCE_STAGE_DIR}" FLASH_IN_PROJECT)
    add_dependencies(fatfs_res_bin app_resources_stage)
endif()
