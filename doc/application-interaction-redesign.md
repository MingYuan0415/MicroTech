# MicroTech 应用与交互重构设计

## 1. 文档定位

本文是 MicroTech 设备应用层和用户交互的产品重构基线，适用于当前
Waveshare ESP32-S3 Touch AMOLED 1.8 硬件、ESP-IDF 6.0.2、LVGL 9.5.0、
`esp_lvgl_adapter` 0.6.2、`device-link/v1` 和现有 App Manager 架构。

本文同时记录当前实现和目标实现：

- “当前”表示代码已经提供的能力，不代表每个页面都已经达到最终产品质量。
- “规划”表示本轮重构要实现的目标，不应在没有代码和测试证据时写入发布说明。
- 本文只描述设备侧产品和交互，不改变 Device Link 契约，也不规定 Android 实现。

产品目标不是制作一个缩小版手机，而是把设备做成一个可长期放在桌面、也可以随身携带的联网信息终端：开机即可查看时间、天气、提醒和设备状态；通过少量触摸完成计时、录音和设备管理；网络暂时不可用时，核心离线功能仍然可用。

## 2. 产品边界

### 2.1 硬件事实

| 能力 | 当前事实 | 产品含义 |
| --- | --- | --- |
| 显示 | 368 x 448 SH8601A QSPI AMOLED，RGB565，40 MHz，非 TE | 适合高对比度、信息密度适中的纵向页面；不追求连续复杂动画 |
| 触摸 | FT5x06 兼容触摸路径 | 适合点击、上下滚动、横向分页和有限的边缘返回手势 |
| Wi-Fi | `connectivity_manager` 提供保存、连接、重连、忘记、自动连接和状态快照 | 支撑天气、时间校准和后续 OTA |
| BLE | Device Link v1，LE Secure Connections Numeric Comparison，手机配网 | 设备不负责扫描 Wi-Fi 或输入密码，手机负责下发凭证 |
| RTC | PCF85063，支持日历闹钟；中断经 TCA9554 轮询 | 支持清醒状态提醒；不能把当前实现描述成可靠关机闹钟 |
| 电源 | AXP2101，电池、电压、充电和 VBUS 状态 | 支撑电量摘要、低电量提示和显示策略 |
| IMU | QMI8658C，加速度、角速度、温度，默认 100 Hz | 支撑水平仪和诊断；没有 GPS、磁力计或健康传感能力 |
| 音频 | ES8311/NS4150B，全双工 PCM，16 kHz，16 bit，双声道 | 支撑 WAV 录音和短音效；不承诺 MP3 播放 |
| SD | SDSPI，可移除存储，默认挂载到 `/sdcard` | 支撑录音文件和受控诊断；不能假设卡永久存在 |
| 低功耗 | 当前轻睡眠的可靠唤醒源只有 HOME GPIO0 | 进入睡眠前必须停止前台资源，不能承诺 IMU/RTC/触摸唤醒 |

详细的板级事实以 [`layers/bsp/README.md`](../layers/bsp/README.md) 为准；应用不得绕过 BSP 直接拥有总线、GPIO、面板、电源或音频驱动。

### 2.2 产品取舍

第一版产品应该优先交付：

1. 可恢复的首次启动引导。
2. 一眼可读的 Home 总览。
3. Weather 的离线缓存和异常状态呈现。
4. 持续运行的时钟、倒计时、秒表和专注计时。
5. 清晰的网络、蓝牙、显示、电源、存储和设备维护设置。
6. WAV 录音和基础文件管理。
7. 隐藏的 Diagnostics，保留硬件验证能力但不干扰普通用户。

第一版不做手机通知同步、语音助手、音乐播放器、指南针、计步、心率、地图、雷达、AQI 和通用文件管理器。这些功能要么超出当前硬件能力，要么需要新的协议、服务端和长期后台策略。

## 3. 总体架构

### 3.1 分层框图

```text
+-----------------------------------------------------------------------+
|                              用户交互层                              |
|  触摸点击/滚动/分页       HOME 单击/双击       启动向导/错误重试       |
+-----------------------------------+-----------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                          Sys Layer / App Manager                      |
|                                                                       |
|  App 注册表  ->  静态 App route  ->  Page 生命周期  ->  LVGL Screen   |
|       |                 |                    |              |          |
|  应用目录/名称/图标      |                    |         显示/触摸适配器 |
|  隐藏/固定策略            |                    |              |          |
|  最近应用系统面板        +--------------------+              |          |
|  导航 command gateway / mailbox / transition / PM             |          |
+-----------------------------------+-----------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                          产品业务服务层                               |
|                                                                       |
|  connectivity    device_link    weather    time    timer    recorder   |
|  power           sd_storage     audio     imu     update   event_bus   |
|                                                                       |
|  服务拥有 worker、硬件会话、持久状态和不可变 snapshot；页面只订阅和发命令 |
+-----------------------------------+-----------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                              BSP / 系统层                             |
|  ESP32-S3 | AMOLED/QSPI | FT5x06 | TCA9554 | AXP2101 | RTC | IMU | I2S |
|  Wi-Fi/BLE host | SDSPI | NVS/LittleFS | FreeRTOS | ESP-IDF           |
+-----------------------------------------------------------------------+
```

### 3.2 数据和命令流

```text
硬件/网络事件
    -> 所属 service worker
    -> service lock 下更新 snapshot 和 generation
    -> event_bus 发布“generation changed”
    -> 前台 Page 在 UI worker 中 acquire snapshot
    -> 只更新本 Page 的 LVGL 对象

用户操作
    -> LVGL event callback
    -> app_ui_request_* 或 app_manager_navigate_async
    -> App Manager command gateway
    -> UI worker / 对应 service API
    -> 异步结果 snapshot/event
    -> 页面刷新、成功、失败或重试
```

页面不得直接执行网络请求、JSON 解析、文件 I/O、PCM 读写、I2C 事务或长时间阻塞操作。页面 worker 只允许作为服务适配器的临时过渡；最终应将持续业务会话收敛到独立 service。

### 3.3 所有权边界

| 对象 | 唯一 owner | 页面允许的行为 |
| --- | --- | --- |
| GPIO、I2C、SPI、I2S、面板、电源 | BSP | 通过服务 API 间接使用 |
| Wi-Fi/BLE host 和连接会话 | connectivity/device-link | 读取状态、提交命令 |
| 网络凭证、天气缓存、录音文件 | 对应 service | 读取无敏感内容的 snapshot |
| LVGL Screen 和控件 | App Manager + 当前 Page | 在 MOUNT 创建，UNMOUNT 销毁 |
| 前台 timer、订阅、输入和活动会话 | 当前 Page | RESUME 获取，PAUSE 释放 |
| 应用元数据和 route | App 编译单元 | 通过静态 registry 枚举，不动态注册 |
| 最近任务面板 | App Manager Sys Layer | 不属于任何业务 App |

## 4. App Manager 与应用注册

### 4.1 注册表原则

`APP_MANAGER_APP_EXPORT` 是应用的唯一静态元数据入口。它目前支持：

- 应用稳定 ID；
- `display_name` 本地化显示名称；
- 可选 `icon`；
- `APP_MANAGER_APP_FLAG_PINNED` 和 `APP_MANAGER_APP_FLAG_HIDDEN`；
- 应用 root page 和完整的静态 route 表。

宏定义见 [`layers/app_manager/app_core/include/app_manager.h`](../layers/app_manager/app_core/include/app_manager.h)，描述类型见 [`app_manager_types.h`](../layers/app_manager/app_core/include/app_manager_types.h)。应用目录必须遍历 App Manager builtin registry，不能再维护第二份应用 ID、名称或可见性列表。

目录枚举规则：

1. 跳过 Home；Home 是固定入口，不在自己的应用目录中重复出现。
2. 跳过带 `APP_MANAGER_APP_FLAG_HIDDEN` 的应用；Diagnostics 由此隐藏。
3. 显示 `display_name`，为空时回退到 `name`，不直接显示内部 app ID。
4. 点击后通过 `app_ui_request_run(app->id)` 异步导航。
5. registry 无效或为空时显示服务错误，不构造伪造菜单项。

### 4.2 图标约定

当前 `icon` 字段类型为 `const void *`，但现有应用均传入 `NULL`。正式使用前必须统一约定，推荐使用产品语义图片 ID，并由 App Manager 通过已有的 `app_manager_get_image()` 解析为受生命周期管理的 `lv_image_dsc_t`。

推荐优先级：

1. 应用语义图标资源；
2. 应用提供的静态 LVGL image descriptor；
3. 统一的内置 symbol；
4. 无图标时使用一致的几何占位符。

图标资源不能由页面保存路径、mmap 句柄或可变 payload。资源对象的有效期必须覆盖页面使用期，并在页面销毁后再释放 App Manager 资源。

### 4.3 Page 生命周期

生产 Page 使用 typed lifecycle ops，遵循：

```text
ONSTART -> ONMOUNT -> ONRESUME -> ONPAUSE -> ONUNMOUNT -> ONSTOP
                         ^                         |
                         +------ ONNEWINTENT ------+
```

- `ONSTART`：初始化非视觉状态和无效句柄。
- `ONMOUNT`：只在 `context->screen` 下创建完整 UI。
- `ONRESUME`：启动 timer、订阅、输入、worker 和前台服务会话。
- `ONPAUSE`：逆序停止前台资源；失败时保留有效句柄以便重试。
- `ONUNMOUNT`：删除页面 UI，清空所有 LVGL 指针。
- `ONSTOP`：释放仍由 Page 持有的非视觉资源，不重复执行 PAUSE。
- `ONNEWINTENT`：校验并复制 Typed Blob，需要时立即刷新已挂载页面。

页面私有状态只能保存句柄、指针、标志和小型快照；大数组、天气正文、音频缓冲和图片 payload 必须由对应服务或独立内存拥有。所有 definition 的 `memory_size` 必须不超过 `APP_MANAGER_PAGE_STATE_BYTES`。

## 5. 启动、首次引导和恢复出厂

### 5.1 启动状态机

```text
                    +----------------------+
                    | 读取 onboarding 状态 |
                    +----------+-----------+
                               |
              +----------------+----------------+
              |                                 |
              v                                 v
       未完成/版本过旧                    已完成或已跳过
              |                                 |
              v                                 v
       Setup 引导模式                         Home
              |                                 |
       +------+-------+                         |
       |              |                         |
       v              v                         |
  完成并保存       稍后设置 --------------------+
       |              |
       +------> 持久化完成标志                 |
                                              |
                                              v
                              Home 显示“完成设备设置”入口
```

首次启动和恢复出厂后的 Setup 引导值得保留，但必须是可退出、可恢复的软门控：没有手机、路由器或互联网时，用户仍应能进入 Home 使用时间、计时、设置和离线缓存。

### 5.2 持久状态

根运行时维护独立的 `onboarding_version` 或等价状态：

- key 不存在或版本低于当前版本：下次启动进入引导。
- 用户完成引导或明确选择“稍后设置”：保存当前版本。
- 恢复出厂：在 reset journal 完成后删除该 key。
- 普通断网、忘记网络、解除 BLE 绑定：不清除该 key。

不能使用“没有 bond 且没有 Wi-Fi profile”推断首次启动，否则用户主动解除绑定后会被错误地重新送入向导。

### 5.3 Setup 引导页面

Setup App 保持独立，不并入 Settings，因为它拥有 BLE pairing window 和恢复入口。

引导页面建议为：

1. 欢迎：说明设备用途和需要手机完成的步骤。
2. 手机连接：打开绑定窗口，提示用户在 App 中发现设备。
3. Numeric Comparison：显示六位数字，提供“确认绑定”和“拒绝”。
4. Wi-Fi 下发：等待手机写入凭证，展示连接中、获取地址、失败和取消。
5. 完成：确认 `IP_READY`，保存 onboarding 完成状态，提供“开始使用”。

必须处理：

- BLE 窗口超时后可重新开启；
- 手机取消、设备拒绝、断连后清除 pending confirmation；
- 收到凭证但连接失败时允许重试，不宣称已完成；
- “稍后设置”不删除已绑定状态和已有网络；
- 首次向导中的所有敏感字段只由服务层持有，页面只显示 SSID 和分类结果。

日常管理模式继续提供：已保存网络、重连、断开、自动连接、忘记网络、重新绑定和解除绑定。

### 5.4 恢复出厂顺序

```text
用户确认恢复出厂
    -> 写入 reset journal
    -> 重启
    -> 启动时清除 Wi-Fi profile
    -> 清除 BLE bond/CCCD
    -> 完成 reset journal
    -> 释放 BLE startup gate
    -> 设置 onboarding 未完成
    -> 首次启动 Setup 引导
```

任何中间步骤失败都必须保持 fail-closed，不能在旧凭证、旧 bond 或未完成 reset 的状态下开放普通配对窗口。

## 6. 全局交互模型

### 6.1 触摸和 Home 键

| 输入 | 行为 |
| --- | --- |
| 点击 action row | 提交异步 RUN 或 OPEN_PAGE |
| 点击 command row | 提交服务命令，页面显示 queued/running/result |
| 上下滑动 | 只由当前 Page 的 scroll container 消费 |
| 横向分页 | 仅用于 Weather 分段或应用目录分页，不与页面滚动抢手势 |
| 左边缘返回 | App Manager Sys Layer 策略，页面不自行实现第二套返回系统 |
| HOME 单击 | 进入 Home root |
| HOME 双击 | 打开最近使用系统面板 |
| POWER/自动熄屏 | 进入 App Manager PM 流程，先暂停页面和服务会话 |

输入事件必须经过 App Manager 的统一路由。Page 不创建第二个 LVGL task、tick、Screen manager 或物理输入驱动。

### 6.2 最近使用面板

最近使用和应用目录是两个不同功能：

- 应用目录回答“设备上有哪些应用”；
- 最近使用回答“刚才使用了什么”。

建议保留双击 Home，但将当前截图卡片式切换器收敛为紧凑列表：图标、名称、当前状态、关闭按钮。Home 固定 pinned，Diagnostics 和其他 hidden App 不出现在列表中。最近使用不应成为业务 App，也不应持有 Weather、Wi-Fi 或录音状态。

在当前有限的驻留容量下，最近使用只需要展示最多三个业务 App；不必为每个 App 保存大尺寸 PSRAM 页面截图。若保留预览图能力，必须将其作为可选增强，不能成为导航成功的前置条件。

## 7. 应用总览

| 应用 | 当前状态 | 目标定位 | 优先级 |
| --- | --- | --- | --- |
| Setup | BLE/Wi-Fi 管理已可用 | 首次启动引导 + 日常连接管理 | P0 |
| Home | 时间、状态和固定入口 | 今日总览和离线首页 | P0 |
| Weather | 已按新标准实现 | 天气、预警、缓存和网络状态 | P0 |
| Clock | RTC/SNTP demo | 持续运行的时钟、倒计时、秒表、专注计时 | P0 |
| Settings | 多项设置已可用 | 系统设置中心和维护入口 | P0 |
| Applications | 当前没有正式目录 | 遍历 registry 的应用目录 | P0 |
| Recorder | 还没有产品页面 | WAV 录音、列表、播放和删除 | P1 |
| Level | 当前为 IMU 原始数据 demo | 水平仪和倾角工具 | P1 |
| Diagnostics | 当前内容位于演示中心 | 隐藏硬件诊断和工程验证 | P1 |
| Recent Apps | App Manager Sys Layer 已有雏形 | 紧凑的最近使用和关闭面板 | P1 |

## 8. 应用详细设计

### 8.1 Home：今日总览

#### 目标

Home 是每次正常启动后的第一页面，不是应用列表，也不是硬件状态转储。

#### 页面结构

```text
+---------------------------------------+
| 状态栏：连接/电量/充电/异常提示       |
+---------------------------------------+
|                                       |
|              12:34                    |
|        8月30日  星期日                 |
|        网络时间 / RTC 时间质量          |
|                                       |
+---------------------------------------+
| 今日天气：温度  条件  高/低温  预警    |
+---------------------------------------+
| 下一项：倒计时/专注任务/无             |
+---------------------------------------+
| 天气 | 时钟 | 录音 | 设置              |
|              应用                     |
+---------------------------------------+
```

#### 功能

- 显示本地时间、日期和时间质量。
- Weather 可用时显示当前温度、条件、今日高低温和预警摘要。
- Weather 离线时显示最后一次有效缓存及“数据已过期/等待联网”。
- 显示电量、充电和 VBUS 状态；异常时提高视觉优先级。
- 显示 Wi-Fi 连接状态；未完成首次设置时显示 Setup 入口。
- 显示正在运行的倒计时或专注任务摘要。
- 提供 Weather、Clock、Recorder、Settings 和 Applications 的固定入口。

#### 不做

- 不遍历所有 App 生成动态菜单。
- 不直接触发网络请求。
- 不显示完整 IMU、音频和 SD 调试数据。
- 不因 Wi-Fi 离线而阻止进入其他离线功能。

### 8.2 Weather：天气

Weather 是当前唯一的生产级 App 参考实现，页面只消费 `weather_service` 的不可变 snapshot 和 event。

#### 页面

1. 总览：地点、更新时间、当前温度、体感、条件、湿度、风、降水和预警等级。
2. 预报：24 小时和 7 日切换，显示温度、条件、降水概率和风向。
3. 预警列表：按严重级别和开始时间排序。
4. 预警详情：通过 Typed Blob 传递值复制的 alert key，支持 `NEWINTENT`。

#### 必须状态

- 未配置：提示通过 Setup 完成连接或服务配置。
- 等待网络：保留页面结构，显示最近缓存。
- 更新中：显示轻量进度和上次有效时间。
- Ready：显示最新 snapshot。
- Degraded：显示缓存和过期边界。
- 认证错误、限流、上游错误、解析错误、存储错误：显示可理解的原因和重试入口。

天气服务支持当前、小时、日、预警、地点、湿度、降水、风、气压、能见度、紫外线和昼夜条件；不应增加 AQI、地图、雷达、GPS 或不存在的 feels-like 数据。

### 8.3 Clock：时钟与计时

#### 目标

将当前 RTC/SNTP demo 改为真正持续运行的工具。Clock 页面可以离开前台，但倒计时和专注状态不能随页面暂停而停止。

#### 页面

1. 时钟：本地时间、UTC、时间来源、最近同步时间。
2. 倒计时：预设时长、加减时间、开始、暂停、继续、重置。
3. 秒表：开始、暂停、分段、重置。
4. 专注：工作/休息周期、当前阶段、剩余时间、完成次数。
5. 提醒：清醒状态的 RTC 日历提醒和下一次提醒时间。

#### 规则

- 计时使用单调时钟计算剩余时间，不能依赖页面 timer 的调用次数。
- `timer_service` 应拥有状态、持久化策略和 event generation；Clock Page 只负责呈现。
- 设备休眠前暂停前台渲染，恢复后按单调时间重新计算，不产生补发风暴。
- 当前硬件不能可靠地从轻睡眠由 RTC 中断唤醒，文案必须明确“设备清醒时提醒”。

### 8.4 Recorder：WAV 录音

#### 目标

利用已有全双工 PCM 能力，提供简单可靠的语音备忘录，而不是通用音乐播放器。

#### 页面

1. 录音首页：录音按钮、当前时长、剩余空间、麦克风电平。
2. 录音中：暂停、继续、停止；屏幕熄灭前提示录音状态。
3. 录音列表：日期、时长、文件大小、播放、删除。
4. 播放页：进度、暂停、继续、停止、音量。

#### 服务要求

- `recorder_service` 独占音频流和录音文件写入。
- 使用标准 PCM WAV header，停止时回填数据长度；异常停止时保留可识别的文件状态。
- SD 卡拔出、空间不足、音频 I/O 失败必须进入可恢复错误状态。
- 录音文件命名、索引和删除由服务管理，页面不得直接枚举和修改目录。
- 不支持 MP3、无损解码或后台无限录音，除非以后新增明确的编解码资源和测试证据。

### 8.5 Level：水平仪与倾角

#### 目标

把 IMU 的原始六轴数据转化为用户能理解的短时工具。

#### 页面

- 大型水平仪圆盘或十字线，显示左右/前后倾斜。
- 数字角度和“水平/轻微倾斜/明显倾斜”语义状态。
- 可选的稳定性指示，提示当前读数是否抖动。
- 校准入口只保存用户确认的零点偏移，不改变 BSP 传感器校准。
- 原始加速度、角速度和温度放到 Diagnostics。

#### 限制

- 没有磁力计，不能显示真实方位或指南针。
- 没有 GPS，不能提供位置和运动轨迹。
- 页面暂停时释放 IMU 前台会话；服务层保留必要的采样策略。

### 8.6 Settings：系统设置

Settings 是设备管理中心，不应再次复制 Setup 的配网实现。

#### 页面结构

1. 连接：进入 Setup 日常管理模式，显示 Wi-Fi、BLE 绑定和网络操作。
2. 显示：亮度、自动熄屏、转场策略和显示诊断入口（开发构建）。
3. 电源：电量、电压、充电来源、待机策略和立即熄屏。
4. 时间：时区、时间来源、手动校时、网络校时和清醒提醒。
5. 存储：SD 挂载状态、空间摘要、安全卸载；格式化必须单独确认。
6. 设备：型号、固件、协议版本、资源版本和构建信息。
7. 维护：恢复出厂、日志导出、OTA 更新状态。

#### 规则

- 亮度和电源操作由 App Manager/app-control 串行执行。
- 恢复出厂必须先写 durable journal，再允许重启；失败可重试。
- Settings 不直接操作 NVS、BSP 或文件系统。
- 蓝牙开关属于本地恢复策略，不能由远端 Device Link 命令关闭。

### 8.7 Applications：应用目录

建议复用现有 `menu_app` 的 App 身份，将根页从“演示中心”改成“应用”。应用目录是唯一的普通用户全量 App 入口。

#### 页面

- 使用 registry 枚举所有非隐藏 App。
- 每个项目显示图标、`display_name`、简短状态和进入箭头。
- 根据内容数量采用纵向滚动或固定分页，不使用依赖屏幕宽度的绝对坐标。
- 点击整行启动应用；启动失败保留目录并显示错误。
- 目录不持有目标 App 的 Page state，不创建目标页面对象。

#### 分类

第一版不增加复杂的分类元数据：按产品优先级固定排序即可。推荐顺序为 Weather、Clock、Recorder、Level、Settings、连接。Diagnostics hidden，不在目录中出现。

### 8.8 Diagnostics：隐藏诊断

Diagnostics 不是普通用户应用，使用 `APP_MANAGER_APP_FLAG_HIDDEN`。

保留以下工程能力：

- IMU 原始快照、采样率和中断轮询状态；
- 音频播放短音、麦克风电平和 I/O 状态；
- SD 挂载、容量、安全临时文件读写自检；
- RTC 读写、闹钟配置、pending/interrupt 状态；
- PMU、电源、显示刷新和资源摘要；
- BLE/Wi-Fi 互操作诊断和版本信息。

诊断页必须使用与生产页面相同的生命周期和 worker 规则，但不应占用普通导航入口，也不应在发布文档中伪装成产品功能。

### 8.9 Recent Apps：最近使用

Recent Apps 属于 App Manager Sys Layer，不属于 `layers/apps` 中任何业务 App。

目标交互：

- 双击 Home 打开；再次双击或单击 Home 关闭并回到 Home。
- 显示最多三个非 pinned、非 hidden 的最近业务 App。
- 使用图标、名称、当前 Page 名称和关闭按钮。
- 点击项目切换到该 App；关闭按钮只退出该 App，不影响 Home。
- App 正在清理或导航失败时显示忙状态，不重复提交命令。
- 无任务时显示简单空态，不分配大图预览作为必需资源。

## 9. 服务层规划

当前已有 `connectivity_manager`、`device_link_service`、`weather_service`、`time_service`、`power_service`、`imu_service`、`audio_service`、`sd_storage_service`、`factory_reset_service`、`system_pm` 和 `event_bus`。应用重构还需要把持续业务从 demo adapter 收敛为产品服务：

### 9.1 timer_service（规划）

- 拥有倒计时、秒表和专注状态。
- 使用单调时间计算，发布不可变 snapshot 和 generation。
- 允许 Clock 和 Home 同时读取，但只允许一个产品 owner 提交控制命令。
- 支持暂停、恢复、重置和完成事件。
- 与 system PM 协作，明确休眠期间是否继续计时。

### 9.2 recorder_service（规划）

- 拥有音频会话、WAV 文件、SD 空间和录音索引。
- 页面只提交 start/pause/resume/stop/play/delete 命令。
- 所有文件 I/O 在 service worker 执行；事件只携带小型状态和 ID。
- 将拔卡、空间不足、PCM 超时和文件损坏映射为稳定错误分类。

### 9.3 update_service（规划）

- 检查、下载、校验、安装、重启和失败回滚。
- 下载过程不阻塞 UI worker；必须显示可取消和失败重试状态。
- 使用 HTTPS、镜像身份校验和 OTA 分区策略。
- 更新中禁止恢复出厂、进入低功耗或启动新的 BLE 配对窗口。

### 9.4 配置和持久化

```text
构建期固定事实       -> BSP / Kconfig / sdkconfig.defaults
根运行时产品策略     -> main/app_product_config.c
用户偏好和完成状态   -> 对应 service / reset journal / NVS
大文件和缓存         -> LittleFS / SD service
页面临时状态         -> Page state arena
```

Kconfig 不承载用户时区、录音列表、计时状态或页面布局；硬件事实也不能由 App 覆盖。

## 10. UI 设计规则

### 10.1 屏幕和布局

- 所有 Page 以 368 x 448 为基准，采用 content-driven Flex/Grid 布局。
- 标题、数值、状态和操作按视觉优先级排列，不使用大面积装饰卡片。
- 长文本默认换行；不得使用会隐藏关键信息的单行省略作为错误处理。
- 行高、按钮尺寸、图标尺寸和分页 pitch 使用稳定约束，动态文字不能改变相邻控件位置。
- 可滚动内容只有一个明确 owner；嵌套容器默认移除 `SCROLLABLE` 和 `CLICKABLE`，避免子对象抢触摸。
- 所有异步结果都有 loading、empty、success、degraded、error 和 retry 表达。

### 10.2 视觉语言

- 深色背景、高对比文字和少量语义颜色：成功、进行中、警告、错误。
- 主页面使用大号数字和短标签，详细解释放在二级页面。
- 按钮使用图标和明确动作文字；熟悉的返回、关闭、播放、暂停使用图标。
- 不使用宣传式 Hero、装饰渐变、无意义的圆形光斑或堆叠卡片。
- 中文字体必须在动态文本设置前绑定，避免出现方框或首次布局错误。

### 10.3 状态文案

用户看到的是可操作的结果，而不是内部错误码：

| 内部状态 | 页面表达 |
| --- | --- |
| service unavailable | 服务暂不可用，请重试 |
| queued | 已加入队列 |
| running | 正在处理 |
| retry wait | 将在稍后重试 |
| stale cache | 显示上次有效数据，等待更新 |
| invalid state | 当前状态不能执行此操作 |
| operation conflict | 已有操作进行中 |

详细错误码只进入 Diagnostics 或受控日志。

## 11. 重构阶段

### 阶段一：产品骨架（P0）

- 实现 onboarding 持久状态和启动路由。
- Setup 增加引导模式与日常管理模式。
- Home 改为今日总览，保留离线可用。
- 复用 `menu_app` 创建 Applications 目录。
- 明确 App registry 图标和名称的资源约定。
- 将 Diagnostics route 与普通入口分离。

### 阶段二：核心工具（P0）

- 实现 `timer_service`。
- 重建 Clock 页面：时钟、倒计时、秒表、专注。
- 将 Home 的下一项任务接入 timer snapshot。
- 将 Settings 的时间与提醒入口接入统一服务。

### 阶段三：媒体和传感（P1）

- 实现 `recorder_service` 和 Recorder App。
- 将 IMU demo 改为 Level App。
- 原始硬件页面全部移入 hidden Diagnostics。
- 完成 SD 拔卡、空间不足和音频异常恢复。

### 阶段四：可维护产品（P1）

- 实现 update_service 和 OTA 页面。
- 增加时区、单位和用户偏好持久化。
- 重构 Recent Apps 为轻量列表，减少大图预览依赖。
- 增加版本迁移和恢复出厂后的全链路验证。

## 12. 测试和验收

### 12.1 宿主测试

- App Manager：registry、隐藏/固定策略、导航、生命周期、Typed Blob、清理重试和 Recent Apps。
- Apps：Home/Setup/Settings/Weather 页面状态、导航、字体、内容尺寸和错误重试。
- Services：timer、recorder、weather、connectivity 的 snapshot generation 和取消语义。
- 跨层：首次启动、稍后设置、完成引导、恢复出厂、服务失败后的页面恢复。
- 普通、ASan/UBSan、TSan 依次运行；页面 worker、订阅、队列和文件所有权变化必须覆盖 sanitizer。

### 12.2 真机验收

必须单独验证，不能由宿主测试替代：

1. 冷启动、首次启动、引导中断和恢复出厂后的 Setup 路由。
2. BLE Numeric Comparison、手机先确认和设备先确认。
3. Wi-Fi 凭证传输、保存、连接、IP_READY、断网和重试。
4. Home、Weather、Clock、Recorder 的显示、触摸、滚动和长文本。
5. SD 插拔、空间不足、录音停止和文件恢复。
6. 音频播放、麦克风电平、静音、PA 和休眠恢复。
7. RTC 清醒提醒、系统时间恢复和 NTP 校时。
8. 轻睡眠进入/唤醒、HOME 键、显示恢复和后台 service 状态。
9. Recent Apps 快速切换、关闭、导航失败和清理重试。
10. 长时间运行、内存高水位、Wi-Fi/BLE 共存和电量变化。

### 12.3 验收边界

以下结果在没有对应证据前不得宣称：

- 宿主测试通过不等于真机显示、触摸、DMA、射频或功耗通过。
- 一次正常启动不等于冷启动和恢复出厂通过。
- 清醒状态 RTC alarm 通过不等于睡眠唤醒通过。
- BLE/Wi-Fi 一次配网成功不等于断连、重试、旧 bond 清理和第二客户端兼容。
- 页面截图正常不等于中文字体、极长文本和异常状态没有布局问题。

## 13. 非目标和决策记录

- 不修改 `device-link/v1` 契约来承载应用目录、计时器或通知。
- 不修改 Android 以弥补设备侧页面问题。
- 不让 Home 复制 App registry，也不维护第二份应用菜单。
- 不让业务 Page 直接拥有 BSP 驱动或服务 worker。
- 不因为“应用数量少”而省略正式应用目录；注册元数据应有实际消费者。
- 不把 Diagnostics、display benchmark 或原始 demo 当成普通产品功能。
- 不在当前硬件没有证据时承诺 GPS、指南针、健康数据、睡眠 RTC 唤醒或 MP3。

本文的首个实施目标是阶段一。后续代码、测试和真机证据应逐项回填到本文件或对应服务文档；若实现与本文冲突，应先更新设计决策，再修改代码。
