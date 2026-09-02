# MicroTech LVGL 宿主模拟器（sim）

详细说明见 [`doc/lvgl-simulator.md`](../doc/lvgl-simulator.md)。本目录是独立 CMake
工程，不接入根 ESP-IDF 构建；渲染 `layers/` 真实 UI 代码，供开发调试、Agent 驱动与
CI 回归使用。

## 前置条件（WSL2 / Debian / Ubuntu）

```sh
sudo apt install build-essential cmake ninja-build python3 python3-pip \
    libsdl2-dev libcurl4-openssl-dev
# 不要 apt install libfreetype-dev 来链接 sim：发行版 2.13.x 与设备 2.14.3 不符。
# FreeType 2.14.3 使用树内 managed_components/espressif__freetype/freetype 源码。
pip3 install -r requirements-weather-assets.txt
```

`managed_components/lvgl__lvgl`、`espressif__cjson`、`espressif__freetype`、
`espressif__libpng` 必须先存在，且 `build/config/sdkconfig.h` 已由一次
`idf.py build` 生成（sim 的 CONFIG 镜像来源）。缺失时 CMake `FATAL_ERROR`，
不回退 FetchContent。

## 构建

```sh
cmake -S sim -B build/sim -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/sim
```

LVGL 选项（DEMOS/EXAMPLES/THORVG=OFF、`LV_BUILD_CONF_PATH`）已在
`sim/CMakeLists.txt` 内固化，命令行无需重复传。

## 日常开发（推荐入口）

```sh
python3 sim/dev.py
```

数字菜单：窗口 1:1 运行 / 直达某页 / 无头驻留 / 一键截图 / 控件树导出 /
CI 回归 / 更新金样 / 停止会话 / 重置设备状态。全部路径自动注入
（`build/sim/dev_nvs`、`build/sim/shots`、端口 5002），零参数。
无 DISPLAY 时"运行"自动转无头驻留并提示 simctl 用法。

## 运行

```sh
# 无头（WSL / Agent / CI 默认路径；DISPLAY 为空时自动回落 dummy）
SDL_VIDEODRIVER=dummy ./build/sim/microtech_sim --headless --ci \
    --res-dir build/sim/sim_res_fs --agent-port 5002

# 窗口预览（需 WSLg 或 X 服务器）
./build/sim/microtech_sim --res-dir build/sim/sim_res_fs --agent-port 5002

# 冒烟：跑 N 帧后经 mailbox 导航到指定 app 再退出
./build/sim/microtech_sim --headless --res-dir build/sim/sim_res_fs \
    --nvs-dir build/sim/sim_nvs --frames 60 --navigate weather \
    --out-dir build/sim/shots
```

CLI：`--headless`、`--ci`、`--res-dir <staged 资源目录>`、`--nvs-dir <NVS 目录>`、
`--agent-port N`、`--window-scale N`、`--out-dir <截图目录>`、`--frames N`、
`--navigate <app_id>`、`--screenshot NAME`。
Agent 协议（TCP JSON-RPC，默认 `127.0.0.1:5002`，避开真机基准 5001）见
`sim/tools/simctl.py`。

## 编译进 sim 的真实源码（当前里程碑）

- app_manager：app_core 15 个 `.c` + app_theme；`HOST_TEST` 不定义，mailbox 在
  LVGL worker 的 `lv_timer` 上排空（与真机一致）。
- apps：9 个 app + app_ui/app_weather_ui 共 17 个 `.c`，全部作为可执行文件的
  源文件编译，配合 `sim/cmake/app_builtin_apps.ld`（`-Wl,-T`）保住
  `.app_manager_apps` 链接段（注册表发现 9 个 app）。
- middleware：mt_log、event_bus、nv_storage（文件后端 `sim_nvs`）、timer_service、
  power/imu/weather/time/chore/onboarding/factory_reset/connectivity/wifi 业务真源；
  audio/sd/recorder 与 device_link 暂留 `ports/fakes/`。
- 不编：`system_pm`、`ble_runtime`、`wifi_service_idf_port.c`、bsp 真源码
  （sim_bsp 提供 `bsp_hal.h` 三契约与 DISPLAY|TOUCH|INPUT 能力面）。

## 已知设备语义要点（踩坑记录）

- 桌面 LVGL 构建定义 `LV_KCONFIG_IGNORE`，`lv_conf_kconfig.h` 的
  `CONFIG_LV_USE_CLIB_* → LV_USE_STDLIB_*` 映射被编译出去：`lv_conf.h` 必须
  直接给 `LV_USE_STDLIB_MALLOC/STRING/SPRINTF = LV_STDLIB_CLIB`，否则 FreeType
  大块分配落在 35 KB TLSF 池上失败（表现为 `FT_New_Face error(0x40)`）。
- `LV_FREETYPE_USE_LVGL_PORT=1` 时须像 `espressif__esp_lvgl_adapter`
  （CMakeLists:363-386）一样从 freetype 库剔除 `ftsystem.c` 并编入 LVGL 的
  `lv_ftsystem.c`，否则 `FT_New_Face` 走宿主 `fopen("F:font.ttf")` 失败。
- `lv_init()`（9.5）内部已调用 `lv_freetype_init`，shim 不得重复 init。
- LVGL worker 必须经 sim 的 `xTaskCreatePinnedToCore("lvgl", ...)` 创建：
  mailbox 用 `xTaskGetCurrentTaskHandle()` 捕获 worker 身份。

## 资源管线

`sim/cmake/resources.cmake` 复刻 `main/CMakeLists.txt:50-206`：读取
`layers/apps/resource_manifest.cmake` + `app_theme/resource_manifest.cmake`，PNG 经
仓库内 `managed_components/lvgl__lvgl/scripts/LVGLImage.py` 转
`RGB565A8 --ofmt BIN --compress NONE`，staged 到 `build/sim/sim_res_fs/`，并用
`gen_res_meta.py` 生成 `sim_res_meta.h`（文件数 + 与 `spiffs_assets_gen.py` 同算法的
mmap 校验和）。与设备一致性核对：

```sh
diff -rq build/sim/sim_res_fs build/esp-idf/main/app_res_fs   # 字节一致
python3 sim/cmake/gen_res_meta.py \
    --assets-dir build/sim/sim_res_fs --output /tmp/meta.h \
    --check-res-bin build/mmap_build/app_res_fs/res/res.bin   # 校验和 parity
```

## Kconfig 自动同步与配置防漂移

**sim 的 `sdkconfig.h` 不入库、不手维护**：`sim/cmake/gen_sdkconfig.py` 在每次
CMake configure 时从 `build/config/sdkconfig.h`（最近一次 `idf.py build` 的权威
展开，含本地私有天气域名/token）verbatim 生成 `build/sim/gen_inc/sdkconfig.h`
并 `-include` 注入，与固件逐宏一致；`CMAKE_CONFIGURE_DEPENDS` 挂了
`sdkconfig.h/.defaults/build autoconf`，任何一侧变化都会在下次
`dev.py`/`cmake --build` 自动重生成。

新鲜度门禁：源码实际引用的 CONFIG_ 符号按值比对 `sdkconfig` ↔ 展开（sdkconfig
里的历史残留项不参与，避免 mtime/全集比对误报）；不一致时 configure 直接失败并
提示 `idf.py build`。

```sh
python3 sim/ci/check_lv_conf.py   # lv_conf.h ↔ 生成 mirror ↔ defaults ↔ 设备 autoconf
```

LVGL 侧仍四方核对并强制 CANVAS/SNAPSHOT/IMAGE/FREETYPE PORT/FS_POSIX=0 等门槛
（lv_conf.h 是桌面构建手写文件，Kconfig 映射被 LV_KCONFIG_IGNORE 编译掉，需人工
与真实配置保持一致，门禁负责兜底）。

## Agent 桥（M6）

`--agent-port N`（默认 5002，仅绑 127.0.0.1；5001 留给真机基准）。协议为逐行
JSON-RPC：`{"id":1,"method":"sim.ping","params":{...}}` →
`{"id":1,"ok":true,"result":{...}}`。

方法：`sim.ping`、`sim.step {ms}`（33 的倍数，仅 CI）、`sim.wait_idle
{timeout_ms}`（帧哈希连续 2 步不变 + `lv_anim_count_running()==0`）、
`sim.screenshot {name,wait_idle}`、`sim.tree`（绝对坐标 + 多 part 计算样式 +
类型值 + 图像语义 ID）、`sim.touch {action,x,y}`、`sim.key {button,action}`
（press/release/click）、`sim.navigate {app}`（投 mailbox，不阻塞）、`sim.apps`、
`sim.set_time {epoch}`、`sim.set_power {voltage,pct,charging,vbus}`、
`sim.set_imu {pitch,roll}`（度）、`sim.set_weather {endpoint,status,body}`
（灌 sim_http 脚本表后 `request_refresh`，真解析链）、`sim.pause {enabled}`、
`sim.exit`。客户端：`python3 sim/tools/simctl.py <command>`。

锁纪律：tree/screenshot 持 `esp_lv_adapter_lock` 只读/拷贝；step/wait/navigate
不持锁；touch/key 只写原子态。CI 会话驻留直到 `sim.exit` 或信号。

## 回归（sim/ci/run_ci.sh）

```sh
sim/ci/run_ci.sh                    # 端口预检 + check_lv_conf + 构建 + staged parity + 全部场景
sim/ci/run_ci.sh build/sim --update # 重新生成 PNG 金样（人工 review 后入库）
```

**PNG 辅门禁当前挂起**：固件 GUI 仍在调整，`sim/golden/` 已被 .gitignore 排除。
默认只跑树断言；本地残留 PNG 不会自动打开辅门禁。显式 `SIM_PNG_GOLDEN=1`
或 `--update` 才比对/生成。GUI 定标后人工 review 再入库。

**tree_assert 基线约定**：场景文本断言代表当前固件 UI 快照。GUI 调整期
`run_ci.sh` FAIL 是回归信号——有意改动在同一提交里更新场景断言；意外改动即
门禁生效。

每个场景独立 sim 进程 + 独立 NVS 目录（resident-app 策略与冷启动一致）。
场景位于 `sim/ci/scenarios/`：`all_apps.json`（9 app 主页面文本断言 + 截图）、
`smoke_pages.json`（导航/手势/多页）、`weather_flow.json`（真 weather 管线：
`sim_http` 脚本层灌入 `sim/ci/fixtures/*.json`，验证 location→current/hourly/
daily 解析与渲染，并打开 forecast 子页）。树断言为主门禁（≥20）。

确定性依赖数据冻结：场景首步 `set_time/set_power/set_imu/set_wifi`，UI 稳定判据
为帧哈希连续两步不变 + `lv_anim_count_running()==0`。PNG 辅门禁默认关闭。

TSan（WSL2 必须 `setarch -R`，同 app_core/tests/host/README.md）：

```sh
cmake -S sim -B build/sim-tsan -G Ninja -DSIM_SANITIZER=thread
cmake --build build/sim-tsan
setarch $(uname -m) -R build/sim-tsan/microtech_sim --headless --ci \
    --res-dir build/sim/sim_res_fs --nvs-dir /tmp/sim-tsan-nvs &
python3 sim/tools/run_scenarios.py sim/ci/scenarios/all_apps.json
```

当前 all_apps 场景在 TSan 下 0 警告（sim 自身线程与 layers 锁纪律均干净）。

## 里程碑状态

- M1/M2/M3/M4：完成（资源/绑定表与设备字节一致；mailbox 握手；转场/左滑返回/
  电源 suspend-resume/boot 单击回 home 均像素级验证；`--ci` step 可排空 mailbox，
  CI 与自由运行终态像素一致）。
- M5：完成主体（product config 同源直编；time/power/imu/chore/onboarding/
  factory_reset/weather/connectivity_manager/wifi 业务真源；weather 走
  libcurl 真实管线（`sim_http` 脚本层支持 CI 灌入）；NVS 文件后端持久化已验证；
  audio/sd/recorder 与 device_link 暂留 fake（`ports/fakes/`），weather 的
  CI fixture JSON 与 setup Wi-Fi 场景化验证在 M7 落地）。
- M6：完成（上节）。
- M7：完成主干（runner + 4 场景 ≥21 断言点 + 双轮字节一致验证 + TSan 0 警告 +
  `run_ci.sh` 一键回归）。PNG 金样入库挂起（待固件 GUI 定标；`SIM_PNG_GOLDEN=1`）。
  待扩展：每 app 子页/转场中间态场景、setup Wi-Fi 交互脚本化（sim.set_wifi
  目前仅驱动 weather 网络门控）、audio/sd/recorder 真源化、MCP 适配层（可选）。

## PM 策略与已知环境特性

- **默认常亮**：`sim_runtime_boot` 在 startup_commit 后调用
  `app_manager_pm_set_timeout_ms(-1)` + `set_standby_delay_ms(-1)`（固件"永不"档
  真实路径）。原因：PM 用真实墙钟（不受 `--ci` tick 冻结影响），30s 息屏会把
  CI 会话拖进 mailbox 停排（同步 ui_call 永久阻塞）、触摸禁用、全黑帧。
- **息屏是要测的行为而不是环境噪声**：Agent `sim.pm {off_ms,standby_ms,get}`
  （公开 API 包装）+ 场景 `pm_lifecycle.json`：短超时→黑帧断言→power click
  唤醒→唤醒帧与息屏前**逐字节一致**（本目录 `sim/golden/pm_clock_*.png` 已证）。
- **WSL mirrored 网络**：boot 完成前连接 agent 端口会得到 SYN 静默超时而非
  RST-refused（mirrored 的 localhost 共享路由特性，非 sim 缺陷）；dev.py 已有
  60s 就绪等待，手工调试时先 `ss -tln | grep 5002` 确认监听。
- 退出链已验证：窗口关闭/`sim.exit`/SIGTERM 三路径均干净退出，dev.py 选项 8
  带 PID kill 兜底。

## 线程与像素模型

- LVGL worker 独占 `lv_timer_handler`，主线程泵 SDL，Agent 线程只走锁纪律表；
- 唯一像素路径：flush_cb → `custom_draw_bitmap` → `esp_lcd_panel_draw_bitmap`
  （sim_bsp blit 进 368×448 RGB565 FB）→ 同步 `lv_display_flush_ready`；
- 宿主禁止 `lv_draw_sw_rgb565_swap`（真机 swap 仅服务 QSPI 面板端序）。

## 目录

`apps/` 入口；`bsp/` 模拟板；`runtime/` 装配（M2+）；`ports/` shim 与服务 port；
`agent/` TCP 桥（M6）；`ci/` 检查与场景；`cmake/` 资源管线与链接脚本；
`golden/` PNG 基线（M7）；`tools/` simctl/MCP；`stubs/` 平台桩与 CONFIG 镜像。
