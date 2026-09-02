/** @file PNG writer for simulator screenshots (RGB565 source). */
#ifndef SIM_PNG_H
#define SIM_PNG_H

#include "sim_bsp.h"

/** @brief Save the framebuffer as an RGB8 PNG, enlarged by `scale`. */
int sim_png_save_rgb565(const char *path, const uint16_t *fb,
                        unsigned width, unsigned height, int scale);

/** @brief Save the live sim framebuffer through the same encoder. */
int sim_png_save_frame(const char *path, int scale);

#endif /* SIM_PNG_H */
