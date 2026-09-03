# MicroTech LVGL host simulator (standalone; never included by the root
# ESP-IDF build). Configure with:
#   cmake -S sim -B build/sim -G Ninja
cmake_minimum_required(VERSION 3.22)
project(microtech_sim C CXX ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT EXISTS "${MT_ROOT}/managed_components/lvgl__lvgl/lvgl.h")
    message(FATAL_ERROR
        "managed_components/lvgl__lvgl is missing. Populate it with an "
        "idf.py build first; FetchContent fallback is forbidden.")
endif()
if(NOT EXISTS "${MT_ROOT}/managed_components/espressif__cjson/cJSON")
    message(FATAL_ERROR
        "managed_components/espressif__cjson is missing. Populate it with "
        "an idf.py build first.")
endif()

find_package(SDL2 REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ---- LVGL 9.5 from the repo tree (same bytes as the firmware) ----
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MT_ROOT}/managed_components/lvgl__lvgl" lvgl_build)

# ---- FreeType 2.14.3 from the pinned in-tree component (no downloads) ----
set(FT_DISABLE_HARFBUZZ ON)
set(FT_DISABLE_BZIP2 ON)
set(FT_DISABLE_BROTLI ON)
set(FT_DISABLE_PNG ON)
set(FT_DISABLE_ZLIB ON)
set(SKIP_INSTALL_ALL TRUE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(
    "${MT_ROOT}/managed_components/espressif__freetype/freetype"
    freetype_build)
# Mirrors espressif__esp_lvgl_adapter/CMakeLists.txt:327-335.
target_compile_options(freetype PRIVATE
    -Wno-dangling-pointer -Wno-unused-but-set-variable)
target_compile_definitions(freetype PUBLIC
    FT_CONFIG_OPTION_ERROR_STRINGS FT_ERR_PREFIX=FT_Err_ FT2_BUILD_LIBRARY)
target_link_libraries(lvgl PUBLIC freetype)

# Mirror espressif__esp_lvgl_adapter/CMakeLists.txt:363-386: with
# LV_FREETYPE_USE_LVGL_PORT the stream opener must be LVGL's lv_ftsystem.c
# (F: volume), not FreeType's fopen-based ftsystem.c.
get_target_property(SIM_FT_SOURCES freetype SOURCES)
list(FILTER SIM_FT_SOURCES EXCLUDE REGEX ".*/ftsystem\\.c$")
set_target_properties(freetype PROPERTIES SOURCES "${SIM_FT_SOURCES}")
target_sources(freetype PRIVATE
    "${MT_ROOT}/managed_components/lvgl__lvgl/src/libs/freetype/lv_ftsystem.c")
target_include_directories(freetype PRIVATE "${MT_ROOT}/managed_components/lvgl__lvgl")
target_compile_definitions(freetype PRIVATE
    "LV_CONF_PATH=\"${CMAKE_CURRENT_LIST_DIR}/lv_conf.h\"" LV_KCONFIG_IGNORE)
target_include_directories(freetype PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

# ---- libpng from the in-tree component (screenshot encoder; host zlib) ----
set(PNG_SHARED OFF)
set(PNG_STATIC ON)
set(PNG_TESTS OFF)
set(PNG_TOOLS OFF)
set(PNG_EXECUTABLES OFF)
set(PNG_HARDWARE_OPTIMIZATIONS OFF)
add_subdirectory("${MT_ROOT}/managed_components/espressif__libpng/libpng"
    libpng_build)

# ---- Resource pipeline (host replica of main/CMakeLists.txt:50-206) ----
include("${CMAKE_CURRENT_LIST_DIR}/cmake/resources.cmake")

# ---- Generated sdkconfig mirror (device Kconfig is the single truth) ----
# build/config/sdkconfig.json is fully rewritten by every `idf.py build` and
# carries the locally expanded values (including the private weather
# endpoint/token); reconfigure whenever it or its inputs change so the sim
# mirror stays in lockstep with the firmware.
set(SIM_SDKCONFIG_AUTOCONF "${MT_ROOT}/build/config/sdkconfig.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gen_inc")
set(SIM_SDKCONFIG_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/gen_inc/sdkconfig.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SIM_SDKCONFIG_AUTOCONF}"
    "${MT_ROOT}/sdkconfig"
    "${MT_ROOT}/sdkconfig.defaults")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/cmake/gen_sdkconfig.py"
        "${MT_ROOT}" "${SIM_SDKCONFIG_AUTOCONF}" "${SIM_SDKCONFIG_GENERATED}"
    RESULT_VARIABLE SIM_SDKCONFIG_RESULT)
if(NOT SIM_SDKCONFIG_RESULT EQUAL 0)
    message(FATAL_ERROR
        "sdkconfig mirror generation failed (see message above)")
endif()

# ---- layers under test (real sources; submodules stay untouched) ----
set(APP_CORE "${MT_ROOT}/layers/app_manager/app_core")
set(APP_THEME "${MT_ROOT}/layers/app_manager/app_theme")
set(MIDDLEWARE "${MT_ROOT}/layers/middleware/components")
set(APPS "${MT_ROOT}/layers/apps")

set(SIM_LAYER_SOURCES
    "${MIDDLEWARE}/mt_log/src/mt_log.c"
    "${MIDDLEWARE}/event_bus/src/event_bus.c"
    "${MIDDLEWARE}/nv_storage/src/nv_storage.c"
    "${MIDDLEWARE}/nv_storage/src/nv_storage_blob.c"
    "${MIDDLEWARE}/timer_service/src/timer_service.c"
    "${MIDDLEWARE}/power_service/src/power_service.c"
    "${MIDDLEWARE}/imu_service/src/imu_service.c"
    "${MIDDLEWARE}/time_service/src/time_service.c"
    "${MIDDLEWARE}/time_service/src/time_service_core.c"
    "${MIDDLEWARE}/chore_service/src/chore_service.c"
    "${MIDDLEWARE}/onboarding_service/src/onboarding_service.c"
    "${MIDDLEWARE}/weather_service/src/weather_service.c"
    "${MIDDLEWARE}/weather_service/src/weather_service_cache.c"
    "${MIDDLEWARE}/weather_service/src/weather_service_parse.c"
    "${MIDDLEWARE}/weather_service/src/weather_service_port.c"
    "${MT_ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c"
    "${MIDDLEWARE}/factory_reset_service/src/factory_reset_service.c"
    "${MIDDLEWARE}/connectivity_manager/src/connectivity_manager.c"
    "${MIDDLEWARE}/wifi_service/src/wifi_service.c"
    "${MIDDLEWARE}/wifi_service/src/wifi_service_credentials.c"
    "${MIDDLEWARE}/wifi_service/src/wifi_service_worker.c"
    "${MIDDLEWARE}/wifi_service/src/wifi_service_worker_ops.c"
    "$ENV{IDF_PATH}/components/bt/common/tinycrypt/src/sha256.c"
    "$ENV{IDF_PATH}/components/bt/common/tinycrypt/src/utils.c"
    "${MT_ROOT}/main/app_product_config.c"
    "${APP_THEME}/src/app_theme.c"
    "${APP_CORE}/src/app_manager_pm.c"
    "${APP_CORE}/src/app_manager_pm_api.c"
    "${APP_CORE}/src/app_manager_pm_init.c"
    "${APP_CORE}/src/app_manager_pm_worker.c"
    "${APP_CORE}/src/app_manager_mailbox.c"
    "${APP_CORE}/src/app_manager_builtin.c"
    "${APP_CORE}/src/app_manager_back_gesture.c"
    "${APP_CORE}/src/app_manager_nav_model.c"
    "${APP_CORE}/src/app_manager_navigation.c"
    "${APP_CORE}/src/app_manager_presentation.c"
    "${APP_CORE}/src/app_manager_lifecycle.c"
    "${APP_CORE}/src/app_manager_lifecycle_api.c"
    "${APP_CORE}/src/app_manager_task_switcher.c"
    "${APP_CORE}/src/app_manager_display_diagnostics.c"
    "${APP_CORE}/src/app_manager_lvgl_adapter.c")

# apps + fakes must land in the executable as sources (or whole-archive):
# with only the .ld fragment, GNU ld drops unreferenced objects and the
# .app_manager_apps section stays empty (app_manager_builtin.c gate).
set(SIM_APP_SOURCES
    "${APPS}/common/src/app_ui.c"
    "${APPS}/common/src/app_weather_ui.c"
    "${APPS}/home_app/src/home_app.c"
    "${APPS}/clock_app/src/clock_app.c"
    "${APPS}/clock_app/src/clock_countdown_page.c"
    "${APPS}/clock_app/src/clock_duration_page.c"
    "${APPS}/clock_app/src/clock_stopwatch_page.c"
    "${APPS}/clock_app/src/clock_focus_page.c"
    "${APPS}/recorder_app/src/recorder_app.c"
    "${APPS}/recorder_app/src/recorder_files_page.c"
    "${APPS}/level_app/src/level_app.c"
    "${APPS}/level_app/src/level_app_persistence.c"
    "${APPS}/diagnostics_app/src/diagnostics_app.c"
    "${APPS}/weather_app/src/weather_app.c"
    "${APPS}/weather_app/src/weather_ui.c"
    "${APPS}/weather_app/src/weather_root_page.c"
    "${APPS}/weather_app/src/weather_forecast_page.c"
    "${APPS}/weather_app/src/weather_alert_pages.c"
    "${APPS}/menu_app/src/menu_app.c"
    "${APPS}/settings_app/src/settings_app.c"
    "${APPS}/settings_app/src/settings_display_page.c"
    "${APPS}/settings_app/src/settings_wifi_page.c"
    "${APPS}/settings_app/src/settings_bluetooth_page.c"
    "${APPS}/settings_app/src/settings_device_page.c"
    "${APPS}/settings_app/src/settings_factory_reset_page.c"
    "${APPS}/setup_app/src/setup_app.c"
    "${APPS}/setup_app/src/setup_wifi_adapter.c"
    "ports/fakes/sim_fakes_services.c"
    "${MIDDLEWARE}/device_link_service/src/device_link_confirmation.c"
    "ports/fakes/sim_fakes_device_link.c"
    "ports/fakes/sim_wifi_port.c"
    "ports/fakes/sim_connectivity_digest.c"
    "ports/sim_network_runtime.c"
    "ports/fakes/sim_fakes_recorder.c")

set(SIM_PORT_SOURCES
    "apps/main_sim.c"
    "bsp/sim_bsp.c"
    "runtime/sim_runtime.c"
    "ports/sim_freertos.c"
    "ports/sim_lv_adapter.c"
    "ports/sim_mmap_assets.c"
    "ports/sim_fs.c"
    "ports/sim_nvs.c"
    "ports/sim_time.c"
    "ports/sim_http.c"
    "ports/sim_backends.c"
    "ports/sim_platform.c"
    "ports/sim_png.c"
    "agent/sim_agent_server.c"
    "agent/sim_agent_tree.c")

add_executable(microtech_sim
    ${SIM_PORT_SOURCES}
    ${SIM_LAYER_SOURCES}
    ${SIM_APP_SOURCES})

target_include_directories(microtech_sim PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/gen_inc"
    "${CMAKE_CURRENT_LIST_DIR}/stubs"
    "${CMAKE_CURRENT_LIST_DIR}/stubs/include"
    "${MT_ROOT}/main"
    "${CMAKE_CURRENT_LIST_DIR}/bsp"
    "${CMAKE_CURRENT_LIST_DIR}/ports"
    "${CMAKE_CURRENT_LIST_DIR}/runtime"
    "${CMAKE_CURRENT_LIST_DIR}/agent"
    "${MT_ROOT}/managed_components/lvgl__lvgl/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${MT_ROOT}/layers/bsp/include"
    "${APP_CORE}/include"
    "${APP_CORE}/src"
    "${APP_THEME}/include"
    "${APPS}/common/include"
    "${APPS}/weather_app/src"
    "${APPS}/menu_app/src"
    "${APPS}/settings_app/src"
    "${APPS}/setup_app/src"
    "${APPS}/clock_app/src"
    "${APPS}/recorder_app/src"
    "${APPS}/level_app/src"
    "${APPS}/diagnostics_app/src"
    "${MIDDLEWARE}/mt_log/include"
    "${MIDDLEWARE}/event_bus/include"
    "${MIDDLEWARE}/power_service/include"
    "${MIDDLEWARE}/imu_service/include"
    "${MIDDLEWARE}/audio_service/include"
    "${MIDDLEWARE}/sd_storage_service/include"
    "${MIDDLEWARE}/time_service/include"
    "${MIDDLEWARE}/time_service/src"
    "${MIDDLEWARE}/timer_service/include"
    "${MIDDLEWARE}/nv_storage/include"
    "${MIDDLEWARE}/recorder_service/include"
    "${MIDDLEWARE}/chore_service/include"
    "${MIDDLEWARE}/onboarding_service/include"
    "${MIDDLEWARE}/weather_service/include"
    "${MIDDLEWARE}/weather_service/src"
    "${MT_ROOT}/managed_components/espressif__cjson/cJSON"
    "${MIDDLEWARE}/connectivity_manager/include"
    "${MIDDLEWARE}/wifi_service/include"
    "${MIDDLEWARE}/wifi_service/src"
    "${MIDDLEWARE}/connectivity_manager/src"
    "$ENV{IDF_PATH}/components/bt/common/tinycrypt/include"
    "${MIDDLEWARE}/device_link/include"
    "${MIDDLEWARE}/device_link_service/include"
    "${MIDDLEWARE}/device_link_service/src"
    "${MIDDLEWARE}/factory_reset_service/include"
    "${MIDDLEWARE}/ble_runtime/include")

# Production-config mirror is force-injected exactly like IDF does; HOST_TEST
# is deliberately NOT defined (mailbox must drain on the LVGL worker timer).
target_compile_definitions(microtech_sim PRIVATE
    APP_RESOURCES_ENABLED=1)
target_compile_options(microtech_sim PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wno-variadic-macros -UNDEBUG
    "-include;${SIM_SDKCONFIG_GENERATED}")

# Keep the built-in app registry section alive (INSERT fragment script).
target_link_options(microtech_sim PRIVATE
    "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/cmake/app_builtin_apps.ld")

target_link_directories(microtech_sim PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(microtech_sim PRIVATE
    lvgl
    png_static
    ${SDL2_LIBRARIES}
    CURL::libcurl
    Threads::Threads
    m)

add_dependencies(microtech_sim sim_resources)

# Optional sanitizer for the simulator + layers sources (third-party stays
# unsanitized; their correctness is covered by the firmware build).
set(SIM_SANITIZER "none" CACHE STRING "none|address|thread")
if(NOT SIM_SANITIZER STREQUAL "none")
    if(SIM_SANITIZER STREQUAL "address")
        set(SIM_SANITIZE_FLAGS -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all)
    elseif(SIM_SANITIZER STREQUAL "thread")
        set(SIM_SANITIZE_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR "Unsupported SIM_SANITIZER=${SIM_SANITIZER}")
    endif()
    target_compile_options(microtech_sim PRIVATE ${SIM_SANITIZE_FLAGS})
    target_link_options(microtech_sim PRIVATE ${SIM_SANITIZE_FLAGS})
endif()
