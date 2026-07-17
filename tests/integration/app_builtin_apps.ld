SECTIONS
{
    .app_builtin_apps : ALIGN(8) SUBALIGN(8)
    {
        _app_builtin_apps_start = .;
        KEEP(*(.app_builtin_apps))
        _app_builtin_apps_end = .;
    }
}
INSERT AFTER .rodata;
