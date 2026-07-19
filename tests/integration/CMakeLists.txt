cmake_minimum_required(VERSION 3.16)
project(mt_cross_layer_tests C)

enable_testing()
find_package(Threads REQUIRED)

set(CROSS_LAYER_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE CROSS_LAYER_SANITIZER PROPERTY STRINGS none address thread)
find_program(CROSS_LAYER_SETARCH setarch)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(APP_CORE "${PROJECT_ROOT}/layers/app_manager/app_core")
set(APP_THEME "${PROJECT_ROOT}/layers/app_manager/app_theme")
set(APPS "${PROJECT_ROOT}/layers/apps")
set(MIDDLEWARE "${PROJECT_ROOT}/layers/middleware/components")

add_executable(test_cross_layer
    test_cross_layer.c
    host/apps_integration_lvgl.c
    host/host_freertos.c
    host/host_services.c
    host/wifi_service_fake.c
    "${APP_CORE}/src/app_manager_mailbox.c"
    "${APP_CORE}/src/app_manager_back_gesture.c"
    "${APP_CORE}/src/app_manager_builtin.c"
    "${APP_CORE}/src/app_manager_lifecycle.c"
    "${APP_CORE}/src/app_manager_lifecycle_api.c"
    "${APP_CORE}/src/app_manager_nav_model.c"
    "${APP_CORE}/src/app_manager_navigation.c"
    "${APP_CORE}/src/app_manager_presentation.c"
    "${APP_THEME}/src/app_theme.c"
    "${MIDDLEWARE}/event_bus/src/event_bus.c"
    "${APPS}/common/src/app_ui.c"
    "${APPS}/home_app/src/home_app.c"
    "${APPS}/menu_app/src/menu_app.c"
    "${APPS}/menu_app/src/audio_demo.c"
    "${APPS}/menu_app/src/audio_demo_adapter.c"
    "${APPS}/menu_app/src/motion_demo.c"
    "${APPS}/menu_app/src/clock_demo.c"
    "${APPS}/menu_app/src/clock_demo_adapter.c"
    "${APPS}/menu_app/src/storage_demo.c"
    "${APPS}/menu_app/src/storage_demo_adapter.c"
    "${APPS}/settings_app/src/settings_app.c"
    "${APPS}/setup_app/src/setup_app.c"
    "${APPS}/setup_app/src/setup_wifi_adapter.c"
)

add_executable(test_presentation
    test_presentation.c
    host/apps_integration_lvgl.c
    "${APP_CORE}/src/app_manager_presentation.c"
)

add_executable(test_audio_demo_adapter
    test_audio_demo_adapter.c
    host/host_freertos.c
    "${APPS}/menu_app/src/audio_demo_adapter.c"
)

add_executable(test_storage_demo_adapter
    test_storage_demo_adapter.c
    host/host_freertos.c
    "${APPS}/menu_app/src/storage_demo_adapter.c"
)

add_executable(test_clock_demo_adapter
    test_clock_demo_adapter.c
    host/host_freertos.c
    "${APPS}/menu_app/src/clock_demo_adapter.c"
)

target_compile_features(test_cross_layer PRIVATE c_std_11)
target_compile_features(test_presentation PRIVATE c_std_11)
target_compile_definitions(test_cross_layer PRIVATE
    HOST_TEST=1
)
target_compile_definitions(test_presentation PRIVATE
    HOST_TEST=1
)
target_include_directories(test_cross_layer PRIVATE
    host/include
    "${APP_CORE}/include"
    "${APP_CORE}/src"
    "${APP_THEME}/include"
    "${APPS}/common/include"
    "${APPS}/menu_app/src"
    "${APPS}/setup_app/src"
    "${MIDDLEWARE}/event_bus/include"
    "${MIDDLEWARE}/power_service/include"
    "${MIDDLEWARE}/imu_service/include"
    "${MIDDLEWARE}/audio_service/include"
    "${MIDDLEWARE}/sd_storage_service/include"
    "${MIDDLEWARE}/time_service/include"
    "${MIDDLEWARE}/wifi_service/include"
)
target_include_directories(test_presentation PRIVATE
    host/include
    "${APP_CORE}/include"
    "${APP_CORE}/src"
    "${APP_THEME}/include"
)
target_compile_options(test_cross_layer PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wno-variadic-macros -UNDEBUG)
target_compile_options(test_presentation PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wno-variadic-macros -UNDEBUG)
target_link_libraries(test_cross_layer PRIVATE Threads::Threads)
target_link_options(test_cross_layer PRIVATE
    "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/app_builtin_apps.ld")

set(DEMO_ADAPTER_TARGETS
    test_audio_demo_adapter
    test_storage_demo_adapter
    test_clock_demo_adapter
)
foreach(target IN LISTS DEMO_ADAPTER_TARGETS)
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE=1)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
    target_include_directories(${target} PRIVATE
        host/include
        "${APPS}/menu_app/src"
    )
    target_link_libraries(${target} PRIVATE Threads::Threads)
endforeach()
target_include_directories(test_audio_demo_adapter PRIVATE
    "${MIDDLEWARE}/audio_service/include"
)
target_include_directories(test_storage_demo_adapter PRIVATE
    "${MIDDLEWARE}/sd_storage_service/include"
)
target_include_directories(test_clock_demo_adapter PRIVATE
    "${MIDDLEWARE}/event_bus/include"
    "${MIDDLEWARE}/time_service/include"
)
target_link_options(test_storage_demo_adapter PRIVATE
    -Wl,--wrap=open
    -Wl,--wrap=read
    -Wl,--wrap=write
    -Wl,--wrap=close
    -Wl,--wrap=fsync
    -Wl,--wrap=unlink
)

if(CROSS_LAYER_SANITIZER STREQUAL "address")
    target_compile_options(test_cross_layer PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(test_cross_layer PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_compile_options(test_presentation PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    target_link_options(test_presentation PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    foreach(target IN LISTS DEMO_ADAPTER_TARGETS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
    endforeach()
elseif(CROSS_LAYER_SANITIZER STREQUAL "thread")
    target_compile_options(test_cross_layer PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(test_cross_layer PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_compile_options(test_presentation PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(test_presentation PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    foreach(target IN LISTS DEMO_ADAPTER_TARGETS)
        target_compile_options(${target} PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer)
    endforeach()
elseif(NOT CROSS_LAYER_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported CROSS_LAYER_SANITIZER=${CROSS_LAYER_SANITIZER}")
endif()

if(CROSS_LAYER_SANITIZER STREQUAL "thread" AND CROSS_LAYER_SETARCH AND
        CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME presentation COMMAND
        ${CROSS_LAYER_SETARCH} x86_64 -R $<TARGET_FILE:test_presentation>)
    add_test(NAME cross_layer COMMAND
        ${CROSS_LAYER_SETARCH} x86_64 -R $<TARGET_FILE:test_cross_layer>)
    foreach(target IN LISTS DEMO_ADAPTER_TARGETS)
        add_test(NAME ${target} COMMAND
            ${CROSS_LAYER_SETARCH} x86_64 -R $<TARGET_FILE:${target}>)
    endforeach()
else()
    add_test(NAME presentation COMMAND test_presentation)
    add_test(NAME cross_layer COMMAND test_cross_layer)
    foreach(target IN LISTS DEMO_ADAPTER_TARGETS)
        add_test(NAME ${target} COMMAND ${target})
    endforeach()
endif()
set_tests_properties(presentation PROPERTIES TIMEOUT 10)
set_tests_properties(cross_layer PROPERTIES TIMEOUT 40)
set_tests_properties(${DEMO_ADAPTER_TARGETS} PROPERTIES TIMEOUT 10)
