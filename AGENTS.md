# 存储库指南

## 项目结构

- `main/`：入口、运行时、存储/网络装配与显示基准；资源由 `layers/apps/*/assets/` 和
  `layers/app_manager/app_theme/assets/` manifest 聚合后打包写入 `res` 分区。
- `layers/`：`bsp`、`middleware`（`components/` 下按服务拆分）、`app_manager`、`apps`。四个目录均为独立 Git 子模块仓库（远程均属 MingYuan0415），层内改动须在子模块内提交并推送，父仓库再提交指针更新。
- `sim/`：独立 CMake 宿主 LVGL 模拟器，不接入根 ESP-IDF 构建；渲染 `layers/` 真源，
  Agent 绑 `127.0.0.1:5002`。操作见 [`sim/README.md`](sim/README.md)，详细说明见
  [`doc/lvgl-simulator.md`](doc/lvgl-simulator.md)。
- `tests/`：`connectivity`、`integration` 为跨层宿主测试；`display` 为真机基准的主机工具与 Python unittest。`managed_components/` 由 Component Manager 生成，只读。

## 构建与烧录

```sh
git submodule update --init --recursive
idf.py set-target esp32s3 && idf.py build
idf.py -p <PORT> flash monitor
idf.py size   # 改缓存/DMA 预留/资源后检查
```

`sdkconfig` 已 gitignore：改动须落在 `sdkconfig.defaults`（用 `idf.py save-defconfig`），不得手改 sdkconfig。根 `CMakeLists.txt` 用 `FATAL_ERROR` 强制 LVGL profile（libc allocator/string、RGB565、32K I-cache/64K D-cache、128 KiB 内部 SPIRAM 预留），不得为规避问题弱化这些门槛。

CMake 纪律：`REQUIRES`/`PRIV_REQUIRES` 不得依赖 `CONFIG_xxx`（组件依赖图在配置加载前展开）；移动源文件或改变 CMake 源发现后运行 `idf.py reconfigure`，不得手工修补生成的 Ninja/CMake 状态。烧录会改变硬件状态，仅在明确要求时执行。

## 调试工作流

设备故障（启动失败、panic、卡死）时先取最近日志，按序检查：reset reason、panic 回溯、heap 余量、task WDT、启动放置（startup placement）、app 状态迁移。硬件相关改动须上板验证并记录验证范围。

## 宿主测试

每套独立 CMake 工程，C11、`-Wall -Wextra -Werror -Wpedantic` 编译：

```sh
cmake -S main/tests/host -B /tmp/mt-main -G Ninja && cmake --build /tmp/mt-main && ctest --test-dir /tmp/mt-main --output-on-failure
```

- 同流程还用于 `tests/connectivity`、`tests/integration` 及 `layers/*/tests/host`（bsp 测试含 C++，需宿主编译器支持）。
- sanitizer：`-DMAIN_HOST_SANITIZER=address|thread`（另有 `CONNECTIVITY_SANITIZER`、`CROSS_LAYER_SANITIZER`，默认 none）；行为变更按风险启用。
- 宿主测试不替代真机验证：驱动时序、射频、DMA、功耗与资源占用须上板。

## LVGL 宿主模拟器（sim/）

Linux/WSL2 上以仓库内 LVGL 9.5 渲染内置 UI，供页面调试、Agent 驱动与控件树回归。
**不**定义 `HOST_TEST`（mailbox 仍走 LVGL worker 的 `lv_timer`）；**不**改 `layers/`
四个子模块；**禁止**用发行版 `libfreetype-dev` 链接（树内 FreeType 2.14.3）。
前置：`managed_components/` 已填充，且一次 `idf.py build` 已生成
`build/config/sdkconfig.h`（sim 的 CONFIG 镜像来源）。缺失则 CMake `FATAL_ERROR`，
不 FetchContent。

```sh
python3 sim/dev.py
# 或
cmake -S sim -B build/sim -G Ninja && cmake --build build/sim
sim/ci/run_ci.sh build/sim
```

Agent 仅绑 `127.0.0.1:5002`（5001 留给真机显示基准）。主门禁为
`sim/ci/scenarios/` 控件树断言；PNG 金样默认关闭（`SIM_PNG_GOLDEN=1` 或
`run_ci.sh --update`）。有意改 UI 文案/布局须在同一提交更新场景断言。模拟器不覆盖
QSPI/TE/DMA/功耗路径，不替代上板验证。

## 显示基准（tests/display）

仅在硬件上执行：Kconfig 同时启用 `APP_MANAGER_DISPLAY_DIAGNOSTICS`、`MAIN_DISPLAY_BENCHMARK`（生产配置必须关闭），并通过 `DISPLAY_BENCHMARK_PROFILE_DIR` 提供由严格 JSON 生成的类型化 profile。主机端运行 `python3 tests/display/tcp_echo_server.py --host 0.0.0.0 --port 5001`，设备默认连 `192.168.0.205:5001`；WSL mirrored 网络模式需 PowerShell 的 Hyper-V 防火墙放行 TCP 5001。验收门槛与配置基线详见 `tests/display/README.md`；Waveshare 板固定 40 MHz、bounce/10、queue 2、非 TE，LVGL 默认使用 60 行 draw buffer。

## 编码风格

遵循 [`doc/code-style.md`](doc/code-style.md)：4 空格、Allman、大写蛇形宏、小写蛇形函数，静态函数加 `_` 前缀；头文件 API/结构体写 Doxygen；用 AStyle 3.6.9 格式化。性能优先，再减少 ROM/RAM；低耦合、高内聚优先，不为缩短文件拆散共享状态；避免非必要防御，硬件/并发场景保留必要防护。存在释放、解锁、回滚或集中日志义务的有序操作以 `goto` 汇入单一清理出口并逆序清理；无清理义务的参数校验和普通失败直接返回。日志简短，多失败点可在出口统一记录行号。

## 提交与 PR

提交统一 **Header-Body-Footer**：Header 为 `<type>(<scope>): <subject>`，空一行写动机与影响，再空一行写 `Refs:` 或 `BREAKING CHANGE:`。PR 列出变更、测试与资源影响；界面附截图；配置说明迁移与回退。`layers/` 的改动提交到各自子模块仓库并推送，父仓库同步子模块指针。提交前运行 `git diff --check` 并过一遍宿主测试与 astyle 自检（见 doc/code-style.md）。

**父仓库子模块指针纪律**：指针更新必须随内容提交一起（`git add <子模块>` 与本次变更同一提交），禁止单独的 pointer-only bump 提交；同一子模块的多次小步提交在父仓库只体现一次指针变化。历史整理重写历史后必须核对最终树与重写前一致（`git rev-parse HEAD^{tree}`）。

## Agent Skills

仓库根 `.agents/skills/` 提供七个职责收窄的 skill：`app-development` 负责内置 App/Page
的生命周期、路由、导航、参数与应用层资源所有权；`lvgl-ui-layout` 负责 LVGL 9
控件布局、Flex/Grid、文本测量与换行、裁切/遮挡及触摸传递；`esp-idf` 负责组件、配置、构建与
设备工作流；`esp32` 只负责当前 ESP32-S3/Waveshare 板的 BSP 与硬件约束；
`lvgl-integration` 只负责现有 `esp_lvgl_adapter` 显示管线、缓冲、flush、生命周期与性能；
`debug-esp32s3` 负责串口、ELF、core dump、OpenOCD/JTAG/GDB、task/WDT/heap 及 USB
环境的现场证据；`validate-firmware` 负责按变更风险编排宿主测试、sanitizer、构建、size、
真机与长稳验证并限定验收结论。应用任务先用 `app-development` 确认页面契约，涉及控件
自适应布局、文本显示或事件/滚动传递时再用 `lvgl-ui-layout`；涉及工程
结构或配置时再用 `esp-idf`，并按事实所有权加载
`esp32` 或 `lvgl-integration`，需要现场诊断时加载 `debug-esp32s3`，需要验证范围或结论时
加载 `validate-firmware`。涉及内置 UI 的宿主回归可跑 `sim/ci/run_ci.sh`，但不替代
`validate-firmware` 的真机范围。优先级：本文件与 `doc/code-style.md` > 当前代码、锁文件及仓库
专题文档 > skill 中的摘要；不得用 skill 中的常量覆盖当前实现。skill 均采用 MIT 许可；
属于第三方改编的项目在各自 `LICENSE` 中记录来源。

## 修改边界

不得修改 ESP-IDF、`managed_components/`、BSP 内第三方库（如 XPowersLib）；优先升级依赖。分析任务只读；未授权时不改计划外代码，但应报告问题。结论须基于代码、测试或文档，不得猜测。
