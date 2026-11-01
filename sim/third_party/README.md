# Third-party simulator dependencies

- `stb_image.h` (v2.30): public-domain image decoder from
  <https://github.com/nothings/stb>. It is compiled only by the opt-in real
  music backend to decode JPEG cover art; the firmware keeps using
  `espressif__esp_new_jpeg`.
