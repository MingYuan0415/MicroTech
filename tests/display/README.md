# 显示性能与稳定性验收

本目录保存 ESP32-S3 显示基准的主机工具。基准只用于开发固件；生产配置关闭
`CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS` 和 `CONFIG_MAIN_DISPLAY_BENCHMARK`。
benchmark 启动后由根运行时抑制待机，结束或启动回滚时释放抑制状态。

## 固定显示基线

Waveshare ESP32-S3 Touch AMOLED 1.8 的板级 profile 固定为：

- 368x448、QSPI 40 MHz、4 data lines、RGB565；
- 10 行逻辑传输、44 行 DMA 完整行上限、transaction queue depth 2；
- PSRAM Direct DMA 关闭，TE 关闭。

这些值由 BSP 私有 profile 和只读 transport descriptor 提供，不进入 Kconfig 或 benchmark
defaults。LVGL 的 60 行双 PSRAM draw buffer、Snapshot、任务栈、诊断 gate 和核心亲和性
仍是跨板静态资源或构建能力。

历史表征显示，80 MHz 对最差 P95 的改善约 3.45%，未达到 5% 判定线，且出现 DMA 连续块
不足和运动中的接缝；非 TE PSRAM Direct DMA 在 TCP 负载开始后出现顶部蓝线和 GUI 冻结。
因此两者均不作为当前板型可执行 profile，也不保留时钟、Direct DMA 或 RGB swapped A/B
构建入口。40 MHz 仍是项目经验基线，不代表 SH8601A preliminary 时序保证。

## 基准 profile

`tests/display/profile_defaults/benchmark_baseline.defaults` 固定 60 行 draw buffer、Snapshot
和 benchmark gate。模式、时长、负载和网络参数由 `tests/display/profiles/*.json` 选择，
工具在每个隔离构建目录生成 `display_benchmark_profile.h`；gate 开启但未传入
`DISPLAY_BENCHMARK_PROFILE_DIR` 时 CMake 直接拒绝构建。

- `STRESS`：运行生产转场以及 profile 选择的 Audio/TCP/BLE/应用负载。
- `CHARACTERIZATION`：fade、push-left、push-right、cover-left、reveal-right 分别运行
  display-only 和指定负载窗口。

最终报告将稳定性与性能分开：

- `stability=PASS`：完整结束，无采样、显示提交、Audio、TCP 或 Wi-Fi 错误，且
  DMA/internal 最大连续块不低于 14,720 B。
- `performance=TARGET`：active 平均至少 30 FPS，至少 95% 帧间隔不超过 33.333 ms，
  P99 不超过 41.667 ms，且没有连续两个长帧。
- `performance=FLOOR`：active 平均至少 25 FPS，P95/P99/max 分别不超过
  50/66.667/100 ms。

`display config` 从 BSP descriptor 记录 `transport=qspi`、`bus_hz`、DMA 几何、queue、
Direct/TE 以及 LVGL 配置。`dma_fail` 表示采样时 DMA/internal 最大连续块低于门槛；实际
显示提交失败由 `submit_fail` 统计。

## TCP echo 服务

在设备可访问的主机地址运行：

```sh
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001
```

设备默认连接 `192.168.0.205:5001`，地址、端口和每方向速率来自严格 JSON profile。
WSL mirrored 网络模式需要在 Hyper-V 防火墙放行 TCP 5001。

## Snapshot 转场

默认 RGB565 配置启用 `CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION`。对有效的非
`NONE` 转场，App Manager 将 source 和 target 分别采集为两张全分辨率 RGB565 PSRAM
图片。368x448 下每张 329,728 B，两个常驻缓冲共 659,456 B，不占 internal/DMA heap。

Snapshot 只依赖 SPIRAM 和自身 build gate。缓冲、采集、overlay 或动画启动失败时回退
原生 LVGL Screen 动画；`NONE`、零时长、同一 Screen 和边缘返回 `PRESSING` 不创建快照。
真机验收要求所有有效效果无 fallback，prepare P95 不超过 100 ms。

## LVGL 内部 RAM profile

LVGL RAM 工具只改变 LVGL OS、software draw unit、draw-thread stack 和 FreeType render
pool，并复用固定板型显示基线：

| Profile | LVGL OS | Draw unit / stack | FreeType pool | 理论内部 RAM 回收 |
| --- | --- | --- | ---: | ---: |
| B0 | FreeRTOS | 2 / 32 KiB | 16 KiB | 基线 |
| C | NONE | 1 / 无 draw task | 16 KiB | 64 KiB |
| A | FreeRTOS | 1 / 32 KiB | 16 KiB | 32 KiB |
| B24/B20/B16 | FreeRTOS | 1 / 24/20/16 KiB | 4 KiB | 40/44/48 KiB |

准备、构建和验证隔离候选：

```sh
python3 tests/display/lvgl_ram_profiles.py prepare --reset --profiles B0 C
# 依次执行上一步打印的 idf.py build size 命令
python3 tests/display/lvgl_ram_profiles.py validate --profiles B0 C
python3 tests/display/analyze_lvgl_ram.py \
  --log B0=/path/to/b0.log --log C=/path/to/c.log
```

源码变更后必须重新 `prepare --reset`。分析器校验两阶段五种 effect、稳定性、FLOOR、
Snapshot、DMA、render task、栈位置/high-water 和 internal free 收益。13 种转场、字形、
10 分钟 full-load、1800 秒 stress、冷启动和休眠恢复是独立验收项。

## C_EXT 与保留压测

`C_EXT` 是生产默认：LVGL 使用 `LV_OS_NONE`、单 draw unit 和 32 KiB PSRAM adapter worker，
并固定到 CPU1；项目任务和 ESP-IDF 可配置的主要系统任务固定到 CPU0；NimBLE 动态内存
使用 PSRAM。原始 C profile 保留为历史基线。IDLE、IPC、驱动内部任务和 ISR 不属于项目
任务亲和性保证。

```sh
python3 tests/display/lvgl_ram_profiles.py prepare --reset \
  --profiles C_EXT C_EXT_STRESS
# 依次执行生成的隔离构建命令
python3 tests/display/lvgl_ram_profiles.py validate \
  --profiles C_EXT C_EXT_STRESS
```

`C_EXT_STRESS` 在 `C_EXT` 基础上加载 Device Link ACL 连接负载（`ble_connected`
模式，验证绑定窗口打开、手机连接、断连重连无抖动），保留用于后续稳定性复现，
不修改生产 `sdkconfig.defaults`。设备端执行 Device Link ACL、Wi-Fi/TCP、Audio、
Microphone、显示转场和 App Manager 路由负载；Device Link v1 加密流量验收在
硬件 Numeric Comparison 验证后回归。日志分析命令：

```sh
python3 tests/display/analyze_c_ext_stress.py /path/to/c_ext_stress.log
```

分析器检查阶段顺序、heap/DMA、任务 HWM/栈位置和核心、BLE 连接保持、TCP、Audio TX/RX、
13 种动画、页面覆盖及 cleanup 恢复。一次 1800 秒结果不能替代 10 分钟预检、BLE 开关、
TCP 中断恢复、冷启动、休眠恢复或 8 小时 soak。

## 生命周期 trace

`CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` 控制 START、MOUNT、RESUME、PAUSE、UNMOUNT、
STOP 和 NEW_INTENT 的 INFO trace。生产默认关闭；错误、警告、handler、observer 和状态
迁移不受影响。

## 调试顺序

1. 先用纯色 flush 验证 adapter 管理的 `lv_display_flush_ready` 握手。
2. 核对分辨率、RGB565、stride、60 行 draw buffer、10 行 DMA staging 和 queue depth 2。
3. 检查 adapter tick、唯一 `lv_timer_handler` worker 和 UI 进度。
4. 独立校验触摸坐标与旋转，再叠加 UI。
5. 核对 widget、字体、图片、Snapshot 和 PSRAM/internal/DMA 预算。
6. 用真机 benchmark 判断帧时间、flush 完成、fallback 和连续块门槛。

宿主测试只能验证配置、生命周期和解析逻辑，不能证明 SPI 时序、DMA、射频、功耗或真实
显示稳定性。
