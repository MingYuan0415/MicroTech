**English** | [简体中文](README_ZH.md)

# Product Manifests

A product binds a board, a design target and the App set that ships in one
firmware image. The root `CMakeLists.txt` resolves the board first, then the
product, then expands `layers/apps/app_catalog.cmake` into sources, include
directories, firmware service requirements and App resource manifests. App
capabilities are validated against the selected board and product service
set. Host harnesses such as the simulator and integration suite reuse the
same App sources and resource selection but keep their own service fakes.

| Product | Board | Purpose |
| --- | --- | --- |
| `s3_reference` | `waveshare_esp32_s3_touch_amoled_1_8` | Full S3 reference feature set, including the Level App |
| `p4_reference` | `waveshare_esp32_p4_wifi6_touch_lcd_3_5` | P4 reference feature set without the IMU-only Level App |
| `minimal` | `waveshare_esp32_s3_touch_amoled_1_8` | Composition fixture: Home, Menu and Settings only |

Each `product.cmake` declares `MT_PRODUCT_ID`, `MT_PRODUCT_BOARD`,
`MT_PRODUCT_DESIGN_TARGET`, `MT_PRODUCT_SERVICE_CAPABILITIES`, the product
radio policy fields, `MT_PRODUCT_APPS` and `MT_PRODUCT_REQUIRED_APPS`. The
resolver inherits panel, platform, system-PM and hosted-radio facts from the
selected board manifest and emits them into the generated product profile;
products never duplicate those facts. Products may add service capabilities
such as `radio`, but the board must declare `MT_BOARD_RADIO_SUPPORTED` for
that service. The product resolver validates board facts and App
requirements against the board manifest.

Build a product explicitly with:

```sh
idf.py set-target esp32p4
idf.py -DMT_PRODUCT=p4_reference build
```

The product is part of the build identity: changing the App set changes the
compiled sources and the packaged `res` partition. Run the product/catalog
configuration tests after any change:

```sh
python3 tests/run_python_tests.py configuration resources
```
