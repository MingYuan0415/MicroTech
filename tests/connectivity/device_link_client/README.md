# Device Link v2 真机矩阵参考客户端

宿主机侧 Python 客户端，用于执行 `doc/device-link-implementation.md` 的
硬件矩阵。实现依据：契约 reference codec（`tooling/contractcheck/wire.py` /
`codec.py`，金标校验过）与 ESP-IDF v6.0.2 protocomm Security 2
（`security2.c` / `esp_srp.c`，逐行对齐）。

## 依赖与运行

```sh
cd contracts/provisioning
.venv/bin/pip install bleak          # 一次性；venv 已被 .gitignore
.venv/bin/python3 tests/connectivity/device_link_client/link_client.py scan
.venv/bin/python3 tests/connectivity/device_link_client/link_client.py \
    matrix --item N --device AA:BB:CC:DD:EE:FF --boot-id 0x... [选项]
```

BlueZ 的 ATT MTU 取决于系统配置（默认 23 可退化为多分片，矩阵第 1 项会
记录实际协商值；如需 498 请配置系统 BlueZ MTU）。

## 验证状态（务必在执行矩阵前阅读）

- **已验证（离线）**：分片组帧/重组、应用头/状态码、link_state/广告/QR
  解码复用契约金标 codec；SRP/AES-GCM 模块带 `--self-test`（Python 内部
  客户端/设备双侧轮转 + M1/M2 校验）。
- **未验证（须上板）**：SRP/AES-GCM 模块尚未与真实固件互操作；计数器/
  nonce 约定按 ESP-IDF 适配器源码实现（IV=session_id(8)+counter(4 BE，自 1
  递增、双方向严格有序、无 nonce 前缀、密文||tag），由矩阵第 2 项判定。

## 各条目操作

| 条目 | 命令/流程 |
| --- | --- |
| 1 | `matrix --item 1`：记录协商 MTU、link_state 读取；写入 495B 应被 ATT 接受、496B 应被拒绝（会话随之关闭属预期）。 |
| 2 | `matrix --item 2 --pop <32hex>` 或 `--public-password <6hex>`（实例 ID 取广告 service data 后 3 字节）；成功后 GetManifest 应 OK。 |
| 3 | `matrix --item 3 --state-file f.json`：Prepare→Commit 探针（CONFIRMATION_REQUIRED+token）→**在设备端点击确认**→Enter→Commit→AUTHORIZED→Recovery Query；长凭据写入 state file 供重启后用。 |
| 4 | `matrix --item 4 --wifi-ssid S --wifi-password P`：需先完成条目 3 授权（同一次会话或先用 state file 重建）。 |
| 5 | `matrix --item 5`：冷启动瞬时调用 get_manifest/get_link_snapshot/get_status/start_scan/get_scan_results，逐项断言状态 ∈ allowed_statuses。 |
| 6 | 操作员流程：flash `CONFIG_DEVICE_LINK_SECURITY_STORAGE_FAULT_TEST=y` 调试构建（WRITE_N=1 或 CORRUPT_WRITE=y，见 Kconfig）→ 按 debug-esp32s3 规则记录启动/恢复日志 → 回烧生产构建。断电点：在 commit 持久化前/后断电，重启后 Recovery Query 或 fail-closed 行为符合 F-5。 |
| 7 | 操作员流程：工厂重置、蓝牙禁用→重启→启用，配合条目 3 的 state file 验证凭据保留。 |
| 8 | `matrix --item 8 --soak-seconds 7200`：每 5s 一次 get_link_snapshot 并断言 OK。 |

## 证据记录

每条目输出 JSON 行；硬件证据（reset reason、panic 回溯、heap、WDT、状态
迁移、原始日志）按 `debug-esp32s3` 规则留存，全部 8 项通过后条目写入
`contracts/provisioning/compatibility/verified.yaml`（firmware hash + 日期 +
client 版本），随后才允许 registry/config 发布翻转。
