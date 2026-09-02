# LVGL 宿主模拟器

`sim/` 是独立 CMake 工程，在 Linux / WSL2 上以仓库内 LVGL 9.5 渲染
`layers/` 的真实 App Manager 与内置应用。进程名为 `microtech_sim`，提供窗口或
无头帧缓冲、TCP Agent（截图、控件树、注入）以及声明式 CI 场景。不接入根
ESP-IDF 构建，不定义 `HOST_TEST`（mailbox 仍在 LVGL worker 的 `lv_timer` 上排空）。

操作入口见 [`sim/README.md`](../sim/README.md)。

## 构建与运行

### 依赖

```sh
sudo apt install build-essential cmake ninja-build python3 python3-pip \
    libsdl2-dev libcurl4-openssl-dev
pip3 install -r requirements-weather-assets.txt
```

FreeType 2.14.3 与 libpng 编译自 `managed_components/espressif__freetype`、
`espressif__libpng`，不链接发行版 `libfreetype`。LVGL、cJSON 同样取自
`managed_components/`。另需：

- 已填充的 `managed_components/lvgl__lvgl` 等组件树；
- 一次 `idf.py build` 生成的 `build/config/sdkconfig.h`（sim 的 CONFIG 镜像来源）。

缺失时 CMake `FATAL_ERROR`，不 FetchContent。

### 命令

```sh
python3 sim/dev.py                 # 数字菜单（推荐）

cmake -S sim -B build/sim -G Ninja
cmake --build build/sim

# 无头 / CI
SDL_VIDEODRIVER=dummy ./build/sim/microtech_sim --headless --ci \
    --res-dir build/sim/sim_res_fs --agent-port 5002

# 窗口预览（需 DISPLAY / WSLg）
./build/sim/microtech_sim --res-dir build/sim/sim_res_fs --agent-port 5002

sim/ci/run_ci.sh build/sim
```

`sim/CMakeLists.txt` 已强制 `LV_BUILD_CONF_PATH`、DEMOS/EXAMPLES/THORVG=OFF、
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`。C11、`-Wall -Wextra -Werror -Wpedantic`。
可选 `-DSIM_SANITIZER=address|thread`；WSL2 上 TSan 使用 `setarch $(uname -m) -R`。

### CLI

| 选项 | 作用 |
| --- | --- |
| `--headless` | `SDL_VIDEODRIVER=dummy`，像素仍写入 368×448 帧缓冲 |
| `--ci` | mailbox 握手后切显式 tick；HTTP 仅走脚本表；禁用 SNTP |
| `--res-dir DIR` | mmap 资源目录（默认 `sim_res_fs`，构建产出在 `build/sim/sim_res_fs`） |
| `--nvs-dir DIR` | NVS 文件目录 |
| `--out-dir DIR` | Agent 截图目录 |
| `--agent-port N` | JSON-RPC 端口，默认 `5002`，仅绑 `127.0.0.1` |
| `--window-scale N` | 窗口整数放大 |
| `--frames N` | 跑 N 帧后退出 |
| `--navigate APP` | 启动后经 mailbox 打开指定 app |
| `--screenshot NAME` | 退出前写一张 PNG |
| `--brightness N` | 启动后设置亮度 |
| `--key boot\|power` | 注入物理键（可重复） |
| `--swipe ...` / `--ci-step N` | 脚本化手势 / 额外 CI 步进 |

无 `--agent-port` 时仍监听 `127.0.0.1:5002`。`--ci` 下 Agent 启动失败则进程以失败码退出。

### `sim/dev.py`

零参数菜单：窗口 1:1、直达某页、无头驻留、截图、导出控件树、跑 CI、更新 PNG 金样、
停会话、清空 `build/sim/dev_nvs`。会话端口固定 5002，产物在 `build/sim/`。
无 `DISPLAY` 时「运行」改为无头驻留。

## 目录

```
sim/
  CMakeLists.txt
  lv_conf.h                 # 手写 LVGL profile，与设备镜像核对
  README.md
  dev.py
  apps/main_sim.c           # 入口、CLI、SDL 主循环
  bsp/sim_bsp.c             # 368×448 RGB565 帧缓冲与 bsp_hal 三契约
  runtime/sim_runtime.c     # 服务装配与 app_manager 启动
  ports/                    # LVGL shim、NVS/HTTP/时间/FreeRTOS、PNG、假硬件 ops
  ports/fakes/              # Wi-Fi port、audio/sd/recorder、device_link
  agent/                    # TCP JSON-RPC 与控件树 dump
  cmake/                    # 资源管线、sdkconfig 镜像、内置 app 链接脚本
  ci/                       # check_lv_conf.py、run_ci.sh、scenarios/、fixtures/
  tools/simctl.py           # Agent 客户端
  tools/run_scenarios.py    # 声明式场景 runner
  stubs/include/            # 平台头（esp_err、esp_lcd、nvs、FreeRTOS…）
```

生成物在 `build/sim/`：`microtech_sim`、`sim_res_fs/`、`gen_inc/sdkconfig.h`、
`compile_commands.json`。不入库。

## 构成

```
主线程 SDL ──► 原子指针/按键 ──► LVGL indev
     │
     ├─ sim_runtime_boot() 装配 app_manager + 中间件
     └─ 可选窗口：只读帧缓冲呈现

LVGL worker（任务名 "lvgl"）
     lock → lv_timer_handler（含 mailbox 5 ms timer）→ unlock

Agent 线程
     accept 127.0.0.1:5002 → 一行一条 JSON-RPC
```

可执行文件直接编译：

- `layers/app_manager/app_core` 全量源 + `app_theme`；
- 9 个内置 app 与 `app_ui` / `app_weather_ui`（源文件列入可执行文件，配合
  `cmake/app_builtin_apps.ld` 保住 `.app_manager_apps`）；
- middleware：`mt_log`、`event_bus`、`nv_storage`、`timer_service`、
  `power_service`、`imu_service`、`time_service`、`chore_service`、
  `onboarding_service`、`factory_reset_service`、`weather_service`、
  `connectivity_manager`、`wifi_service`（业务源）及
  `device_link_confirmation`；
- `main/app_product_config.c`。

sim 自有：`esp_lv_adapter` shim、模拟板、FreeRTOS pthread port、NVS 文件后端、
HTTP/时间 port、PNG 编码。不编译 `main/app_runtime.c`；装配由 `sim_runtime.c`
完成。内置 app 通过链接段发现，启动时 `app_manager_builtin_discover()` 必须
得到正数。

## 显示

分辨率 368×448，RGB565。`bsp_display_port_t` 中 `te.enabled = false`。draw buffer
高度取自 `display_config.profile.buffer_height`（生产镜像为 60 行 PARTIAL 双缓冲）。

像素只经一条路径写入 sim 帧缓冲：

```
flush_cb
  → 必要时把 stride 压成 packed RGB565
  → custom_draw_bitmap（app_manager 安装的回调）
      → esp_lcd_panel_draw_bitmap（sim_bsp 矩形 blit）
  → lv_display_flush_ready()     # 宿主无 DMA ISR，同步完成
```

不调用 `lv_draw_sw_rgb565_swap`（宿主帧缓冲即原生 RGB565）。Agent 截图拷贝该 FB。
SDL 窗口只读呈现，不再二次 blit。`--headless` 使用 dummy 驱动，截图与树仍可用。

字体：`LV_USE_FREETYPE=1`、`LV_FREETYPE_USE_LVGL_PORT=1`。shim 以盘符 `F` 注册
`lv_fs_drv_t`，open/read 走 mmap 资源；`lv_freetype_font_create("F:font.ttf", …)`。
`LV_USE_FS_POSIX=0`，避免抢同一盘符。CMake 从 freetype 剔除 `ftsystem.c`，改用
LVGL 的 `lv_ftsystem.c`。`lv_init()` 已初始化 FreeType，shim 不再重复 init。

触摸：`LV_INDEV_TYPE_POINTER`。坐标来自主线程 SDL 鼠标或 `sim.touch`，写入原子
状态，worker 持锁时 read。窗口坐标按 `--window-scale` 映射到 368×448。

物理键：窗口下 `F1`→BOOT、`F2`→电源、`Esc` 退出；Agent `sim.key` 走同一
`sim_bsp_key` 边沿桥，接到 `app_manager_input_ops_t`。

## 运行时装配

`sim_runtime_boot()` 顺序：

1. `sim_bsp_init()`，能力面 `DISPLAY|TOUCH|INPUT`；
2. `event_bus_init`、`nv_storage_init`、factory_reset、onboarding；
3. `time_service` / `timer_service` / `chore_service`；
4. 注册 `sim_power_ops` / `sim_imu_ops` 后 `power_service_init`、`imu_service_init`
   （level 页读 IMU 快照，不依赖 `BSP_CAPABILITY_IMU`）；
5. 构造 `app_manager_config_t`（显示端口、屏幕 ops、输入 ops、空 standby ops、
   `APP_THEME_FONT_SIZES`、`res_fs_letter='F'`、资源绑定表），`app_manager_init`；
6. `app_manager_get_ui_dispatch_fn` → `event_bus_register_ui_dispatch`；
7. `weather_service_init`、`network_runtime_init`、`connectivity_manager_init`；
8. 发现内置 app；onboarding 未完成则 `RUN setup`，否则 `RUN home`；
9. `app_manager_display_commit_initial`、`app_manager_startup_commit`；
10. `app_manager_pm_set_timeout_ms(-1)` 与 `set_standby_delay_ms(-1)`（常亮）。

天气缓存目录为 `$SIM_DATA_DIR`，缺省 `sim_data`。PM 用 `esp_timer_get_time`
墙钟，不受 `--ci` tick 冻结影响；场景经 `sim.pm` 临时打开超时。

`sim.pm` 的 `pm_state`：`0` ACTIVE、`1` DIM、`2` SCREEN_OFF、`3` STANDBY_PREPARING、
`4` STANDBY。

## 线程与时间

| 线程 | 职责 |
| --- | --- |
| 主线程 | `SDL_Init`、装配、泵 SDL、呈现窗口；`SDL_*` 仅此线程 |
| `"lvgl"` worker | 独占 `lv_timer_handler`；递归锁覆盖同线程重入 |
| 中间件 worker | time/weather/imu/power/wifi 等在 `*_init` 后运行 |
| Agent | accept + 每连接一线程；RPC 由进程内互斥串行 |

启动阶段 tick 读 `CLOCK_MONOTONIC`，worker 自由循环，以便 5 ms mailbox timer
完成握手。`--ci` 仅在 `sim_runtime_boot()` 返回后调用 `sim_lv_ci_mode_set(true)`：
tick 改为显式计数器，worker 阻塞等 `sim.step`。`sim.step` 的 `ms` 必须是 33 的倍数。

锁：

- `sim.tree` / 截图拷贝：持 `esp_lv_adapter_lock` 只读，不得等待 worker；
- `sim.step` / `sim.wait_idle` / `sim.navigate`：不持锁；
- `sim.touch` / `sim.key`：只写原子态。

`sim.wait_idle`：默认超时 5 s。CI 下每次内部 `step 33` 两次，比较帧缓冲哈希，且
`lv_anim_count_running()==0`、连续两轮哈希相同才成功。

## 服务端口

| 路径 | 行为 |
| --- | --- |
| `sim_nvs.c` | 文件后端；同命名空间共享 store；全局锁；`commit` 为临时文件 + rename |
| `sim_http.c` | `esp_http_client` 子集 → libcurl；可注册 URL 子串脚本。`--ci` 下
  `script_only`：未命中脚本则失败。查找时拷贝 body，避免 UAF |
| `sim_time.c` | 可覆盖 epoch；`--ci` 在 boot 后 `sim_time_set_sntp_enabled(false)` |
| `sim_backends.c` | `sim.set_power` / `sim.set_imu` 写入 power/imu ops |
| `sim_wifi_port.c` | wifi_service 的宿主 port |
| `sim_fakes_services.c` | audio / sd 等宿主实现，供对应 app 链接 |
| `sim_fakes_recorder.c` / `sim_fakes_device_link.c` | recorder、device_link 宿主实现 |
| `sim_freertos.c` | pthread 任务/队列/信号量/event group/`esp_timer`；接受 core 0/1/`-1`，忽略亲和 |

`sim.set_wifi` 当前调用 `weather_service_set_network_ready`（`connected` /
其它），供天气网络门控。`sim.set_weather` 按 endpoint 写入脚本表
（`location` → `/api/v1/location`，其余 → `/api/v1/weather/<endpoint>`）并
`weather_service_request_refresh()`，走真实解析链。

## Agent

传输：TCP，`127.0.0.1`，逐行 JSON。请求
`{"id":1,"method":"sim.ping","params":{}}`，应答
`{"id":1,"ok":true,"result":{...}}`。非法 JSON 回 `{"ok":false,"error":"invalid json"}`。
应答用 `write(2)` 写套接字（不在 `fdopen(..., "r+")` 的 `FILE*` 上 `fgets` 后再
`fputs`）。客户端 `python3 sim/tools/simctl.py [--host] [--port] <cmd>`。

| 方法 | params | 说明 |
| --- | --- | --- |
| `sim.ping` | — | `version`（`sim-m6`）、宽高、`ci`、`frames`；非 CI 另给 `active_app` |
| `sim.step` | `{ms}` | 仅 `--ci`；默认 33 |
| `sim.wait_idle` | `{timeout_ms}` | 见上；返回 `idle`/`hash`/`steps` |
| `sim.screenshot` | `{name, wait_idle?}` | 默认先 wait_idle；`name` 不得含 `/` `\\` `..`；PNG 写入 `--out-dir` |
| `sim.tree` | — | 活动屏幕控件树（持锁 dump） |
| `sim.touch` | `{action, x, y}` | `down`/`move` 按下，`up` 抬起 |
| `sim.key` | `{button, action}` | `boot`/`power`；`press`/`release`/`click` |
| `sim.navigate` | `{app, page?}` | 无 `page` 为 `NAV_OP_RUN`；有 `page` 为 `NAV_OP_OPEN_PAGE`；投 mailbox |
| `sim.apps` | — | 注册表 `app_id`/`name`/`root_page`；非 CI 另给 `active` |
| `sim.set_time` | `{epoch}` | 注入时钟 |
| `sim.set_power` | `{voltage, pct, charging?, vbus?}` | mV、百分比 |
| `sim.set_imu` | `{pitch, roll}` | 度，内部转 centidegree |
| `sim.set_wifi` | `{state}` | `connected` 或其它 |
| `sim.set_weather` | `{endpoint, body, status?}` | 见上节 |
| `sim.pause` | `{enabled}` | `esp_lv_adapter_pause(-1)` / `resume` |
| `sim.pm` | `{off_ms?, standby_ms?, get?}` | 读写超时；`get` 返回 `pm_state` |
| `sim.exit` | — | 置退出标志 |

`simctl` 覆盖上述方法（`wait` → `sim.wait_idle`，`pause on\|off`）。

## 控件树

`sim_agent_tree.c` 在锁内遍历 `lv_screen_active()`。节点字段：

| 字段 | 内容 |
| --- | --- |
| `type` | `lv_obj_class` 的 `name`（`lv_label` 等） |
| `coords` | 绝对屏幕坐标 `x,y,w,h` |
| `flags` | `visible` / `clickable` / `scrollable` |
| `state` | `pressed` / `checked` / `focused` / `disabled` |
| `styles` | `LV_PART_MAIN`：`bg_color`/`text_color`（`#rrggbb`）、`bg_opa`、`radius`、padding、`line_height` |
| `children` | 子节点数组 |

类型值：label `text`；image `image_semantic_id`（绑定表反查，否则 `unknown`）；
slider/bar/arc `value`/`min`/`max`；switch/checkbox `checked`；dropdown/roller
`selected`。

CI 主门禁为 `tree_assert.contains`：树上任一节点字段超集匹配即通过。有意改 UI
文案或布局时，同一提交更新 `sim/ci/scenarios/`。

## CI

```sh
sim/ci/run_ci.sh [build-dir] [--update]
```

流程：端口 5002 预检 → cmake 构建 → `check_lv_conf.py --mirror $BUILD/gen_inc/sdkconfig.h`
→ 若存在设备暂存则 `diff -rq` `sim_res_fs` 与 `build/esp-idf/main/app_res_fs` →
每个 `sim/ci/scenarios/*.json` 独立进程、独立 NVS → `run_scenarios.py`。

默认只跑树断言。`SIM_PNG_GOLDEN=1` 或 `--update` 才对 `sim/golden/` 做 PNG
sha256 比对 / 生成。截图命令在场景里始终会写 `--out-dir`。

场景：

| 文件 | 覆盖 |
| --- | --- |
| `all_apps.json` | 九个 app 主页面文本断言与截图 |
| `smoke_pages.json` | 导航、左滑手势、clock/weather/level/settings |
| `weather_flow.json` | fixture 灌入 location/current/hourly/daily/alerts，当前页与 `forecast` 子页 |
| `pm_lifecycle.json` | 短超时 → `pm_state==2` 黑帧 → 电源键唤醒 → `pm_state==0` |

场景命令：`navigate`、`step`、`wait_idle`、`sleep_ms`、`touch`、`key`、`pm`、
`set_time`/`set_power`/`set_imu`/`set_wifi`/`set_weather`、`screenshot`、
`tree_assert`。fixture 相对路径相对仓库根。

`--ci` 会话：HTTP 仅脚本、SNTP 关闭、PM 默认永不息屏；weather/imu/power worker
保持运行，以便 `sim.set_*` 走真实发布链。

## 配置与资源

**sdkconfig**：`cmake/gen_sdkconfig.py` 在 configure 时从
`build/config/sdkconfig.h` 生成 `build/sim/gen_inc/sdkconfig.h` 并 `-include`。
`CMAKE_CONFIGURE_DEPENDS` 挂了设备 `sdkconfig.h` / `sdkconfig` / `sdkconfig.defaults`。

**lv_conf.h**：手写，与镜像及 `sdkconfig.defaults` 对齐。`check_lv_conf.py` 核对
`CONFIG_LV_*`，并强制 RGB565、libc allocator/string/sprintf、style cache、
`LV_USE_CANVAS=1`、`LV_USE_SNAPSHOT=1`、`LV_USE_IMAGE=1`、
`LV_FREETYPE_USE_LVGL_PORT=1`、`LV_USE_FS_POSIX=0`。须在 CMake configure 之后运行。

当前关键值：`LV_COLOR_DEPTH=16`、`LV_USE_OS=LV_OS_NONE`、`LV_DEF_REFR_PERIOD=15`、
`LV_DRAW_SW_DRAW_UNIT_CNT=1`、`LV_USE_FLOAT=1`、`LV_USE_QRCODE=1`、
`LV_FONT_DEFAULT=&lv_font_montserrat_18`。桌面 LVGL 定义 `LV_KCONFIG_IGNORE`，
故 `LV_USE_STDLIB_*=LV_STDLIB_CLIB` 写在 `lv_conf.h` 中。

**资源**：`cmake/resources.cmake` 读取
`layers/apps/resource_manifest.cmake` 与 `app_theme/resource_manifest.cmake`，
PNG 经仓库内 `lvgl__lvgl/scripts/LVGLImage.py` 转为
`RGB565A8 --ofmt BIN --compress NONE`，staged 到 `build/sim/sim_res_fs/`。
`gen_res_meta.py` 生成文件数与 mmap 校验和。`sim_mmap_assets.c` 把 staged 文件读入
RAM 常驻，供 `app_manager_get_image()` 做 RGB565A8 头校验。排序键与设备
`main/CMakeLists.txt` 相同（`扩展名|basename`）。
