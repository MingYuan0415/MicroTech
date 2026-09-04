# MicroTech LVGL 宿主模拟器（sim）

本目录是独立 CMake 工程，不接入根 ESP-IDF 构建；渲染 `layers/` 真实 UI 代码，供开发
调试、Agent 驱动与 CI 回归使用。本 README 是模拟器的唯一使用与契约文档。

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

数字菜单：启动窗口或无头会话时可选择联网/离线，默认离线；另提供直达某页、一键截图、
控件树导出、CI 回归、更新金样、停止会话和重置设备状态。全部路径自动注入
（`build/sim/dev_nvs`、`build/sim/shots`、端口 5002），零参数。
无 DISPLAY 时"运行"自动转无头驻留并提示 simctl 用法。
联网会话使用宿主机真实 HTTP/HTTPS；离线无头会话使用 CI 时钟，不会发起天气请求。

窗口启动后使用固定的面板尺寸，不响应边缘拖拽；需要改变显示倍率时使用
`--window-scale N` 启动模拟器。

## 运行

```sh
# 无头（WSL / Agent / CI 默认路径；DISPLAY 为空时自动回落 dummy）
SDL_VIDEODRIVER=dummy ./build/sim/microtech_sim --headless --ci \
    --res-dir build/sim/sim_res_fs --agent-port 5002

# 窗口预览（需 WSLg 或 X 服务器）
./build/sim/microtech_sim --res-dir build/sim/sim_res_fs --agent-port 5002

# 冒烟：运行 N 次模拟器循环，经 mailbox 导航到指定 app 后退出
./build/sim/microtech_sim --headless --res-dir build/sim/sim_res_fs \
    --nvs-dir build/sim/sim_nvs --frames 60 --navigate weather \
    --out-dir build/sim/shots
```

CLI：`--headless`、`--ci`、`--res-dir <staged 资源目录>`、`--nvs-dir <NVS 目录>`、
`--agent-port N`、`--window-scale N`、`--out-dir <截图目录>`、`--frames N`、
`--navigate <app_id>`、`--screenshot NAME`、`--brightness N`、`--key boot|power`、
`--swipe x1,y1,x2,y2[,steps]`、`--ci-step N`。`--frames N` 在 CI 中每次推进 33 ms，
非 CI 中每次泵事件并呈现；`0` 表示驻留。截图只有显式指定 `--screenshot` 才生成，
未指定 `--out-dir` 时写入当前目录，否则写入该目录。
Agent 协议（TCP JSON-RPC，默认 `127.0.0.1:5002`，避开真机基准 5001）见
`sim/tools/simctl.py`。

## 编译进 sim 的真实源码

- app_manager：app_core 15 个 `.c` + app_theme；`HOST_TEST` 不定义，mailbox 在
  LVGL worker 的 `lv_timer` 上排空（与真机一致）。
- apps：9 个 app + app_ui/app_weather_ui 共 17 个 `.c`，全部作为可执行文件的
  源文件编译，配合 `sim/cmake/app_builtin_apps.ld`（`-Wl,-T`）保住
  `.app_manager_apps` 链接段（注册表发现 9 个 app）。
- middleware：mt_log、event_bus、nv_storage（文件后端 `sim_nvs`）、timer_service、
  power/imu/weather/time/chore/onboarding/factory_reset/connectivity/wifi/recorder
  业务真源；audio/sd 与 device_link 留 `ports/fakes/`——SD 由 `--sd-dir` 指定的
  宿主目录真实承载（挂载语义用目录 rename 模拟，容量走 `statvfs`），recorder 的
  WAV 读写、rename、删除都落在该真实文件系统上。注意服务内部文件名为 64 字节
  全路径缓冲，`--sd-dir` 需保持足够浅（如 `/tmp/...`），过深时 boot 会打印告警
  且录音不会入列。
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

## Agent 桥

`--sd-dir DIR` 缺省 `/tmp/mt_sim_sdcard`；与 `--nvs-dir` 一样是纯宿主目录，
删除即重置设备 SD 状态。

`--agent-port N`（默认 5002，仅绑 127.0.0.1；5001 留给真机基准）。协议为逐行
JSON-RPC：`{"id":1,"method":"sim.ping","params":{...}}` →
`{"id":1,"ok":true,"result":{...}}`。

方法：`sim.ping`、`sim.step {ms}`（33 的倍数，仅 CI）、`sim.wait_idle
{timeout_ms}`（帧哈希连续 2 步不变 + `lv_anim_count_running()==0`；暂停时返回顶层
`error`）、
`sim.screenshot {name,wait_idle}`、`sim.tree`（绝对坐标 + 多 part 计算样式 +
类型值 + 图像语义 ID）、`sim.touch {action,x,y}`、`sim.key {button,action}`
（press/release/click）、`sim.navigate {app}`（投 mailbox，不阻塞）、`sim.apps`、
`sim.set_time {epoch}`、`sim.set_power {voltage?,pct?,charging,vbus}`（voltage/pct 可各自省略；缺 pct 走电压回退显示）、
`sim.set_wifi {state}`（connected/disconnected）、`sim.set_imu {pitch,roll}`（度）、
`sim.set_weather {endpoint,status,body}`
（灌 sim_http 脚本表后 `request_refresh`，真解析链）、`sim.pause {enabled}`、
`sim.set_wifi_scan {records, request?, trigger?, wait_scan?, status?}`（灌 port
扫描记录；`trigger` 注入真实 SCAN_DONE，`wait_scan` 轮询 port 扫描窗口确保事件
落在扫描中，`request` 先经 connectivity_manager 发起一次扫描）、
`sim.sd {action: mount|umount|write|clear|list|svc, name?, seconds?}`（挂载/
卸载宿主目录卷、合成静音 WAV、清空、列目录、读 recorder 服务缓存索引与
generation）、`sim.nvs {action: get|set|erase, key, value?}`（真 nv_storage
字符串键）、`sim.connectivity`（connectivity_manager 与 wifi_service 状态、
扫描缓存快照）、`sim.switcher`、`sim.exit`。客户端：`python3 sim/tools/simctl.py <command>`。

`sim.ping` 返回 `network_ready`、`weather_state`、`weather_failure`、`active_app`、
`ci` 和 `frames`，可用于 Agent 判断当前会话，而不必解析页面文本。
`simctl` 无法连接时会快速失败并提示启动 `python3 sim/dev.py`；端口冲突时先停止
开发会话再运行 CI。真实联网请求失败时，优先检查 `network_ready`、天气状态和
`build/sim/dev_session.log`。

失败响应使用顶层 `error`，不带 `result`；成功响应使用 `result`。`sim.wait_idle`
暂停时返回 `error: "adapter paused"`；`sim.screenshot` 默认先等待稳定，暂停画面需
显式传 `wait_idle:false`。

## 控件树

`sim_agent_tree.c` 在锁内遍历 `lv_screen_active()`。节点字段包括：

| 字段 | 内容 |
| --- | --- |
| `type` | `lv_obj_class` 的名称（如 `lv_label`） |
| `coords` | 绝对屏幕坐标 `x,y,w,h` |
| `flags` | `visible` / `clickable` / `scrollable` |
| `state` | `pressed` / `checked` / `focused` / `disabled` |
| `styles` | `LV_PART_MAIN` 的颜色、透明度、圆角、padding、行高 |
| `children` | 子节点数组 |

label 提供 `text`；image 提供 `image_semantic_id`；slider/bar/arc 提供
`value`/`min`/`max`；switch/checkbox 提供 `checked`；dropdown/roller 提供
`selected`。场景的 `tree_assert.contains` 对节点字段做超集匹配。

锁纪律：tree/screenshot/hash 持 `esp_lv_adapter_lock` 只读/拷贝；step/navigate 不持锁；
touch/key 只写原子态。`pause` 返回前完成 worker/flush 栅栏；暂停期间 `wait_idle`
不推进 tick。CI 会话驻留直到 `sim.exit` 或信号。

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
daily 解析与渲染，并打开 forecast 子页）、`pm_lifecycle.json`（息屏/唤醒）和
`agent_contract.json`（参数错误、暂停和截图契约）。树断言为主门禁。

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

## PM 策略与已知环境特性

- **默认常亮**：`sim_runtime_boot` 在 startup_commit 后调用
  `app_manager_pm_set_timeout_ms(-1)` + `set_standby_delay_ms(-1)`（固件"永不"档
  真实路径）。原因：PM 用真实墙钟（不受 `--ci` tick 冻结影响），30s 息屏会把
  CI 会话拖进 mailbox 停排（同步 ui_call 永久阻塞）、触摸禁用、全黑帧。
- **息屏是要测的行为而不是环境噪声**：Agent `sim.pm {off_ms,standby_ms,get}`
  （公开 API 包装）+ 场景 `pm_lifecycle.json`：短超时→黑帧→power click 唤醒。
- **WSL mirrored 网络**：boot 完成前连接 agent 端口会得到 SYN 静默超时而非
  RST-refused（mirrored 的 localhost 共享路由特性，非 sim 缺陷）；dev.py 已有
  60s 就绪等待，手工调试时先 `ss -tln | grep 5002` 确认监听。
- 退出方式：窗口关闭、`sim.exit` 和 SIGTERM 均设置退出标志；`dev.py` 另带 PID
  kill 兜底。

## 线程与像素模型

- LVGL worker 独占 `lv_timer_handler`，主线程泵 SDL，Agent 线程只走锁纪律表；
- 唯一像素路径：flush_cb → `custom_draw_bitmap` → `esp_lcd_panel_draw_bitmap`
  （sim_bsp blit 进 368×448 RGB565 FB）→ 同步 `lv_display_flush_ready`；
- 宿主禁止 `lv_draw_sw_rgb565_swap`（真机 swap 仅服务 QSPI 面板端序）。

## 目录

`apps/` 入口；`bsp/` 模拟板；`runtime/` 装配；`ports/` shim 与服务 port；
`agent/` TCP 桥；`ci/` 检查与场景；`cmake/` 资源管线与链接脚本；`golden/` PNG
基线；`tools/` simctl/MCP；`stubs/` 平台桩与 CONFIG 镜像。
