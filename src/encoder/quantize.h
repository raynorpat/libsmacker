#ifndef SMK_QUANT_H
#define SMK_QUANT_H

#include "smk_encoder.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMK_QUANT_MAX_COLORS 256
#define SMK_QUANT_LEVELS 64
#define SMK_QUANT_BINS (SMK_QUANT_LEVELS * SMK_QUANT_LEVELS * SMK_QUANT_LEVELS)

/* One palette entry, in the 6-bit levels stored by the format. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} smk_rgb6;

typedef struct {
    uint32_t bins[SMK_QUANT_BINS];
} smk_histogram;

typedef struct {
    smk_rgb6 entry[SMK_QUANT_MAX_COLORS];
    uint8_t display[SMK_QUANT_MAX_COLORS * 3]; /* palmap-expanded RGB24 */
    unsigned count;
} smk_palette;

/*
 * Memoized nearest-color lookup over the 64^3 level space. Reuse one table for
 * as long as a palette stays current; initialize it with smk_quant_lut_init().
 */
typedef struct {
    uint8_t index[SMK_QUANT_BINS];
    uint8_t known[SMK_QUANT_BINS];
} smk_quant_lut;

void smk_histogram_init(smk_histogram *hist);
void smk_histogram_add(smk_histogram *hist, const uint8_t *rgb, size_t pixels);

/*
 * Build up to max_colors entries from a histogram or a single RGB24 frame.
 * Both return the number of entries written (at least one) and are
 * deterministic for identical input.
 */
unsigned smk_quant_build_global(const smk_histogram *hist, unsigned max_colors,
                                smk_palette *palette);
unsigned smk_quant_build_frame(const uint8_t *rgb, size_t pixels,
                               unsigned max_colors, smk_palette *palette);

void smk_quant_lut_init(smk_quant_lut *lut);

/*
 * Map RGB24 pixels to palette indices, optionally dithering. `lut` may be NULL,
 * in which case a temporary table is used. pixels == width * height.
 */
void smk_quant_map(const smk_palette *palette, const uint8_t *rgb,
                   unsigned width, unsigned height,
                   smk_enc_dither_mode dither, uint8_t *indices,
                   smk_quant_lut *lut);

/* Mean absolute error per component, on the 0..255 scale. */
double smk_quant_mean_error(const smk_palette *palette, const uint8_t *rgb,
                            size_t pixels, const uint8_t *indices);

#ifdef __cplusplus
}
#endif

#endif
