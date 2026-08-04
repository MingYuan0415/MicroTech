# 显示性能与稳定性验收

本目录保存 ESP32-S3 显示基准所需的主机端工具。基准只用于开发固件；生产配置应关闭
`CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS` 和
`CONFIG_MAIN_DISPLAY_BENCHMARK`。

## TCP echo 服务

在设备能够访问的主机地址上运行：

```sh
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001
```

服务会为每个连接输出 peer、持续时间、接收/发送字节和关闭原因。reset、broken pipe
或普通关闭均作为正常连接结束统计，不产生 Python traceback。设备默认连接
`192.168.0.205:5001`；地址、端口和每方向速率来自构建目录中的严格 JSON profile。
设备使用
不超过 lwIP 发送窗口的最大整 MSS payload，当前为 5,760 B；普通调度落后会沿绝对
时间线追赶，最终报告的 `tcp_pacing_late` 和 `tcp_pacing_max_lag_us` 用于区分调度迟到
与网络断线。`tcp_active_us` 记录 TCP worker 从启动到收到停止请求的实际活动时长，
`tcp_target_bytes` 按配置速率和该时长计算。

若在 WSL 上运行服务且 `wslinfo --networking-mode` 返回 `mirrored`，需在管理员
PowerShell 为 WSL 的 Hyper-V 防火墙放行 TCP 5001：

```powershell
New-NetFirewallHyperVRule `
  -Name "MicroTech-TCP-Echo-5001" `
  -DisplayName "MicroTech TCP Echo 5001" `
  -Direction Inbound `
  -VMCreatorId "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}" `
  -Protocol TCP -LocalPorts 5001 -Action Allow -Enabled True
```

ESP32 所在网络必须能直接访问该 WSL 地址。

## 基准模式

sdkconfig fragment 启用 `SYSTEM_PM_DEVELOPMENT_MODE`、
`APP_MANAGER_DISPLAY_DIAGNOSTICS` 和 `MAIN_DISPLAY_BENCHMARK`。模式和负载由
`tests/display/profiles/*.json` 选择；工具会在每个隔离构建目录生成固定名称的
`display_benchmark_profile.h`，gate 开启但缺少生成目录时 CMake 直接失败。

- `STRESS`：默认 1800 秒，生产 fade/push 转场与 audio+TCP 全负载同时运行。
- `CHARACTERIZATION`：fade、push-left、push-right、cover-left、reveal-right 各运行
  profile 指定时长的 display-only，再按 JSON load 运行第二阶段；两个负载窗口分别
  统计。load 可为 `full`、`audio_only` 或 `tcp_only`。audio-only
  不等待 Wi-Fi 或创建 TCP worker；tcp-only 不启动 audio service/worker。`STRESS`
  profile 当前使用 audio+TCP full-load。

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
`RGB565`并关闭 direct/TE。40 MHz 是项目经验默认，SH8601A preliminary Table 14 并未
保证它的 Quad SPI 时序。80 MHz 在当前样机上可运行，但已归类为不稳定的超规格实验档：
十组最差 P95 从 E40 的约 145 ms 降至 E80 的约 140 ms，改善约 3.45%，未达到 5% 判定
线；一轮 E80 还出现 `min_dma=8704`、`dma_fail=1` 和运动中的 T1 单条接缝。它不能成为
生产默认，任何需要它的复现实验都必须显式选择 80 MHz。上述数据属于原生 Screen 动画和
`RGB565_SWAPPED` 对照，不构成快照转场的性能结论。

## 全分辨率快照转场

默认 `RGB565` 生产配置启用 `CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION`。对有效的
非 `NONE` 转场，App Manager 把 source 和 target 分别采集到两张全分辨率 RGB565 PSRAM
图片，并仅动画这两个 Image；不做缩放、压缩或 transform。368 x 448 下每张快照为
329,728 B，两个常驻缓冲共 659,456 B（644 KiB），不占用 internal/DMA heap。该资源成本
独立于 LVGL 60 行 draw buffer、10 行 bounce DMA chunk 和 queue depth 2，后者保持不变。

该模式依赖 `SPIRAM` 且不能与 `RGB565_SWAPPED` 同时启用。SWAPPED A/B 固件、功能关闭配置，
以及快照缓冲、采集、overlay 或动画启动失败的转场，均自动回退原生 LVGL Screen 动画；
这些运行必须标记为 snapshot N/A 或 fallback，不能用于宣称快照架构通过。`NONE`、零时长、
同一 Screen 和边缘返回 `PRESSING` 阶段不创建快照。非空 Page Screen 没有全屏不透明根，
或对象树设置了 `LV_OBJ_FLAG_OVERFLOW_VISIBLE` 时，同样逐次回退原生动画。

快照候选的真机 benchmark 除既有稳定性与 25/30 FPS 门槛外，还要求每种有效效果没有
snapshot fallback，且快照准备 P95 不超过 100 ms。准备耗时从输入屏障开始到 Image 动画确认
启动，失败尝试也计入 fallback；完整上板验证须覆盖全部效果、触摸/按键、休眠/恢复、10 分钟
full-load 预检和 1800 秒压力测试。

ESP32-S3 单次 DMA 的 32 KiB 边界下，368 像素宽 RGB565 最多容纳 44 个完整行：
`368 * 44 * 2 = 32,384 B`，而 45 行为 `33,120 B`。因此 direct 大块 A/B 使用 44 行。
`BSP_DISPLAY_SPI_MAX_TRANSFER_LINES` 仍表示逻辑 panel 请求高度；配置超过 44 行时，
ESP-IDF 会将请求拆成不超过硬件边界的 DMA 段，不应将其误认为单个物理 DMA 传输。

候选配置先运行 10 分钟显示图案和全负载检查，再用 `STRESS` 运行完整 1800 秒。IMU/I2C
超时应单独记录，不归因于显示性能；WDT、panic、OOM、音频错误、显示冻结或任何 Wi-Fi
断连均使稳定性失败。

## LVGL 内部 RAM 候选

LVGL RAM 工具固定生产显示基线、full-load characterization 和源码指纹，只改变
LVGL OS、software draw unit、draw-thread stack 与 FreeType render pool：

| Profile | LVGL OS | Draw unit / stack | FreeType pool | 理论内部 RAM 回收 |
| --- | --- | --- | ---: | ---: |
| B0 | FreeRTOS | 2 / 32 KiB | 16 KiB | 基线 |
| C | NONE | 1 / 无 draw task | 16 KiB | 64 KiB |
| A | FreeRTOS | 1 / 32 KiB | 16 KiB | 32 KiB |
| B24/B20/B16 | FreeRTOS | 1 / 24/20/16 KiB | 4 KiB | 40/44/48 KiB |

先准备并构建 B0/C；只有 C 未通过时才依次准备 A、B24、B20、B16：

```sh
python3 tests/display/lvgl_ram_profiles.py prepare --reset
python3 tests/display/lvgl_ram_profiles.py prepare --reset \
  --profiles A B24 B20 B16
```

命令只写 `/tmp/mt-lvgl-ram/<profile>/`，并打印隔离的 `idf.py build size` 命令。
源码改动后必须重新 `prepare --reset`。构建完成后验证镜像、源码指纹及配置差异：

```sh
python3 tests/display/lvgl_ram_profiles.py validate --profiles B0 C
```

保存同负载的 characterization 串口日志后执行候选门槛分析：

```sh
python3 tests/display/analyze_lvgl_ram.py \
  --log B0=/path/to/b0.log --log C=/path/to/c.log
```

只有 C 失败时才追加 `A`，并按 `B24`、`B20`、`B16` 顺序追加日志。分析器校验
profile 指纹、两阶段五种 effect、稳定性、FLOOR、snapshot、DMA、render task、栈位置、
high-water 和实际 internal free 收益，并按固定停止规则输出 `selected`。`--json` 可生成
机器可读结果。该结果仅覆盖 characterization 日志；13 种转场人工检查、完整字号/字形、
10 分钟 full-load、1800 秒 stress、冷启动和休眠恢复仍须分别执行并记录，全部通过前不得
将候选写入生产 `sdkconfig.defaults`。

C 必须满足既有稳定性、FLOOR、snapshot 与 DMA 门槛，并报告 `render_task=1`、
`render_stack_psram=1`、`render_stack_hwm>=4096`；相同负载下
`min_internal_free` 至少比 B0 增加 48 KiB。A/B 的 high-water 同样至少 4 KiB，
内部 RAM 收益至少达到理论值的 75%。adapter 出现 PSRAM 栈分配失败回退日志时，
候选直接失败。B0 只提供同负载基线，其自身可因现存内部 RAM 问题失败，但日志必须完整、
配置有效并包含内存汇总。B24/B20/B16 任一级失败后不继续测试更小栈。

### BLE 配网诊断

`C_EXT` 现为生产默认配置：它把 NimBLE Host 动态分配从内部 RAM 移到 PSRAM，
并将唯一的 `lvgl` adapter worker 固定到 CPU1，将项目任务及 ESP-IDF 可配置的 main、
Wi-Fi、BT Controller、NimBLE、LwIP、esp_timer、FreeRTOS timer 和 pthread 默认任务
固定到 CPU0。原始 C profile 通过隔离 fragment 恢复内部 NimBLE，以及项目、LVGL、
LwIP、FreeRTOS timer 和 pthread 的修改前亲和性，用于历史基线对照。IDLE、IPC、
驱动内部任务和 ISR 不在项目任务保证范围内；esp_timer ISR 仅按 ESP-IDF 提供的配置固定。
C_EXT 仍可用于判别 BLE 激活后的内部 DMA 连续块并验证双核隔离。它不属于旧候选矩阵，
不能传给 `analyze_lvgl_ram.py`；其 benchmark 构建仍保持隔离：

```sh
python3 tests/display/lvgl_ram_profiles.py prepare --reset --profiles C_EXT
python3 tests/display/lvgl_ram_profiles.py command C_EXT
python3 tests/display/lvgl_ram_profiles.py validate --profiles C_EXT
```

执行 `prepare` 后运行其打印的隔离构建命令，再验证镜像。真机按主页、Setup、BLE 二维码、
Security 2 配网、关闭 BLE 的顺序记录 internal/DMA/PSRAM 最小 free/largest；二维码必须
持续可读、不得出现 SPI/DMA 分配失败，BLE 激活时 `dma_largest` 至少为 `14,720 B`，
`display config` 必须报告 `lvgl_core=1 project_core=0`，memory 记录必须保持
`render_core=1`。关闭后内存应恢复。NimBLE Host task 的 6 KiB 栈仍在内部 RAM；该配置
只移动 NimBLE 动态分配。ESP-IDF Security 2 内部 SRP/session heap 释放前未保证显式清零的问题也仍然
存在，发布前仍须完成安全与生命周期复核。

### C_EXT 并发系统压力测试

`C_EXT_STRESS` 在 C_EXT 上额外启用 provisioning diagnostics，并固定加载
`c_ext_stress_1800.json`。它仍是隔离的开发 profile，不修改生产 `sdkconfig.defaults`：

```sh
python3 tests/display/lvgl_ram_profiles.py prepare --reset \
  --profiles C_EXT C_EXT_STRESS
python3 tests/display/lvgl_ram_profiles.py command C_EXT
python3 tests/display/lvgl_ram_profiles.py command C_EXT_STRESS
python3 tests/display/lvgl_ram_profiles.py validate \
  --profiles C_EXT C_EXT_STRESS
```

依次执行命令输出的隔离 `idf.py build size`。记录两份 size 输出，
`C_EXT_STRESS` 相比 C_EXT 的静态 DIRAM 增量不得超过 2 KiB。烧录
`C_EXT_STRESS` 前启动 TCP echo server：

```sh
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001
```

设备进入 `provisioning_wait` 后，手机扫描 Setup 页二维码并建立 Security 2。手机侧按
`contracts/provisioning/docs/stress-session.md` 严格串行发送 `GetSnapshot`：2 秒一次、
request ID 非零且不复用，不发送 mutation 或 `FinishSession`，测量阶段不主动断连或重连。
设备在首个 protected snapshot 成功后完成 warmup，并进入 1800 秒 measure。保存完整串口
日志后运行：

```sh
python3 tests/display/analyze_c_ext_stress.py /path/to/c_ext_stress.log
```

分析器要求七个 phase 按固定顺序各出现一次，并校验显示基线、heap/DMA、任务 HWM/栈位置、
BLE cadence、TCP、Audio TX/RX、13 种动画、页面覆盖、cleanup 恢复和致命日志。完整通过只
证明一次 1800 秒并发 campaign；10 分钟预检、20 次 BLE 开关、3 次 TCP 中断恢复、冷启动、
休眠恢复和 8 小时 soak 均是独立验收项，不能互相替代。

## 40/80 MHz 单板 A/B

SH8601A preliminary Table 14 给出的 Quad SPI 最小写周期为 50 ns，对应约 20 MHz
名义上限；因此 40 MHz 已是两倍名义上限的项目经验默认，80 MHz 是四倍名义上限的
超规格实验。ESP32-S3 SPI2 以 APB /1 可以实际输出 80 MHz，但该事实不构成面板稳定性
证明。时钟 A/B 工具仅用于复现和诊断当前样机，不能将 80 MHz 提升为默认；生产默认由
`layers/bsp/Kconfig.projbuild` 的 `BSP_DISPLAY_SPI_CLOCK` choice 保持为 40 MHz，A/B
构建则通过 `tests/display/profile_defaults/clock_40.defaults` 或 `clock_80.defaults` 显式选择。

四个 characterization profile 都固定 bounce/10、queue depth 2、Direct/TE 关闭、
双 draw worker、128 KiB internal reserve、full load 和每效果 30 秒：

| Profile | 时钟 | Draw buffer | 色彩 |
| --- | ---: | ---: | --- |
| E40 | 40 MHz | 120 行 | `RGB565_SWAPPED` |
| E80 | 80 MHz | 120 行 | `RGB565_SWAPPED` |
| P40 | 40 MHz | 60 行 | `RGB565` |
| P80 | 80 MHz | 60 行 | `RGB565` |

生成隔离配置并构建：

```sh
python3 tests/display/clock_ab_profiles.py prepare --reset
```

该命令只在 `/tmp/mt-display-clock-ab/<profile>/` 下生成独立 sdkconfig/build 路径，
并为每个 profile 保存 `source_manifest.json`。manifest 对根仓库及全部子模块的 commit、
tracked diff 和未忽略的新文件内容取指纹，随后打印四条 `idf.py build size` 命令。准备后
不得再修改源码；如需修改，必须使用 `prepare --reset` 重新生成全部候选。依次执行打印
出的命令，完成后验证固件存在、源码指纹及完整配置差异：

```sh
python3 tests/display/clock_ab_profiles.py validate --pair all
```

E40/E80 和 P40/P80 都必须来自同一源码指纹，并且只报告三个时钟相关 symbol：两个
choice 与派生的 `CONFIG_BSP_DISPLAY_SPI_CLOCK_HZ`。启动 TCP 服务并单独保存服务端日志：

```sh
mkdir -p /home/mingyuan/display-logs
python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001 \
  2>&1 | tee "/home/mingyuan/display-logs/tcp-clock-ab-$(date +%Y%m%d-%H%M%S).log"
```

characterization 按 `E40 -> E80 -> E80 -> E40 -> P40 -> P80` 执行。每轮先设置小写
`PROFILE` 和 `RUN`，E profile 的 `RUN` 使用 1/2，P profile 使用 1。先烧录，不与
monitor 合并：

```sh
PROFILE=e40
RUN=1
idf.py -B "/tmp/mt-display-clock-ab/${PROFILE}/build" \
  -p /dev/ttyACM0 flash
```

烧录完成后完整断开设备的 USB、电池和充电供电，再重新上电。等待串口节点恢复后只启动
monitor；`--no-reset` 避免附加 monitor 时再次软复位：

```sh
idf.py -B "/tmp/mt-display-clock-ab/${PROFILE}/build" \
  -p /dev/ttyACM0 monitor --no-reset --timestamps --disable-auto-color \
  2>&1 | tee "/home/mingyuan/display-logs/${PROFILE}-${RUN}-$(date +%Y%m%d-%H%M%S).log"
```

如果冷启动早期日志在串口枚举期间丢失，分析器会因启动指纹缺失拒绝该轮，必须重新执行，
不得改用一次普通软复位日志顶替冷启动样本。

日志开头的 board、adapter 和 benchmark 指纹必须一致。每个效果的 full-load 阶段至少
录制 10 秒，优先使用 120/240 FPS；视频同时覆盖静止页面、转场和转场后的稳定画面。
按所有候选视频中最差现象记录视觉等级：

| 等级 | 现象 | 判定 |
| --- | --- | --- |
| T0 | 无撕裂 | 仅记录视觉结果，不能使 80 MHz 成为生产配置 |
| T1 | 仅运动时单条瞬时接缝，静止后立即消失且正常观看不明显 | 当前样机可继续复现实验，但仍是不稳定超规格选项 |
| T2 | 明显或多条接缝、固定顶部线、静态残留、随机像素、错色、黑帧或冻结 | 当前 80 MHz 实验无效，立即回退 40 MHz |

数值分析命令可同时输入重复运行：

```sh
python3 tests/display/analyze_clock_ab.py \
  --log E40=/path/e40-1.log --log E40=/path/e40-2.log \
  --log E80=/path/e80-1.log --log E80=/path/e80-2.log \
  --log P40=/path/p40-1.log --log P80=/path/p80-1.log
```

分析器严格检查启动指纹、每阶段 150/总计 300 个样本、十组 perf/cost、最终状态和
TCP/运行时错误；按重复运行平均后
计算 `W_clock`，并用累计 `panel_us / active_frames` 计算 panel 帧成本。数值结果用于
比较：最差 P95 至少改善 5%、panel 帧成本至少下降 35%，且任一效果平均 FPS 不得回退
超过 5%。当前记录的最差 P95 改善约 3.45%，不满足该比较条件。视觉等级是独立的人工
证据；分析器输出的 `transport=PASS` 不能替代 T0/T1 判定，也不能使 80 MHz 成为生产
配置。既有 full audio+TCP 的 `min_dma=8192` 会单独显示为 `system=FAIL`，但不使时钟
传输 A/B 失效，前提是 40/80 MHz 两侧都只因该 DMA 门槛失败；如果仅 80 MHz 侧出现
DMA 稳定性失败，当前实验直接拒绝。

专用 TCP 服务日志应按物理执行顺序恰好出现六条 `closed` 记录。逐轮核对设备
`tcp_tx_bytes` 等于服务端 `rx_bytes`；正常停止可能发生在一次 send 完成而 echo 尚未读取
时，因此服务端 `tx_bytes - 设备 tcp_rx_bytes` 允许为 `0..5760 B`。超出一个 payload、
出现额外连接或缺失关闭记录时，该轮稳定性失败，即使设备侧吞吐门槛通过。

可生成相互隔离的 1800 秒 stress 与 28,800 秒 soak 固件，以复现当前样机的 80 MHz
行为或排查问题：

```sh
python3 tests/display/clock_ab_profiles.py prepare --reset \
  --profiles E80-STRESS E80-SOAK
# 依次执行上一个命令打印的两条 idf.py build size 命令
python3 tests/display/clock_ab_profiles.py validate \
  --profiles E80-STRESS E80-SOAK
```

两份物化 sdkconfig 必须完全相同，生成的类型化 profile 只能在
`stress_duration_sec` 上不同。可运行
10 分钟 UI/触摸/按键预检、1800 秒 `STRESS`、冷启动/休眠恢复和 8 小时 full-load soak，
以记录当前样机行为。任何 T2、WDT、panic、OOM、重启、GUI/输入冻结、提交错误、TCP
重连、Wi-Fi 断线或 audio/TCP 错误都使该实验失败并立即回退 40 MHz。即使该轮没有错误，
在取得面板的新规格或供应商书面确认前，80 MHz 仍不得成为生产默认；只因两个时钟共同
存在的 DMA 门槛失败时，结论必须写成 `transport=PASS, system=FAIL`。

当前 TE 模式会同时切换全屏单 buffer 和 PSRAM Direct DMA，不能用于本次撕裂归因。
如需研究 80 MHz 下的 T1 接缝，只制作 bounce 路径的 GPIO13 TE 边沿探针，并把
`TESCAN` 从 465 改为 0。只有连续 10 分钟获得稳定约 60 Hz 且无丢边沿后，才另行设计
TE 与 Direct DMA 解耦测试；不使用无反馈的软件定时模拟 TE。该探针不改变 80 MHz 的
超规格实验属性，也不启用 TE 或 Direct DMA 作为生产路径。

## 生命周期 trace

`CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` 控制 START、MOUNT、RESUME、PAUSE、UNMOUNT、
STOP 和 NEW_INTENT 的 `app_lifecycle` INFO trace。生产默认关闭，关闭时事件名称表和日志
调用也不参与编译；错误、警告、handler、observer 与状态迁移不受影响。需要核对生命周期
顺序时可在开发固件中临时启用，例如输出
`app_lifecycle: [settings][root] ON MOUNT`。

## LVGL 显示调试顺序

显示问题（花屏、撕裂、触摸偏移、卡顿、冻屏）按序排查，多数根因是驱动时序或缓冲
所有权而非 widget 逻辑：

1. 纯色 flush：先以整屏纯色验证 flush 提交与 adapter 管理的
   `lv_display_flush_ready` 握手，再谈 widget；应用层 custom draw callback 不重复调
   ready。
2. draw buffer：行数、颜色格式（`RGB565` 与 `RGB565_SWAPPED` 的字节序）、stride 必须
   与显示路径一致；DMA 路径用 internal/DMA heap（128 KiB 预留），PSRAM 缓冲按
   `MALLOC_CAP_SPIRAM` 分配。
3. tick 与时基：单调 tick 与 `lv_timer_handler` 周期执行；UI 冻结先查此环。
4. 输入：触摸独立校准后再叠加。
5. 内存预算：widget/字体/图片/缓冲总量核对。
6. 性能：帧时间、flush 完成与 `lv_display_flush_ready` 时机；用本文档的基准门槛判定。

常见失败：flush 未调 ready 导致黑屏；16-bit 端序/颜色顺序不符；触摸坐标需旋转
变换；DMA 读到陈旧 cache 行或写非 DMA 内存；字体/图片超 flash/RAM 预算。基准环境
的 `display config` 行会记录 draw/DMA 行数、颜色格式、direct/TE 等，A/B 日志据此
独立识别配置。
