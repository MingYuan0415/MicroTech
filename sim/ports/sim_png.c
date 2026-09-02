/** @file PNG screenshot encoder (host libpng, deterministic settings). */
#include <stdio.h>
#include <stdlib.h>

#include <png.h>

#include "sim_png.h"

static int _write_png(const char *path, unsigned width, unsigned height,
                      const uint16_t *fb, int scale)
{
    FILE *fp = NULL;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep row = NULL;
    unsigned y;
    int ret = -1;

    fp = fopen(path, "wb");
    if (fp == NULL)
    {
        return -1;
    }
    png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL)
    {
        goto exit;
    }
    info = png_create_info_struct(png);
    if (info == NULL)
    {
        goto exit;
    }
    if (setjmp(png_jmpbuf(png)))
    {
        goto exit;
    }
    row = malloc((size_t)width * (size_t)scale * 3U);
    if (row == NULL)
    {
        goto exit;
    }
    png_init_io(png, fp);
    png_set_compression_level(png, 9);
    png_set_compression_mem_level(png, 8);
    png_set_compression_strategy(png, PNG_Z_DEFAULT_STRATEGY);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    png_set_IHDR(png, info, width * (unsigned)scale, height * (unsigned)scale,
                 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);
    for (y = 0; y < height; y++)
    {
        const uint16_t *src = fb + (y * width);
        unsigned x;
        unsigned s;
        for (x = 0; x < width; x++)
        {
            unsigned r5 = (src[x] >> 11) & 0x1FU;
            unsigned g6 = (src[x] >> 5) & 0x3FU;
            unsigned b5 = src[x] & 0x1FU;
            const png_byte r = (png_byte)((r5 << 3) | (r5 >> 2));
            const png_byte g = (png_byte)((g6 << 2) | (g6 >> 4));
            const png_byte b = (png_byte)((b5 << 3) | (b5 >> 2));
            for (s = 0; s < (unsigned)scale; s++)
            {
                const unsigned col = (x * (unsigned)scale + s) * 3U;
                row[col] = r;
                row[col + 1U] = g;
                row[col + 2U] = b;
            }
        }
        for (s = 0; s < (unsigned)scale; s++)
        {
            png_write_row(png, row);
        }
    }
    png_write_end(png, NULL);
    ret = 0;

exit:
    free(row);
    if (png != NULL)
    {
        png_destroy_write_struct(&png, info ? &info : NULL);
    }
    if (fp != NULL)
    {
        fclose(fp);
    }
    return ret;
}

int sim_png_save_rgb565(const char *path, const uint16_t *fb,
                        unsigned width, unsigned height, int scale)
{
    if ((path == NULL) || (fb == NULL) || (width == 0U) || (height == 0U) ||
            (scale < 1))
    {
        return -1;
    }
    return _write_png(path, width, height, fb, scale);
}

int sim_png_save_frame(const char *path, int scale)
{
    const uint16_t *fb = sim_bsp_framebuffer();

    if (fb == NULL)
    {
        return -1;
    }
    return sim_png_save_rgb565(path, fb, SIM_BSP_H_RES, SIM_BSP_V_RES, scale);
}
