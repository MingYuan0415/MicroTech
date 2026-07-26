cmake_minimum_required(VERSION 3.16)
project(main_runtime_host_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(MAIN_HOST_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE MAIN_HOST_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(MAIN_HOST_SETARCH setarch)

function(configure_main_host_target target)
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_options(${target} PRIVATE
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
    ../../app_runtime.c
)

target_include_directories(app_runtime_test PRIVATE
    fakes
    ../..
    ../../../layers/app_manager/app_core/include
    ../../../layers/app_manager/app_theme/include
    ../../../layers/bsp/include
    ../../../layers/middleware/components/ble_service/include
    ../../../layers/middleware/components/audio_service/include
    ../../../layers/middleware/components/event_bus/include
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/nv_storage/include
    ../../../layers/middleware/components/power_service/include
    ../../../layers/middleware/components/imu_service/include
    ../../../layers/middleware/components/sd_storage_service/include
    ../../../layers/middleware/components/system_pm/include
    ../../../layers/middleware/components/time_service/include
    ../../../layers/middleware/components/wifi_service/include
)

configure_main_host_target(app_runtime_test)
target_compile_definitions(app_runtime_test PRIVATE
    CONFIG_APP_MANAGER_DISPLAY_BENCHMARK=1
)
add_main_host_test(app_runtime_test app_runtime_test)

add_executable(app_runtime_pm_test
    app_runtime_pm_test.c
    ../../app_runtime_pm.c
)
target_include_directories(app_runtime_pm_test PRIVATE
    fakes
    ../..
    ../../../layers/app_manager/app_core/include
    ../../../layers/app_manager/app_theme/include
    ../../../layers/bsp/include
    ../../../layers/middleware/components/event_bus/include
    ../../../layers/middleware/components/audio_service/include
    ../../../layers/middleware/components/imu_service/include
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/power_service/include
    ../../../layers/middleware/components/system_pm/include
    ../../../layers/middleware/components/time_service/include
    ../../../layers/middleware/components/wifi_service/include
)
target_link_libraries(app_runtime_pm_test PRIVATE Threads::Threads)
configure_main_host_target(app_runtime_pm_test)
add_main_host_test(app_runtime_pm_test app_runtime_pm_test)

function(add_display_benchmark_test target port stress characterization
         load_full load_audio_only load_tcp_only)
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
        ../../../layers/middleware/components/audio_service/include
        ../../../layers/middleware/components/event_bus/include
        ../../../layers/middleware/components/mt_log/include
        ../../../layers/middleware/components/wifi_service/include
    )
    target_compile_definitions(${target} PRIVATE
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK=1
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_MODE_STRESS=${stress}
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_MODE_CHARACTERIZATION=${characterization}
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_FULL=${load_full}
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_AUDIO_ONLY=${load_audio_only}
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_TCP_ONLY=${load_tcp_only}
        CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG=0
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_DURATION_SEC=1800
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_EFFECT_DURATION_SEC=30
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_HOST="127.0.0.1"
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_PORT=${port}
        CONFIG_APP_MANAGER_DISPLAY_BENCHMARK_TCP_RATE_KBIT_S=2048
        CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT=60
        CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED=0
        CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ=40000000
        CONFIG_BSP_DISPLAY_SPI_MAX_TRANSFER_LINES=10
        CONFIG_BSP_DISPLAY_NON_TE_PSRAM_DMA_DIRECT=0
        CONFIG_BSP_DISPLAY_TE_SYNC=0
        CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2
        CONFIG_LV_DRAW_THREAD_PRIO=3
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
    target_link_libraries(${target} PRIVATE Threads::Threads)
    configure_main_host_target(${target})
    add_main_host_test(${target} ${target})
endfunction()

set(DISPLAY_BENCHMARK_STRESS_PORT 55001)
set(DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT 55011)
set(DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT 55021)
set(DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT 55031)
if(MAIN_HOST_SANITIZER STREQUAL "address")
    set(DISPLAY_BENCHMARK_STRESS_PORT 55002)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT 55012)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT 55022)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT 55032)
elseif(MAIN_HOST_SANITIZER STREQUAL "thread")
    set(DISPLAY_BENCHMARK_STRESS_PORT 55003)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT 55013)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT 55023)
    set(DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT 55033)
endif()
add_display_benchmark_test(display_benchmark_test
    ${DISPLAY_BENCHMARK_STRESS_PORT} 1 0 0 0 0)
add_display_benchmark_test(display_benchmark_characterization_full_test
    ${DISPLAY_BENCHMARK_CHARACTERIZATION_FULL_PORT} 0 1 1 0 0)
add_display_benchmark_test(display_benchmark_characterization_audio_test
    ${DISPLAY_BENCHMARK_CHARACTERIZATION_AUDIO_PORT} 0 1 0 1 0)
add_display_benchmark_test(display_benchmark_characterization_tcp_test
    ${DISPLAY_BENCHMARK_CHARACTERIZATION_TCP_PORT} 0 1 0 0 1)

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
