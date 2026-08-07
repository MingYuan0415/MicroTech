# MicroTech Device Link 实施记录

## 1. 定位

本文记录 Device Link v1（provisioning 契约子模块中的 device-link-* 系列文档）在
固件侧 `ble_runtime` 组件的实施进展、架构决策、验证边界与遗留项。契约权威来源是
`contracts/provisioning`（`docs/device-link-{framing,gatt,lifecycle,security,
discovery}-v1.md` 与 `profiles/device-link-v1.yaml`）。

实施按单元提交，每个单元在提交前经过多轮 critical-review 至零 finding，并满足：
host 测试（none/ASan/TSan）全绿、`idf.py build` 通过、AStyle 与 `git diff --check`
干净。`ble_nimble_port.c` 因缺少 NimBLE fakes 不在 host 编译范围内，其生产时序
只能由真机验证覆盖。

## 2. 单元清单（P2.1–P2.8）

| 单元 | 提交 | 内容 |
|---|---|---|
| P2.1 | `ff04a6f` | 静态 GATT registry 的 admission → ATT 加密标志映射 |
| P2.2 | `bd9b8ad` | Link v1 envelope wire codec（flags 原始值、超宽 varint、负枚举防护） |
| P2.3 | `6788d8e` | 请求 dispatcher（判重、动态 session-id 集） |
| P2.4 | `a7ff8d5` | link_state 编码 + framing reassembler（减法边界、完成保留 buffer） |
| P2.5 | `f4a08cf` | link_session 状态机（generation/epoch/revision 三重乱序防护） |
| P2.6 | `4303784` | 事件序列 + snapshot 编码（序列边界 seam、零值省略） |
| P2.7 | `6f250ef` | link_service 会话编排（双通道 slot、MTU 分帧、bootstrap 可达、epoch 锁死） |
| P2.8 | `bbf7ad2` | GATT 生产接线（transport-only，SMP admission fail-closed） |

## 3. P2.8 架构定案

P2.8 是 28 轮审查后收敛的 transport 接线，关键决策：

- **GATT 数据库**：注册冻结的 Device Link v1 五特征（link_state/session_rx/session_tx/
  control_rx/control_tx，UUID 与 admission 来自 profile）与 transfer 三特征（未实现，
  全部拒绝）；`ble_gatt_registry_seal()` 在 host 启动前调用。
- **RX 通道**：`_ble_link_gatt_access` 先核对连接句柄与按通道 admission，再进入
  reassembler；boot id 不匹配在 envelope decode 后立即终态关闭 session（不发响应）。
- **一次一个 transaction**：`pending_transactions` 计数门——响应/事件最终片确认后
  释放，期间新请求返回 BUSY；断连、超时、发送失败与 generation 变更全部清理。
- **TX 调度**：scheduler 串行化通知与指示；同步/异步失败都终止整个 transaction
  （fatal 粘滞 + `transaction_aborted`），`is_last` 贯穿 submit/completion 元数据。
- **定时器**：esp_timer arg 编码 `(epoch<<2)|kind` 作为不可变身份；callback 只向
  静态（永不释放）命令队列投递；owner task 独占 deadline/armed 状态并做 deadline
  扫描，旧 tick/丢失 tick 均无害；2 s 指示确认窗口与 5 s reassembly idle 到期
  fail-closed。
- **锁**：`s_link_state_lock` 为递归互斥量，覆盖 bridge feed、TX consumer 与 owner
  到期处理；NimBLE 在 ops 调用内同步投递 NOTIFY_TX 时同任务重入安全。
- **安全**：SMP 启用 SC bonding 与 ENC+ID key distribution；但 bond-store 的
  identity/SC/LTK 校验尚未实现，`SC_BOND_VERIFIED` 与 `identity_known` 保持
  false，session admission 因此 fail-closed（生产不可达安全会话是已知边界）。

## 4. 验证范围

已执行：

- `idf.py build` 通过；
- host 16 目标在 none/ASan/TSan 下全绿（全新构建目录）；
- AStyle 与 `git diff --check` 干净；
- 契约 fixture 由 `tests/connectivity` 与 `tests/integration` 覆盖（跨层宿主测试）。

未执行（需真机）：

- GATT database 注册与特征读写行为；
- SMP bond-store identity / SC / LTK 校验（当前 fail-closed）；
- 逐特征 CCCD 恢复与授权抑制；
- MTU 23 与协商 MTU 下的多分片收发；
- 断连重连、conn_handle 重用与第二 ACL 拒绝场景；
- 2 s / 5 s 定时器 race 与 esp_timer task 调度行为。

## 5. 遗留项

1. SMP bond-store identity / SC / LTK 校验实现（当前 fail-closed），完成后恢复
   `SC_BOND_VERIFIED` 与 `identity_known` 的上报并解除 admission 门。
2. Security 2（AES-GCM）真实加密与本地确认流程；生产 `authorize_enabled=false`
   下的 UNSUPPORTED_OPERATION 占位需要替换。
3. 真机验证清单（见第 4 节未执行项）。
4. setup_app unavailable UI（GUI 任务统一排期，见主仓库 UI 工作流）。
5. Android 侧按同一契约生成消费代码与真机联调。

## 6. 结论

Device Link GATT transport 已具备可上板验证的形态；安全 admission 与真机 conformance
是有意的后续 scope，不构成 transport 单元的缺陷声明。
