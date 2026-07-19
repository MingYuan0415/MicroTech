/** @file Heap capability constants used by integration host tests. */
#ifndef __CROSS_LAYER_ESP_HEAP_CAPS_H__
#define __CROSS_LAYER_ESP_HEAP_CAPS_H__

#include <stdint.h>

#define MALLOC_CAP_8BIT   (UINT32_C(1) << 2)
#define MALLOC_CAP_SPIRAM (UINT32_C(1) << 10)

#endif /* __CROSS_LAYER_ESP_HEAP_CAPS_H__ */
