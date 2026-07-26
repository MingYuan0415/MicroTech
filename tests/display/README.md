# 显示性能与稳定性验收

本目录保存 ESP32-S3 显示基准所需的主机端工具。基准只用于开发固件；生产配置应关闭
`CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS` 和
`CONFIG_APP_MANAGER_DISPLAY_BENCHMARK`。

## TCP echo 服务

在设备能够访问的主机地址上运行：

```sh
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001
```

服务会为每个连接输出 peer、持续时间、接收/发送字节和关闭原因。reset、broken pipe
或普通关闭均作为正常连接结束统计，不产生 Python traceback。设备默认连接
`192.168.0.205:5001`；地址、端口和每方向速率由 App Manager Kconfig 配置。设备使用
不超过 lwIP 发送窗口的最大整 MSS payload，当前为 5,760 B；普通调度落后会沿绝对
时间线追赶，最终报告的 `tcp_pacing_late` 和 `tcp_pacing_max_lag_us` 用于区分调度迟到
与网络断线。`tcp_active_us` 记录 TCP worker 从启动到收到停止请求的实际活动时长，
`tcp_target_bytes` 按配置速率和该时长计算。

## 基准模式

先启用 `SYSTEM_PM_DEVELOPMENT_MODE`、`APP_MANAGER_DISPLAY_DIAGNOSTICS` 和
`APP_MANAGER_DISPLAY_BENCHMARK`，再选择一种模式：

- `STRESS`：默认 1800 秒，生产 fade/push 转场与 audio+TCP 全负载同时运行。
- `CHARACTERIZATION`：fade、push-left、push-right、cover-left、reveal-right 各运行
  30 秒 display-only，再按 Kconfig 选择的第二阶段 profile 各运行 30 秒；两个负载窗口
  分别统计。profile 默认为 `FULL`，也可选择 `AUDIO_ONLY` 或 `TCP_ONLY`。audio-only
  不等待 Wi-Fi 或创建 TCP worker；tcp-only 不启动 audio service/worker。`STRESS`
  始终固定使用 audio+TCP full-load，不受 characterization profile 影响。

第二阶段 choice 对应
`APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_FULL`、
`APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_AUDIO_ONLY` 和
`APP_MANAGER_DISPLAY_BENCHMARK_CHARACTERIZATION_LOAD_TCP_ONLY`。

最终报告将稳定性与性能分开：

- `stability=PASS` 要求完整结束、无采样/显示提交/audio/TCP/Wi-Fi 错误、无 TCP
  重连、每方向吞吐至少达到配置值的 95%，且 DMA/internal 最大连续块不低于
  14,720 B。
- `performance=TARGET` 要求每种生产转场 active 平均至少 30 FPS、至少 95% 帧间隔
  不超过 33.333 ms、P99 不超过 41.667 ms，且没有连续两个长帧。
- `performance=FLOOR` 要求 active 平均至少 25 FPS，P95/P99/max 分别不超过
  50/66.667/100 ms；其余为 `FAIL`。

`sample_err` 保持为总采样错误数，并由 `lock_err` 与 `fps_read_err` 分类；
`fps_lock_max_us` 是一次 FPS 采样获取 LVGL 锁的最长等待。最终结果拆为
`display benchmark` 与 `display load` 两行，避免长日志截断。每次运行还会输出
`display config`，记录 QSPI、draw buffer、颜色格式、DMA、TE、draw worker、TCP
payload、`load_profile` 与 `lifecycle_log`，因此比较日志时不再依赖启动段。第二阶段
日志使用 `load=full|audio-only|tcp-only`；最终 `display load` 使用 `profile` 和
`tcp_required` 标识实际负载。TCP 未启用时统计保持为零，`tcp_required=0`、
`tcp_rate_ok=1` 表示吞吐条件不适用，不表示执行过 TCP 测试。

`dma_fail` 统计采样期间 DMA/internal 最大连续块低于 14,720 B 的次数，是内存余量门槛
指标，不是 SPI DMA 提交失败次数。实际显示提交失败由 `submit_fail` 统计。

adapter 的一秒 FPS 只作为交叉检查。发布判断使用 presentation 标记的真实动画区间和
最终 flush 提交间隔，不包含 mount/unmount、下一次导航准备和 idle。

## 2026-07-26 阶段结论

本轮在 40 MHz QSPI、TE 关闭、queue depth 2、双软件绘制线程和
128 KiB internal reserve 下完成 60/120/240/448 行 buffer 表征。120 行是表征
阶段的实验候选，不代替 60 行生产默认。负载隔离结果如下：

| 120 行、`RGB565_SWAPPED`、bounce/10 配置 | 十组最差 P95 | `min_dma` | 稳定性 |
| --- | ---: | ---: | --- |
| full（audio+TCP） | 145.000 ms | 8,192 B | FAIL，低于 14,720 B 门槛 |
| audio-only | 128.423 ms | 16,384 B | PASS |
| tcp-only | 145.000 ms | 21,504 B | PASS |

TCP 是主要性能压力；audio-only 和 tcp-only 均能保持 DMA 连续块门槛，但
audio+TCP 组合负载会产生更低的内部内存峰值。`direct/10 + full` 在 display-only
阶段结束、TCP 连接后出现屏幕顶部蓝线，随后 LVGL 无法继续取得显示锁且 GUI
冻结；调度器和诊断任务仍在运行，未见 panic、WDT 或 OOM。该配置已拒绝，
`direct/44` 取消上板测试。非 TE Direct DMA 开关仅保留为默认关闭的问题复现入口，
host 配置测试不表示硬件可用。

生产配置继续使用 40 MHz、60 行 LVGL buffer、10 行 bounce chunk、queue depth 2、
`RGB565`并关闭 direct/TE。80 MHz 仍未经上板验证。当前传输参数调优未达到
25 FPS 发布底线或 30 FPS 目标；全屏预渲染/快照式转场架构评估暂缓。

ESP32-S3 单次 DMA 的 32 KiB 边界下，368 像素宽 RGB565 最多容纳 44 个完整行：
`368 * 44 * 2 = 32,384 B`，而 45 行为 `33,120 B`。因此 direct 大块 A/B 使用 44 行。
`BSP_DISPLAY_SPI_MAX_TRANSFER_LINES` 仍表示逻辑 panel 请求高度；配置超过 44 行时，
ESP-IDF 会将请求拆成不超过硬件边界的 DMA 段，不应将其误认为单个物理 DMA 传输。

候选配置先运行 10 分钟显示图案和全负载检查，再用 `STRESS` 运行完整 1800 秒。IMU/I2C
超时应单独记录，不归因于显示性能；WDT、panic、OOM、音频错误、显示冻结或任何 Wi-Fi
断连均使稳定性失败。

## 生命周期 trace

`CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` 控制 START、MOUNT、RESUME、PAUSE、UNMOUNT、
STOP 和 NEW_INTENT 的 `app_lifecycle` INFO trace。生产默认关闭，关闭时事件名称表和日志
调用也不参与编译；错误、警告、handler、observer 与状态迁移不受影响。需要核对生命周期
顺序时可在开发固件中临时启用，例如输出
`app_lifecycle: [settings][root] ON MOUNT`。
