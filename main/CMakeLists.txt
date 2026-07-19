idf_component_register(
    SRCS "app_main.c" "app_runtime.c" "app_runtime_pm.c" "network_runtime.c"
         "fs_storage/fs_storage.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        apps
        bsp
        mt_log
        fatfs
        littlefs
        esp_event
        esp_netif
        esp_partition
        esp_driver_usb_serial_jtag
        event_bus
        nv_storage
        time_service
        system_pm
        power_service
        imu_service
        audio_service
        sd_storage_service
        wifi_service
        ble_service
        app_core
        app_theme
)

# 资源分区打包规则
if(CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS)
    spiffs_create_partition_assets(res res_fs FLASH_IN_PROJECT MMAP_FILE_SUPPORT_FORMAT ".ttf")
else()
    fatfs_create_rawflash_image(res res_fs FLASH_IN_PROJECT)
endif()
