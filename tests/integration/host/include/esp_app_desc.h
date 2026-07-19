/** @file ESP application-description compatibility declarations. */
#ifndef __CROSS_LAYER_ESP_APP_DESC_H__
#define __CROSS_LAYER_ESP_APP_DESC_H__

#include <stdint.h>

/** @brief Host subset matching fields consumed by the applications. */
typedef struct esp_app_desc
{
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserved[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
} esp_app_desc_t;

/** @brief Return the immutable host application description. */
const esp_app_desc_t *esp_app_get_description(void);

#endif /* __CROSS_LAYER_ESP_APP_DESC_H__ */
