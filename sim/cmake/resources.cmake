# 宿主资源管线：清单解析与暂存复用父仓库 cmake/mt_app_resources.cmake
# （与固件同一规则：PNG 经仓库内 LVGLImage.py 转 RGB565A8 BIN，SVG 先经
# tools/asset_pipeline/svg2png.py 栅格化，FONT 原样拷贝），并生成
# app_resources_generated.h 与 sim_res_meta.h（文件数 + mmap 校验和，算法
# 对齐 spiffs_assets_gen.py）。

find_package(Python3 REQUIRED COMPONENTS Interpreter)

include("${MT_ROOT}/cmake/mt_app_resources.cmake")
include("${MT_ROOT}/layers/apps/resource_manifest.cmake")
include("${MT_ROOT}/layers/app_manager/app_theme/resource_manifest.cmake")
set(MT_APP_RESOURCE_RECORDS ${MICROTECH_APP_RESOURCE_RECORDS}
    ${MICROTECH_THEME_RESOURCE_RECORDS})

set(MT_APP_RESOURCE_PYTHON "${Python3_EXECUTABLE}")
set(MT_APP_RESOURCE_LVGL_DIR "${MT_ROOT}/managed_components/lvgl__lvgl")
set(MT_APP_RESOURCE_CONVERTER "${MT_APP_RESOURCE_LVGL_DIR}/scripts/LVGLImage.py")
if(NOT EXISTS "${MT_APP_RESOURCE_CONVERTER}")
    message(FATAL_ERROR
        "managed_components/lvgl__lvgl is missing scripts/LVGLImage.py; "
        "run an idf.py build once to populate managed_components")
endif()
set(MT_APP_RESOURCE_RASTERIZER
    "${MT_ROOT}/tools/asset_pipeline/svg2png.py")
set(MT_APP_RESOURCE_ENABLE_FS 1)
set(MT_APP_RESOURCE_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/sim_res_fs")
set(MT_APP_RESOURCE_TMP_ROOT "${CMAKE_CURRENT_BINARY_DIR}")

mt_stage_app_resources()

# 生成头对齐设备 configure_file 路径；校验和由构建期 sim_res_meta.h 提供，
# 避免在 CMake 配置阶段读取二进制文件内容。
set(APP_RESOURCE_FILE_COUNT_VALUE ${MT_APP_RESOURCE_RECORD_COUNT})
set(APP_RESOURCE_CHECKSUM_VALUE APP_RESOURCES_SIM_CHECKSUM)
set(APP_RESOURCE_IMAGE_ENTRIES ${MT_APP_RESOURCE_IMAGE_ENTRIES})
configure_file("${MT_ROOT}/main/app_resources_generated.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app_resources_generated.h" @ONLY)

set(SIM_RESOURCE_META_HEADER "${CMAKE_CURRENT_BINARY_DIR}/sim_res_meta.h")
add_custom_command(OUTPUT "${SIM_RESOURCE_META_HEADER}"
    COMMAND "${MT_APP_RESOURCE_PYTHON}"
        "${CMAKE_CURRENT_LIST_DIR}/gen_res_meta.py"
        --assets-dir "${MT_APP_RESOURCE_STAGE_DIR}"
        --output "${SIM_RESOURCE_META_HEADER}"
    DEPENDS ${MT_APP_RESOURCE_STAGED_FILES}
        "${CMAKE_CURRENT_LIST_DIR}/gen_res_meta.py"
    VERBATIM)
add_custom_target(sim_resources ALL
    DEPENDS ${MT_APP_RESOURCE_STAGED_FILES} "${SIM_RESOURCE_META_HEADER}")
