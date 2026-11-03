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
table. The write covers `0x0`-`0x400000`; the upper 4 MB of the 8 MB flash is
left untouched. A unit that still carries the factory CP image is not
recoverable once written, so dump it first when that matters:

```sh
esptool.py --chip esp32c6 -p /dev/ttyUSB0 read_flash 0 0x800000 c6-factory.bin
```

## Flash over UART

1. Wire the USB-UART to J6:
   - adapter TX -> pin 23 (`C6_U0RXD`)
   - adapter RX -> pin 25 (`C6_U0TXD`)
   - adapter GND -> pin 29 (GND)
2. Hold `C6_IO9` (J6 pin 27) low at power-up; keep the P4 in BOOT mode.
3. `idf.py -p /dev/ttyUSB0 flash`, then release `C6_IO9` and reset the board.
4. On the P4 side the product config already sets `radio_enabled = true` for
   this board, and `p4_radio_runtime` performs the explicit hosted
   initialization. The image that previously ran with the radio disabled had
   deferred onboarding, so the first boot after the upgrade lands on Home:
   reopen provisioning from Settings -> Setup, or perform a factory reset when
   a first-run Setup flow is required.
5. Confirm the running CP in the P4 boot log: `esp_hosted` prints the SDIO
   handshake and `esp-hosted fw versions: host=... coprocessor=... (match)`;
   `p4_radio_runtime` logs only `hosted SDIO transport ready`. The C6 reset is
   visible in the `eh_sdio` / `eh_host_port_reset` lines.

The factory CP on a new board is an esp_hosted 1.x image without the hosted CP
OTA RPC (added in 2.6.0). This image is 3.0.10 and does expose that RPC, but the
P4 side has no CP update entry point yet, so UART remains the supported
upgrade path.

## Notes

- `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` on the P4 host stays
  disabled: `p4_radio_runtime` initializes the transport explicitly before
  Device Link, and the CP image does not depend on that setting.
- The hosted bring-up resets the C6 before the first frame; expect about 1.5 s
  of additional startup while the co-processor boots.
- The 4 MB partition table is compatible with the 8 MB C6H8; the unused upper
  flash is left for a future larger layout.
