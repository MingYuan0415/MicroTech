/** @file Application descriptor compatibility (settings_app version line). */
#ifndef __SIM_ESP_APP_DESC_H__
#define __SIM_ESP_APP_DESC_H__

#include <stdint.h>

/** @brief Trimmed esp_app_desc_t exposing the version field. */
typedef struct
{
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t rb_ver;
    char version[32];
    char idf_ver[32];
    char project_name[32];
    char time[16];
    char date[16];
    char author[32];
} esp_app_desc_t;

/** @brief Return the injected (CI) or built-in version descriptor. */
const esp_app_desc_t *esp_app_get_description(void);
/** @brief Agent/runtime override of the reported version string. */
void sim_app_desc_set_version(const char *version);

#endif /* __SIM_ESP_APP_DESC_H__ */
