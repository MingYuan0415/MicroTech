cmake_minimum_required(VERSION 3.16)
project(main_runtime_host_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(MAIN_HOST_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE MAIN_HOST_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(MAIN_HOST_SETARCH setarch)

get_filename_component(MT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${MT_ROOT}/layers/bsp/boards.cmake")
include("${MT_ROOT}/products/products.cmake")
mt_resolve_board("esp32s3" "waveshare_esp32_s3_touch_amoled_1_8")
set(_requested_product "s3_reference")
mt_resolve_product("waveshare_esp32_s3_touch_amoled_1_8" "${_requested_product}")
mt_validate_product_capabilities(
    "${MT_BOARD_HAL_CAPABILITIES}" "${MT_PRODUCT_SERVICE_CAPABILITIES}")
mt_product_expand_apps()
set(MAIN_S3_PRODUCT_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/mt_product_s3_reference.h")
mt_product_write_header("${MAIN_S3_PRODUCT_HEADER}")
mt_resolve_board("esp32p4" "waveshare_esp32_p4_wifi6_touch_lcd_3_5")
set(_requested_product "p4_reference")
mt_resolve_product("waveshare_esp32_p4_wifi6_touch_lcd_3_5" "${_requested_product}")
mt_validate_product_capabilities(
    "${MT_BOARD_HAL_CAPABILITIES}" "${MT_PRODUCT_SERVICE_CAPABILITIES}")
mt_product_expand_apps()
set(MAIN_P4_PRODUCT_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/mt_product_p4_reference.h")
mt_product_write_header("${MAIN_P4_PRODUCT_HEADER}")

function(configure_main_host_target target)
    target_compile_features(${target} PRIVATE c_std_11)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../../tests/host"
    )
    target_compile_options(${target} PRIVATE
        -include "${CMAKE_CURRENT_LIST_DIR}/../../../tests/host/sdkconfig.h"
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -UNDEBUG
    )

    if(MAIN_HOST_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
            -fsanitize=address,undefined
        )
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
            -fsanitize=address,undefined
        )
    elseif(MAIN_HOST_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
            -fsanitize=thread
        )
        target_link_options(${target} PRIVATE
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
            -fsanitize=thread
        )
    elseif(NOT MAIN_HOST_SANITIZER STREQUAL "none")
        message(FATAL_ERROR
            "Unsupported MAIN_HOST_SANITIZER=${MAIN_HOST_SANITIZER}")
    endif()
endfunction()

function(add_main_host_test test_name target)
    if(MAIN_HOST_SANITIZER STREQUAL "thread" AND
            MAIN_HOST_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_test(NAME ${test_name} COMMAND
            ${MAIN_HOST_SETARCH} x86_64 -R $<TARGET_FILE:${target}>
            ${ARGN})
    else()
        add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target}> ${ARGN})
    endif()
    # 10 s starves the threaded runtime tests when ctest runs next to a
    # parallel compiler/simulator load; 30 s keeps them deterministic.
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
endfunction()

add_executable(app_runtime_test
    app_runtime_test.c
    ../../app_product_config.c
    ../../app_runtime.c
    ../../app_runtime_control.c
    timer_service_fake.c
    music_service_fake.c
)

target_include_directories(app_runtime_test PRIVATE
    fakes
    ../..
    ../../../layers/app_manager/app_core/include
    ../../../layers/app_manager/app_theme/include
    ../../../layers/apps/common/include
    ../../../layers/bsp/include
    ../../../layers/middleware/components/chore_service/include
    ../../../layers/middleware/components/device_link_service/include
    ../../../layers/middleware/components/factory_reset_service/include
    ../../../layers/middleware/components/ble_runtime/include
    ../../../layers/middleware/components/audio_service/include
    ../../../layers/middleware/components/connectivity_manager/include
    ../../../layers/middleware/components/event_bus/include
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/nv_storage/include
    ../../../layers/middleware/components/power_service/include
    ../../../layers/middleware/components/imu_service/include
    ../../../layers/middleware/components/sd_storage_service/include
    ../../../layers/middleware/components/system_pm/include
    ../../../layers/middleware/components/time_service/include
    ../../../layers/middleware/components/timer_service/include
    ../../../layers/middleware/components/recorder_service/include
    ../../../layers/middleware/components/file_service/include
    ../../../layers/middleware/components/onboarding_service/include
    ../../../layers/middleware/components/weather_service/include
    ../../../layers/middleware/components/music_service/include
)

configure_main_host_target(app_runtime_test)
target_compile_options(app_runtime_test PRIVATE
    "SHELL:-include ${MAIN_S3_PRODUCT_HEADER}")
add_main_host_test(app_runtime_test app_runtime_test)

set(MAIN_PRODUCT_TEST_INCLUDE_DIRS
    fakes
    ../..
    ../../../layers/app_manager/app_core/include
    ../../../layers/app_manager/app_theme/include
    ../../../layers/apps/common/include
    ../../../layers/bsp/include
    ../../../layers/middleware/components/chore_service/include
    ../../../layers/middleware/components/device_link_service/include
    ../../../layers/middleware/components/factory_reset_service/include
    ../../../layers/middleware/components/ble_runtime/include
    ../../../layers/middleware/components/audio_service/include
    ../../../layers/middleware/components/connectivity_manager/include
    ../../../layers/middleware/components/event_bus/include
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/nv_storage/include
    ../../../layers/middleware/components/power_service/include
    ../../../layers/middleware/components/imu_service/include
    ../../../layers/middleware/components/sd_storage_service/include
    ../../../layers/middleware/components/system_pm/include
    ../../../layers/middleware/components/time_service/include
    ../../../layers/middleware/components/timer_service/include
    ../../../layers/middleware/components/recorder_service/include
    ../../../layers/middleware/components/file_service/include
    ../../../layers/middleware/components/onboarding_service/include
    ../../../layers/middleware/components/weather_service/include
    ../../../layers/middleware/components/music_service/include
)

add_executable(app_product_config_p4_test
    app_product_config_p4_test.c
    ../../app_product_config.c
)
target_include_directories(app_product_config_p4_test PRIVATE
    ${MAIN_PRODUCT_TEST_INCLUDE_DIRS})
configure_main_host_target(app_product_config_p4_test)
target_compile_options(app_product_config_p4_test PRIVATE
    "SHELL:-include ${MAIN_P4_PRODUCT_HEADER}")
target_compile_definitions(app_product_config_p4_test PRIVATE
    CONFIG_IDF_TARGET_ESP32P4=1
)
add_main_host_test(app_product_config_p4_test app_product_config_p4_test)

add_executable(app_product_config_s3_test
    app_product_config_s3_test.c
    ../../app_product_config.c
)
target_include_directories(app_product_config_s3_test PRIVATE
    ${MAIN_PRODUCT_TEST_INCLUDE_DIRS})
configure_main_host_target(app_product_config_s3_test)
target_compile_options(app_product_config_s3_test PRIVATE
    "SHELL:-include ${MAIN_S3_PRODUCT_HEADER}")
add_main_host_test(app_product_config_s3_test app_product_config_s3_test)

add_executable(app_runtime_pm_test
    app_runtime_pm_test.c
    ../../app_runtime_pm.c
    timer_service_fake.c
    music_service_fake.c
)
target_include_directories(app_runtime_pm_test PRIVATE
    fakes
    ../..
    ../../../layers/app_manager/app_core/include
    ../../../layers/app_manager/app_theme/include
    ../../../layers/bsp/include
    ../../../layers/middleware/components/chore_service/include
    ../../../layers/middleware/components/event_bus/include
    ../../../layers/middleware/components/audio_service/include
    ../../../layers/middleware/components/connectivity_manager/include
    ../../../layers/middleware/components/imu_service/include
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/power_service/include
    ../../../layers/middleware/components/device_link_service/include
    ../../../layers/middleware/components/ble_runtime/include
    ../../../layers/middleware/components/system_pm/include
    ../../../layers/middleware/components/time_service/include
    ../../../layers/middleware/components/timer_service/include
    ../../../layers/middleware/components/recorder_service/include
    ../../../layers/middleware/components/file_service/include
    ../../../layers/middleware/components/onboarding_service/include
    ../../../layers/middleware/components/weather_service/include
    ../../../layers/middleware/components/music_service/include
)
target_link_libraries(app_runtime_pm_test PRIVATE Threads::Threads)
configure_main_host_target(app_runtime_pm_test)
add_main_host_test(app_runtime_pm_test app_runtime_pm_test)

add_executable(app_runtime_control_test
    app_runtime_control_test.c
    ../../app_runtime_control.c
)
target_include_directories(app_runtime_control_test PRIVATE
    fakes
    ../..
    ../../../layers/app_manager/app_core/include
    ../../../layers/middleware/components/audio_service/include
    ../../../layers/middleware/components/connectivity_manager/include
    ../../../layers/middleware/components/device_link/include
    ../../../layers/middleware/components/device_link_service/include
    ../../../layers/middleware/components/ble_runtime/include
    ../../../layers/middleware/components/event_bus/include
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/time_service/include
)
configure_main_host_target(app_runtime_control_test)
add_main_host_test(app_runtime_control_test app_runtime_control_test)

function(add_fs_storage_test target mmap_enabled)
    add_executable(${target}
        fs_storage_test.c
        ../../fs_storage/fs_storage.c
    )
    target_include_directories(${target} PRIVATE
        fakes
        ../..
        ../../../layers/middleware/components/mt_log/include
    )
    # The mount path is a compile-time constant of the production source, so
    # the fixture cannot pick a unique directory at run time. Derive it from
    # the build tree instead: two checkouts building concurrently no longer
    # share a path, and RESOURCE_LOCK below serializes the two variants.
    string(MD5 build_tag "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${build_tag}" 0 8 build_tag)
    target_compile_definitions(${target} PRIVATE
        CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS=${mmap_enabled}
        FS_STORAGE_TEST_MMAP=${mmap_enabled}
        FS_DATA_MOUNT_PATH="/tmp/mt-${target}-${build_tag}-data"
    )
    configure_main_host_target(${target})
    add_main_host_test(${target} ${target})
    # Both variants mount the same compile-time data path; overlapping them
    # would let one test wipe the other's tree.
    set_tests_properties(${target} PROPERTIES RESOURCE_LOCK fs_storage_data)
endfunction()

add_fs_storage_test(fs_storage_vfs_test 0)
add_fs_storage_test(fs_storage_mmap_test 1)

add_executable(network_runtime_test
    network_runtime_test.c
    ../../network_runtime.c
)
target_include_directories(network_runtime_test PRIVATE
    fakes
    ../..
)
target_link_libraries(network_runtime_test PRIVATE Threads::Threads)
configure_main_host_target(network_runtime_test)

set(NETWORK_RUNTIME_SCENARIOS
    owned
    shared
    owned-netif-shared-loop
    shared-netif-owned-loop
    netif-failure-retry
    event-loop-failure-retry
    shared-netif-event-loop-retry
    concurrent-init
)

foreach(scenario IN LISTS NETWORK_RUNTIME_SCENARIOS)
    string(REPLACE "-" "_" test_suffix "${scenario}")
    add_main_host_test(network_runtime_${test_suffix}
        network_runtime_test ${scenario})
endforeach()
