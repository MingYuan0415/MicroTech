cmake_minimum_required(VERSION 3.16)
project(main_runtime_host_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MAIN_HOST_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE MAIN_HOST_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(MAIN_HOST_SETARCH setarch)

add_test(NAME configuration_governance COMMAND
    ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/../../../tests/configuration/test_configuration_governance.py")

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
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 10)
endfunction()

add_executable(app_runtime_test
    app_runtime_test.c
    ../../app_product_config.c
    ../../app_runtime.c
    timer_service_fake.c
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
    ../../../layers/middleware/components/onboarding_service/include
    ../../../layers/middleware/components/weather_service/include
)

configure_main_host_target(app_runtime_test)
target_compile_definitions(app_runtime_test PRIVATE
    CONFIG_MAIN_DISPLAY_BENCHMARK=1
)
add_main_host_test(app_runtime_test app_runtime_test)

add_executable(app_runtime_pm_test
    app_runtime_pm_test.c
    ../../app_runtime_pm.c
    timer_service_fake.c
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
    ../../../layers/middleware/components/onboarding_service/include
    ../../../layers/middleware/components/weather_service/include
)
target_link_libraries(app_runtime_pm_test PRIVATE Threads::Threads)
configure_main_host_target(app_runtime_pm_test)
add_main_host_test(app_runtime_pm_test app_runtime_pm_test)

function(add_display_benchmark_test target port stress characterization
         load_full load_audio_only load_tcp_only snapshot_animation os_none)
    set(c_ext_stress 0)
    if(ARGC GREATER 9)
        set(c_ext_stress ${ARGV9})
    endif()
    if(os_none)
        set(lvgl_core_id 1)
    else()
        set(lvgl_core_id -1)
    endif()
    add_executable(${target}
        display_benchmark_test.c
        display_benchmark_host_port.c
        ../../display_benchmark.c
    )
    target_include_directories(${target} PRIVATE
        fakes
        ../..
        ../../../layers/app_manager/app_core/include
        ../../../layers/app_manager/app_theme/include
        ../../../layers/bsp/include
        ../../../layers/middleware/components/audio_service/include
        ../../../layers/middleware/components/connectivity_manager/include
        ../../../layers/middleware/components/event_bus/include
        ../../../layers/middleware/components/mt_log/include
        ../../../layers/middleware/components/system_pm/include
        ../../../layers/middleware/components/device_link_service/include
        ../../../layers/middleware/components/ble_runtime/include
    )
    target_compile_definitions(${target} PRIVATE
        CONFIG_MAIN_DISPLAY_BENCHMARK=1
        CONFIG_MAIN_PROJECT_TASK_CORE_ID=0
        CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0=1
        CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1=0
        CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID=${lvgl_core_id}
        TEST_PROJECT_CORE_ID=0
        TEST_LVGL_CORE_ID=${lvgl_core_id}
        CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS=${c_ext_stress}
        TEST_C_EXT_STRESS=${c_ext_stress}
        TEST_DISPLAY_BENCHMARK_MODE_STRESS=${stress}
        TEST_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION=${characterization}
        TEST_DISPLAY_BENCHMARK_LOAD_FULL=${load_full}
        TEST_DISPLAY_BENCHMARK_LOAD_AUDIO_ONLY=${load_audio_only}
        TEST_DISPLAY_BENCHMARK_LOAD_TCP_ONLY=${load_tcp_only}
        TEST_DISPLAY_BENCHMARK_TCP_PORT=${port}
        TEST_DISPLAY_BENCHMARK_TCP_RATE_KBIT_S=2048
        CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT=60
        CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION=${snapshot_animation}
        CONFIG_LV_OS_NONE=${os_none}
        TEST_LV_OS_NONE=${os_none}
        CONFIG_LWIP_TCP_MSS=1440
        CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5760
        DISPLAY_BENCHMARK_DURATION_US=200000LL
        DISPLAY_BENCHMARK_PHASE_US=15000LL
        DISPLAY_BENCHMARK_CHARACTERIZATION_PHASE_US=10000LL
        DISPLAY_BENCHMARK_WIFI_POLL_MS=5U
        DISPLAY_BENCHMARK_IO_TIMEOUT_MS=100U
        DISPLAY_BENCHMARK_AUDIO_TIMEOUT_MS=10U
        DISPLAY_BENCHMARK_TCP_RECONNECT_MS=10U
    )
    if(c_ext_stress)
        target_compile_definitions(${target} PRIVATE
            DISPLAY_BENCHMARK_STRESS_SAMPLE_MS=1U
            DISPLAY_BENCHMARK_STRESS_TCP_CONNECT_MS=1000U
            DISPLAY_BENCHMARK_STRESS_PROVISIONING_MS=100U
            DISPLAY_BENCHMARK_STRESS_CLEANUP_MS=100U
            DISPLAY_BENCHMARK_STRESS_NORMAL_DWELL_MS=1U
            DISPLAY_BENCHMARK_STRESS_HEAVY_DWELL_MS=1U
        )
    endif()
    if(os_none)
        target_compile_definitions(${target} PRIVATE
            CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE=32768
            CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1
        )
    else()
        target_compile_definitions(${target} PRIVATE
            CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE=8192
            CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2
            CONFIG_LV_DRAW_THREAD_STACK_SIZE=32768
            CONFIG_LV_DRAW_THREAD_PRIO=3
        )
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads)
    configure_main_host_target(${target})
    add_main_host_test(${target} ${target})
endfunction()

set(DISPLAY_BENCHMARK_STRESS_PORT 55001)
set(DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT 55011)
set(DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT 55021)
set(DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT 55031)
set(DISPLAY_BENCHMARK_SNAPSHOT_DISABLED_PORT 55041)
set(DISPLAY_BENCHMARK_OS_NONE_PORT 55051)
set(DISPLAY_BENCHMARK_OS_NONE_CHARACTERIZATION_PORT 55061)
set(DISPLAY_BENCHMARK_C_EXT_STRESS_PORT 55071)
if(MAIN_HOST_SANITIZER STREQUAL "address")
    set(DISPLAY_BENCHMARK_STRESS_PORT 55002)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT 55012)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT 55022)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT 55032)
    set(DISPLAY_BENCHMARK_SNAPSHOT_DISABLED_PORT 55042)
    set(DISPLAY_BENCHMARK_OS_NONE_PORT 55052)
    set(DISPLAY_BENCHMARK_OS_NONE_CHARACTERIZATION_PORT 55062)
    set(DISPLAY_BENCHMARK_C_EXT_STRESS_PORT 55072)
elseif(MAIN_HOST_SANITIZER STREQUAL "thread")
    set(DISPLAY_BENCHMARK_STRESS_PORT 55003)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT 55013)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT 55023)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT 55033)
    set(DISPLAY_BENCHMARK_SNAPSHOT_DISABLED_PORT 55043)
    set(DISPLAY_BENCHMARK_OS_NONE_PORT 55053)
    set(DISPLAY_BENCHMARK_OS_NONE_CHARACTERIZATION_PORT 55063)
    set(DISPLAY_BENCHMARK_C_EXT_STRESS_PORT 55073)
endif()
add_display_benchmark_test(display_benchmark_test
    ${DISPLAY_BENCHMARK_STRESS_PORT} 1 0 0 0 0 1 0)
add_display_benchmark_test(display_benchmark_characterization_full_test
    ${DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT} 0 1 1 0 0 1 0)
add_display_benchmark_test(display_benchmark_characterization_audio_test
    ${DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT} 0 1 0 1 0 1 0)
add_display_benchmark_test(display_benchmark_characterization_tcp_test
    ${DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT} 0 1 0 0 1 1 0)
add_display_benchmark_test(display_benchmark_snapshot_disabled_test
    ${DISPLAY_BENCHMARK_SNAPSHOT_DISABLED_PORT} 1 0 0 0 0 0 0)
add_display_benchmark_test(display_benchmark_os_none_test
    ${DISPLAY_BENCHMARK_OS_NONE_PORT} 1 0 0 0 0 1 1)
add_display_benchmark_test(display_benchmark_os_none_characterization_test
    ${DISPLAY_BENCHMARK_OS_NONE_CHARACTERIZATION_PORT} 0 1 0 0 1 1 1)
add_display_benchmark_test(display_benchmark_c_ext_stress_test
    ${DISPLAY_BENCHMARK_C_EXT_STRESS_PORT} 1 0 1 0 0 1 1 1)

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
    target_compile_definitions(${target} PRIVATE
        CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS=${mmap_enabled}
        FS_STORAGE_TEST_MMAP=${mmap_enabled}
    )
    configure_main_host_target(${target})
    add_main_host_test(${target} ${target})
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
