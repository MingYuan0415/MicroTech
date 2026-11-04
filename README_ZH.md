[English](README.md) | **简体中文**

# MicroTech

MicroTech 是面向 Waveshare ESP32-S3 Touch AMOLED 1.8 以及第二版型 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 的 ESP-IDF 固件工程，集成显示与触摸、电源管理，提供基于 LVGL 的内置应用、Wi-Fi 服务、BLE 扩展接口和持久化存储。ESP32-S3 版型另提供 RTC 与 CPU 待机/唤醒；ESP32-P4 后端提供显示、触摸、按键输入、AXP2101 电源遥测、ES8311 音频和 SDMMC 存储，仅支持熄屏（无 CPU light sleep，且无 RTC 可唤醒 GPIO）。其 Wi-Fi 与 BLE 经板载 ESP32-C6 的 ESP-Hosted SDIO 链路工作，协处理器固件见 [`firmware/c6-cp/`](firmware/c6-cp/README_ZH.md)。

## 工程结构

| 路径 | 职责 |
| --- | --- |
| `main/` | 固件入口、服务装配、运行时电源管理及文件系统挂载 |
| `layers/bsp/` | 开发板显示、触摸、I2C、RTC 和电源硬件抽象 |
| `layers/middleware/` | 日志、事件、存储、网络、时间及系统电源服务 |
| `layers/app_manager/` | 应用注册、生命周期、UI mailbox、主题和显示电源适配 |
| `layers/apps/` | Home、Menu、Settings、Setup 等内置 LVGL 应用 |
| `products/` | 产品清单：绑定板卡、设计 target 与所选 App 集合 |
| `sim/` | 独立 CMake 宿主 LVGL 模拟器（不接入根 IDF 构建） |
| `tests/` | 跨层宿主测试与真机显示基准工具 |

四个 `layers/` 目录是独立 Git 子模块；`managed_components/` 由 ESP-IDF Component Manager 管理，均不应直接修改。

## 环境要求

- ESP-IDF 6.1.x；目标芯片 ESP32-S3（16 MB Flash、Octal PSRAM）或 ESP32-P4（16 MB Flash、32 MB HEX PSRAM）
- CMake 3.22+、Ninja 和支持 C11/Pthreads 的宿主编译器（宿主测试与 `sim/` 需要）
- 宿主模拟器另需 `libsdl2-dev`、`libcurl4-openssl-dev`；**不要**安装发行版 `libfreetype-dev` 链接 sim（树内 FreeType 2.14.3）

首次检出后初始化子模块：

```sh
git submodule update --init --recursive
```

## 构建与烧录

```sh
idf.py set-target esp32s3   # P4 版型使用 esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

target 自动选择其唯一已注册板卡及该板卡的默认产品（`s3_reference` 或 `p4_reference`）。如需其他组合，用 `-DMT_BOARD=<board-id>` 或 `-DMT_PRODUCT=<product-id>` 选择；详见 [`products/`](products/) 与 [`layers/bsp/README.md`](layers/bsp/README.md)。

P4 版型还需烧录 [`firmware/c6-cp/`](firmware/c6-cp/README_ZH.md) 中的 ESP32-C6 协处理器镜像，否则 P4 主机无法使用 Wi-Fi 与 BLE。

配置改动落在 `sdkconfig.defaults` 及其按 target 的伴随文件 `sdkconfig.defaults.esp32s3` / `sdkconfig.defaults.esp32p4`（`idf.py save-defconfig`），不手改 `sdkconfig`。修改缓存、DMA 预留或资源后运行 `idf.py size`，并在真机检查显示、触摸、熄屏，以及（仅 ESP32-S3）待机和唤醒。

## 宿主测试

按改动影响面运行根 `AGENTS.md` 中对应的 CMake/CTest 套件。配置、资源、显示工具和模拟器 Python 测试使用统一入口：

```sh
python3 tests/run_python_tests.py configuration resources
python3 tests/run_python_tests.py display-tools sim-tools
python3 tests/run_python_tests.py contracts
```

## 功能

- **无线**：S3 版型使用芯片自带的 Wi-Fi/BLE 控制器；P4 版型作为 ESP-Hosted 主机经 SDIO slot 1 工作（hosted 版本以 `main/idf_component.yml` 的 pin 为准），`esp_wifi_*` 控制路径走 `esp_wifi_remote`，NimBLE 经 `esp_hosted_bt_host_stack` 的 hosted HCI 通道访问 C6 的 BT controller。C6 镜像位于 `firmware/c6-cp/` 独立工程。hosted 链路启动失败时 P4 保留显示与本地服务，降级为离线 UI。
- **天气**：menuconfig `MicroTech product integration` 中设置 `Weather server base URL` 与 `Weather device token`（不含 `Bearer ` 前缀）；任一为空时天气页显示“服务未配置”。获取、缓存、定位与会话刷新由后台 `weather_service` 负责，细节见 [`layers/middleware/README_ZH.md`](layers/middleware/README_ZH.md)。构建图片资源（SVG 栅格化与 RGB565A8 转换）前安装依赖：`python -m pip install -r requirements-weather-assets.txt`；图片源文件是 `layers/apps/*/assets/*.svg`（唯一入库形态，构建期栅格化），字体在 `layers/app_manager/app_theme/assets/`，由 main 聚合打包进 `res` 分区。
- **Device Link**：BLE 契约以 [`contracts/device_link`](contracts/device_link/) 的 `device-link/v1` freeze candidate 为规范源；协议语义由该子模块校验，实现边界见对应 middleware README。
- **恢复出厂**：持久化 reset marker 后重启，启动阶段幂等清除 Wi-Fi、引导状态、显示/待机偏好、水平仪校准、内部 `/data` 及 BLE bond/CCCD；全部收敛后才清除 marker 并恢复广告。外置 SD 卡及录音不受影响。
- **文件与录音存储**：普通挂载不格式化；文件服务初始化失败时释放部分分配并降级启动，runtime 只清理成功取得的服务所有权，停止与重启不依赖失败实例的反初始化。浏览、显式维护和录音使用权契约见各 middleware 组件文档。
- **LVGL 宿主模拟器**：页面调试、JSON-RPC Agent 驱动与控件树回归，见 [`sim/README_ZH.md`](sim/README_ZH.md)。
- **显示压力基准**：仅真机执行，验收门槛、profile 与配置基线见 [`tests/display/README.md`](tests/display/README.md)。

## 开发约束

贡献前阅读 [AGENTS.md](AGENTS.md) 与[代码风格](doc/code-style.md)；Kconfig、运行时策略、板级事实与协议常量的归属规则见 [`doc/configuration-governance.md`](doc/configuration-governance.md)。不得通过修改 ESP-IDF、`managed_components/` 或 BSP 第三方库来规避工程自身问题。

## 声明

本项目仅作为个人兴趣爱好而开发，不提供任何形式的保证、维护承诺或技术支持。

## 许可证

本项目采用 [MIT License](LICENSE)。
