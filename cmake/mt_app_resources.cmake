# 应用/主题资源清单的统一解析与暂存规则(main 固件与 sim 宿主共用)。
#
# 记录格式(6 字段, | 分隔): "源文件|输出名|语义ID|宽|高|类型"
#   类型:  PNG  -- 仓库内位图; SVG -- 矢量源, 构建期栅格化为 RGBA PNG;
#          FONT -- 原样拷贝。
# 图像记录(PNG/SVG)参与 app_manager 语义 ID -> mmap 索引表; 宽/高即最终
# 资源尺寸(SVG 记录为栅格化目标尺寸, 同一 SVG 可登记多条不同尺寸记录)。
#
# 调用方在函数作用域可见的输入变量:
#   MT_APP_RESOURCE_RECORDS      清单记录列表
#   MT_APP_RESOURCE_ENABLE_FS    真值时图像转 RGB565A8 BIN, 否则原样拷贝/直出 PNG
#   MT_APP_RESOURCE_PYTHON       Python 解释器(需 Pillow; SVG 记录还需 PyMuPDF)
#   MT_APP_RESOURCE_CONVERTER    LVGLImage.py 路径(FS 模式必需, 由调用方校验)
#   MT_APP_RESOURCE_RASTERIZER   tools/asset_pipeline/svg2png.py 路径(存在 SVG 记录时必需)
#   MT_APP_RESOURCE_STAGE_DIR    暂存目录
#   MT_APP_RESOURCE_TMP_ROOT     每记录临时目录的根
#
# 输出(设置到调用方作用域):
#   MT_APP_RESOURCE_STAGED_FILES   全部暂存产物完整路径
#   MT_APP_RESOURCE_IMAGE_ENTRIES  生成的图像资源表 C 片段
#   MT_APP_RESOURCE_RECORD_COUNT   记录总数

function(mt_stage_app_resources)
    set(MT_AR_OUTPUTS "")
    set(MT_AR_SEMANTICS "")
    set(MT_AR_STAGED_OUTPUTS "")
    set(MT_AR_SORTED_NAMES "")
    set(MT_AR_STAGED_NAMES "")
    set(MT_AR_SEMANTIC_IDS "")
    set(MT_AR_WIDTHS "")
    set(MT_AR_HEIGHTS "")
    set(MT_AR_KINDS "")

    foreach(MT_AR_RECORD IN LISTS MT_APP_RESOURCE_RECORDS)
        string(REPLACE "|" ";" MT_AR_FIELDS "${MT_AR_RECORD}")
        list(LENGTH MT_AR_FIELDS MT_AR_FIELD_COUNT)
        if(NOT MT_AR_FIELD_COUNT EQUAL 6)
            message(FATAL_ERROR "Invalid resource manifest record: ${MT_AR_RECORD}")
        endif()
        list(GET MT_AR_FIELDS 0 MT_AR_SOURCE)
        list(GET MT_AR_FIELDS 1 MT_AR_OUTPUT)
        list(GET MT_AR_FIELDS 2 MT_AR_SEMANTIC)
        list(GET MT_AR_FIELDS 3 MT_AR_WIDTH)
        list(GET MT_AR_FIELDS 4 MT_AR_HEIGHT)
        list(GET MT_AR_FIELDS 5 MT_AR_KIND)
        if(NOT EXISTS "${MT_AR_SOURCE}")
            message(FATAL_ERROR "Resource source does not exist: ${MT_AR_SOURCE}")
        endif()
        list(FIND MT_AR_OUTPUTS "${MT_AR_OUTPUT}" MT_AR_OUTPUT_INDEX)
        if(NOT MT_AR_OUTPUT_INDEX EQUAL -1)
            message(FATAL_ERROR "Duplicate resource output name: ${MT_AR_OUTPUT}")
        endif()
        list(APPEND MT_AR_OUTPUTS "${MT_AR_OUTPUT}")
        if(MT_AR_KIND STREQUAL "PNG" OR MT_AR_KIND STREQUAL "SVG")
            if(MT_AR_WIDTH LESS 1 OR MT_AR_HEIGHT LESS 1)
                message(FATAL_ERROR "Invalid ${MT_AR_KIND} dimensions: ${MT_AR_SOURCE}")
            endif()
            list(FIND MT_AR_SEMANTICS "${MT_AR_SEMANTIC}" MT_AR_SEMANTIC_INDEX)
            if(NOT MT_AR_SEMANTIC_INDEX EQUAL -1)
                message(FATAL_ERROR "Duplicate resource semantic ID: ${MT_AR_SEMANTIC}")
            endif()
            list(APPEND MT_AR_SEMANTICS "${MT_AR_SEMANTIC}")
        elseif(NOT MT_AR_KIND STREQUAL "FONT")
            message(FATAL_ERROR "Unsupported resource kind ${MT_AR_KIND}: ${MT_AR_SOURCE}")
        endif()
        if(MT_APP_RESOURCE_ENABLE_FS AND
                (MT_AR_KIND STREQUAL "PNG" OR MT_AR_KIND STREQUAL "SVG"))
            string(REGEX REPLACE "\\.[^.]+$" ".bin" MT_AR_STAGED_NAME "${MT_AR_OUTPUT}")
        else()
            set(MT_AR_STAGED_NAME "${MT_AR_OUTPUT}")
        endif()
        get_filename_component(MT_AR_STAGED_EXT "${MT_AR_STAGED_NAME}" EXT)
        get_filename_component(MT_AR_STAGED_BASE "${MT_AR_STAGED_NAME}" NAME_WE)
        list(FIND MT_AR_STAGED_OUTPUTS "${MT_AR_STAGED_NAME}" MT_AR_STAGED_INDEX)
        if(NOT MT_AR_STAGED_INDEX EQUAL -1)
            message(FATAL_ERROR
                "Duplicate staged resource name: ${MT_AR_STAGED_NAME}")
        endif()
        list(APPEND MT_AR_STAGED_OUTPUTS "${MT_AR_STAGED_NAME}")
        list(APPEND MT_AR_SORTED_NAMES
            "${MT_AR_STAGED_EXT}|${MT_AR_STAGED_BASE}")
        list(APPEND MT_AR_STAGED_NAMES "${MT_AR_STAGED_NAME}")
        list(APPEND MT_AR_SEMANTIC_IDS "${MT_AR_SEMANTIC}")
        list(APPEND MT_AR_WIDTHS "${MT_AR_WIDTH}")
        list(APPEND MT_AR_HEIGHTS "${MT_AR_HEIGHT}")
        list(APPEND MT_AR_KINDS "${MT_AR_KIND}")
    endforeach()

    list(SORT MT_AR_SORTED_NAMES)

    set(MT_AR_IMAGE_ENTRIES "")
    set(MT_AR_STAGED_FILES "")
    set(MT_AR_INDEX 0)
    list(LENGTH MT_AR_KINDS MT_AR_RECORD_COUNT)
    while(MT_AR_INDEX LESS MT_AR_RECORD_COUNT)
        list(GET MT_AR_KINDS ${MT_AR_INDEX} MT_AR_KIND)
        list(GET MT_AR_STAGED_NAMES ${MT_AR_INDEX} MT_AR_STAGED_NAME)
        list(GET MT_AR_SEMANTIC_IDS ${MT_AR_INDEX} MT_AR_SEMANTIC)
        list(GET MT_AR_WIDTHS ${MT_AR_INDEX} MT_AR_WIDTH)
        list(GET MT_AR_HEIGHTS ${MT_AR_INDEX} MT_AR_HEIGHT)
        list(GET MT_APP_RESOURCE_RECORDS ${MT_AR_INDEX} MT_AR_RECORD)
        string(REPLACE "|" ";" MT_AR_FIELDS "${MT_AR_RECORD}")
        list(GET MT_AR_FIELDS 0 MT_AR_SOURCE)
        list(GET MT_AR_FIELDS 1 MT_AR_OUTPUT)
        get_filename_component(MT_AR_STAGED_EXT "${MT_AR_STAGED_NAME}" EXT)
        get_filename_component(MT_AR_STAGED_BASE "${MT_AR_STAGED_NAME}" NAME_WE)
        list(FIND MT_AR_SORTED_NAMES "${MT_AR_STAGED_EXT}|${MT_AR_STAGED_BASE}"
            MT_AR_ASSET_INDEX)
        if(MT_AR_KIND STREQUAL "PNG" OR MT_AR_KIND STREQUAL "SVG")
            string(APPEND MT_AR_IMAGE_ENTRIES
                "    { ${MT_AR_SEMANTIC}, ${MT_AR_ASSET_INDEX}U, ${MT_AR_WIDTH}U, ${MT_AR_HEIGHT}U },\n")
        endif()
        set(MT_AR_DEST "${MT_APP_RESOURCE_STAGE_DIR}/${MT_AR_STAGED_NAME}")
        get_filename_component(MT_AR_OUTPUT_BASE "${MT_AR_OUTPUT}" NAME_WE)
        if(MT_AR_KIND STREQUAL "FONT" OR
                (MT_AR_KIND STREQUAL "PNG" AND NOT MT_APP_RESOURCE_ENABLE_FS))
            add_custom_command(OUTPUT "${MT_AR_DEST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${MT_APP_RESOURCE_STAGE_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MT_AR_SOURCE}" "${MT_AR_DEST}"
                DEPENDS "${MT_AR_SOURCE}"
                VERBATIM)
        elseif(MT_AR_KIND STREQUAL "PNG")
            set(MT_AR_TMP "${MT_APP_RESOURCE_TMP_ROOT}/mt_res_tmp_${MT_AR_INDEX}")
            get_filename_component(MT_AR_SOURCE_STEM "${MT_AR_SOURCE}" NAME_WE)
            add_custom_command(OUTPUT "${MT_AR_DEST}"
                COMMAND ${CMAKE_COMMAND} -E remove_directory "${MT_AR_TMP}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${MT_AR_TMP}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${MT_APP_RESOURCE_STAGE_DIR}"
                COMMAND "${MT_APP_RESOURCE_PYTHON}"
                    "${MT_APP_RESOURCE_CONVERTER}"
                    --ofmt BIN --cf RGB565A8 --compress NONE
                    --output "${MT_AR_TMP}" "${MT_AR_SOURCE}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MT_AR_TMP}/${MT_AR_SOURCE_STEM}.bin" "${MT_AR_DEST}"
                DEPENDS "${MT_AR_SOURCE}" "${MT_APP_RESOURCE_CONVERTER}"
                VERBATIM)
        elseif(MT_APP_RESOURCE_ENABLE_FS)
            if(NOT EXISTS "${MT_APP_RESOURCE_RASTERIZER}")
                message(FATAL_ERROR
                    "SVG resources require the rasterizer at ${MT_APP_RESOURCE_RASTERIZER}")
            endif()
            set(MT_AR_TMP "${MT_APP_RESOURCE_TMP_ROOT}/mt_res_tmp_${MT_AR_INDEX}")
            add_custom_command(OUTPUT "${MT_AR_DEST}"
                COMMAND ${CMAKE_COMMAND} -E remove_directory "${MT_AR_TMP}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${MT_AR_TMP}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${MT_APP_RESOURCE_STAGE_DIR}"
                COMMAND "${MT_APP_RESOURCE_PYTHON}"
                    "${MT_APP_RESOURCE_RASTERIZER}"
                    --width "${MT_AR_WIDTH}" --height "${MT_AR_HEIGHT}"
                    --output "${MT_AR_TMP}/${MT_AR_OUTPUT_BASE}.png"
                    "${MT_AR_SOURCE}"
                COMMAND "${MT_APP_RESOURCE_PYTHON}"
                    "${MT_APP_RESOURCE_CONVERTER}"
                    --ofmt BIN --cf RGB565A8 --compress NONE
                    --output "${MT_AR_TMP}"
                    "${MT_AR_TMP}/${MT_AR_OUTPUT_BASE}.png"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MT_AR_TMP}/${MT_AR_OUTPUT_BASE}.bin" "${MT_AR_DEST}"
                DEPENDS "${MT_AR_SOURCE}" "${MT_APP_RESOURCE_CONVERTER}"
                    "${MT_APP_RESOURCE_RASTERIZER}"
                VERBATIM)
        else()
            if(NOT EXISTS "${MT_APP_RESOURCE_RASTERIZER}")
                message(FATAL_ERROR
                    "SVG resources require the rasterizer at ${MT_APP_RESOURCE_RASTERIZER}")
            endif()
            add_custom_command(OUTPUT "${MT_AR_DEST}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${MT_APP_RESOURCE_STAGE_DIR}"
                COMMAND "${MT_APP_RESOURCE_PYTHON}"
                    "${MT_APP_RESOURCE_RASTERIZER}"
                    --width "${MT_AR_WIDTH}" --height "${MT_AR_HEIGHT}"
                    --output "${MT_AR_DEST}" "${MT_AR_SOURCE}"
                DEPENDS "${MT_AR_SOURCE}" "${MT_APP_RESOURCE_RASTERIZER}"
                VERBATIM)
        endif()
        list(APPEND MT_AR_STAGED_FILES "${MT_AR_DEST}")
        math(EXPR MT_AR_INDEX "${MT_AR_INDEX} + 1")
    endwhile()

    set(MT_APP_RESOURCE_STAGED_FILES "${MT_AR_STAGED_FILES}" PARENT_SCOPE)
    set(MT_APP_RESOURCE_IMAGE_ENTRIES "${MT_AR_IMAGE_ENTRIES}" PARENT_SCOPE)
    set(MT_APP_RESOURCE_RECORD_COUNT "${MT_AR_RECORD_COUNT}" PARENT_SCOPE)
endfunction()
