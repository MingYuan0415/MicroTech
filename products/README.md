**English** | [简体中文](README_ZH.md)

# Product Manifests

A product binds a board, a design target and the App set that ships in one
firmware image. The root `CMakeLists.txt` resolves the board first, then the
product, then expands `layers/apps/app_catalog.cmake` into sources, includes,
services, capabilities and resource manifests.

| Product | Board | Purpose |
| --- | --- | --- |
| `s3_reference` | `waveshare_esp32_s3_touch_amoled_1_8` | Full S3 reference feature set, including the Level App |
| `p4_reference` | `waveshare_esp32_p4_wifi6_touch_lcd_3_5` | P4 reference feature set without the IMU-only Level App |
| `minimal` | `waveshare_esp32_s3_touch_amoled_1_8` | Composition fixture: Home, Menu and Settings only |

Each `product.cmake` declares `MT_PRODUCT_ID`, `MT_PRODUCT_BOARD`,
`MT_PRODUCT_DESIGN_TARGET`, `MT_PRODUCT_HAL_CAPABILITIES`, the product policy
fields, `MT_PRODUCT_APPS` and `MT_PRODUCT_REQUIRED_APPS`. Products may add
service capabilities such as `radio`; they may not declare a BSP peripheral
capability that the board backend has not implemented.

Build a product explicitly with:

```sh
idf.py -DMT_PRODUCT=p4_reference build
```

The product is part of the build identity: changing the App set changes the
compiled sources and the packaged `res` partition. Run the product/catalog
configuration tests after any change:

```sh
python3 tests/run_python_tests.py configuration resources
```
