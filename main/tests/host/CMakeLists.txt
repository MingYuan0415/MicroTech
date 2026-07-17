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
    ../../../layers/middleware/components/event_bus/include
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/nv_storage/include
    ../../../layers/middleware/components/power_service/include
    ../../../layers/middleware/components/system_pm/include
    ../../../layers/middleware/components/time_service/include
    ../../../layers/middleware/components/wifi_service/include
)

configure_main_host_target(app_runtime_test)
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
    ../../../layers/middleware/components/mt_log/include
    ../../../layers/middleware/components/power_service/include
    ../../../layers/middleware/components/system_pm/include
    ../../../layers/middleware/components/wifi_service/include
)
target_link_libraries(app_runtime_pm_test PRIVATE Threads::Threads)
configure_main_host_target(app_runtime_pm_test)
add_main_host_test(app_runtime_pm_test app_runtime_pm_test)

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
