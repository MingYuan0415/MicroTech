# 存储库指南

## 地图

- `main/`：入口、运行时、存储/网络装配；资源由 `layers/apps/*/assets/` 和 `layers/app_manager/app_theme/assets/` manifest 聚合后打包写入 `res` 分区。
- `layers/`：`bsp`、`middleware`、`app_manager`、`apps`，四个独立 Git 子模块。层内改动在子模块内提交并推送，父仓库同一提交更新指针。
- `sim/`：独立宿主 LVGL 模拟器，见 [`sim/README.md`](sim/README.md)；Agent 仅绑 `127.0.0.1:5002`（5001 留给真机基准）。
- `tests/`：`connectivity`、`integration` 为跨层宿主测试；`display` 为真机基准工具。`managed_components/` 只读。

## 默认工作方式

- 只改与任务直接相关的代码；分析任务只读；不改计划外代码，但报告发现的问题。结论基于代码、测试或文档，不猜测。
- **验证最小化**：按「宿主测试与验证分层」的 L0–L3 执行，每个门禁每任务只跑一次。不默认跑全仓库 CTest、sanitizer、`idf.py build`/`size`、显示基准或真机；只有用户明确要求提交、验收或发布验证时才扩大范围，并说明扩大部分。
- **不新增流程门禁**：不得为解决问题新增 CMake `FATAL_ERROR`、Kconfig 强制开关、CI 闸或新的“不得”条款。根 `CMakeLists.txt` 的 LVGL profile 门槛与 `sdkconfig.defaults` 基线保持原样——既不弱化，也不加码。
- 烧录/擦除仅在用户明确要求时执行。

## 构建

```sh
git submodule update --init --recursive
idf.py set-target esp32s3 && idf.py build
idf.py size   # 仅当改动缓存/DMA 预留/资源时检查
```

配置改动落在 `sdkconfig.defaults`（`idf.py save-defconfig`），不手改 `sdkconfig`。`REQUIRES`/`PRIV_REQUIRES` 不依赖 `CONFIG_xxx`；改变源文件发现后运行 `idf.py reconfigure`，不手改生成的 Ninja/CMake 状态。

## 宿主测试与验证分层

每套独立 CMake 工程（C11，`-Wall -Wextra -Werror -Wpedantic`），流程相同：

```sh
cmake -S <套件路径> -B /tmp/mt-<名> -G Ninja && cmake --build /tmp/mt-<名> && ctest --test-dir /tmp/mt-<名> --output-on-failure
```

**影响面**：套件被波及 = 本任务改动了它编译的任何源文件。

| 改动位置 | 受影响套件 |
| --- | --- |
| `main/` | `main/tests/host` |
| `layers/bsp/`、`layers/middleware/` | 对应 `layers/*/tests/host`（bsp 含 C++）；被 `main` 链接的再加 `main/tests/host` |
| `layers/apps/`（页面、app_ui） | `layers/apps/tests/host`、`tests/integration` |
| `layers/app_manager/`（app_core、app_theme） | `layers/app_manager/app_core/tests/host`、`tests/integration` |
| 网络 runtime/连接性 | `tests/connectivity` |
| Kconfig/契约文档、资源 manifest/资产 | `pytest tests/configuration`、`pytest tests/resources`（宿主无 pytest 时仅语法级校验，报告说明请人工补跑） |

`tests/integration` 默认 none profile；sanitizer（`-DMAIN_HOST_SANITIZER=address|thread` 等）仅在改动涉及内存所有权或并发时启用。宿主测试不覆盖驱动时序、射频、DMA、功耗，需要时上板并记录验证范围。

**分层**（对应「验证最小化」）：

- L0 每批编辑：对改动文件直接跑 `doc/code-style.md` §8.1 astyle（幂等）+ 受影响工程增量构建。
- L1 每任务：受影响宿主套件各跑一次。
- L2 UI 改动完成前：`sim/ci/run_ci.sh` ×1 + `sim/tools/review_pages.py --check` ×1；评审分档见 `ui-review` skill。
- L3 提交/验收：`doc/code-style.md` §8.2 完整清单。

被测产物未变化时同一门禁无需重跑；flaky 修复的验证 = 串行一次 + 并行一次。

## 设备故障

仅查看用户当前提供的日志，按序检查：reset reason、panic 回溯、heap 余量、task WDT、启动放置、app 状态迁移。显示基准门槛与配置见 `tests/display/README.md`；内置 UI 宿主回归用 `sim/ci/run_ci.sh`。

## 编码风格

遵循 [`doc/code-style.md`](doc/code-style.md)：4 空格、Allman、大写蛇形宏、小写蛇形函数、静态函数 `_` 前缀、头文件 API 写 Doxygen，用 AStyle 3.6.9 格式化。性能优先；避免非必要防御代码，不为形式统一而加锁、加检查或加单一返回点——无清理义务的失败直接返回，`goto` 单一出口只用于确有释放/解锁/回滚义务的函数。

## 文档风格

说明文档应以实现事实为基准内容，不写实现过程&未实现内容；文档应专注于其负责的模块本身，不应包含其他无关模块的说明。

## 提交

Header `<type>(<scope>): <subject>`，正文写动机与影响，尾部 `Refs:` 或 `BREAKING CHANGE:`；PR 列出变更、测试与资源影响。子模块指针更新必须与内容变更同一提交，禁止 pointer-only bump。仅在用户要求提交/验收时执行完整清单（`git diff --check` + astyle + 受影响宿主套件，见 code-style.md §8.2）。

## Skills

`.agents/skills/` 按需加载，不预加载全部：

| 场景 | Skill |
| --- | --- |
| App/Page 生命周期、路由、导航、参数 | `app-development` |
| LVGL 布局、文本换行、裁切、触摸/滚动传递 | `lvgl-ui-layout` |
| UI 改动完成前的模拟器对抗式审查 | `ui-review` |
| 板级引脚、总线、硬件约束 | `esp32` |
| 显示管线所有权边界、draw 提交、flush 握手 | `lvgl-integration` |

构建/配置/调试/验证的通用流程已在本文件定义，不为它们加载 skill。

优先级：本文件与 `doc/code-style.md` > 当前代码、锁文件及专题文档 > skill 摘要；不用 skill 常量覆盖当前实现。
