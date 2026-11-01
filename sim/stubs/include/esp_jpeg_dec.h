/** @file Minimal host-side JPEG decoder API for the music service. */
#ifndef __SIM_ESP_JPEG_DEC_H__
#define __SIM_ESP_JPEG_DEC_H__

#include "esp_jpeg_common.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *jpeg_dec_handle_t;

typedef struct
{
    jpeg_pixel_format_t output_type;
    jpeg_resolution_t scale;
    jpeg_resolution_t clipper;
    jpeg_rotate_t rotate;
    bool block_enable;
} jpeg_dec_config_t;

typedef struct
{
    uint16_t width;
    uint16_t height;
} jpeg_dec_header_info_t;

typedef struct
{
    uint8_t *inbuf;
    int inbuf_len;
    int inbuf_remain;
    uint8_t *outbuf;
    int out_size;
} jpeg_dec_io_t;

jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *config,
                           jpeg_dec_handle_t *jpeg_dec);
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t jpeg_dec,
                                   jpeg_dec_io_t *io,
                                   jpeg_dec_header_info_t *out_info);
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t jpeg_dec,
                                     int *outbuf_len);
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t jpeg_dec, jpeg_dec_io_t *io);
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t jpeg_dec);

#ifdef __cplusplus
}
#endif

#endif /* __SIM_ESP_JPEG_DEC_H__ */
