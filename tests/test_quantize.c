#include "test.h"
#include "quantize.h"

#include <stdlib.h>
#include <string.h>

/* The format's fixed 6-bit to 8-bit expansion table (see docs/smacker.md). */
static const uint8_t palmap[64] = {
    0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
    0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
    0x41, 0x45, 0x49, 0x4d, 0x51, 0x55, 0x59, 0x5d,
    0x61, 0x65, 0x69, 0x6d, 0x71, 0x75, 0x79, 0x7d,
    0x82, 0x86, 0x8a, 0x8e, 0x92, 0x96, 0x9a, 0x9e,
    0xa2, 0xa6, 0xaa, 0xae, 0xb2, 0xb6, 0xba, 0xbe,
    0xc3, 0xc7, 0xcb, 0xcf, 0xd3, 0xd7, 0xdb, 0xdf,
    0xe3, 0xe7, 0xeb, 0xef, 0xf3, 0xf7, 0xfb, 0xff
};

static unsigned rgb6_bin(unsigned r, unsigned g, unsigned b)
{
    return (r << 12) | (g << 6) | b;
}

static void fill_pattern(uint8_t *rgb, unsigned width, unsigned height)
{
    unsigned x;
    unsigned y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint8_t *p = rgb + ((size_t)y * width + x) * 3;
            p[0] = (uint8_t)((x * 7 + y * 13) & 0xff);
            p[1] = (uint8_t)((x * 11 + y * 5) & 0xff);
            p[2] = (uint8_t)((x * 3 + y * 17) & 0xff);
        }
    }
}

void test_quantize_histogram_accumulates(void)
{
    smk_histogram *hist = (smk_histogram *)malloc(sizeof(*hist));
    uint8_t pixels[9] = {
        0, 0, 0,
        255, 0, 0,
        0, 255, 0
    };
    unsigned i;

    SMK_ASSERT(hist != NULL);
    if (hist == NULL)
        return;
    smk_histogram_init(hist);
    for (i = 0; i < SMK_QUANT_BINS; i++)
        SMK_ASSERT_EQ_INT(hist->bins[i], 0);

    smk_histogram_add(hist, pixels, 3);
    SMK_ASSERT_EQ_INT(hist->bins[rgb6_bin(0, 0, 0)], 1);
    SMK_ASSERT_EQ_INT(hist->bins[rgb6_bin(63, 0, 0)], 1);
    SMK_ASSERT_EQ_INT(hist->bins[rgb6_bin(0, 63, 0)], 1);
    SMK_ASSERT_EQ_INT(hist->bins[rgb6_bin(1, 0, 0)], 0);
    free(hist);
}

void test_quantize_frame_palette_limit(void)
{
    unsigned width = 64;
    unsigned height = 64;
    uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3);
    smk_palette palette;
    unsigned i;

    SMK_ASSERT(rgb != NULL);
    if (rgb == NULL)
        return;
    fill_pattern(rgb, width, height);
    memset(&palette, 0xaa, sizeof(palette));
    SMK_ASSERT(smk_quant_build_frame(rgb, (size_t)width * height, 16,
                                     &palette) <= 16);
    SMK_ASSERT(palette.count >= 1);
    SMK_ASSERT(palette.count <= 16);
    for (i = 0; i < palette.count; i++) {
        SMK_ASSERT(palette.entry[i].r < 64);
        SMK_ASSERT(palette.entry[i].g < 64);
        SMK_ASSERT(palette.entry[i].b < 64);
        SMK_ASSERT_EQ_INT(palette.display[i * 3], palmap[palette.entry[i].r]);
        SMK_ASSERT_EQ_INT(palette.display[i * 3 + 1],
                          palmap[palette.entry[i].g]);
        SMK_ASSERT_EQ_INT(palette.display[i * 3 + 2],
                          palmap[palette.entry[i].b]);
    }
    free(rgb);
}

void test_quantize_global_from_histogram(void)
{
    uint8_t pixels[12] = {
        255, 0, 0,   0, 255, 0,   0, 0, 255,   255, 255, 255
    };
    smk_histogram *hist = (smk_histogram *)malloc(sizeof(*hist));
    smk_palette palette;
    unsigned i;

    SMK_ASSERT(hist != NULL);
    if (hist == NULL)
        return;
    smk_histogram_init(hist);
    smk_histogram_add(hist, pixels, 4);
    SMK_ASSERT_EQ_INT(smk_quant_build_global(hist, 8, &palette), 4);
    for (i = 0; i < palette.count; i++) {
        SMK_ASSERT(palette.entry[i].r < 64);
        SMK_ASSERT(palette.entry[i].g < 64);
        SMK_ASSERT(palette.entry[i].b < 64);
    }
    free(hist);
}

void test_quantize_exact_256(void)
{
    unsigned width = 16;
    unsigned height = 16;
    uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3);
    uint8_t indices[256];
    smk_palette palette;
    smk_quant_lut *lut = (smk_quant_lut *)malloc(sizeof(smk_quant_lut));
    unsigned i;

    SMK_ASSERT(rgb != NULL && lut != NULL);
    if (rgb == NULL || lut == NULL) {
        free(rgb);
        free(lut);
        return;
    }
    for (i = 0; i < 256; i++) {
        unsigned r = i & 7;
        unsigned g = (i >> 3) & 7;
        unsigned b = i >> 6; /* 0..3 */
        uint8_t *p = rgb + (size_t)i * 3;

        p[0] = palmap[r];
        p[1] = palmap[g];
        p[2] = palmap[b];
    }

    SMK_ASSERT_EQ_INT(smk_quant_build_frame(rgb, 256, 256, &palette), 256);
    smk_quant_lut_init(lut);
    smk_quant_map(&palette, rgb, width, height, SMK_ENC_DITHER_NONE, indices,
                  lut);
    for (i = 0; i < 256; i++) {
        unsigned index = indices[i];
        SMK_ASSERT(index < palette.count);
        SMK_ASSERT_EQ_INT(palette.display[index * 3], rgb[i * 3]);
        SMK_ASSERT_EQ_INT(palette.display[index * 3 + 1], rgb[i * 3 + 1]);
        SMK_ASSERT_EQ_INT(palette.display[index * 3 + 2], rgb[i * 3 + 2]);
    }
    SMK_ASSERT(smk_quant_mean_error(&palette, rgb, 256, indices) == 0.0);

    free(rgb);
    free(lut);
}

void test_quantize_deterministic(void)
{
    unsigned width = 32;
    unsigned height = 32;
    uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3);
    smk_palette first;
    smk_palette second;
    uint8_t indices_a[32 * 32];
    uint8_t indices_b[32 * 32];
    smk_quant_lut *lut = (smk_quant_lut *)malloc(sizeof(smk_quant_lut));

    SMK_ASSERT(rgb != NULL && lut != NULL);
    if (rgb == NULL || lut == NULL) {
        free(rgb);
        free(lut);
        return;
    }
    fill_pattern(rgb, width, height);
    SMK_ASSERT_EQ_INT(smk_quant_build_frame(rgb, (size_t)width * height, 32,
                                            &first),
                      smk_quant_build_frame(rgb, (size_t)width * height, 32,
                                            &second));
    SMK_ASSERT_MEM_EQ(&first, &second, sizeof(first));
    smk_quant_lut_init(lut);
    smk_quant_map(&first, rgb, width, height, SMK_ENC_DITHER_NONE, indices_a,
                  lut);
    smk_quant_map(&first, rgb, width, height, SMK_ENC_DITHER_NONE, indices_b,
                  lut);
    SMK_ASSERT_MEM_EQ(indices_a, indices_b, sizeof(indices_a));

    free(rgb);
    free(lut);
}

void test_quantize_dither_error(void)
{
    unsigned width = 256;
    unsigned height = 32;
    size_t pixels = (size_t)width * height;
    uint8_t *rgb = (uint8_t *)malloc(pixels * 3);
    uint8_t *indices = (uint8_t *)malloc(pixels);
    smk_palette palette;
    smk_quant_lut *lut = (smk_quant_lut *)malloc(sizeof(smk_quant_lut));
    double none;
    double ordered;
    double fs;
    unsigned y;
    size_t i;

    SMK_ASSERT(rgb != NULL && indices != NULL && lut != NULL);
    if (rgb == NULL || indices == NULL || lut == NULL) {
        free(rgb);
        free(indices);
        free(lut);
        return;
    }
    for (y = 0; y < height; y++) {
        unsigned x;
        for (x = 0; x < width; x++) {
            uint8_t *p = rgb + ((size_t)y * width + x) * 3;
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)x;
            p[2] = (uint8_t)x;
        }
    }
    SMK_ASSERT_EQ_INT(smk_quant_build_frame(rgb, pixels, 4, &palette), 4);

    smk_quant_lut_init(lut);
    smk_quant_map(&palette, rgb, width, height, SMK_ENC_DITHER_NONE, indices,
                  lut);
    none = smk_quant_mean_error(&palette, rgb, pixels, indices);
    for (i = 0; i < pixels; i++)
        SMK_ASSERT(indices[i] < palette.count);

    smk_quant_lut_init(lut);
    smk_quant_map(&palette, rgb, width, height, SMK_ENC_DITHER_ORDERED,
                  indices, lut);
    ordered = smk_quant_mean_error(&palette, rgb, pixels, indices);
    for (i = 0; i < pixels; i++)
        SMK_ASSERT(indices[i] < palette.count);

    smk_quant_lut_init(lut);
    smk_quant_map(&palette, rgb, width, height, SMK_ENC_DITHER_FS, indices,
                  lut);
    fs = smk_quant_mean_error(&palette, rgb, pixels, indices);
    for (i = 0; i < pixels; i++)
        SMK_ASSERT(indices[i] < palette.count);

    /* Dithering trades per-pixel accuracy for pattern; keep it close. */
    SMK_ASSERT(ordered <= none + 12.0);
    SMK_ASSERT(fs <= none + 12.0);
    SMK_ASSERT(none > 0.0);

    free(rgb);
    free(indices);
    free(lut);
}

void test_quantize_global_spans_frames(void)
{
    unsigned width = 48;
    unsigned height = 48;
    size_t pixels = (size_t)width * height;
    uint8_t *first = (uint8_t *)malloc(pixels * 3);
    uint8_t *second = (uint8_t *)malloc(pixels * 3);
    uint8_t *indices = (uint8_t *)malloc(pixels);
    smk_histogram *hist = (smk_histogram *)malloc(sizeof(*hist));
    smk_palette palette;
    smk_quant_lut *lut = (smk_quant_lut *)malloc(sizeof(smk_quant_lut));
    double error;

    SMK_ASSERT(first != NULL && second != NULL && indices != NULL &&
               lut != NULL && hist != NULL);
    if (first == NULL || second == NULL || indices == NULL || lut == NULL ||
        hist == NULL) {
        free(first);
        free(second);
        free(indices);
        free(lut);
        free(hist);
        return;
    }
    fill_pattern(first, width, height);
    fill_pattern(second, width, height);
    {
        unsigned i;
        for (i = 0; i < pixels * 3; i++)
            second[i] = (uint8_t)(255 - second[i]);
    }

    smk_histogram_init(hist);
    smk_histogram_add(hist, first, pixels);
    smk_histogram_add(hist, second, pixels);
    SMK_ASSERT_EQ_INT(smk_quant_build_global(hist, 16, &palette), 16);
    smk_quant_lut_init(lut);
    smk_quant_map(&palette, first, width, height, SMK_ENC_DITHER_NONE, indices,
                  lut);
    error = smk_quant_mean_error(&palette, first, pixels, indices);
    SMK_ASSERT(error < 40.0);

    smk_quant_lut_init(lut);
    smk_quant_map(&palette, second, width, height, SMK_ENC_DITHER_NONE,
                  indices, lut);
    error = smk_quant_mean_error(&palette, second, pixels, indices);
    SMK_ASSERT(error < 40.0);

    free(first);
    free(second);
    free(indices);
    free(lut);
    free(hist);
}

void test_quantize_empty_histogram(void)
{
    smk_histogram *hist = (smk_histogram *)malloc(sizeof(*hist));
    smk_palette palette;

    SMK_ASSERT(hist != NULL);
    if (hist == NULL)
        return;
    smk_histogram_init(hist);
    SMK_ASSERT_EQ_INT(smk_quant_build_global(hist, 256, &palette), 1);
    SMK_ASSERT_EQ_INT(palette.entry[0].r, 0);
    SMK_ASSERT_EQ_INT(palette.entry[0].g, 0);
    SMK_ASSERT_EQ_INT(palette.entry[0].b, 0);
    free(hist);
}
