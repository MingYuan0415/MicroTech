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

`sdkconfig.defaults` 固定性能、PSRAM、LVGL 和 16 MB 分区基线。分区表提供双 OTA 应用槽、8 MB 只读 `res` 分区、LittleFS `data` 分区和 coredump 分区。修改缓存、DMA 内存预留或资源后，应运行 `idf.py size` 并在真机上检查显示、触摸、待机和唤醒。

## 宿主测试

主运行时测试：

```sh
cmake -S main/tests/host -B /tmp/mt-main -G Ninja
cmake --build /tmp/mt-main
ctest --test-dir /tmp/mt-main --output-on-failure
```

连接链路和跨层测试分别位于 [`tests/connectivity`](tests/connectivity/README.md) 与 [`tests/integration`](tests/integration/README.md)，使用相同的 CMake、构建和 CTest 流程。各套件可通过 `MAIN_HOST_SANITIZER`、`CONNECTIVITY_SANITIZER` 或 `CROSS_LAYER_SANITIZER` 选择 `address`、`thread` 或默认的 `none`。

宿主测试覆盖生命周期、并发和失败回滚，但不替代 ESP32-S3 上的驱动时序、射频、DMA、功耗及资源占用验证。

## 开发约束

贡献前阅读 [AGENTS.md](AGENTS.md) 和 [代码风格](doc/code-style.md)。代码以性能和低资源占用为优先，保持模块低耦合；硬件及并发边界保留必要防护。不得通过修改 ESP-IDF、`managed_components/` 或 BSP 第三方库来规避工程自身问题。

## 许可证

本项目采用 [MIT License](LICENSE)。
