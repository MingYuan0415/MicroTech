# MicroTech

MicroTech 是面向 Waveshare ESP32-S3 Touch AMOLED 1.8 的 ESP-IDF 固件工程，集成显示与触摸、RTC、电源管理，提供基于 LVGL 的内置应用、Wi-Fi 服务、BLE 扩展接口、持久化存储以及待机/唤醒流程。

## 工程结构

| 路径 | 职责 |
| --- | --- |
| `main/` | 固件入口、服务装配、运行时电源管理及文件系统挂载 |
| `layers/bsp/` | 开发板显示、触摸、I2C、RTC 和电源硬件抽象 |
| `layers/middleware/` | 日志、事件、存储、网络、时间及系统电源服务 |
| `layers/app_manager/` | 应用注册、生命周期、UI mailbox、主题和显示电源适配 |
| `layers/apps/` | Home、Menu、Settings、Setup 等内置 LVGL 应用 |
| `sim/` | 独立 CMake 宿主 LVGL 模拟器（不接入根 IDF 构建） |
| `tests/` | 跨层宿主测试与真机显示基准工具 |

四个 `layers/` 目录是独立 Git 子模块；`managed_components/` 由 ESP-IDF Component Manager 管理，均不应直接修改。

## 环境要求

- ESP-IDF 6.0.x，目标芯片 ESP32-S3（16 MB Flash、Octal PSRAM）
- CMake 3.16+、Ninja 和支持 C11/Pthreads 的宿主编译器（宿主测试与 `sim/` 需要）
- 宿主模拟器另需 `libsdl2-dev`、`libcurl4-openssl-dev`；**不要**安装发行版 `libfreetype-dev` 链接 sim（树内 FreeType 2.14.3）

首次检出后初始化子模块：

```sh
git submodule update --init --recursive
```

## 构建与烧录

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

配置改动落在 `sdkconfig.defaults`（`idf.py save-defconfig`），不手改 `sdkconfig`。修改缓存、DMA 预留或资源后运行 `idf.py size`，并在真机检查显示、触摸、待机和唤醒。

## 功能

- **天气**：menuconfig `MicroTech product integration` 中设置 `Weather server base URL` 与 `Weather device token`（不含 `Bearer ` 前缀）；任一为空时天气页显示“服务未配置”。获取、缓存、定位与会话刷新由后台 `weather_service` 负责，细节见 [`layers/middleware/README.md`](layers/middleware/README.md)。构建图片资源（SVG 栅格化与 RGB565A8 转换）前安装依赖：`python -m pip install -r requirements-weather-assets.txt`；图片源文件是 `layers/apps/*/assets/*.svg`（唯一入库形态，构建期栅格化），字体在 `layers/app_manager/app_theme/assets/`，由 main 聚合打包进 `res` 分区。
- **Device Link**：BLE 契约以 [`contracts/device_link`](contracts/device_link/) 的 `device-link/v1` freeze candidate 为规范源；实现与真机验证范围见 [`doc/device-link-implementation.md`](doc/device-link-implementation.md)。
- **恢复出厂**：持久化 reset marker 后重启，启动阶段幂等清除 Wi-Fi、引导状态、显示/待机偏好、水平仪校准、内部 `/data` 及 BLE bond/CCCD；全部收敛后才清除 marker 并恢复广告。外置 SD 卡及录音不受影响。
- **LVGL 宿主模拟器**：页面调试、JSON-RPC Agent 驱动与控件树回归，见 [`sim/README.md`](sim/README.md)。
- **显示压力基准**：仅真机执行，验收门槛、profile 与配置基线见 [`tests/display/README.md`](tests/display/README.md)。

## 开发约束

贡献前阅读 [AGENTS.md](AGENTS.md) 与[代码风格](doc/code-style.md)；Kconfig、运行时策略、板级事实与协议常量的归属规则见 [`doc/configuration-governance.md`](doc/configuration-governance.md)。不得通过修改 ESP-IDF、`managed_components/` 或 BSP 第三方库来规避工程自身问题。

## 声明

本项目仅作为个人兴趣爱好而开发，不提供任何形式的保证、维护承诺或技术支持。

## 许可证

本项目采用 [MIT License](LICENSE)。
