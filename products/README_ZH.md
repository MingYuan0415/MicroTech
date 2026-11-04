[English](README.md) | **简体中文**

# 产品清单

产品把板卡、设计 target 和随固件发布的 App 集合绑定在一起。根 `CMakeLists.txt` 先解析板卡，再解析产品，最后由 `layers/apps/app_catalog.cmake` 展开为源文件、include、固件服务依赖与 App 资源清单；App capability 由所选板卡与产品服务能力共同校验。模拟器和集成测试复用同一份 App 源文件与资源选择，但保留各自的服务 fake。

| 产品 | 板卡 | 用途 |
| --- | --- | --- |
| `s3_reference` | `waveshare_esp32_s3_touch_amoled_1_8` | S3 完整参考功能集，包含水平仪 App |
| `p4_reference` | `waveshare_esp32_p4_wifi6_touch_lcd_3_5` | P4 参考功能集，不包含仅依赖 IMU 的水平仪 App |
| `minimal` | `waveshare_esp32_s3_touch_amoled_1_8` | 组合测试用的最小产品：仅 Home、Menu、Settings |

每个 `product.cmake` 声明 `MT_PRODUCT_ID`、`MT_PRODUCT_BOARD`、`MT_PRODUCT_DESIGN_TARGET`、`MT_PRODUCT_SERVICE_CAPABILITIES`、产品 radio 策略字段、`MT_PRODUCT_APPS` 与 `MT_PRODUCT_REQUIRED_APPS`。面板、平台、系统待机和 hosted radio 等板卡事实由所选板卡清单继承，产品清单不再重复声明；解析器会把它们写入生成的产品 profile，并校验板卡事实与 App 需求。产品可以增加 `radio` 这类服务能力，但板卡必须声明 `MT_BOARD_RADIO_SUPPORTED`。

显式选择产品构建：

```sh
idf.py set-target esp32p4
idf.py -DMT_PRODUCT=p4_reference build
```

产品是构建身份的一部分：改变 App 集合会改变参与编译的源文件和打包进 `res` 分区的资源。任何改动后运行产品/catalog 配置测试：

```sh
python3 tests/run_python_tests.py configuration resources
```
