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
include("${CMAKE_CURRENT_LIST_DIR}/../layers/apps/resource_manifest.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../layers/app_manager/app_theme/resource_manifest.cmake")
set(APP_RESOURCE_RECORDS ${MICROTECH_APP_RESOURCE_RECORDS}
    ${MICROTECH_THEME_RESOURCE_RECORDS})
set(APP_RESOURCE_OUTPUTS "")
set(APP_RESOURCE_SEMANTICS "")
set(APP_RESOURCE_STAGED_OUTPUTS "")
set(APP_RESOURCE_IMAGE_ENTRIES "")
set(APP_RESOURCE_STAGED_FILES "")
set(APP_RESOURCE_SORTED_NAMES "")
set(APP_RESOURCE_ENABLED 0)

foreach(APP_RESOURCE_RECORD IN LISTS APP_RESOURCE_RECORDS)
    string(REPLACE "|" ";" APP_RESOURCE_FIELDS "${APP_RESOURCE_RECORD}")
    list(LENGTH APP_RESOURCE_FIELDS APP_RESOURCE_FIELD_COUNT)
    if(NOT APP_RESOURCE_FIELD_COUNT EQUAL 6)
        message(FATAL_ERROR "Invalid resource manifest record: ${APP_RESOURCE_RECORD}")
    endif()
    list(GET APP_RESOURCE_FIELDS 0 APP_RESOURCE_SOURCE)
    list(GET APP_RESOURCE_FIELDS 1 APP_RESOURCE_OUTPUT)
    list(GET APP_RESOURCE_FIELDS 2 APP_RESOURCE_SEMANTIC)
    list(GET APP_RESOURCE_FIELDS 3 APP_RESOURCE_WIDTH)
    list(GET APP_RESOURCE_FIELDS 4 APP_RESOURCE_HEIGHT)
    list(GET APP_RESOURCE_FIELDS 5 APP_RESOURCE_KIND)
    if(NOT EXISTS "${APP_RESOURCE_SOURCE}")
        message(FATAL_ERROR "Resource source does not exist: ${APP_RESOURCE_SOURCE}")
    endif()
    list(FIND APP_RESOURCE_OUTPUTS "${APP_RESOURCE_OUTPUT}" APP_RESOURCE_OUTPUT_INDEX)
    if(NOT APP_RESOURCE_OUTPUT_INDEX EQUAL -1)
        message(FATAL_ERROR "Duplicate resource output name: ${APP_RESOURCE_OUTPUT}")
    endif()
    list(APPEND APP_RESOURCE_OUTPUTS "${APP_RESOURCE_OUTPUT}")
    if(APP_RESOURCE_KIND STREQUAL "PNG")
        if(APP_RESOURCE_WIDTH LESS 1 OR APP_RESOURCE_HEIGHT LESS 1)
            message(FATAL_ERROR "Invalid PNG dimensions: ${APP_RESOURCE_SOURCE}")
        endif()
        list(FIND APP_RESOURCE_SEMANTICS "${APP_RESOURCE_SEMANTIC}" APP_RESOURCE_SEMANTIC_INDEX)
        if(NOT APP_RESOURCE_SEMANTIC_INDEX EQUAL -1)
            message(FATAL_ERROR "Duplicate resource semantic ID: ${APP_RESOURCE_SEMANTIC}")
        endif()
        list(APPEND APP_RESOURCE_SEMANTICS "${APP_RESOURCE_SEMANTIC}")
    elseif(NOT APP_RESOURCE_KIND STREQUAL "FONT")
        message(FATAL_ERROR "Unsupported resource kind ${APP_RESOURCE_KIND}: ${APP_RESOURCE_SOURCE}")
    endif()
    if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS AND APP_RESOURCE_KIND STREQUAL "PNG")
        string(REGEX REPLACE "\\.[^.]+$" ".bin" APP_RESOURCE_STAGED_NAME "${APP_RESOURCE_OUTPUT}")
    else()
        set(APP_RESOURCE_STAGED_NAME "${APP_RESOURCE_OUTPUT}")
    endif()
    get_filename_component(APP_RESOURCE_STAGED_EXT
        "${APP_RESOURCE_STAGED_NAME}" EXT)
    get_filename_component(APP_RESOURCE_STAGED_BASE
        "${APP_RESOURCE_STAGED_NAME}" NAME_WE)
    list(FIND APP_RESOURCE_STAGED_OUTPUTS "${APP_RESOURCE_STAGED_NAME}"
        APP_RESOURCE_STAGED_INDEX)
    if(NOT APP_RESOURCE_STAGED_INDEX EQUAL -1)
        message(FATAL_ERROR "Duplicate staged resource name: ${APP_RESOURCE_STAGED_NAME}")
    endif()
    list(APPEND APP_RESOURCE_STAGED_OUTPUTS "${APP_RESOURCE_STAGED_NAME}")
    list(APPEND APP_RESOURCE_SORTED_NAMES
        "${APP_RESOURCE_STAGED_EXT}|${APP_RESOURCE_STAGED_BASE}")
endforeach()

list(SORT APP_RESOURCE_SORTED_NAMES)
list(LENGTH APP_RESOURCE_RECORDS APP_RESOURCE_RECORD_COUNT)
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS AND APP_RESOURCE_RECORD_COUNT GREATER 0)
    set(APP_RESOURCE_ENABLED 1)
endif()

if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    idf_build_get_property(APP_RESOURCE_PYTHON PYTHON)
    idf_component_get_property(APP_RESOURCE_LVGL_DIR lvgl__lvgl COMPONENT_DIR)
    set(APP_RESOURCE_CONVERTER "${APP_RESOURCE_LVGL_DIR}/scripts/LVGLImage.py")
    if(NOT EXISTS "${APP_RESOURCE_CONVERTER}")
        message(FATAL_ERROR "The locked LVGL component does not provide scripts/LVGLImage.py")
    endif()
    set(APP_RESOURCE_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/app_res_fs")
else()
    set(APP_RESOURCE_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/app_res_raw")
endif()

set(APP_RESOURCE_INDEX 0)
foreach(APP_RESOURCE_RECORD IN LISTS APP_RESOURCE_RECORDS)
    string(REPLACE "|" ";" APP_RESOURCE_FIELDS "${APP_RESOURCE_RECORD}")
    list(GET APP_RESOURCE_FIELDS 0 APP_RESOURCE_SOURCE)
    list(GET APP_RESOURCE_FIELDS 1 APP_RESOURCE_OUTPUT)
    list(GET APP_RESOURCE_FIELDS 2 APP_RESOURCE_SEMANTIC)
    list(GET APP_RESOURCE_FIELDS 3 APP_RESOURCE_WIDTH)
    list(GET APP_RESOURCE_FIELDS 4 APP_RESOURCE_HEIGHT)
    list(GET APP_RESOURCE_FIELDS 5 APP_RESOURCE_KIND)
    if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS AND APP_RESOURCE_KIND STREQUAL "PNG")
        string(REGEX REPLACE "\\.[^.]+$" ".bin" APP_RESOURCE_STAGED_NAME "${APP_RESOURCE_OUTPUT}")
    else()
        set(APP_RESOURCE_STAGED_NAME "${APP_RESOURCE_OUTPUT}")
    endif()
    get_filename_component(APP_RESOURCE_STAGED_EXT
        "${APP_RESOURCE_STAGED_NAME}" EXT)
    get_filename_component(APP_RESOURCE_STAGED_BASE
        "${APP_RESOURCE_STAGED_NAME}" NAME_WE)
    list(FIND APP_RESOURCE_SORTED_NAMES
        "${APP_RESOURCE_STAGED_EXT}|${APP_RESOURCE_STAGED_BASE}"
        APP_RESOURCE_ASSET_INDEX)
    if(APP_RESOURCE_KIND STREQUAL "PNG")
        string(APPEND APP_RESOURCE_IMAGE_ENTRIES
            "    { ${APP_RESOURCE_SEMANTIC}, ${APP_RESOURCE_ASSET_INDEX}U, ${APP_RESOURCE_WIDTH}U, ${APP_RESOURCE_HEIGHT}U },\n")
    endif()
    set(APP_RESOURCE_DEST "${APP_RESOURCE_STAGE_DIR}/${APP_RESOURCE_STAGED_NAME}")
    if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS AND APP_RESOURCE_KIND STREQUAL "PNG")
        set(APP_RESOURCE_TMP "${CMAKE_CURRENT_BINARY_DIR}/app_res_tmp_${APP_RESOURCE_INDEX}")
        get_filename_component(APP_RESOURCE_SOURCE_STEM "${APP_RESOURCE_SOURCE}" NAME_WE)
        add_custom_command(OUTPUT "${APP_RESOURCE_DEST}"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${APP_RESOURCE_TMP}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_RESOURCE_TMP}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_RESOURCE_STAGE_DIR}"
            COMMAND "${APP_RESOURCE_PYTHON}" "${APP_RESOURCE_CONVERTER}"
                --ofmt BIN --cf RGB565A8 --compress NONE
                --output "${APP_RESOURCE_TMP}" "${APP_RESOURCE_SOURCE}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APP_RESOURCE_TMP}/${APP_RESOURCE_SOURCE_STEM}.bin" "${APP_RESOURCE_DEST}"
            DEPENDS "${APP_RESOURCE_SOURCE}" "${APP_RESOURCE_CONVERTER}"
            VERBATIM)
    else()
        add_custom_command(OUTPUT "${APP_RESOURCE_DEST}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_RESOURCE_STAGE_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APP_RESOURCE_SOURCE}" "${APP_RESOURCE_DEST}"
            DEPENDS "${APP_RESOURCE_SOURCE}"
            VERBATIM)
    endif()
    list(APPEND APP_RESOURCE_STAGED_FILES "${APP_RESOURCE_DEST}")
    math(EXPR APP_RESOURCE_INDEX "${APP_RESOURCE_INDEX} + 1")
endforeach()

set(APP_RESOURCE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/app_resources_generated.h")
set(APP_RESOURCE_FILE_COUNT_VALUE 0)
set(APP_RESOURCE_CHECKSUM_VALUE 0)
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    set(APP_RESOURCE_FILE_COUNT_VALUE MMAP_RES_FILES)
    set(APP_RESOURCE_CHECKSUM_VALUE MMAP_RES_CHECKSUM)
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/app_resources_generated.h.in"
    "${APP_RESOURCE_HEADER}" @ONLY)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    APP_RESOURCES_ENABLED=${APP_RESOURCE_ENABLED})

add_custom_target(app_resources_stage DEPENDS ${APP_RESOURCE_STAGED_FILES})
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    spiffs_create_partition_assets(res "${APP_RESOURCE_STAGE_DIR}"
        FLASH_IN_PROJECT MMAP_FILE_SUPPORT_FORMAT ".ttf,.bin"
        DEPENDS ${APP_RESOURCE_STAGED_FILES})
    add_dependencies(${COMPONENT_LIB} assets_res_bin)
else()
    fatfs_create_rawflash_image(res "${APP_RESOURCE_STAGE_DIR}" FLASH_IN_PROJECT)
    add_dependencies(fatfs_res_bin app_resources_stage)
endif()
