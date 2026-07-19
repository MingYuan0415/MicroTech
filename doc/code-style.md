# 编程风格

## 1. 目录名称

目录名称如果无特殊的需求，请使用全小写的形式；目录名称应能够反映部分的意思，例如各硬件驱动由其硬件名称构成或硬件类别构成。

## 2. 文件名称

文件名称如果无特殊的需求（如果是引用其他地方，可以保留相应的名称），请使用全小写的形式。另外为了避免文件名重名的问题，一些地方请尽量不要使用通用化、使用频率高的名称。

设备驱动源码文件：`drv_class.c` 的命名方式，如：
- `drv_spi.c`
- `drv_gpio.c`

## 3. 头文件定义

C 语言头文件为了避免多次重复包含，需要定义一个符号。这个符号的定义形式请采用如下的风格：

```c
#ifndef __FILE_H__
#define __FILE_H__

/* header file content */

#endif
```

即定义的符号两侧采用 `__` 以避免重名，另外也可以根据文件名中是否包含多个词语而采用 `_` 连接起来。

## 4. 结构体定义

结构体名称请使用小写英文名的形式，单词与单词之间采用 `_` 连接，例如：

```c
struct list_node
{
    struct list_node *next;
    struct list_node *prev;
};
```

其中，`{`、`}` 独立占用一行，后面的成员定义使用缩进的方式定义。

结构体等的类型定义请以结构体名称加上 `_t` 的形式作为名称，例如：

```c
typedef struct list_node list_t;
```

## 5. 宏定义

请使用大写英文名称作为宏定义，单词之间使用 `_` 连接，例如：

```c
#define TRUE 1
```

## 6. 函数名称、声明

函数名称请使用小写英文的形式，单词之间使用 `_` 连接。提供给上层应用使用的 API 接口，必须在相应的头文件中声明；如果函数入口参数是空，必须使用 `void` 作为入口参数，例如：

```c
thread_t thread_self(void);
```

内部静态函数命名：以下划线开头，使用 `_class_method` 格式，如内核或驱动文件中的函数命名：

```c
/* IPC object init */
static err_t _ipc_object_init(void);

/* UART driver ops */
static err_t _uart_configure(void);
static err_t _uart_control(void);
```

调用注册设备接口的函数命名：使用 `hw_class_init()` 格式，举例：

```c
int hw_uart_init(void);
int hw_spi_init(void);
```

## 7. 注释编写

注释语言不做强制要求，可根据项目需要选择中文或英文。对于个人项目，推荐使用中文注释，以便于理解和维护。若团队协作或需要与国际开发者交流，则建议使用英文。

**语句注释：** 源代码的注释不应该过多，更多的说明应该是代码做了什么，仅当个别关键点才需要一些相应提示性的注释以解释一段复杂的算法它是如何工作的。对语句的注释只能写在它的上方或右方，其他位置都是非法的。

```c
/* 你的注释，中文或英文均可 */
```

**函数注释：** 注释以 `/**` 开头，以 `*/` 结尾，中间写入函数注释，组成元素如下，每个元素描述之间空一行，且首列对齐：
- `@brief` — 简述函数作用。在描述中，着重说明该函数的作用，每句话首字母大写，句尾加英文句号（若使用中文注释，则遵循中文标点习惯）。
- `@note` — 函数说明。在上述简述中未能体现到的函数功能或作用的一些点，可以做解释说明。
- `@see` — 相关 API 罗列。若有与当前函数相关度较高的 API，可以进行列举。
- `@param` — 以参数为主语 + 动词 + 描述，说明参数的意义或来源。
- `@return` — 枚举返回值 + 返回值的意思，若返回值为数据，则直接介绍数据的功能。
- `@warning` — 函数使用注意要点。在函数使用时，描述需要注意的事项，如使用环境、使用方式等。

注释模版可以参考以下示例（此处使用英文示例，实际可用中文替换）：

```c
/**
 * @brief The function will initialize a static event object.
 *
 * @note For the static event object, its memory space is allocated by the
 *       compiler during compiling, and shall placed on the read-write data
 *       segment or on the uninitialized data segment. By contrast, the
 *       event_create() function will allocate memory space automatically
 *       and initialize the event.
 *
 * @see event_create()
 *
 * @param event is a pointer to the event to initialize. It is assumed that
 *              storage for the event will be allocated in your application.
 * @param name is a pointer to the name that given to the event.
 * @param value is the initial value for the event.
 *              If want to share resources, you should initialize the value
 *              as the number of available resources.
 *              If want to signal the occurrence of an event, you should
 *              initialize the value as 0.
 * @param flag is the event flag, which determines the queuing way of how
 *             multiple threads wait when the event is not available.
 *             The event flag can be ONE of the following values:
 *             IPC_FLAG_PRIO  The pending threads will queue in order of priority.
 *             IPC_FLAG_FIFO  The pending threads will queue in the first-in-first-out
 *                            method (also known as first-come-first-served (FCFS)
 *                            scheduling strategy).
 *             NOTE: IPC_FLAG_FIFO is a non-real-time scheduling mode. It is strongly
 *             recommended to use IPC_FLAG_PRIO to ensure the thread is real-time
 *             UNLESS your applications concern about the first-in-first-out principle,
 *             and you clearly understand that all threads involved in this event
 *             will become non-real-time threads.
 *
 * @return Return the operation status. When the return value is EOK, the
 *         initialization is successful. If the return value is any other
 *         values, it represents the initialization failed.
 *
 * @warning This function can ONLY be called from threads.
 */
err_t event_init(event_t event, const char *name, uint8_t flag)
{
    ...
}
```

## 8. 缩进及分行

缩进请采用 4 个空格的方式。如果没有什么特殊意义，请在 `{` 后进行分行，并在下一行都采用缩进的方式，例如：

```c
if (condition)
{
    /* others */
}
```

唯一的例外是 `switch` 语句，`switch-case` 语句采用 `case` 语句与 `switch` 对齐的方式，例如：

```c
switch (value)
{
case value1:
    break;
}
```

`case` 语句与前面的 `switch` 语句对齐，后续的语句则采用缩进的方式。

分行上，如果没有什么特殊考虑，请不要在代码中连续使用两个以上的空行。

## 9. 大括号与空格

从代码阅读角度，建议每个大括号单独占用一行，而不是跟在语句的后面，例如：

```c
if (condition)
{
    /* others */
}
```

匹配的大括号单独占用一行，代码阅读起来就会有相应的层次而不会容易出现混淆的情况。

空格建议在非函数方式的括号调用前留一个空格以和前面的进行区分，例如：

```c
if (x <= y)
{
    /* others */
}

for (index = 0; index < MAX_NUMBER; index ++)
{
    /* others */
}
```

建议在括号前留出一个空格（涉及的包括 `if`、`for`、`while`、`switch` 语句），而运算表达式中，运算符与字符串间留一个空格。

另外，不要在括号的表达式两侧留空格，例如：

```c
if ( x <= y )   /* 这样括号内两侧的空格是不允许的 */
{
    /* other */
}
```

## 10. 日志信息

代码中多使用日志模块的方式来输出日志，例如：

```c
#define DBG_TAG "Driver"
#define DBG_LVL DBG_INFO
#include <log.h>

LOG_D("this is a debug log.");
```

- 普遍使用的日志输出方式是通过 `LOG_D`、`LOG_I`、`LOG_W`、`LOG_E` 的方式来输出日志，同时它也可以通过 `DBG_TAG` 来区分日志类别，`DBG_LVL` 控制日志输出的等级。
- 日志应该是以输出易懂易定位问题的方式。"天书式"的日志系统是糟糕的，不合理的，不应该出现在代码中。
- 禁止在头文件中重定义 `DBG_TAG`，防止其他模块包含时 `DBG_TAG` 出现不可控。
- 严禁在 timer 或者中断打印大量日志，尽可能的避免或轻量化。
- 不建议使用 `printf` 来作为日志输出方式，`printf` 一般作为终端命令行交互使用。

## 11. 错误处理与清理

函数存在资源释放、锁释放、状态回滚等清理义务时，应使用 `goto`
汇入单一清理出口，并按资源获取或状态变更的逆序清理，避免在多个分支中
重复清理或遗漏清理。分阶段初始化等有序操作，仅在失败后需要共享清理或
回滚路径时采用这种写法；普通参数校验、无清理义务的失败分支可以直接
`return`，不得为了形式上的单一返回点引入 `goto`。

函数包含多个需要区分和打印的失败分支时，也可保存错误码和源码行号，仅在
统一出口输出一次必要日志。`MT_ERROR_HANDLE` 定义在 `mt_log.h` 中，包含该
头文件即可使用（`__LINE__` 用于记录触发错误的源码行）：

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

使用该宏时应遵守以下约束：

- `err` 必须是当前函数中的错误变量，并在跳转前保存失败结果；`line` 应在函数入口初始化，避免未初始化日志字段。
- 宏只适用于包含 `exit` 标签的函数，不能在需要跳转到其他标签或跨越变长数组作用域的场景使用；标签名称冲突时应定义局部等价宏或直接使用 `goto`。
- `exit` 标签中的清理按资源获取的逆序执行；清理失败时保留首个业务错误，除非接口明确要求返回清理错误。
- 对采用统一清理出口的函数，成功路径应自然落入或跳转到 `exit`，由该出口完成共同清理并返回。单一返回点不是普通函数的普遍要求；没有清理或集中日志需要时，应优先直接返回。
- 同一错误只在合适的抽象层记录一次，包含简洁的操作名称；使用行号宏时再附行号和错误码。高频路径、定时器和中断上下文应避免日志或进行限频。

如果错误来自表达式而不是变量，先将表达式结果赋给 `err` 再调用宏，避免重复求值并确保日志记录的错误码与判断一致。

## 12. 函数

在编程中，函数应该尽量精简，仅完成相对独立的简单功能。函数的实现不应该太长，函数实现太长，应该反思能够如何修改（或拆分）使得函数更为精简、易懂。

## 13. 文件与模块边界

文件行数仅作为可维护性审查信号，不设置硬性上限。约 1000 行的工程自有源文件应在评审中说明保留或拆分理由，但不得作为 CI 失败或强制拆分条件。拆分必须以职责、状态所有权、依赖方向和可独立测试性为依据，低耦合、高内聚优先于缩短文件。

不得仅为降低行数而拆散同一状态机、共享不变量或资源逆序清理链，也不得因此新增跨文件可变全局或宽泛的内部 API。拆分后应保持状态唯一所有、依赖单向且接口更窄；任一条件不成立时，应保留内聚实现并使用短函数和清晰分区组织代码。

## 14. 对象

采用了 C 语言对象化技术，命名表现形式是：对象名结构体表示类定义、对象名 + 动词短语形式表示类方法，例如：

```c
struct timer
{
    struct object parent;
    /* other fields */
};

typedef struct timer* timer_t;
```

结构体定义 `timer` 代表了 timer 对象的类定义；

```c
timer_t timer_create(const char *name,
                     void (*timeout)(void *parameter),
                     void *parameter,
                     tick_t time,
                     uint8_t flag);
err_t timer_delete(timer_t timer);
err_t timer_start(timer_t timer);
err_t timer_stop(timer_t timer);
```

`timer` + 动词短语的形式表示能够应用于 timer 对象的方法。

在创建一个新的对象时，应该思考好，对象的内存操作处理：是否允许一个静态对象存在，或仅仅支持从堆中动态分配的对象。

## 15. 格式化代码

格式化代码是指通过脚本自动整理你的代码，并使其符合编码规范。本文提供以下两种自动格式化代码方法，可以自行选择或配合使用。

### 使用 astyle 格式化

当前格式基线为 AStyle 3.6.9。用 astyle 自动格式化工程自有代码，参数如下：

```
--style=allman
--indent=spaces=4
--indent-preproc-block
--pad-oper
--pad-header
--unpad-paren
--suffix=none
--align-pointer=name
--lineend=linux
--convert-tabs
--verbose
```

能满足函数空格、缩进、函数语句等的规范。

提交前在上述参数后增加 `--dry-run --error-on-changes` 执行只读检查，并在格式化后重新编译和测试。检查范围不包含构建生成文件（如 `main/mmap_generate_*.h`）以及 ESP-IDF、`managed_components/`、`layers/bsp/XPowersLib/` 等第三方代码；不得为了绕过稳定的格式化结果添加文件级排除或 `INDENT-OFF` 标记。

### 使用 formatting 格式化

使用 [formatting](https://github.com/mysterywolf/formatting) 扫描文件来格式化代码，可以满足编码规则的基本要求，如：
- 将源文件编码统一为 UTF-8
- 将 TAB 键替换为 4 空格
- 将每行末尾多余的空格删除，并统一换行符为 '\n'
