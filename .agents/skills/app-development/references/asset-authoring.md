# App Image Asset Workflow (SVG-first)

仓库内 App 图片只有 SVG 源;不存在也不新增提交的 PNG。构建期由
`cmake/mt_app_resources.cmake` 统一执行
`SVG → tools/asset_pipeline/svg2png.py (PyMuPDF) → RGBA PNG → LVGLImage.py →
RGB565A8 BIN → res 分区(mmap)`,固件与 sim 共用同一规则。

## Authoring rules for an SVG source

- 终色写入文件:`fill`/`stroke` 必须是显式 hex(取自
  `layers/apps/common/include/app_ui_theme.h` 色板);`currentColor` 会被
  `svg2png.py` 拒绝构建。
- 必须有 `viewBox`;导出尺寸不写在 SVG 里,由 manifest 的宽/高字段决定
  (栅格化 4× 过采样,任意目标尺寸同源)。
- 画布用 16 或 24 网格(对齐 QWeather 图标风格),透明底,几何描边端点用
  `stroke-linecap="round"`。

## Registering the asset

1. 源文件放到 owning App 的 `assets/`(语义名,如 `tile_recorder.svg`)。
2. 在该 App 的 `resource_manifest.cmake` 逐条登记 6 字段记录:
   `"${CMAKE_CURRENT_LIST_DIR}/assets/<name>.svg|<staged输出名>.png|APP_IMAGE_<X>|W|H|SVG"`;
   同一源需要多个尺寸时登记多条,输出名互不相同。
3. 在 `layers/apps/common/include/app_image_ids.h` 的 owning App 预留段内分配
   新的 `APP_IMAGE_*` ID(主页瓦片用 0x5770-0x577F)。
4. UI 代码经 `app_manager_get_image()` 取描述符并保留 LVGL symbol 回退。

## Vendoring QWeather icons

天气状况图标固定为 `~/esp/project/Icons`(qwd/Icons, MIT)的 `N-fill.svg`
变体:`icons/<code>-fill.svg` 复制后把 `currentColor` 替换为主题色,重命名为
`weather_<slot>.svg`,保留 `qweather-icons-LICENSE.txt` 并在 manifest 头注明
来源。条件 code→slot 映射只改 `app_weather_ui.c`,不改资源。

## Validation

- `idf.py reconfigure` 后构建资源阶段即验证整条栅格化链;改动 manifest 后必须
  reconfigure。
- 宿主契约:`python3 -m unittest discover -s tests/resources`(SVG 集合、无位图
  残留、manifest 记录与源存在)。
- 目视:sim 截图(`sim/ci/run_ci.sh` 或 dev 会话)确认 40px 小图标不糊色、
  112px 大图无锯齿;构建 python 需 Pillow + PyMuPDF
  (`requirements-weather-assets.txt`)。
