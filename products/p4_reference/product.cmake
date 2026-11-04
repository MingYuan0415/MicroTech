# MicroTech P4 reference product.
#
# The board has no IMU backend, so the Level App is intentionally absent. The
# setup, weather and music Apps remain available because the hosted radio is a
# product service, not a native BSP peripheral bit.

set(MT_PRODUCT_ID "p4_reference")
set(MT_PRODUCT_BOARD "waveshare_esp32_p4_wifi6_touch_lcd_3_5")
set(MT_PRODUCT_DESIGN_TARGET "panel_320x480_touch_30hz")
set(MT_PRODUCT_HAL_CAPABILITIES radio)
set(MT_PRODUCT_RADIO_ENABLED ON)
set(MT_PRODUCT_RADIO_OPTIONAL ON)
set(MT_PRODUCT_SYSTEM_PM_SUPPORTED OFF)
set(MT_PRODUCT_HOSTED_RADIO ON)
set(MT_PRODUCT_DEVICE_PANEL "320 x 480 IPS")
set(MT_PRODUCT_DEVICE_PLATFORM "ESP32-P4")
set(MT_PRODUCT_FIRMWARE_REQUIRES esp_hosted)
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
    flybirds
    music
    diagnostics
)
set(MT_PRODUCT_REQUIRED_APPS home menu settings)
