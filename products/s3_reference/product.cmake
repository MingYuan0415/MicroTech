# MicroTech S3 reference product.
#
# The product owns App composition and runtime policy identity. Hardware
# capabilities remain in the selected board manifest; this file cannot declare
# a capability that the board does not implement.

set(MT_PRODUCT_ID "s3_reference")
set(MT_PRODUCT_BOARD "waveshare_esp32_s3_touch_amoled_1_8")
set(MT_PRODUCT_DESIGN_TARGET "panel_368x448_touch_60hz")
set(MT_PRODUCT_HAL_CAPABILITIES radio)
set(MT_PRODUCT_RADIO_ENABLED ON)
set(MT_PRODUCT_RADIO_OPTIONAL OFF)
set(MT_PRODUCT_SYSTEM_PM_SUPPORTED ON)
set(MT_PRODUCT_HOSTED_RADIO OFF)
set(MT_PRODUCT_DEVICE_PANEL "368 x 448 AMOLED")
set(MT_PRODUCT_DEVICE_PLATFORM "ESP32-S3")
set(MT_PRODUCT_FIRMWARE_REQUIRES "")
set(MT_PRODUCT_APPS
    common
    home
    menu
    settings
    setup
    weather
    clock
    calendar
    calculator
    recorder
    files
    level
    flybirds
    music
    diagnostics
)
set(MT_PRODUCT_REQUIRED_APPS home menu settings)
