#ifndef SMK_IMAGE_H
#define SMK_IMAGE_H

#include "smk_encoder.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Load an image file (TGA, BMP, PCX) as top-down RGB24.
 *
 * Alpha is composited over background[3]. On success *rgb receives a malloc'd
 * width * height * 3 buffer that the caller must free.
 */
smk_enc_status smk_image_load(const char *path, const uint8_t background[3],
                              uint8_t **rgb, unsigned *width, unsigned *height,
                              char *err, size_t errlen);

#endif
