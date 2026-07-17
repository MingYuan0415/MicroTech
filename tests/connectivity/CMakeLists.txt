cmake_minimum_required(VERSION 3.16)
project(mt_connectivity_pipeline_tests LANGUAGES C)

enable_testing()
find_package(Threads REQUIRED)

set(CONNECTIVITY_SANITIZER "none" CACHE STRING
    "Host-test sanitizer: none, address, or thread")
set_property(CACHE CONNECTIVITY_SANITIZER PROPERTY STRINGS
    none address thread)
find_program(CONNECTIVITY_SETARCH setarch)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(APP_CORE "${PROJECT_ROOT}/layers/app_manager/app_core")
set(APPS "${PROJECT_ROOT}/layers/apps")
set(MIDDLEWARE "${PROJECT_ROOT}/layers/middleware/components")
set(WIFI_SERVICE "${MIDDLEWARE}/wifi_service")

add_executable(test_connectivity_pipeline
    test_connectivity_pipeline.c
    host/freertos_host.c
    host/host_mailbox_port.c
    host/wifi_service_port_fake.c
    "${WIFI_SERVICE}/src/wifi_service.c"
    "${WIFI_SERVICE}/src/wifi_service_worker.c"
    "${WIFI_SERVICE}/src/wifi_service_worker_ops.c"
    "${MIDDLEWARE}/event_bus/src/event_bus.c"
    "${APP_CORE}/src/app_manager_mailbox.c"
    "${APPS}/setup_app/src/setup_wifi_adapter.c"
)

target_compile_features(test_connectivity_pipeline PRIVATE c_std_11)
target_compile_definitions(test_connectivity_pipeline PRIVATE
    HOST_TEST=1
    CONFIG_WIFI_SERVICE_QUEUE_DEPTH=16
    CONFIG_WIFI_SERVICE_TASK_STACK=4096
    CONFIG_WIFI_SERVICE_TASK_PRIORITY=4
    CONFIG_WIFI_SERVICE_WORKER_POLL_MS=2
    WIFI_SERVICE_RETRY_DELAY_1_MS=100
    WIFI_SERVICE_RETRY_DELAY_2_MS=200
    WIFI_SERVICE_RETRY_DELAY_3_MS=400
)
target_include_directories(test_connectivity_pipeline PRIVATE
    host/include
    "${WIFI_SERVICE}/include"
    "${WIFI_SERVICE}/src"
    "${MIDDLEWARE}/event_bus/include"
    "${APP_CORE}/include"
    "${APP_CORE}/src"
    "${APPS}/setup_app/src"
)
target_compile_options(test_connectivity_pipeline PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-variadic-macros
    -UNDEBUG
)
target_link_libraries(test_connectivity_pipeline PRIVATE Threads::Threads)

if(CONNECTIVITY_SANITIZER STREQUAL "address")
    target_compile_options(test_connectivity_pipeline PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    target_link_options(test_connectivity_pipeline PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
elseif(CONNECTIVITY_SANITIZER STREQUAL "thread")
    target_compile_options(test_connectivity_pipeline PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    target_link_options(test_connectivity_pipeline PRIVATE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
elseif(NOT CONNECTIVITY_SANITIZER STREQUAL "none")
    message(FATAL_ERROR
        "Unsupported CONNECTIVITY_SANITIZER=${CONNECTIVITY_SANITIZER}")
endif()

if(CONNECTIVITY_SANITIZER STREQUAL "thread" AND
        CONNECTIVITY_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_test(NAME connectivity_pipeline COMMAND
        ${CONNECTIVITY_SETARCH} x86_64 -R
        $<TARGET_FILE:test_connectivity_pipeline>)
else()
    add_test(NAME connectivity_pipeline COMMAND test_connectivity_pipeline)
endif()
set_tests_properties(connectivity_pipeline PROPERTIES TIMEOUT 40)
