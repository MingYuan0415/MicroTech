# MicroTech Device Link App（Android）开发指引

## 1. 定位与消费方约定

本文面向 Android 伴生 App 侧 Device Link v1 客户端开发。权威来源是契约子模块
`contracts/provisioning`；本仓库固件是参考实现，不替代契约。父仓库 gitlink 和
`device_link/proto.lock` 均钉住 immutable commit `02827e7`。v1 UUID、广告布局和 protobuf
wire schema 未变化；契约与生成检查通过仍不构成 Android interoperability 证据。

消费方必须：

- 以 Git submodule 钉住最终契约 immutable commit，不引用浮动分支或未提交工作树；
- 自带 buf/protoc 生成管线，生成物不提交到契约仓库；
- 按 `docs/conformance.md` 的 consumer checks 自测：解码每个 golden vector 并
  复现规范字节、无强转地拒绝 malformed/非法 QR、执行全部 semantic 用例
  （含无加密事件时的轮询路径）、保留或安全忽略未知 protobuf 字段、拒绝未知
  枚举而非猜测、证明 secret 不落日志且不超过生命周期。

## 2. 必读文件（按顺序）

### 2.1 契约（权威）

| 文件 | 用途 |
|---|---|
| `contracts/provisioning/README.md` | 仓库布局、验证命令、消费方约定 |
| `contracts/provisioning/docs/conformance.md` | 消费者验收要求 |
| `contracts/provisioning/profiles/device-link-v1.yaml` | 静态 GATT profile：UUID、admission、MTU、消息上限（单一来源） |
| `contracts/provisioning/docs/device-link-gatt-v1.md` | GATT 特征读写、CCCD、MTU 23/498 分片、事务串行化（客户端首个实现对象） |
| `contracts/provisioning/docs/device-link-framing-v1.md` | 分片/重组、响应语义、超时 |
| `contracts/provisioning/docs/device-link-session-transport-v1.md` | session/control 通道的 Protocomm Security 2 承载：type 字节、握手/密文区分、verifier 选择、重握手恢复 |
| `contracts/provisioning/docs/device-link-lifecycle-v1.md` | 生命周期状态机、boot id、generation |
| `contracts/provisioning/docs/device-link-security-v1.md` | SC bonding、identity/LTK 校验、配对窗口准入、换机顺序 |
| `contracts/provisioning/docs/device-link-discovery-v1.md` | 广播/扫描发现 |
| `contracts/provisioning/docs/device-link-qr-v1.md` | `link-v1` QR schema：service UUID、discriminator、POP、有效期 |
| `contracts/provisioning/proto/microtech/link/v1/*.proto` | capabilities/envelope/errors/events/session/transfer 的 wire schema |
| `contracts/provisioning/fixtures/` | golden protobuf、framing、profile、link/wire limits、semantic 用例 |

### 2.2 固件实施（参考，非权威）

| 文件 | 用途 |
|---|---|
| 本仓库 `doc/device-link-implementation.md` | 当前固件 owner、并发、安全、恢复出厂架构和验证边界 |
| `layers/middleware/components/ble_runtime/include/ble_link_service.h` | 公共 API：`ble_link_service_feed`、RX/TX 通道枚举、`response_in_flight`、`abort_transactions`、`idle_timeout` |
| `layers/middleware/components/ble_runtime/include/ble_link_gatt.h` | 特征接线：`ble_link_gatt_init`、`set_att_mtu`、`publish_link_state`、`tx_queue_depth` |
| `layers/middleware/components/ble_runtime/include/ble_link_dispatcher.h` | LinkError 码（OK=1、UNAUTHENTICATED=6、UNSUPPORTED_OPERATION=4、BUSY=10 等） |
| `layers/middleware/components/ble_runtime/include/ble_link_session.h` | 会话 admission 语义 |
| `contracts/provisioning/compatibility/known-good.yaml` | 真机验证组合记录（Device Link 尚无 verified 条目） |

## 3. 联调前的现状确认（重要）

1. **固件逻辑链路已落地，真机互操作尚未验收**：生产固件由
   `device_link_service` 启动 NimBLE，管理常驻 slow 广告和本地显式绑定窗口，并已实现
   bond-store identity/SC/LTK 校验、Security 2 Cmd0/Cmd1、应用授权、恢复查询、replacement
   与崩溃后清理。宿主测试和固件构建不能替代 ESP32-S3/Android 真机绑定、重连和 soak；
   `known-good.yaml` 登记前不得宣称互操作通过。
2. **安全 admission 分层且 fail-closed**：未知 peer 闭窗时可保留 ACL 并读取公开
   `link_state`，但 SMP 被拒绝且不写 store；开窗才允许 SC-only 配对。session 通道要求
   encrypted 且匹配已验证 bond，control 通道还要求当前 Security 2 会话认证和应用授权。
3. **事务串行化与 Cmd0 例外**：普通请求一次仅一个未确认响应，客户端应等待上一响应
   最终片确认。新完整 Cmd0 会立即退休旧 Security 2 epoch；若旧 indication 尚未确认，
   设备只保留一份精确 Cmd0，精确重复不重处理，其他 ingress 返回 `BUSY`。旧 indication
   确认只释放发送占用，2 s 原期限到达则终止 ACL。Cmd0 只分配一次新 epoch，随后 Cmd1
   必须在同一 epoch 完成认证；它不会再推进 epoch。
4. **boot id 不匹配是终态**：客户端发现 `link_state.boot_id` 变化即视为设备重启，
   必须重建会话（重新发现/重连），不要继续旧会话。
5. **MTU 23 是强制基线**：分片重组必须以 MTU 23 为最坏情况实现；协商 MTU 后可
   放大帧长，但协议路径必须保持 MTU 23 正确。
6. **超时契约**：2 s 指示确认窗口、5 s 重组 idle 窗口由固件执行；客户端侧应有
   对应的请求超时与重试/放弃策略（具体时限以 framing 文档为准）。
7. **绑定流程按 v1 契约执行**：扫描按 Device Link UUID + QR discriminator；
   绑定窗口内未知手机才可 SMP 配对（SC-only、单 bond）；Security 2 握手用 QR
   `pop` 作 SRP 口令，重连用已保存的应用口令；握手/密文由 session transport
   type 字节区分（见 `device-link-session-transport-v1.md`）。

## 4. 建议开发顺序

1. 集成契约 submodule + buf 生成管线；跑通 conformance golden/semantic 向量。
2. 消费端 QR：按 `device-link-qr-v1.md` 严格解析/校验 `link-v1` 载荷并复现
   fixtures 下 valid/invalid 向量（含 discriminator↔广播交叉校验）；`pop`
   必须解码为 16 个原始字节后作为 SRP 口令，不得把 Base64URL 文本当口令。
3. BLE 基础：扫描发现（device-link-discovery）、连接、MTU 协商（请求不超过
   498；设备会把更高请求压回 498）、CCCD 订阅。所有 session/control RX 分片使用
   ATT Write Request，不依赖 Write Command 或 Prepare/Execute Write。
4. `link_state` 读取与轮询。Cmd1 认证成功和 `link_state` CCCD 启用都会请求 fresh
   `PublicLinkState` notification；固件会保留失败投递并以 100 ms cooldown 重试，但
   notification 仍是 best effort。v1 不广告 encrypted events：`SubscribeEvents` 一律返回
   `UNSUPPORTED_OPERATION`，首版仍以 `GetLinkSnapshot` polling 作为状态恢复路径。
5. 帧收发：envelope 编码、分片提交、重组解码（以 MTU 23 为基线，对照 fixtures/
   framing 向量）；session transport type 字节与握手/密文状态机。设备端响应
   按 indication 确认逐片下发，客户端必须逐片确认。
6. 会话状态机与事务串行化（lifecycle + BUSY 处理）。
7. 绑定流：窗口内 SMP 配对（SC-only）→ Security 2 握手（QR `pop` 原始字节）
   → 加密 GetCapabilities/AuthorizePrepare → 客户端先持久化 prepared credential
   → 首次 AuthorizeCommit 返回 `CONFIRMATION_REQUIRED` → 设备本地确认
   → 使用新 request ID 再次 Commit 成功 → 持久化 device authorization id；重连用
   应用口令重新握手。设备本地 confirmation token 是 boot-scoped UI 身份，不上 wire。
8. 错误码映射与超时/断连恢复（errors.md、framing 超时、ambiguous Commit
   恢复）：Commit 响应丢失时用保留的应用口令重连，带 `RECOVERY_QUERY` 旗标
   发送 `GetAuthorization` 取回同一 `device_authorization_id`，并比对返回的
   credential id。设备会在当前 ACL 保留已成功 Commit 的终态，因此同一 peer 真实完成
   long-term Security 2 重握手后可对相同 transaction/credential 使用 fresh request ID 得到
   幂等成功；replacement cutover 或 ACL 终态会清缓存。此能力不是 ambiguous recovery：
   客户端结果不确定时仍不得重发旧 Commit，必须执行 Recovery Query。
9. 使用 `02827e7` 契约和对应固件镜像真机联调完整绑定、重连、replacement、
   ambiguous Commit recovery、MTU 23/185/498、reset/cold-cycle 和 BLE soak。

## 5. 验收门槛

- 消费端通过 conformance.md 全部 consumer checks；
- 复现 fixtures 下 framing/profile/link-limits/wire-limits 向量；
- 真机联调通过后，按 `compatibility.md` 流程在 `known-good.yaml` 登记
  （当前尚无 Device Link verified 条目，登记前不宣称互操作）。
