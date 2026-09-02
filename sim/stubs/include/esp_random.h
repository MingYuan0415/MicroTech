/** @file Hardware-random compatibility declarations for integration tests. */
#ifndef __CROSS_LAYER_ESP_RANDOM_H__
#define __CROSS_LAYER_ESP_RANDOM_H__

#include <stdint.h>

/** @brief Return a deterministic unique nonce for host storage tests. */
uint32_t esp_random(void);

#endif /* __CROSS_LAYER_ESP_RANDOM_H__ */
