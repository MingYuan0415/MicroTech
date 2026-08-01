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
| `main/res_fs/` | 构建时写入 `res` 分区的字体及其他只读资源 |
| `tests/` | 连接链路和跨层宿主集成测试 |

四个 `layers/` 目录是独立 Git 子模块。`managed_components/` 由 ESP-IDF Component Manager 管理，不应直接修改。

## 环境要求

- ESP-IDF 6.0.x，目标芯片为 ESP32-S3
- 16 MB Flash、Octal PSRAM 的目标板配置
- CMake 3.16+、Ninja 和支持 C11/Pthreads 的宿主编译器（仅宿主测试需要）

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

`sdkconfig.defaults` 固定性能、PSRAM、RGB565 LVGL 和 16 MB 分区基线，并为
DMA/internal 分配保留 128 KiB 内部内存。分区表提供双 OTA 应用槽、8 MB 只读 `res` 分区、
LittleFS `data` 分区和 coredump 分区。修改缓存、DMA 内存预留或资源后，应运行
`idf.py size` 并在真机上检查显示、触摸、待机和唤醒。

## 宿主测试

主运行时测试：

```sh
cmake -S main/tests/host -B /tmp/mt-main -G Ninja
cmake --build /tmp/mt-main
ctest --test-dir /tmp/mt-main --output-on-failure
```

连接链路和跨层测试分别位于 [`tests/connectivity`](tests/connectivity/README.md) 与 [`tests/integration`](tests/integration/README.md)，使用相同的 CMake、构建和 CTest 流程。各套件可通过 `MAIN_HOST_SANITIZER`、`CONNECTIVITY_SANITIZER` 或 `CROSS_LAYER_SANITIZER` 选择 `address`、`thread` 或默认的 `none`。

宿主测试覆盖生命周期、并发和失败回滚，但不替代 ESP32-S3 上的驱动时序、射频、DMA、功耗及资源占用验证。

## 显示压力基准

App Manager 的显示诊断和自动压力基准默认关闭，仅在开发固件中启用：menuconfig
同时打开 `SYSTEM_PM_DEVELOPMENT_MODE`、`APP_MANAGER_DISPLAY_DIAGNOSTICS` 和
`MAIN_DISPLAY_BENCHMARK`（生产配置必须保持关闭）。campaign 的模式、时长、负载和
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

默认显示传输配置为 40 MHz QSPI、60 行双 PSRAM draw buffer、10 行 SPI DMA chunk 和
`RGB565`；默认启用的全分辨率快照转场
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
