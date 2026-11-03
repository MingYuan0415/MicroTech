[English](README.md) | **简体中文**

# ESP32-C6 协处理器固件

用于 ESP32-P4-WIFI6-Touch-LCD-3.5 的 ESP-Hosted 3.0.10 协处理器（CP）固件。
P4 通过 SDIO 运行 host 侧；C6 负责 Wi-Fi 与 BT controller，并把 BLE HCI 交给
P4 的 NimBLE/Device Link。

这是独立的 ESP-IDF 工程，不参与根工程构建。

## 镜像

| 项目 | 值 |
| --- | --- |
| 线协议版本 | ESP-Hosted 3.0.10（与 P4 host 一致） |
| Wi-Fi | CP Wi-Fi 开启 |
| 蓝牙 | 仅 CP BT controller（`BT_CONTROLLER_ONLY`），HCI 走 SDIO VHCI |
| 传输 | SDIO（C6 SDIO slave） |
| Flash 布局 | 4 MB 双 OTA 表（`partitions_eh_cp_ota_4m.csv`） |
| C6 flash | 8 MB；4 MB 镜像不触碰上半区 |

## 构建

```sh
cd firmware/c6-cp
idf.py set-target esp32c6
idf.py build
```

产物为 `build/microtech_c6_cp.bin` 以及 C6 bootloader、分区表。写入覆盖
`0x0`–`0x400000`，8 MB flash 的上半区不受影响。仍带工厂 CP 镜像的设备一旦写入
便不可恢复，需要时先备份：

```sh
esptool.py --chip esp32c6 -p /dev/ttyUSB0 read_flash 0 0x800000 c6-factory.bin
```

## UART 烧录

1. USB-UART 接 J6：
   - 适配器 TX -> pin 23（`C6_U0RXD`）
   - 适配器 RX -> pin 25（`C6_U0TXD`）
   - 适配器 GND -> pin 29（GND）
2. 上电时把 `C6_IO9`（J6 pin 27）拉低；P4 保持 BOOT 模式。
3. `idf.py -p /dev/ttyUSB0 flash`，然后松开 `C6_IO9` 并复位板子。
4. P4 侧产品配置已为该版型设置 `radio_enabled = true`，`p4_radio_runtime` 负责
   显式初始化 hosted 链路。之前 radio 关闭的镜像已把 onboarding 置为 defer，
   因此升级后的首次启动进入 Home：从 Settings -> Setup 重新配网，或出厂重置后
   走完整的首次 Setup 流程。
5. 在 P4 启动日志确认 CP：`esp_hosted` 会打印 SDIO 握手和
   `esp-hosted fw versions: host=... coprocessor=... (match)`；`p4_radio_runtime`
   只打印 `hosted SDIO transport ready`。C6 复位见 `eh_sdio` /
   `eh_host_port_reset` 日志。

新板的工厂 CP 是 esp_hosted 1.x，不带 hosted CP OTA RPC（2.6.0 才加入）。本镜像
为 3.0.10、已提供该 RPC，但 P4 侧尚无 CP 更新入口，因此仍以 UART 升级为准。

## 说明

- P4 host 保持 `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` 为 n：
  `p4_radio_runtime` 在 Device Link 之前显式初始化传输，CP 镜像本身不依赖该
  配置。
- hosted 初始化会在首帧之前复位 C6，CP 启动期间大约多出 1.5 秒启动时间。
- 4 MB 分区表兼容 8 MB 的 C6H8，上半区留给后续更大的布局。
