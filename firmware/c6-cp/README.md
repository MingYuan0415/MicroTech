**English** | [简体中文](README_ZH.md)

# ESP32-C6 coprocessor firmware

ESP-Hosted 3.0.10 coprocessor (CP) for the ESP32-P4-WIFI6-Touch-LCD-3.5
board. The P4 runs the host side over SDIO; the C6 owns Wi-Fi and the BT
controller and hands BLE HCI to the P4's NimBLE/Device Link stack.

This is a standalone ESP-IDF project and is not part of the root build.

## Image

| Item | Value |
| --- | --- |
| Wire version | ESP-Hosted 3.0.10 (same as the P4 host) |
| Wi-Fi | CP Wi-Fi enabled |
| Bluetooth | CP BT controller only (`BT_CONTROLLER_ONLY`), HCI over SDIO VHCI |
| Transport | SDIO (C6 SDIO slave) |
| Flash layout | 4 MB dual-OTA table (`partitions_eh_cp_ota_4m.csv`) |
| C6 flash | 8 MB; the 4 MB image leaves the upper region untouched |

## Build

```sh
cd firmware/c6-cp
idf.py set-target esp32c6
idf.py build
```

The image is `build/microtech_c6_cp.bin` plus the C6 bootloader and partition
table. The C6 factory 8 MB flash must be dumped before the first write; see
below.

## Flash (after the 3.3 V USB-UART arrives)

1. Back up the factory C6 flash, 8 MB:

   ```sh
   esptool.py --chip esp32c6 -p /dev/ttyUSB0 read_flash 0 0x800000 c6-factory.bin
   ```

2. Wire the USB-UART to J6:
   - adapter TX -> pin 23 (`C6_U0RXD`)
   - adapter RX -> pin 25 (`C6_U0TXD`)
   - adapter GND -> pin 29 (GND)
3. Hold `C6_IO9` (J6 pin 27) low at power-up; keep the P4 in BOOT mode.
4. `idf.py -p /dev/ttyUSB0 flash`, then release `C6_IO9`, reset the board and
   restore the P4 firmware.
5. On the P4 side, set `radio_enabled = true` in the product config. The
   `p4_radio_runtime` layer now performs the explicit hosted initialization;
   reset the persisted onboarding state if Setup must be rerun.

The factory CP is expected to be an esp_hosted 1.x image without the hosted
CP OTA RPC (added in 2.6.0), so the UART path above is the supported upgrade;
do not rely on P4-side CP OTA before reading the CP version.

## Notes

- `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` on the P4 host stays
  disabled while the radio is disabled; the CP image itself does not depend on
  that setting.
- The 4 MB partition table is compatible with the 8 MB C6H8; the unused upper
  flash is left for a future larger layout.
