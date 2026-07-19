# 存储库指南

## 项目结构与模块组织

`main/` 放入口、运行时和存储/网络装配，`main/res_fs/` 放打包资源。`layers/` 分为 `bsp`、`middleware`、`app_manager`、`apps`；宿主测试位于 `tests/`、`main/tests/host` 或模块 `tests/host`。`doc/` 放规范，根目录保存构建配置和分区表。

## 构建与开发命令

初始化：`git submodule update --init --recursive`。构建：`idf.py set-target esp32s3 && idf.py build`；烧录：`idf.py -p <PORT> flash monitor`；资源检查：`idf.py size`。

## 编码风格与设计原则

严格遵循 [`doc/code-style.md`](doc/code-style.md)：4 空格、Allman、大写蛇形宏、小写蛇形函数，静态函数加 `_` 前缀；头文件 API/结构体写 Doxygen，并运行 AStyle。性能优先，再减少 ROM/RAM；文件长度仅作审查信号，低耦合、高内聚优先，不为缩短文件拆散共享状态；避免非必要防御，硬件/并发场景保留必要防护。存在释放、解锁、回滚或集中日志义务的有序操作，以 `goto` 汇入单一清理出口并逆序清理；无清理义务的参数校验和普通失败直接返回，不为单一返回点强行使用 `goto`。日志简短，多失败点可在出口统一记录行号。

## 测试指南

主套件：`cmake -S main/tests/host -B /tmp/mt-tests -G Ninja && cmake --build /tmp/mt-tests && ctest --test-dir /tmp/mt-tests`；其他层指向各自 `tests/host`。CTest 无覆盖率门槛；行为变更须补回归测试，并按风险运行 sanitizer。硬件、电源、显示和存储改动须上板验证。

## 提交与拉取请求指南

历史样本较少；新提交统一采用 **Header-Body-Footer**。Header 为 `<type>(<scope>): <subject>`；Body 空一行说明动机和影响；Footer 再空一行写 `Refs:` 或 `BREAKING CHANGE:`。PR 列出变更、测试和资源影响；界面附截图，配置说明迁移与回退。

## 修改边界

不得修改 ESP-IDF、`managed_components/` 等外部代码，优先升级依赖。分析任务只读；未授权时不改计划外代码，但应报告问题。结论须基于代码、测试或文档，不得猜测。
