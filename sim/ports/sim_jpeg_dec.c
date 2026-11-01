/** @file Host JPEG decoder used by the simulator's real music backend. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include "esp_jpeg_dec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct sim_jpeg_decoder
{
    jpeg_dec_config_t config;
    int width;
    int height;
} sim_jpeg_decoder_t;

void *jpeg_calloc_align(size_t size, int aligned)
{
    if (aligned < (int)sizeof(void *))
    {
        aligned = (int)sizeof(void *);
    }
    void *memory = NULL;
    if (posix_memalign(&memory, (size_t)aligned, size) != 0)
    {
        return NULL;
    }
    memset(memory, 0, size);
    return memory;
}

void jpeg_free_align(void *data)
{
    free(data);
}

jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *config,
                           jpeg_dec_handle_t *jpeg_dec)
{
    if (config == NULL || jpeg_dec == NULL)
    {
        return JPEG_ERR_INVALID_PARAM;
    }
    sim_jpeg_decoder_t *decoder = calloc(1, sizeof(*decoder));
    if (decoder == NULL)
    {
        return JPEG_ERR_NO_MEM;
    }
    decoder->config = *config;
    *jpeg_dec = decoder;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t jpeg_dec,
                                   jpeg_dec_io_t *io,
                                   jpeg_dec_header_info_t *out_info)
{
    sim_jpeg_decoder_t *decoder = jpeg_dec;
    if (decoder == NULL || io == NULL || out_info == NULL ||
            io->inbuf == NULL || io->inbuf_len <= 0)
    {
        return JPEG_ERR_INVALID_PARAM;
    }
    int width = 0;
    int height = 0;
    int components = 0;
    if (!stbi_info_from_memory(io->inbuf, io->inbuf_len, &width, &height,
                               &components) ||
            width <= 0 || height <= 0)
    {
        return JPEG_ERR_BAD_DATA;
    }
    decoder->width = width;
    decoder->height = height;
    out_info->width = (uint16_t)width;
    out_info->height = (uint16_t)height;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t jpeg_dec,
                                     int *outbuf_len)
{
    sim_jpeg_decoder_t *decoder = jpeg_dec;
    if (decoder == NULL || outbuf_len == NULL)
    {
        return JPEG_ERR_INVALID_PARAM;
    }
    int width = decoder->config.scale.width != 0U ?
                decoder->config.scale.width : decoder->width;
    int height = decoder->config.scale.height != 0U ?
                 decoder->config.scale.height : decoder->height;
    if (width <= 0 || height <= 0 ||
            (int64_t)width * height > INT32_MAX / 2)
    {
        return JPEG_ERR_INVALID_SIZE;
    }
    *outbuf_len = width * height * 2;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t jpeg_dec, jpeg_dec_io_t *io)
{
    sim_jpeg_decoder_t *decoder = jpeg_dec;
    if (decoder == NULL || io == NULL || io->inbuf == NULL ||
            io->outbuf == NULL || io->inbuf_len <= 0)
    {
        return JPEG_ERR_INVALID_PARAM;
    }
    int width = 0;
    int height = 0;
    int components = 0;
    stbi_uc *rgb = stbi_load_from_memory(io->inbuf, io->inbuf_len,
                                         &width, &height, &components, 3);
    if (rgb == NULL || width <= 0 || height <= 0)
    {
        return JPEG_ERR_BAD_DATA;
    }
    int target_width = decoder->config.scale.width != 0U ?
                       decoder->config.scale.width : width;
    int target_height = decoder->config.scale.height != 0U ?
                        decoder->config.scale.height : height;
    if (target_width <= 0 || target_height <= 0 ||
            (int64_t)target_width * target_height * 2 > io->out_size)
    {
        stbi_image_free(rgb);
        return JPEG_ERR_INVALID_PARAM;
    }
    uint16_t *out = (uint16_t *)io->outbuf;
    for (int y = 0; y < target_height; ++y)
    {
        const int source_y = y * height / target_height;
        for (int x = 0; x < target_width; ++x)
        {
            const int source_x = x * width / target_width;
            const uint8_t *pixel = rgb +
                                   ((size_t)source_y * (size_t)width +
                                    (size_t)source_x) * 3U;
            out[(size_t)y * (size_t)target_width + (size_t)x] =
                (uint16_t)(((uint16_t)(pixel[0] >> 3U) << 11U) |
                           ((uint16_t)(pixel[1] >> 2U) << 5U) |
                           ((uint16_t)(pixel[2] >> 3U)));
        }
    }
    stbi_image_free(rgb);
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t jpeg_dec)
{
    free(jpeg_dec);
    return JPEG_ERR_OK;
}
