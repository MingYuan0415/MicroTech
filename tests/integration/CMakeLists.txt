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
set(MT_BOARD "waveshare_esp32_s3_touch_amoled_1_8" CACHE STRING
    "MicroTech board fixture used by the cross-layer suite")
set(MT_TARGET "esp32s3" CACHE STRING
    "IDF target matching the cross-layer board fixture")
set(MT_PRODUCT "s3_reference" CACHE STRING
    "MicroTech product fixture used by the cross-layer suite")
include("${PROJECT_ROOT}/layers/bsp/boards.cmake")
include("${PROJECT_ROOT}/products/products.cmake")
mt_resolve_board("${MT_TARGET}" "${MT_BOARD}")
set(_requested_product "${MT_PRODUCT}")
mt_resolve_product("${MT_BOARD}" "${_requested_product}")
mt_validate_product_capabilities(
    "${MT_BOARD_HAL_CAPABILITIES}" "${MT_PRODUCT_SERVICE_CAPABILITIES}")
mt_product_expand_apps()
set(INTEGRATION_APP_SOURCES ${MT_PRODUCT_APP_SOURCES})
set(INTEGRATION_APP_INCLUDE_DIRS ${MT_PRODUCT_APP_INCLUDE_DIRS})
# The cross-layer test keeps compile-time references to optional App internals
# for its full-product scenarios. Keep header directories visible even when a
# minimal product does not compile that App; the selected sources still decide
# what is linked.
foreach(_app_id IN LISTS MT_APP_CATALOG_IDS)
    mt_app_dir_name("${_app_id}" _app_dir)
    if(EXISTS "${APPS}/${_app_dir}/src")
        list(APPEND INTEGRATION_APP_INCLUDE_DIRS "${APPS}/${_app_dir}/src")
    endif()
endforeach()
unset(_app_id)
unset(_app_dir)
set(INTEGRATION_PRODUCT_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/mt_product_profile.h")
mt_product_write_header("${INTEGRATION_PRODUCT_HEADER}")
set(MIDDLEWARE "${PROJECT_ROOT}/layers/middleware/components")

add_executable(test_cross_layer
    test_cross_layer.c
    host/apps_integration_lvgl.c
    host/app_manager_device_info_stub.c
    host/host_freertos.c
    host/host_services.c
    host/esp_timer_fake.c
    host/onboarding_service_fake.c
    host/recorder_service_fake.c
    host/music_service_fake.c
    host/chore_service_fake.c
    host/connectivity_manager_fake.c
    host/device_link_service_fake.c
    host/factory_reset_service_fake.c
    "${APP_CORE}/src/app_manager_mailbox.c"
    "${APP_CORE}/src/app_manager_back_gesture.c"
    "${APP_CORE}/src/app_manager_builtin.c"
    "${APP_CORE}/src/app_manager_lifecycle.c"
    "${APP_CORE}/src/app_manager_lifecycle_api.c"
    "${APP_CORE}/src/app_manager_nav_model.c"
    "${APP_CORE}/src/app_manager_navigation.c"
    "${APP_CORE}/src/app_manager_presentation.c"
    "${APP_CORE}/src/app_manager_task_switcher.c"
    "${APP_CORE}/src/app_manager_control_center.c"
    "${APP_THEME}/src/app_theme.c"
    "${MIDDLEWARE}/event_bus/src/event_bus.c"
    "${MIDDLEWARE}/event_bus/src/event_bus_core.c"
    "${MIDDLEWARE}/music_service/src/music_snapshot.c"
    "${MIDDLEWARE}/device_link_service/src/device_link_confirmation.c"
    ${INTEGRATION_APP_SOURCES}
    "${MIDDLEWARE}/timer_service/src/timer_service.c"
)

target_compile_definitions(test_cross_layer PRIVATE
    MT_PRODUCT_ID="${MT_PRODUCT_ID}"
)

add_executable(test_presentation
    test_presentation.c
    host/apps_integration_lvgl.c
    "${APP_CORE}/src/app_manager_presentation.c"
)

target_compile_features(test_cross_layer PRIVATE c_std_11)
target_compile_features(test_presentation PRIVATE c_std_11)
target_compile_definitions(test_cross_layer PRIVATE
    HOST_TEST=1
    CONFIG_SPIRAM=1
    CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION=1
    heap_caps_aligned_calloc=host_lv_heap_caps_aligned_calloc
    heap_caps_free=host_lv_heap_caps_free
)
target_compile_definitions(test_presentation PRIVATE
    HOST_TEST=1
    CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS=1
    CONFIG_SPIRAM=1
    CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION=1
    heap_caps_aligned_calloc=host_lv_heap_caps_aligned_calloc
    heap_caps_free=host_lv_heap_caps_free
)
target_include_directories(test_cross_layer PRIVATE
    host/include
    "${APP_CORE}/include"
    "${APP_CORE}/src"
    "${APP_THEME}/include"
    "${APPS}/common/include"
    ${INTEGRATION_APP_INCLUDE_DIRS}
    "${MIDDLEWARE}/event_bus/include"
    "${MIDDLEWARE}/power_service/include"
    "${MIDDLEWARE}/imu_service/include"
    "${MIDDLEWARE}/audio_service/include"
    "${MIDDLEWARE}/sd_storage_service/include"
    "${MIDDLEWARE}/file_service/include"
    "${MIDDLEWARE}/time_service/include"
    "${MIDDLEWARE}/timer_service/include"
    "${MIDDLEWARE}/nv_storage/include"
    "${MIDDLEWARE}/recorder_service/include"
    "${MIDDLEWARE}/music_service/include"
    "${MIDDLEWARE}/music_service/src"
    "${MIDDLEWARE}/chore_service/include"
    "${MIDDLEWARE}/onboarding_service/include"
    "${MIDDLEWARE}/weather_service/include"
    "${MIDDLEWARE}/connectivity_manager/include"
    "${MIDDLEWARE}/device_link/include"
    "${MIDDLEWARE}/device_link_service/include"
    "${MIDDLEWARE}/device_link_service/src"
    "${MIDDLEWARE}/factory_reset_service/include"
    "${MIDDLEWARE}/ble_runtime/include"
)
target_include_directories(test_presentation PRIVATE
    host/include
    "${APP_CORE}/include"
    "${APP_CORE}/src"
    "${APP_THEME}/include"
)
target_compile_options(test_cross_layer PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    "SHELL:-include ${INTEGRATION_PRODUCT_HEADER}"
    -Wall -Wextra -Werror -Wpedantic -Wno-variadic-macros -UNDEBUG)
target_compile_options(test_presentation PRIVATE
    -include "${PROJECT_ROOT}/tests/host/sdkconfig.h"
    -Wall -Wextra -Werror -Wpedantic -Wno-variadic-macros -UNDEBUG)
target_link_libraries(test_cross_layer PRIVATE Threads::Threads m)
target_link_options(test_cross_layer PRIVATE
    "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/app_builtin_apps.ld")

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
elseif(CROSS_LAYER_SANITIZER STREQUAL "thread")
    target_compile_options(test_cross_layer PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(test_cross_layer PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_compile_options(test_presentation PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(test_presentation PRIVATE
        -fsanitize=thread -fno-omit-frame-pointer)
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
else()
    add_test(NAME presentation COMMAND test_presentation)
    add_test(NAME cross_layer COMMAND test_cross_layer)
endif()
set_tests_properties(presentation PROPERTIES TIMEOUT 10)
set_tests_properties(cross_layer PROPERTIES TIMEOUT 120)
