# MicroTech Device Link 当前实现

## 1. 契约与实现边界

Device Link v1 的权威语义来自 `contracts/provisioning` 中的 GATT、framing、
lifecycle、security、session transport 和 discovery 文档，以及对应 profile、protobuf
和 semantic fixtures。当前修订钉住 `contracts/provisioning@02827e7`，不改变 v1
UUID、广告 service data 布局或 protobuf wire schema。

固件实现分为四个 owner：

| 组件 | 所有权 |
| --- | --- |
| `ble_runtime` | NimBLE host 生命周期、GAP/GATT/ADV、连接事实、TX 调度、framing 和 host store 协调 |
| `device_link_security` | Security 2 protobuf 解析、SRP verifier、会话密钥和授权记录持久化 |
| `device_link_service` | ACL/Security 2 逻辑会话、绑定窗口、授权事务、清理义务和应用快照的串行 owner |
| `factory_reset_service` | 版本化恢复出厂 marker、重启准入和崩溃恢复状态 |

NimBLE callback、timer callback 和公共 API 只提交事件或工作；它们不直接完成授权、
replacement、revoke 或持久化清理。Device Link worker 串行处理这些变化，并在 owner
状态中保留期限、重试和清理义务。未知 peer 在闭窗时仍可保留 ACL 并读取公开
`link_state`，但 SMP 被拒绝且不会写入 bond store；session/control 准入分别继续要求
已验证的 Secure Connections bond 和当前应用授权。

## 2. 并发身份与期限

所有跨 owner 的连接/会话异步操作都携带不可变身份：

```text
{connection_generation, security_epoch, flow_id, token, kind, conn_handle}
```

DISCONNECT、MTU、ENC_CHANGE、SUBSCRIBE、TX completion、reassembly timeout、
TERMINATE、provisional cleanup 和 replacement 都核对适用的完整身份。连接句柄复用或
旧 generation 的迟到事件只产生 no-op，不能清理或确认新会话。CONNECT 到达时还会从
当前 connection descriptor 补放 encrypted、bond 和 identity 事实，覆盖
ENC_CHANGE 先于延迟 CONNECT 的合法时序。

ESP-IDF v6.0.2 将 `IDENTITY_RESOLVED` 和 `REPEAT_PAIRING` 只投递给
`ble_gap_adv_start()` 捕获的 per-connection GAP callback。固件在每次 ADV START 注册该
callback，并只把这两个事件送入 reducer；其余连接事件继续由 global listener 处理，避免
双重处理。identity callback 可更新当前 ACL 的 normalized identity；fresh public/static
identity 不一定产生 `IDENTITY_RESOLVED`，因此最终 ENC_CHANGE 路径会重新读取 connection
descriptor 和 bond store，以最新 identity、bonded、verified facts 完成同一 ACL 的准入收敛。
无旧 bond 的 CONNECT 只安装 provisional candidate；只有 reducer 观察到当前 ACL 新生成且
verified 的 bond 后才形成 cleanup 义务，SMP 完成前断连不会制造全量 bond 删除。

DISCONNECT、RESET 和 TERMINATE 是 ACL 终态事件：匹配的 generation 与 conn_handle 对
该 ACL 的全部 Security 2 epoch 都有清理权，因此 Cmd0 在终态事件等待 owner 锁期间推进
epoch 也不能留下半开会话。accepted ingress 记录 generation 和 conn_handle；终态在 worker
execute 前清除 queued reservation、推进 ingress epoch 并把该代标记为 retired，已入队但
未执行的 work 不能在断连后打开 Security 2 或生成响应。owner 在同一临界区统一清除
reassembly、TX flow、授权事务、Security 2 adapter 和当前事实；同一终态事件再次到达、
同代后续 ingress 或旧代事件均为 no-op。加密丢失、indication 失败等会话级错误仍必须匹配
security_epoch，以及适用的 flow/token，不能关闭更新的握手。

worker 使用 task notification 作为可合并、不可丢失的唤醒信号；队列只保存有序
命令。owner 按窗口截止、剩余时间快照发布、授权事务到期和 retained cleanup 重试中的
最近绝对 deadline 有界等待。每次命令处理和等待返回后都执行 deadline sweep，因此不依赖
可能丢失的 timer tick，也不会被持续命令流饿死。host FreeRTOS fake 实现相同的 notification
count/wait 语义，因此宿主测试和生产执行同一等待分支。

NimBLE 侧独立的 `ble_link_timer` owner 使用 4096B stack；deadline sweep 可以同步退休
Security 2 flow、推进 TX scheduler 并保留 provisional cleanup，该预算覆盖完整清理调用链。

provisional/orphan/replacement cleanup 不以“命令成功入队”为完成条件。Device Link owner 在
port 暂时拒绝义务时按 100/200/400/800/1000 ms 封顶退避，失败路径不自唤醒；port 侧使用
4 个固定 slot 和 1 个 fail-closed overflow，按同一物理 bond 目标合并重复请求，并保留执行中
新增的更强删除、失效或终止要求。义务保留到目标记录确认删除，或同一事务已经 durable
promote；队列满、断连、句柄复用都不会取消它。任一 cleanup 或 terminal fence 存在时拒绝
新 ACL；当前 ACL 进入 terminal cleanup 后仍可读取公开 `link_state`，但 session/control 写入
被拒绝，且 termination 持续重试，直到匹配的 DISCONNECT/RESET 释放 fence。只有全部 cleanup
清空后才恢复广告；启动 reconciliation 继续处理崩溃遗留的 orphan 和 revoke journal。

peer-store 删除不把 `ble_gap_unpair()` 当作 durable confirmation。port 只执行一次显式
`ble_store_util_delete_peer()`，并逐类确认 peer store 已无残留；任一删除或枚举错误都会在
完整 NimBLE host run 内粘滞。ESP-IDF 先删除 RAM entry 再持久化 NVS，因此同一 host run
中的“记录已不存在”不能把持久化失败翻转为成功；cleanup、journal 和广告 gate 持续保留，
port 的 store write callback 也会把容量溢出交给 replacement owner、把其他写失败粘滞；
bond verification 只有在 guard 保持成功时才接受 RAM mirror。当前 ESP32-S3 构建使用
controller privacy；NimBLE privacy startup 的 `ble_hs_pvcy_set_default_irk()` 路径会在 initial
sync 和 reset resync 前恢复 IDF writer。cold boot callback 为 NULL 时只 arm guard；首次 sync
捕获 writer，后续 resync 只对精确原 writer 重装 wrapper，未知 callback 不会被覆盖并使当前
host run fail closed。

ESP-IDF v6.0.2 的 NVS loader 会记录却吞掉 store restore 错误。非 journaled revoke 的每次
sync 都在同一 storage lock 下、destructive reconciliation 前，对当前构建可加载的 OUR_SEC、
PEER_SEC、CCCD、CSFC、LOCAL_IRK 和 RPA_REC 六族比较 durable key presence count 与 public
RAM-store count。NVS 访问、RAM count 或计数不一致都会粘滞 storage error，不发布 SYNC，
也不开放 SMP/ADV；该审计不声称验证 blob 内容。只有完整 host reload 从 durable NVS 重建
状态后才允许 reconciliation 重试。clean deinit 先取得 shutdown pause，以 host
barrier 关闭 SMP gate，并将 revoke、cleanup、terminal fence 和 ACL terminate 收敛到两次
host barrier 之间的 double-empty fixed point。

## 3. GATT transport

GATT registry 在 host 启动前冻结 Device Link v1 特征和权限。`link_state` 是公开可读的
当前事实；session/control RX 走同一 framing 和 dispatcher，但 admission 独立。当前值与
`{generation, auth_epoch, cccd_epoch}` 投递记录由 GATT 状态锁分开保存，实际投递由 Device
Link worker 独占。只有授权后的 transport submit 成功才更新投递 stamp；并发的事实、认证
或 CCCD 变化不会被较早 submit 的返回覆盖。Cmd1 成功和 CCCD 启用都会清除旧 cooldown、
标记 dirty 并通过 task notification 要求 fresh `PublicLinkState`。同步或异步 notification
失败只保留 dirty obligation，在 100 ms `retry_not_before` 到达后由同一 worker 重试；无当前
授权、订阅或 ACL 时不轮询，等待对应状态事件重新唤醒。

### 3.1 TX 调度

TX scheduler 在一个连接上串行 notification 和 indication，最多一帧 in-flight。
提交帧、in-flight 帧和尚未投递的 completion 共享固定的 `queue_depth + 1` credit；completion
存储在 init 时分配，不在终态路径扩容。每次成功提交严格产生一个终态 completion，reset、
deinit、同步提交失败和异步失败都不能吞掉已接纳身份。

失败按 flow/epoch 隔离，不存在跨会话的全局 failure latch。响应 indication 的超时或歧义
错误只退休对应 flow/epoch 并关闭该 Security 2 会话；best-effort snapshot notification 失败
只把快照标记为 dirty，等待下一次可达机会，不关闭已认证会话。NimBLE 原始 NOTIFY_TX
callback 不携带应用 token；indication timeout 会为旧 raw callback tuple 保留 retired tracker
tombstone，直到迟到终态被消费或 disconnect/reset 清空。tombstone 存续时拒绝提交相同
`{conn_handle, value_handle, indication}` tuple，避免迟到确认冒充新 operation。

### 3.2 重组与去重

重组器返回 `NEW_PARTIAL`、`DUPLICATE` 或 `COMPLETE`。frame ID 只约束当前活动消息；消息
完成后，任意后续消息都可复用任意非零 ID。实现保留最终片段的精确 header/payload 字节，
只把逐字节一致的重传判为 duplicate。重复片段不追加数据，也不刷新 5 秒 reassembly
deadline；gap、overlap、错误 offset、未知 flag 和不一致重传均 fail closed。

## 4. 广告与 SMP gate

ADV manager 通过 slow non-bindable 和 fast bindable lease 收敛唯一目标。普通 arm/cancel
wake 使用 task notification，不占 ADV 命令队列容量，因此窗口取消会立即重新查询目标。
bindable lease 拒绝全零 discriminator。

每次 START 提交都分配新的非零 generation；每个新逻辑 STOP obligation 分配新的非零
generation，同一 STOP 的同步/异步重试复用该值。两个计数在 boot 内跨 manager reinit 不
复用，耗尽时 fail closed。NimBLE command 和 completion 原样携带 generation；manager 只
处理当前 STARTING/STOPPING 状态且 generation 匹配的 completion，迟到旧代结果为 no-op。
异步失败保留
`{action, generation, retry_not_before}`，按 100/200/400/800/1000 ms 封顶退避；poll 只重试
仍匹配当前 transition 的义务，错代 retry 被清除，成功或目标变化会清除当前 retry。这样
START 会自动恢复，持续 STOP 失败也不会忙循环。

打开窗口的可见性顺序是：暂停并停止当前广告，准备 bootstrap verifier 和 bindable lease，
在 NimBLE host event 上同步打开 SMP gate，最后恢复 bindable 广告。关闭顺序是：先停止
广告并关闭 gate，再终止窗口 ACL、清 provisional 状态和 bootstrap verifier，最后恢复 slow
广告。gate 的 host-core 值闭窗为 `sm_sec_lvl=1`，开窗为 `sm_sec_lvl=0`；调用方只有在持久
host event 确认应用后才继续下一步。

窗口请求状态与 cleanup、rejected ACL、revoke 和 shutdown 的独立 hold 分开保存；只有
requested-open 且 hold mask 为空时 gate 才有效打开。被拒绝的 ACL 在 CONNECT host callback
内先取得 pairing-gate/ADV hold，再提交 terminate，因此 ADV STOP 或 HCI terminate 暂时失败
也不会留下可配对区间。accepted/rejected terminate 的 handle-only 副作用都在 NimBLE host
event 队列中串行执行，并保留到 exact DISCONNECT/RESET 终态。

根构建针对固定 ESP-IDF v6.0.2 为 `bt` 组件设置
`MYNEWT_VAL_BLE_RESTART_PAIR=0`，避免闭窗 LTK lookup 失败后 NimBLE 主动发起新的 SMP。
`layers/middleware/components/ble_runtime/scripts/check_idf_assumptions.sh` 同时锁定版本以及
GATT、GAP、ATT、SM、store 和 host event 关键源码假设。脚本失败要求重新审查上游内部
时序，不能只更新 hash 放行；工程不修改 ESP-IDF 或 NimBLE 源码。

## 5. Security 2 与授权

`device_link_security` 使用 ESP-IDF v6.0.2 自带的 protobuf-c `SessionData` 和
`Sec2Payload` 类型解析请求与响应。Cmd0 成功后进入 `HANDSHAKING`；Cmd1 proof 和 Resp1 都
成功后，在返回 response 前进入 `AUTHENTICATED`。`unprotect()` 只接受已经认证的会话，
不再隐式承担认证状态迁移。每个被接受的 Cmd0 只分配一次 security epoch，Cmd1 只认证该
Cmd0 创建的当前 epoch，不再次递增；错 epoch 或重复认证均 fail closed。

### 5.1 Cmd0 replacement

同一 ACL 上的新完整 Cmd0 会立即关闭旧 Security 2 逻辑会话并分配新 security epoch。
没有旧 indication 占用时，旧 flow 被清理后立即处理 Cmd0。存在旧 indication 时，owner
只保留一份完整 Cmd0 原始字节：精确重复不重复处理、不延长期限，其他 ingress 返回既有
busy 错误。旧 indication confirmation 只释放 TX 占用，随后才在新 epoch 处理保留的 Cmd0；
它绝不会恢复旧 epoch。旧 indication 原始 2 秒期限到达时丢弃 Cmd0 并终止 ACL。

### 5.2 授权事务

绑定顺序固定为：

```text
PREPARED -> COMMIT_PROBED -> LOCALLY_CONFIRMED -> COMMITTED
```

客户端必须先持久化 Prepare 结果。第一次结构有效的 Commit 是无 mutation 的 probe，返回
`CONFIRMATION_REQUIRED` 并生成非零、boot 内不复用的 `uint64_t` confirmation token；本地
确认 API 同时核对 token 和当前 generation，旧 UI 点击不能确认新事务。接受后，客户端以
新 request ID 再次 Commit 才 durable commit；拒绝、过期或终态失败继续承担 provisional
cleanup。

durable Commit 同时在当前 ACL 保存一份终态 replay，键包含 generation、conn_handle、peer
identity、transaction ID 和 credential ID。它独立于产生结果的逻辑 Security 2 会话，因此
同一 peer 在同一 ACL 上完成真实的 long-term Cmd0/Cmd1 重握手后，以 fresh request ID 重复
同一 Commit 会返回原 `AuthorizationResult`，不会再次写存储。ACL 终态/generation 变化、
remote replacement cutover、local revoke 和 factory reset 都会先清除此缓存，旧成功不能
越过授权所有权变化。该 server-side 幂等保证不改变客户端恢复规则：结果 ambiguous 时仍须
用 `RECOVERY_QUERY` 查询，不能盲重发旧 Commit。

Commit、remote replacement、local revoke、verifier 更新和 Recovery Query 在同一 owner
内串行。已经进入 `COMMIT_PROBED` 的事务先取得终态，remote replacement 只能稍后重试；
device-local revoke 和 factory reset 仍以 fail-closed 清理优先。Recovery Query 只在记录
确定不存在或凭据/身份确定不匹配时返回 `NOT_FOUND`，NVS I/O 返回 `STORAGE`，损坏或内部
不一致记录返回 `INTERNAL`，避免把 ambiguous 结果误判成未授权。

## 6. 恢复出厂与启动顺序

Settings 的“恢复出厂设置”进入独立确认页；第二次明确点击才调用
`factory_reset_service_request()`。服务先 durable commit 版本化 `factory.reset` marker，
成功后才调用注入的 restart callback。请求已受理后按钮保持禁用等待重启；持久化失败会留在
页面并显示错误。

检测到 marker 后，根运行时按以下顺序启动：

1. 完成 NVS/reset preflight；损坏或不可读 marker 均 fail closed；
2. 在 Connectivity Manager init 前幂等擦除其私有 Wi-Fi profile；
3. 以 `FACTORY_RESET_GATED` 初始化 Device Link，清授权/verifier、revoke marker、完整
   `nimble_bond` peer-store namespace 与 RAM mirror，以及易失 transfer/session 状态；
4. 在全局 marker 仍 durable 时暂停 ADV，并预取得 persistent slow non-bindable lease；
5. 所有 reset domain 和广告前置条件确认成功后清全局 marker；
6. 释放 startup gate，仅执行 visibility commit/unpause，然后才初始化平台与网络连接。

marker 擦除前任一步失败或崩溃都会保留 marker，广告和网络继续 gated，下次启动从头幂等
重放。startup gate 释放前的所有可失败资源操作已经在 marker 存续时完成；释放后的物理
ADV START 失败由 ADV owner 的 retained backoff 自动恢复，不把已经完成的持久化 reset
事务重新解释为失败。

全量 peer-store reset 固定执行 `durable namespace erase -> controller/RAM cleanup -> durable
namespace erase -> empty audit`。首次 erase 先于 IDF RAM-store persistence helper，避免损坏
blob 让撤销永久卡住；第二次 erase 清除 runtime cleanup 可能重写的记录和旧配置超出当前上限
的遗留 key。revoke/reset journal 位于独立 namespace，只有完整命名空间和 RAM mirror 都确认
为空后才清除，因此 journaled recovery 不依赖一次成功的 NimBLE RAM restore。

## 7. 验证与残余边界

自动验证入口覆盖契约 validator/fixtures、`device_link` 生成校验、`ble_runtime`、
`device_link_security`、`device_link_service`、`factory_reset_service`、Settings、根启动顺序
和跨层测试；受影响宿主套件需要同时运行 none、ASan/UBSan 和 TSan。固件侧还必须通过
AStyle、`git diff --check`、`idf.py reconfigure && idf.py build` 和 size 检查。通过这些
自动化检查只能证明相应宿主与构建范围，不等于真机 conformance。

当前只保留以下明确边界：

1. GATT profile 中的 `[write]` 要求合规客户端使用 ATT Write Request。ESP-IDF v6.0.2
   callback 不暴露 Write Command 或 Prepare/Execute Write 的原始 opcode，因此服务端不声明
   opcode 级拒绝；其他 admission、长度和 framing 校验仍执行。
2. 契约修订已冻结为 `02827e7`，`device_link/proto.lock` 钉住该 clean commit；
   `device_link_generated_contract` 会重新生成并核对全部 protobuf-c 文件。wire schema、生成
   C 字节和非注释 header 声明均未变化。
3. ESP32-S3/Android 真机测试、reset/cold-cycle、NVS 断电故障注入、MTU
   23/185/498、RPA/LTK 恢复、late indication、handle reuse 和 BLE soak 尚未在本轮执行。
4. encrypted events 仍是未来功能：固件不发布其 capability，也拒绝
   `SubscribeEvents`；v1 客户端使用 `GetLinkSnapshot` polling。

因此契约、实现、宿主/sanitizer 和固件构建范围已经闭环，但完成上述真机与互操作验证前
仍不能标记 compatibility/known-good。
