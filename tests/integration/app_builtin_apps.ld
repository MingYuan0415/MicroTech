SECTIONS
{
    .app_manager_apps : ALIGN(8) SUBALIGN(8)
    {
        _app_manager_apps_start = .;
        KEEP(*(.app_manager_apps))
        _app_manager_apps_end = .;
    }

    .app_manager_pages : ALIGN(8) SUBALIGN(8)
    {
        _app_manager_pages_start = .;
        KEEP(*(.app_manager_pages))
        _app_manager_pages_end = .;
    }
}
INSERT AFTER .rodata;
