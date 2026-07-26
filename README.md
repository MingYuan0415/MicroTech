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

App Manager 的显示诊断和自动压力基准默认关闭。开发固件可在 menuconfig 中同时启用
`SYSTEM_PM_DEVELOPMENT_MODE`、`APP_MANAGER_DISPLAY_DIAGNOSTICS` 和
`APP_MANAGER_DISPLAY_BENCHMARK`。默认 `STRESS` 模式会在 Wi-Fi 获得 IPv4 地址后运行
1800 秒，持续执行生产 fade/push 转场、16-bit 双声道录放和 2048 kbit/s 双向 TCP
echo。`CHARACTERIZATION` 模式依次运行 fade、push-left、push-right、cover-left 和
reveal-right：每种效果先在 display-only 下运行 30 秒，再按 Kconfig 选择的 profile 运行
30 秒。第二阶段默认为 `full`（audio+TCP），也可单独选择 `audio-only` 或 `tcp-only`；
`STRESS` 始终固定为 full-load，不受该选项影响。两个负载窗口分别重置诊断统计。

默认服务地址为 `192.168.0.205:5001`。在该 WSL 主机运行：

```sh
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001
```

`wslinfo --networking-mode` 返回 `mirrored` 时，需在管理员 PowerShell 为 WSL 的
Hyper-V 防火墙放行 TCP 5001：

```powershell
New-NetFirewallHyperVRule `
  -Name "MicroTech-TCP-Echo-5001" `
  -DisplayName "MicroTech TCP Echo 5001" `
  -Direction Inbound `
  -VMCreatorId "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}" `
  -Protocol TCP -LocalPorts 5001 -Action Allow -Enabled True
```

ESP32 所在网络必须能直接访问该 WSL 地址。服务端将 peer reset、broken pipe 和普通关闭
作为正常连接结束，记录每个连接的持续时间、双向字节和关闭原因，不打印 Python
traceback。设备端使用当前 lwIP 发送窗口内的最大整 MSS payload（默认 5,760 B），并记录
TCP connect/send/recv/verify 阶段、`errno`、已传输字节、重连次数、中断时间和 pacing
迟到。最终 `display load` 日志中的 `tcp_active_us` 是 TCP worker 从启动到收到停止请求的
实际活动时长，`tcp_target_bytes` 按配置速率和该时长计算。TCP 异常后每秒重连并继续
显示基准到预定结束，但任何错误、重连或任一方向吞吐低于配置速率的 95% 都会使稳定性
失败。未启用 TCP 的 audio-only profile 输出 `tcp_required=0`、`tcp_rate_ok=1`；此处的
rate ok 表示 TCP 条件不适用，不表示执行过网络吞吐测试。

诊断仅统计 presentation 确认真正启动到完成的动画区间，排除 mount/unmount、下一次
导航准备和 idle。每种效果输出 active 平均 FPS、最终 flush 提交间隔的
P50/P95/P99/max、render/flush/flush-wait/panel submit 耗时、flush/panel 次数与像素数。
adapter 的一秒 FPS 只作为交叉检查，不直接决定性能结论。`sample_err` 进一步分为
LVGL 锁错误与 FPS 读取错误。最终日志使用独立的 `display benchmark` 和
`display load` 行分别给出
`stability=PASS|FAIL` 和 `performance=TARGET|FLOOR|FAIL`：

- `TARGET`：生产效果平均不低于 30 FPS，至少 95% 间隔不超过 33.333 ms，P99 不超过
  41.667 ms，且不连续两帧超过 41.667 ms；
- `FLOOR`：生产效果平均不低于 25 FPS，P95/P99/max 分别不超过
  50/66.667/100 ms；
- 稳定性要求完整结束、`sample_err=0`、无显示提交/音频/TCP/Wi-Fi 错误、无重连，
  且 DMA/internal 最大连续块不低于 14,720 B。

`display config` 还会记录 `load_profile` 和 `lifecycle_log`，第二阶段日志使用
`load=full|audio-only|tcp-only`，最终 `display load` 使用 `profile` 和 `tcp_required`
标识实际负载。`dma_fail` 是采样期间 DMA/internal 最大连续块低于 14,720 B 的次数，
不代表发生了 SPI DMA 提交失败；实际显示提交失败由 `submit_fail` 统计。

默认显示传输配置为 40 MHz QSPI、60 行双 PSRAM draw buffer、10 行 SPI DMA chunk、
queue depth 2、非 TE bounce DMA 和 `RGB565`。旧的 60 MHz 请求在 ESP32-S3 默认
80 MHz GPSPI 时钟源上实际只能分频为 40 MHz，现已直接配置并记录真实的 40 MHz。
基准开始时的 `display config` 行会同时记录 draw/DMA 行数、颜色格式、direct/TE、
draw worker、TCP payload 和任务优先级，后续 A/B 日志可独立识别配置。
buffer 表征已从 60/120/240/448 行中选出 120 行作为实验候选，但未改变生产
默认。在 120 行、`RGB565_SWAPPED`、bounce/10 下，full 负载的十组最差
P95 为 145 ms，DMA/internal 最大连续块降至 8,192 B；audio-only 和
tcp-only 分别以 128.423 ms/16,384 B 和 145 ms/21,504 B 通过稳定性检查。
结果表明 TCP 是主要性能压力，audio+TCP 组合峰值会压低内部 DMA 连续块。
非 TE direct/10 在 full 负载下出现顶部蓝线和显示冻结，因此已拒绝，direct/44
不再执行。生产默认仍为 60 行、`RGB565`、bounce/10、direct 和 TE 关闭，
诊断、基准与生命周期 trace 均关闭。80 MHz 仍是未经上板验证的实验档。
当前参数调优未达到 25/30 FPS 门槛，全屏预渲染或快照式转场评估暂缓。

`CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` 控制 App Manager 的 START、MOUNT、RESUME、
PAUSE、UNMOUNT、STOP 和 NEW_INTENT INFO trace，例如
`app_lifecycle: [settings][root] ON MOUNT`。默认关闭时不编译这些事件名称和日志调用；
错误与警告日志不受影响。

## 开发约束

贡献前阅读 [AGENTS.md](AGENTS.md) 和 [代码风格](doc/code-style.md)。代码以性能和低资源占用为优先，保持模块低耦合；硬件及并发边界保留必要防护。不得通过修改 ESP-IDF、`managed_components/` 或 BSP 第三方库来规避工程自身问题。

## 许可证

本项目采用 [MIT License](LICENSE)。
