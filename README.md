# MicroTech

MicroTech 是面向 Waveshare ESP32-S3 Touch AMOLED 1.8 的 ESP-IDF 固件工程。工程集成显示与触摸、RTC 和电源管理，提供基于 LVGL 的内置应用、Wi-Fi 服务、BLE 扩展占位接口、持久化存储以及待机/唤醒流程。

## 工程结构

| 路径 | 职责 |
| --- | --- |
| `main/` | 固件入口、服务装配、运行时电源管理及文件系统挂载 |
| `layers/bsp/` | 开发板显示、触摸、I2C、RTC 和电源硬件抽象 |
| `layers/middleware/` | 日志、事件、存储、网络、时间及系统电源服务 |
| `layers/app_manager/` | 应用注册、生命周期、UI mailbox、主题和显示电源适配 |
| `layers/apps/` | Home、Menu、Settings、Setup 等内置 LVGL 应用 |
| `layers/apps/*/assets/`、`layers/app_manager/app_theme/assets/` | App 与全局主题的资源源文件；由 main 聚合写入 `res` 分区 |
| `sim/` | 独立 CMake 宿主 LVGL 模拟器（不接入根 IDF 构建）；Agent `127.0.0.1:5002` |
| `tests/` | 连接链路和跨层宿主集成测试 |

四个 `layers/` 目录是独立 Git 子模块。`managed_components/` 由 ESP-IDF Component Manager 管理，不应直接修改。

## 环境要求

- ESP-IDF 6.0.x，目标芯片为 ESP32-S3
- 16 MB Flash、Octal PSRAM 的目标板配置
- CMake 3.16+、Ninja 和支持 C11/Pthreads 的宿主编译器（宿主测试与 `sim/` 需要）
- 宿主模拟器另需 `libsdl2-dev`、`libcurl4-openssl-dev`；**不要**安装发行版
  `libfreetype-dev` 来链接 sim（树内 FreeType 2.14.3，与设备一致）

首次检出后初始化子模块：

```sh
git submodule update --init --recursive
```

## 构建与烧录

在已加载 ESP-IDF 环境的仓库根目录执行：

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`sdkconfig.defaults` 固定性能、PSRAM、RGB565 LVGL 和 16 MB 分区基线，并采用
`LV_OS_NONE` 单 draw unit、32 KiB PSRAM LVGL worker。LVGL 固定到 CPU1，项目任务和
ESP-IDF 可配置的主要系统任务固定到 CPU0；NimBLE 动态内存使用 PSRAM，同时为
DMA/internal 分配保留 128 KiB 内部内存。分区表提供双 OTA 应用槽、8 MB 只读 `res` 分区、
LittleFS `data` 分区和 coredump 分区。修改缓存、DMA 内存预留或资源后，应运行
`idf.py size` 并在真机上检查显示、触摸、待机和唤醒。

## 天气功能

天气由后台 `weather_service` 获取和缓存，Weather App 只读取引用计数的不可变快照。
服务在每个新的 IPv4 联网会话定位一次，随后依次更新实时天气、预警、24 小时和 7 日
预报；网络 I/O、JSON、重试和 `/data` A/B 缓存均不在 LVGL worker 中执行。大型快照、
HTTP/JSON 缓冲、缓存编解码缓冲和 weather worker stack 优先使用 PSRAM。

通过项目配置填写 mt-server HTTPS Origin 与设备令牌：

```sh
idf.py menuconfig
```

进入 `MicroTech product integration`，设置 `Weather server base URL` 和
`Weather device token`。URL 填写 mt-server HTTPS Origin，令牌不包含 `Bearer ` 前缀。
真实值只保存在已忽略的 `sdkconfig` 中；提交的 `sdkconfig.defaults` 始终使用
`weather.example.com` 和示例令牌。任一配置为空时天气页显示“服务未配置”。定位由同一
mt-server 的 `GET /api/v1/location` 提供：部署须启用 GeoLite2 IP 推断
（`MT_GEOIP_DB`），反向代理场景按需配置可信客户端 IP 头与网段；设备不上传坐标，
仅保留服务端返回的城市级显示字段，IP 推断结果代表公网出口附近的粗略天气区域。服务端
按 0.1° 网格返回不透明的 `location_key`（16 位小写十六进制，同一网格恒定、不暴露
坐标），固件以其作为位置作用域身份：key 变化即清空旧数据并按“实时天气优先”全量刷新，
每次新 IPv4 会话也会重新定位并刷新，避免跨网格的陈旧或混合快照。服务端本地化成功时
还会下发可选的 `district` 区县名（纯显示、不参与身份判定），Weather 首页标题按
“市·区”组合展示。

天气图片采用 1 个 64x64 应用图标，以及 20 类条件各自的 112x112 与 40x40 透明 PNG，
源文件归属 `layers/apps/weather_app/assets/`。全局字体归属
`layers/app_manager/app_theme/assets/font.ttf`；各 App 图标放在对应 App 的 `assets/`
目录。显式 manifest 在配置阶段检查文件、尺寸、输出名和语义 ID，main 只聚合这些清单，
不再维护资源文件列表。首次构建
图片资源前，在当前 ESP-IDF Python 环境安装固定转换依赖：

```sh
python -m pip install -r requirements-weather-assets.txt
```

构建期使用锁定的 LVGL 9.5 `scripts/LVGLImage.py` 转换为 `RGB565A8` BIN，再由
`esp_mmap_assets` 打包进 `res` 分区；转换过程不下载远程脚本。运行时通过 mmap Flash
地址直接构造持久 `lv_image_dsc_t`，不解码图片、不写资源文件路径。未提供整套图片时
保留字体资源和 LVGL symbol 回退，FreeType 继续使用 `F:font.ttf`。

## 宿主测试

主运行时测试：

```sh
cmake -S main/tests/host -B /tmp/mt-main -G Ninja
cmake --build /tmp/mt-main
ctest --test-dir /tmp/mt-main --output-on-failure
```

连接链路和跨层测试分别位于 [`tests/connectivity`](tests/connectivity/README.md) 与 [`tests/integration`](tests/integration/README.md)，使用相同的 CMake、构建和 CTest 流程。各套件可通过 `MAIN_HOST_SANITIZER`、`CONNECTIVITY_SANITIZER` 或 `CROSS_LAYER_SANITIZER` 选择 `address`、`thread` 或默认的 `none`。

宿主测试覆盖生命周期、并发和失败回滚，但不替代 ESP32-S3 上的驱动时序、射频、DMA、功耗及资源占用验证。

Device Link 当前以 [`contracts/device_link`](contracts/device_link/) 中的
`device-link/v1` freeze candidate 为规范源，采用 LE Secure Connections Numeric
Comparison、MITM、单 bond、ATT MTU 498 和可查询确认的异步操作记录。契约 checker 与
金标通过只说明规范数据自洽；配对、bond 替换、MTU/DLE、断连恢复及空中互操作仍需真机验证。

## LVGL 宿主模拟器

`sim/` 是独立 CMake 工程，在 Linux/WSL2 上用仓库内 LVGL 9.5 渲染 `layers/` 真实 UI，
供页面调试、JSON-RPC Agent 驱动和控件树回归。不定义 `HOST_TEST`，不修改四个
`layers/` 子模块。需已有 `managed_components/` 与一次 `idf.py build` 生成的
`build/config/sdkconfig.h`。

```sh
python3 sim/dev.py
# 或
cmake -S sim -B build/sim -G Ninja && cmake --build build/sim
sim/ci/run_ci.sh build/sim
```

默认无头路径使用 `SDL_VIDEODRIVER=dummy`；Agent 仅绑 `127.0.0.1:5002`（避开真机
基准端口 5001）。主门禁为 `sim/ci/scenarios/` 树断言；PNG 金样默认关闭。模拟器不覆盖
面板时序、DMA 与功耗，不能替代上板验证。操作见 [`sim/README.md`](sim/README.md)，
详细说明见 [`doc/lvgl-simulator.md`](doc/lvgl-simulator.md)。

## 显示压力基准

App Manager 的显示诊断和自动压力基准默认关闭，仅在开发固件中启用：menuconfig
同时打开 `APP_MANAGER_DISPLAY_DIAGNOSTICS` 和 `MAIN_DISPLAY_BENCHMARK`（生产配置必须
保持关闭）。benchmark 运行期间由运行时抑制待机；campaign 的模式、时长、负载和
TCP 参数来自 `tests/display/profiles/*.json` 生成的类型化头文件；构建工具为每个隔离
build 目录传入 `DISPLAY_BENCHMARK_PROFILE_DIR`。`STRESS` 运行生产 fade/push 转场与
audio/TCP 负载；`CHARACTERIZATION` 依次测试五种转场的 display-only 和指定负载窗口。

主机端启动 TCP echo 服务（设备默认连接 `192.168.0.205:5001`）：

```sh
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001
```

`wslinfo --networking-mode` 返回 `mirrored` 时，需在管理员 PowerShell 为 WSL 的
Hyper-V 防火墙放行 TCP 5001，规则示例见
[`tests/display/README.md`](tests/display/README.md)。

Waveshare 板型固定使用 368x448、40 MHz QSPI、10 行 SPI DMA chunk、queue depth 2、
`RGB565`，关闭 Direct DMA 和 TE；LVGL 默认使用 60 行双 PSRAM draw buffer。默认启用的全分辨率快照转场
（`CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION`）用于降低动画期间的软件
绘制负担，失败时自动回退原生 Screen 动画。验收门槛、配置基线及 buffer/时钟表征
历史详见 [`tests/display/README.md`](tests/display/README.md)。

Kconfig、运行时产品策略、板级事实、协议常量和测试 profile 的归属规则见
[`doc/configuration-governance.md`](doc/configuration-governance.md)。

## 开发约束

贡献前阅读 [AGENTS.md](AGENTS.md) 和 [代码风格](doc/code-style.md)。代码以性能和低资源占用为优先，保持模块低耦合；硬件及并发边界保留必要防护。不得通过修改 ESP-IDF、`managed_components/` 或 BSP 第三方库来规避工程自身问题。

## 声明

本项目仅作为个人兴趣爱好而开发，不提供任何形式的保证、维护承诺或技术支持。

## 许可证

本项目采用 [MIT License](LICENSE)。
