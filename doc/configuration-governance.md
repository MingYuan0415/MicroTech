# 配置与依赖治理

本文描述 MicroTech 工程中配置项、板级事实、运行时策略、协议常量与外部依赖的归属规则，以及机器校验入口。规则以当前实现为准；`tests/` 下的测试是可执行的规范源，本文只陈述其已实现的内容。

## 1. 归属规则

| 类别 | 归属 | 载体 |
| --- | --- | --- |
| 静态资源预算、编译期开关 | Kconfig | 各组件 `Kconfig` / `Kconfig.projbuild` |
| 运行时策略（采样率、时区、SNTP、PCM 格式、挂载路径、天气端点等） | 调用方 | 根 `app_product_config_t` 及其子配置结构 |
| 板级事实（分辨率、总线频率、DMA 几何、引脚、校准） | 板级后端 | `layers/bsp/waveshare/.../board_display_profile.h` 与 `board.cmake` manifest |
| 协议常量与语义 | 契约子模块 | `contracts/device_link`（`protocol.yaml`、`vectors/golden.json`） |
| 外部依赖版本 | manifest + lock | 各 `idf_component.yml` 与根 `dependencies.lock` |

根 `CMakeLists.txt` 的 LVGL / 板级 profile 门槛（缓存、allocator、RGB565、NimBLE SC-only、TLS、PSRAM 外部 BSS 等）属于构建强制项，保持原样，不弱化也不加码。

## 2. 配置规则（机器校验）

以下规则由 `tests/configuration/test_configuration_governance.py` 扫描生产源（`main/`、`layers/`，排除 `tests/`、`managed_components/`、`XPowersLib/`、`build/`、`probe/`）强制执行：

1. 不得使用 `#ifndef CONFIG_*` 形式的编译回退；配置项一律按构建时已解析值使用。
2. 不得重定义 `configTICK_RATE_HZ`。
3. 不得在 `.c`/`.h` 中调用未 pin core 的 `xTaskCreate`、`xTaskCreateStatic`、`xTaskCreateWithCaps`；任务核归属必须显式。
4. 已移除的配置符号不得重新出现。清单见 `test_configuration_governance.py` 的 `DEPRECATED_CONFIG_TOKENS`（涵盖已删除的 `CONFIG_BSP_AUDIO_*`、`CONFIG_BSP_DISPLAY_*`、`CONFIG_APP_MANAGER_DISPLAY_BENCHMARK`、各服务 task/队列旧符号等）。
5. 被移除的配置不得出现在 `sdkconfig.defaults` 与 `tests/display/profile_defaults/*.defaults` 中。
6. 工程 Kconfig 符号集合固定；新增或删除符号必须在同一次变更中同步 `test_project_kconfig_symbol_count` 的期望集合。
7. `sdkconfig.defaults` 的关键连接性/缓存/任务核基线固定；变更须同步 `test_connectivity_defaults` 的期望映射。

配置改动落在 `sdkconfig.defaults`（`idf.py save-defconfig`），不手改 `sdkconfig`。

## 3. 依赖与 manifest 规则

由 `tests/configuration/test_dependency_pins.py`（本治理的一部分）校验：

1. 所有 `idf_component.yml` 的 registry 依赖版本不得为空或 `*`（测试逐项校验字面 `""`/`"*"`；带界范围版本允许）。
2. `idf_component.yml` 中声明的 registry 依赖名称必须存在于根 `dependencies.lock`；版本一致性由组件管理器按 lock 解析保证，测试不逐项比对版本。
3. 根目录 `requirements*.txt` 的每个依赖必须精确 pin（`name==version`），不得使用范围说明符。

修改 `idf_component.yml` 版本后需重新解析并更新 `dependencies.lock`；不得手改 lock。

## 4. 校验入口

```sh
python3 tests/run_python_tests.py configuration
```

该 group 同时覆盖配置规则、Kconfig 符号集、连接性基线、依赖 pin。它是本地/审查用测试入口，不新增 CMake `FATAL_ERROR` 或 CI 闸。