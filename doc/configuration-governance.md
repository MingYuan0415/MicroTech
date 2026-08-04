# 配置治理

本文定义 MicroTech 固件参数的所有权。新增参数前必须先判断它属于编译裁剪、运行时产品
策略、板级事实、协议常量还是测试 profile，不能仅因为参数可变就加入 Kconfig。

## 分类规则

| 分类 | 适用条件 | 所有者与载体 |
| --- | --- | --- |
| Kconfig | 改变功能是否编译、调试代码是否存在，或改变静态 RAM/栈/队列预算 | 对应组件 `Kconfig`；产品 gate 放在 `main/Kconfig.projbuild` |
| 运行时产品策略 | SKU、用户或部署可选择，且不改变链接依赖或静态对象尺寸 | `main/app_product_config.c` 中的 `app_product_config_t` |
| 板级事实 | 原理图、器件校准或固定总线拓扑决定 | BSP 私有常量，不提供 Kconfig 或应用覆盖入口 |
| 协议常量 | 状态机正确性、重试节奏或内部协作合同的一部分 | 拥有模块的私有常量 |
| 测试 profile | 仅用于一次 benchmark/campaign，生产固件不应携带自由调节项 | `tests/display/profiles/*.json` 生成的类型化头文件 |

Kconfig 不承载音频格式/初始输出状态、SD 挂载策略、IMU ODR、电源轮询、时区、SNTP
server、Wi-Fi worker 优先级或 benchmark 时长和网络参数。运行时配置由服务复制；服务活动
时重复传入相同配置返回 `ESP_OK`，不同配置返回 `ESP_ERR_INVALID_STATE`。

## 产品基线

根应用单点拥有以下生产策略：音频 16 kHz、16-bit、stereo、384x MCLK、音量 60、
unmuted、PA 开启；SD `/sdcard`、最多 5 个文件、16 KiB allocation unit；IMU 100 Hz；
Power 5000/100 ms；时区 `CST-8`、SNTP `pool.ntp.org`。本轮不将这些值持久化到 NVS。

BSP 固定 I2S0、GPIO16/9/45/8/10、PA GPIO46 和 30 dB 麦克风校准。BSP 音频初始化后
保持 PA 关闭，必须先 `bsp_audio_configure()` 才能 start。普通 SD init/start 永不格式化；
只有 `sd_storage_service_recover_and_mount()` 能选择显式格式化恢复。

## 静态资源

| 资源 | 组件 Kconfig 默认值 | Kconfig 范围 |
| --- | ---: | ---: |
| Event Bus subscriber/callback/payload pool | 24/24/24 | 各 1..64 |
| Event Bus payload bytes | 256 | 32..1024 |
| NVS blob pool | 16 | 1..64 |
| App Manager resident/page | 8/12 | 1..16 / 1..32 |
| App Manager navigation/mailbox/control queue | 24/24/16 | 各 1..64 |
| App Manager control stack | 4096 | 2048..16384 |
| Time worker stack | 3072 | 2048..8192 |

当前 Waveshare S3 产品 profile 将 App Manager navigation/mailbox/control queue 覆盖为
`8/12/16`，并由配置治理测试锁定；其他未覆盖的静态资源沿用组件 Kconfig 默认值。

公共容量宏保留原名并映射到 `CONFIG_*`。代码必须以编译期断言保护 callback pool 不小于
subscriber pool，以及所有窄整数索引的容量上限。宿主测试由每个独立 Git 仓库唯一的
fake `sdkconfig.h` 提供静态预算；目标特例可用 CMake definition 覆盖。

## 任务优先级

| 任务 | 优先级 | 所有权 |
| --- | ---: | --- |
| IMU worker | 6 | 根运行时产品配置 |
| System PM worker | 5 | 根运行时产品配置 |
| App Control worker | 5 | 根运行时产品配置 |
| Board Input worker | 5 | BSP 私有常量 |
| Time / Power / Wi-Fi worker | 4 | 根运行时产品配置 |
| Clock demo worker | 4 | 应用私有常量 |
| Audio / Storage demo worker | 2 | 应用私有常量 |
| Display benchmark TCP worker | 2 | 测试实现私有常量 |
| Display diagnostics | 1 | App Manager 诊断私有常量 |
| Display benchmark supervisor/audio | 1/1 | 测试实现私有常量 |

产品优先级必须同时经过编译期断言和服务入口校验，满足
`1 <= priority < configMAX_PRIORITIES`。应用、Board Input、diagnostics 和 benchmark
栈大小没有高水位证据前不加入 Kconfig。

Waveshare ESP32-S3 Touch AMOLED 1.8 的 368x448 分辨率、QSPI 40 MHz、10 行传输、
DMA 完整行上限 44、queue depth 2、RGB565、Direct DMA 关闭和 TE 关闭均为板级 profile
事实，不通过 Kconfig 暴露。LVGL draw buffer、Snapshot、任务栈和诊断 gate 仍属于跨板
静态资源或构建能力。

## Display profile

`MAIN_DISPLAY_BENCHMARK=y` 只负责把 benchmark 编译进开发固件。campaign 使用严格 JSON：
mode 只能为 `stress|characterization`，load 只能为
`full|audio_only|tcp_only`，未知或缺失字段直接拒绝。工具在每个隔离构建目录生成固定名
`display_benchmark_profile.h`；gate 开启但未提供生成目录或头文件时 CMake 必须失败。
固定 benchmark defaults fragment 只声明 60 行 LVGL draw buffer、Snapshot 和诊断
gate 等跨板构建配置；板级传输参数不进入 sdkconfig fragment。stress/soak 仅由 JSON
duration 区分。时钟、Direct DMA 和 RGB swapped A/B 工具已经退役，历史结论保留在
`tests/display/README.md`。

## 变更与检查

不得在生产源码中添加 `#ifndef CONFIG_*` fallback，也不得重定义 `configTICK_RATE_HZ`。
修改 Kconfig 或 CMake 源发现后运行 `idf.py reconfigure`，用 `idf.py save-defconfig`
更新 `sdkconfig.defaults`，不得手改 `sdkconfig`。根 main 宿主 CTest 会运行只读治理扫描，
阻止废弃符号、fallback 和 tick rate 重定义回归。
