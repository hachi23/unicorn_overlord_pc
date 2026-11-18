/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VW_IMAGE_GEOMETRY_H
#define VW_IMAGE_GEOMETRY_H

typedef struct { float width, height; } VwImageSize;

/* Fractions of the swapchain covered by a game image. Zero means no usable image. */
static inline VwImageSize vw_image_coverage(int width, int height, int surface_width, int surface_height) {
    VwImageSize coverage = {0, 0};
    if (width > 0 && height > 0 && surface_width > 0 && surface_height > 0) {
        coverage.width = (float)width / surface_width;
        coverage.height = (float)height / surface_height;
    }
    return coverage;
}

static inline VwImageSize vw_image_logical_size(VwImageSize coverage, int window_width, int window_height) {
    VwImageSize size = {coverage.width * window_width, coverage.height * window_height};
    return size;
}
#endif
