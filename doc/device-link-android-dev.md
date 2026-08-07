# MicroTech Device Link App（Android）开发指引

## 1. 定位与消费方约定

本文面向 Android 伴生 App 侧 Device Link v1 客户端开发。权威来源是契约子模块
`contracts/provisioning`（当前钉住 `80ce7ec`，P0 draft freeze + P3.0 绑定契约）；
本仓库固件 `ble_runtime` 是参考实现，不替代契约。wire 尚未冻结，README 明确
draft 产物 "not evidence of Android interoperability"。

消费方必须：

- 以 Git submodule 钉住契约 commit（当前 `80ce7ec`），不引用浮动的分支指针；
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
| 本仓库 `doc/device-link-implementation.md` | 实施记录：单元清单、P2.8 架构定案、验证边界与遗留项 |
| `layers/middleware/components/ble_runtime/include/ble_link_service.h` | 公共 API：`ble_link_service_feed`、RX/TX 通道枚举、`response_in_flight`、`abort_transactions`、`idle_timeout` |
| `layers/middleware/components/ble_runtime/include/ble_link_gatt.h` | 特征接线：`ble_link_gatt_init`、`set_att_mtu`、`publish_link_state`、`tx_queue_depth` |
| `layers/middleware/components/ble_runtime/include/ble_link_dispatcher.h` | LinkError 码（OK=1、UNAUTHENTICATED=6、UNSUPPORTED_OPERATION=4、BUSY=10 等） |
| `layers/middleware/components/ble_runtime/include/ble_link_session.h` | 会话 admission 语义 |
| `contracts/provisioning/compatibility/known-good.yaml` | 真机验证组合记录（Device Link 尚无 verified 条目） |

## 3. 联调前的现状确认（重要）

1. **固件尚未装配 Device Link 整机链路**：P3.0 冻结了绑定契约（QR、session
   transport、配对窗口准入），但生产固件仍启动旧 `provisioning_service +
   protocomm_ble`，`ble_runtime` 未接入启动路径，也不广播 Device Link
   Service Data。App 侧可以先完成契约消费与自测；真机联调须等固件装配
   （P3.1–P3.4，见实施记录遗留项）后开始。
2. **安全 admission 为 fail-closed**：生产路径不设置 `SC_BOND_VERIFIED` 与
   `identity_known`（SMP bond-store identity/SC/LTK 校验未实现），session/control
   通道的 authorized 会话实际不可达。App 端可先实现并验证：QR 解析、广播发现、
   GATT 特征读写、MTU 协商与分片重组、`link_state` 读取与轮询、错误码路径；
   安全会话联调需等固件 SMP 校验落地（见实施文档遗留项）。
3. **事务串行化**：一次仅一个未确认响应；固件在最终 indication 确认前对后续请求
   返回 `BUSY`（LinkError=10）。客户端必须等待上一响应的最终片确认后再发下一请求。
4. **boot id 不匹配是终态**：客户端发现 `link_state.boot_id` 变化即视为设备重启，
   必须重建会话（重新发现/重连），不要继续旧会话。
5. **MTU 23 是强制基线**：分片重组必须以 MTU 23 为最坏情况实现；协商 MTU 后可
   放大帧长，但协议路径必须保持 MTU 23 正确。
6. **超时契约**：2 s 指示确认窗口、5 s 重组 idle 窗口由固件执行；客户端侧应有
   对应的请求超时与重试/放弃策略（具体时限以 framing 文档为准）。
7. **绑定流程按 P3.0 契约执行**：扫描按 Device Link UUID + QR discriminator；
   绑定窗口内未知手机才可 SMP 配对（SC-only、单 bond）；Security 2 握手用 QR
   `pop` 作 SRP 口令，重连用已保存的应用口令；握手/密文由 session transport
   type 字节区分（见 `device-link-session-transport-v1.md`）。

## 4. 建议开发顺序

1. 集成契约 submodule + buf 生成管线；跑通 conformance golden/semantic 向量。
2. 消费端 QR：按 `device-link-qr-v1.md` 严格解析/校验 `link-v1` 载荷并复现
   fixtures 下 valid/invalid 向量（含 discriminator↔广播交叉校验）。
3. BLE 基础：扫描发现（device-link-discovery）、连接、MTU 协商、CCCD 订阅。
4. `link_state` 读取与事件订阅（先做无安全要求的公开通道）。
5. 帧收发：envelope 编码、分片提交、重组解码（以 MTU 23 为基线，对照 fixtures/
   framing 向量）；session transport type 字节与握手/密文状态机。
6. 会话状态机与事务串行化（lifecycle + BUSY 处理）。
7. 绑定流：窗口内 SMP 配对（SC-only）→ Security 2 握手（QR `pop`）→ 加密
   GetCapabilities/AuthorizePrepare → 本机确认轮询 → AuthorizeCommit → 持久化
   应用口令与 device authorization id；重连用应用口令重新握手。
8. 错误码映射与超时/断连恢复（errors.md、framing 超时、ambiguous Commit 恢复）。
9. 待固件 SMP/SC 校验与 Security 2 落地后，真机联调完整绑定与重连流程。

## 5. 验收门槛

- 消费端通过 conformance.md 全部 consumer checks；
- 复现 fixtures 下 framing/profile/link-limits/wire-limits 向量；
- 真机联调通过后，按 `compatibility.md` 流程在 `known-good.yaml` 登记
  （当前尚无 Device Link verified 条目，登记前不宣称互操作）。
