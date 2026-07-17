cmake_minimum_required(VERSION 3.22)
set(IDF_TARGET "esp32s3")

set(EXTRA_COMPONENT_DIRS
    layers/bsp
    layers/middleware/components
    layers/app_manager
    layers/apps
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(microtech)

if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE AND NOT CONFIG_LV_USE_CLIB_MALLOC)
    message(FATAL_ERROR
        "LVGL FreeType requires the libc allocator for fonts larger than "
        "the fixed LVGL pool")
endif()

if(NOT CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB OR
        NOT CONFIG_ESP32S3_DATA_CACHE_64KB OR
        NOT CONFIG_ESP32S3_DATA_CACHE_LINE_64B)
    message(FATAL_ERROR "The ESP32-S3 LVGL profile requires 32K I-cache and 64K/64B D-cache")
endif()

if(NOT CONFIG_LV_USE_CLIB_STRING OR
        NOT CONFIG_LV_USE_CLIB_SPRINTF OR
        NOT CONFIG_LV_OBJ_STYLE_CACHE)
    message(FATAL_ERROR "The production LVGL profile requires libc string/sprintf and style cache")
endif()

if(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL LESS 98304)
    message(FATAL_ERROR "The 60-line LCD DMA staging path requires a 96 KiB internal reserve")
endif()
