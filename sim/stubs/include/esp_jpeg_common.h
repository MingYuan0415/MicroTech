/** @file Minimal host-side JPEG type compatibility for the music service. */
#ifndef __SIM_ESP_JPEG_COMMON_H__
#define __SIM_ESP_JPEG_COMMON_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    JPEG_PIXEL_FORMAT_RGB888 = 0,
    JPEG_PIXEL_FORMAT_RGB565_BE,
    JPEG_PIXEL_FORMAT_RGB565_LE,
} jpeg_pixel_format_t;

typedef enum
{
    JPEG_ERR_OK = 0,
    JPEG_ERR_FAIL = -1,
    JPEG_ERR_NO_MEM = -2,
    JPEG_ERR_NO_MORE_DATA = -3,
    JPEG_ERR_INVALID_PARAM = -4,
    JPEG_ERR_BAD_DATA = -5,
    JPEG_ERR_UNSUPPORT_FMT = -6,
    JPEG_ERR_UNSUPPORT_STD = -7,
    JPEG_ERR_INVALID_SIZE = -8,
} jpeg_error_t;

typedef enum
{
    JPEG_ROTATE_0D = 0,
    JPEG_ROTATE_90D,
    JPEG_ROTATE_180D,
    JPEG_ROTATE_270D,
} jpeg_rotate_t;

typedef struct
{
    uint16_t width;
    uint16_t height;
} jpeg_resolution_t;

void *jpeg_calloc_align(size_t size, int aligned);
void jpeg_free_align(void *data);

#ifdef __cplusplus
}
#endif

#endif /* __SIM_ESP_JPEG_COMMON_H__ */
