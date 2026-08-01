# 编码风格

适用于工程自有代码（`main/`、`tests/`、`layers/` 中除第三方库外的全部源文件）。规则分三级：

- **必须** — 格式或构建强制，违反即失败；
- **不得** — 红线，禁止行为；
- **建议** — 评审取向，允许合理例外并说明理由。

AStyle 只自动处理布局（缩进、空格、换行），不覆盖命名、Doxygen、日志与错误清理——这些须人工自检。工程示例均取自仓库实际代码。

## 1. 命名

### 1.1 目录与文件

【必须】目录与文件名全小写，单词用 `_` 连接，避免通用化、高频名称。实例：`bsp_audio.c`、`mt_log.h`、`app_runtime.c`。

### 1.2 宏与常量

【必须】宏与枚举常量全大写蛇形：

```c
#define TRUE 1
#define DBG_ERROR 1
```

函数式宏同样全大写，如 `MT_ERROR_HANDLE`。

### 1.3 类型

【必须】结构体名小写蛇形，typedef 名取结构体名加 `_t`，非指针形式：

```c
typedef struct system_pm_wake_event
{
    /* ... */
} system_pm_wake_event_t;
```

【不得】typedef 指针别名（`typedef struct x *x_t;`）；指针一律显式写成 `x_t *p`。仓库实例：`system_pm_config_t *config`。

### 1.4 函数

【必须】函数名小写蛇形；公开 API 必须在头文件中声明；空参数显式写 `void`：

```c
esp_err_t bsp_audio_start(void);
```

【必须】文件内静态函数以下划线前缀，格式 `_<模块>_<动作>`：

```c
static esp_err_t _app_runtime_rtc_alarm_configure(void);
```

【必须】对象方法命名 `<对象名>_<动词短语>`，实例：`bsp_audio_start()`、`wifi_service_stop()`、`app_runtime_pm_build_system_config()`。

## 2. 文件组织

### 2.1 头文件

【必须】唯一 include guard，标识符两侧用 `__` 包裹：

```c
#ifndef __BSP_AUDIO_H__
#define __BSP_AUDIO_H__
/* ... */
#endif /* __BSP_AUDIO_H__ */
```

【必须】头文件自含：包含自身依赖的头文件，能独立编译；只放类型、常量与 API 声明，不放函数实现、静态变量。

【必须】内容用 `extern "C"` 包裹以兼容 C++ 宿主测试：

```c
#ifdef __cplusplus
extern "C" {
#endif
```

【不得】在头文件中定义 `DBG_TAG`（见日志节）或实现函数体。

### 2.2 源文件

【必须】`.c` 顶部先定义 `DBG_TAG`、`DBG_LVL`，再包含 `mt_log.h`（见日志节）。

### 2.3 模块边界

- 文件行数仅是可维护性审查信号，约 1000 行为审查阈值；不设硬上限，不作 CI 失败条件。
- 拆分依据：职责、状态所有权、依赖方向、可独立测试性。不得仅为降低行数拆散同一状态机、共享不变量或逆序清理链，也不得因此新增跨文件可变全局或宽泛内部 API。
- 拆分后须满足：状态唯一所有、依赖单向、接口更窄。任一条件不成立时保留内聚实现，用短函数和清晰分区组织代码。

### 2.4 行宽

【建议】行宽软上限 100 列；长字符串字面量与 Doxygen 注释可放宽，超限优先拆行而非截断语义。

## 3. 布局

### 3.1 缩进与大括号

【必须】4 空格缩进，不使用 Tab；大括号独立成行（Allman 风格）：

```c
if (condition)
{
    /* ... */
}
```

【必须】唯一例外是 `switch`，`case` 与 `switch` 对齐，分支体缩进：

```c
switch (value)
{
case 1:
    break;
default:
    break;
}
```

【必须】连续空行不超过两行。

### 3.2 空格

【必须】`if`、`for`、`while`、`switch` 与括号之间留一空格；二元运算符两侧留一空格；括号内侧不留空格：

```c
if (x <= y)     /* 合法 */
if ( x <= y )   /* 非法：括号内侧空格 */
for (size_t i = 0; i < n; ++i)
```

【必须】函数调用括号前不加空格：`foo(x)`。

【必须】自增自减采用前缀形式且不带空格：`++i`，不得写成 `i ++`。AStyle `--pad-oper` 不会为 `++` 补空格，示例与格式化输出保持一致。

## 4. 注释

【建议】新代码默认中文；中英文不强制统一，但单文件内保持一致。

【必须】注释说明"做什么/为什么"，不逐行翻译代码；语句注释只能写在上方或右方；注释不过量，仅在关键点提示。

【必须】头文件中的公开 API 与结构体写 Doxygen：至少含 `@brief`；有参数或返回值时写 `@param`、`@return`；`@note`、`@see`、`@warning` 按需添加。结构体成员按需在定义处注释。

简洁函数用单行 `@brief`，仓库实例（`bsp_audio.h`）：

```c
/** @brief Report whether board audio is initialized and may be started. */
bool bsp_audio_is_available(void);
```

复杂 API 的多段模板，仓库实例（`bsp_audio.h`）：

```c
/**
 * @brief Change the PCM format while the device is stopped.
 *
 * The sample rate and MCLK ratio must match an ES8311 clock-table entry.
 *
 * @param config is copied by the BSP.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for an unsupported format;
 *         ESP_ERR_INVALID_STATE while running.
 */
esp_err_t bsp_audio_configure(const bsp_audio_config_t *config);
```

各元素描述之间空一行，首列对齐。

## 5. 日志

【必须】日志统一走 `mt_log.h` 的 `LOG_E`、`LOG_W`、`LOG_I`、`LOG_D`、`LOG_V`（内部映射 ESP-IDF 日志）。每个 `.c` 顶部定义 `DBG_TAG` 并设置 `DBG_LVL`：

```c
#define DBG_TAG "app_runtime"
#define DBG_LVL DBG_INFO
#include "mt_log.h"
```

- 【不得】在头文件中定义 `DBG_TAG`，防止包含方互相污染。
- 【不得】用 `printf` 输出日志；`printf` 仅用于终端命令行交互工具。
- 【不得】在中断上下文或高频路径打印大量日志；定时器与热路径日志须限频或轻量化。
- 日志简短且可定位问题；同一错误只在最合适的抽象层记录一次。

## 6. 错误处理与清理

【必须】存在资源释放、锁释放、状态回滚或集中日志义务的函数，用 `goto` 汇入单一 `exit` 出口，并按资源获取或状态变更的逆序清理，避免多分支重复或遗漏清理：

```c
#include "mt_log.h"

static esp_err_t _handle_something(void)
{
    esp_err_t err = ESP_OK;
    uint32_t line = 0;

    err = do_something();
    MT_ERROR_HANDLE(err, line);

    /* More operations may be checked in the same way. */

exit:
    /* Release resources in reverse acquisition order. */
    if (err != ESP_OK)
    {
        LOG_E("handle something fail. %u:%#X", line, err);
    }

    return err;
}
```

【必须】无清理义务的参数校验与普通失败分支直接 `return`，不得为形式上的单一返回点引入 `goto`。

使用 `MT_ERROR_HANDLE`（定义于 `mt_log.h`）的约束：

- `err` 必须是当前函数内的错误变量，跳转前已保存失败结果；`line` 须在函数入口初始化。
- 宏只适用于含 `exit` 标签的函数；不得跨越变长数组作用域或跳转到其他标签；标签名冲突时定义局部等价宏或直接写 `goto`。
- `exit` 中清理按获取逆序执行；清理失败时保留首个业务错误，除非接口明确要求返回清理错误。
- 采用统一出口的函数，成功路径自然落入 `exit`；单一返回点不是普遍要求。
- 错误来自表达式时，先赋给 `err` 再调宏，避免重复求值并保证日志错误码与判断一致。

## 7. 函数

- 【建议】函数仅完成相对独立的简单功能，保持精简、单一职责；实现过长时应反思拆分（拆分理由见 2.3）。
- 【必须】先做参数校验，无清理义务的失败直接返回（见第 6 节）。

## 8. 格式化与提交前自检

### 8.1 AStyle

【必须】格式化基线为 AStyle 3.6.9，覆盖工程自有 `.c`/`.h`：

```sh
astyle --style=allman --indent=spaces=4 --indent-preproc-block --pad-oper \
       --pad-header --unpad-paren --suffix=none --align-pointer=name \
       --lineend=linux --convert-tabs --verbose \
       $(git ls-files '*.c' '*.h')
```

提交前先执行只读自检，通过后再格式化并重新编译、跑宿主测试：

```sh
astyle --style=allman --indent=spaces=4 --indent-preproc-block --pad-oper \
       --pad-header --unpad-paren --suffix=none --align-pointer=name \
       --lineend=linux --convert-tabs \
       --dry-run --error-on-changes \
       $(git ls-files '*.c' '*.h')
```

【必须】检查范围排除构建生成文件（如 `main/mmap_generate_*.h`）与第三方代码：ESP-IDF、`managed_components/`、`layers/bsp/XPowersLib/`。不得为绕过稳定的格式化结果添加文件级排除或 `INDENT-OFF` 标记。

【必须】`layers/` 各目录是独立 Git 子模块：父仓库的 `git ls-files` 不包含子模块内文件，须进入对应子模块目录执行同一命令。

### 8.2 提交前清单

1. AStyle `--dry-run --error-on-changes` 通过；
2. 相关宿主测试套件编译并全绿（命令见 AGENTS.md）；
3. 按本节【必须】项自检命名、Doxygen、日志与清理出口；
4. 硬件、电源、显示、存储相关改动注明真机验证范围，宿主测试不替代上板验证。
