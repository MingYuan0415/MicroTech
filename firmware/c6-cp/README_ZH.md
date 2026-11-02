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

产物为 `build/microtech_c6_cp.bin` 以及 C6 bootloader、分区表。首次写入前必须
备份 C6 工厂 8 MB flash。

## 烧录（3.3 V USB-UART 到货后）

1. 备份 C6 工厂固件（8 MB）：

   ```sh
   esptool.py --chip esp32c6 -p /dev/ttyUSB0 read_flash 0 0x800000 c6-factory.bin
   ```

2. USB-UART 接 J6：
   - 适配器 TX -> pin 23（`C6_U0RXD`）
   - 适配器 RX -> pin 25（`C6_U0TXD`）
   - 适配器 GND -> pin 29（GND）
3. 上电时把 `C6_IO9`（J6 pin 27）拉低；P4 保持 BOOT 模式。
4. `idf.py -p /dev/ttyUSB0 flash`，然后松开 `C6_IO9`、复位板子并恢复 P4 固件。
5. P4 侧把产品配置里的 `radio_enabled` 设为 `true`；`p4_radio_runtime` 已负责
   显式初始化 hosted 链路，如需重新走 Setup，再重置 onboarding 状态。

工厂 CP 预计是 esp_hosted 1.x，不带 hosted CP OTA RPC（2.6.0 才加入），因此以
UART 升级为准；读取 CP 版本前不要依赖 P4 侧 CP OTA。

## 说明

- P4 host 在 radio 关闭期间保持
  `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` 为 n；CP 镜像本身不依赖该
  配置。
- 4 MB 分区表兼容 8 MB 的 C6H8，上半区留给后续更大的布局。
