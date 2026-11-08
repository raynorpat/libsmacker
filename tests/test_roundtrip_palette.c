#include "test.h"
#include "test_util.h"

#include "smacker.h"
#include "smk_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t palmap[64] = {
    0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
    0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
    0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D,
    0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
    0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E,
    0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
    0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF,
    0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF
};

static const uint8_t colors_a[8][3] = {
    { 0x00, 0x00, 0x00 }, { 0x41, 0x00, 0x00 }, { 0x82, 0x00, 0x00 },
    { 0xc3, 0x00, 0x00 }, { 0x00, 0x41, 0x00 }, { 0x00, 0x82, 0x00 },
    { 0x00, 0xc3, 0x00 }, { 0x00, 0x00, 0xff }
};

static const uint8_t colors_b[8][3] = {
    { 0x41, 0x41, 0x00 }, { 0x82, 0x82, 0x00 }, { 0xc3, 0xc3, 0x00 },
    { 0x41, 0x00, 0x41 }, { 0x82, 0x00, 0x82 }, { 0xc3, 0x00, 0xc3 },
    { 0x00, 0x41, 0x41 }, { 0x00, 0x82, 0x82 }
};

typedef struct {
    uint8_t *frames;
    unsigned width;
    unsigned height;
    unsigned frame_count;
} pal_source;

static void pal_source_init(pal_source *source, unsigned width,
                            unsigned height, unsigned frame_count)
{
    memset(source, 0, sizeof(*source));
    source->width = width;
    source->height = height;
    source->frame_count = frame_count;
    source->frames = (uint8_t *)calloc((size_t)width * height * frame_count, 3);
}

static void pal_source_free(pal_source *source)
{
    free(source->frames);
    memset(source, 0, sizeof(*source));
}

static void fill_frame(pal_source *source, unsigned frame,
                       const uint8_t colors[][3], unsigned color_count,
                       unsigned seed)
{
    size_t pixels = (size_t)source->width * source->height;
    unsigned y;
    unsigned x;

    for (y = 0; y < source->height; y++) {
        for (x = 0; x < source->width; x++) {
            unsigned index = ((x / 4) + 2 * (y / 4) + seed +
                              (x & 1) + 2 * (y & 1)) % color_count;
            uint8_t *p = source->frames +
                ((size_t)frame * pixels + (size_t)y * source->width + x) * 3;

            memcpy(p, colors[index], 3);
        }
    }
}

static void fill_gradient(pal_source *source, unsigned frame)
{
    size_t pixels = (size_t)source->width * source->height;
    unsigned y;
    unsigned x;

    for (y = 0; y < source->height; y++) {
        for (x = 0; x < source->width; x++) {
            uint8_t *p = source->frames +
                ((size_t)frame * pixels + (size_t)y * source->width + x) * 3;

            p[0] = (uint8_t)((x * 16 + frame * 8) & 0xff);
            p[1] = (uint8_t)((y * 16 + frame * 4) & 0xff);
            p[2] = (uint8_t)(((x + y) * 8 + frame * 16) & 0xff);
        }
    }
}

static int pal_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    pal_source *source = (pal_source *)user;
    size_t pixels = (size_t)source->width * source->height;

    if (index >= source->frame_count)
        return -1;
    memcpy(rgb, source->frames + (size_t)index * pixels * 3, pixels * 3);
    return 0;
}

static char *make_output_path(const char *name)
{
    char *dir = test_tempdir();
    char *path;
    size_t length;

    if (dir == NULL)
        return NULL;
    length = strlen(dir) + strlen(name) + 2;
    path = (char *)malloc(length);
    if (path != NULL)
        snprintf(path, length, "%s/%s", dir, name);
    test_tempdir_free(dir);
    return path;
}

static smk_enc_status encode_palette(pal_source *state, smk_enc_palette_mode mode,
                                     smk_enc_dither_mode dither,
                                     double threshold, unsigned colors,
                                     const char *path, char *err, size_t errlen)
{
    smk_enc_source source;
    smk_enc_options options;

    memset(&source, 0, sizeof(source));
    source.user = state;
    source.width = state->width;
    source.height = state->height;
    source.frame_count = state->frame_count;
    source.fps_num = 15;
    source.fps_den = 1;
    source.read_frame = pal_read_frame;

    smk_enc_options_default(&options);
    options.palette = mode;
    options.dither = dither;
    options.palette_threshold = threshold;
    options.colors = colors;
    return smk_enc_encode(&source, &options, path, err, errlen);
}

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int palmap_contains(unsigned value)
{
    unsigned i;

    for (i = 0; i < 64; i++) {
        if (palmap[i] == value)
            return 1;
    }
    return 0;
}

/*
 * Decode every frame, compare to the source, and return the mean absolute
 * error per component on the 0..255 scale. When `exact` is set the frames must
 * match byte for byte.
 */
static double decode_and_compare(const char *path, const pal_source *source,
                                 int exact)
{
    smk handle;
    unsigned long frames = 0;
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long frame;
    size_t pixels = (size_t)source->width * source->height;
    uint64_t error = 0;
    uint8_t *actual;

    handle = smk_open_file(path, SMK_MODE_DISK);
    SMK_ASSERT(handle != NULL);
    if (handle == NULL)
        return 0.0;

    SMK_ASSERT_EQ_INT(smk_info_all(handle, NULL, &frames, NULL), 0);
    SMK_ASSERT_EQ_INT(smk_info_video(handle, &width, &height, NULL), 0);
    SMK_ASSERT_EQ_INT(frames, source->frame_count);
    SMK_ASSERT_EQ_INT(width, source->width);
    SMK_ASSERT_EQ_INT(height, source->height);

    actual = (uint8_t *)malloc(pixels * 3);
    SMK_ASSERT(actual != NULL);
    if (actual == NULL) {
        smk_close(handle);
        return 0.0;
    }

    SMK_ASSERT_EQ_INT(smk_enable_all(handle, SMK_VIDEO_TRACK), 0);
    SMK_ASSERT(smk_first(handle) != -1);
    for (frame = 0; frame < frames; frame++) {
        const unsigned char *palette;
        const unsigned char *video;
        const uint8_t *expected;
        size_t i;
        unsigned c;

        if (frame > 0)
            SMK_ASSERT(smk_next(handle) != -1);

        palette = smk_get_palette(handle);
        video = smk_get_video(handle);
        expected = source->frames + (size_t)frame * pixels * 3;
        SMK_ASSERT(palette != NULL && video != NULL);
        if (palette == NULL || video == NULL)
            continue;
        for (i = 0; i < pixels; i++) {
            unsigned index = video[i];

            for (c = 0; c < 3; c++) {
                actual[i * 3 + c] = palette[index * 3 + c];

                if (!exact) {
                    int diff = (int)actual[i * 3 + c] -
                               (int)expected[i * 3 + c];

                    error += (uint64_t)(diff < 0 ? -diff : diff);
                }
            }
        }
        if (exact)
            SMK_ASSERT_MEM_EQ(actual, expected, pixels * 3);
    }

    free(actual);
    smk_close(handle);
    return frames != 0 && pixels != 0
        ? (double)error / ((double)pixels * 3.0 * (double)frames) : 0.0;
}

static void check_palette_is_palmap(const char *path)
{
    smk handle;
    const unsigned char *palette;
    unsigned i;

    handle = smk_open_file(path, SMK_MODE_DISK);
    SMK_ASSERT(handle != NULL);
    if (handle == NULL)
        return;
    SMK_ASSERT_EQ_INT(smk_enable_all(handle, SMK_VIDEO_TRACK), 0);
    SMK_ASSERT(smk_first(handle) != -1);
    palette = smk_get_palette(handle);
    SMK_ASSERT(palette != NULL);
    if (palette != NULL) {
        for (i = 0; i < 256 * 3; i++)
            SMK_ASSERT(palmap_contains(palette[i]));
    }
    smk_close(handle);
}

void test_palette_global_one_chunk(void)
{
    pal_source state;
    char *path = make_output_path("global.smk");
    char err[256] = { 0 };
    void *raw = NULL;
    size_t raw_size = 0;
    unsigned frame;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    pal_source_init(&state, 16, 16, 6);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    for (frame = 0; frame < state.frame_count; frame++)
        fill_frame(&state, frame, colors_a, 8, frame);

    SMK_ASSERT_EQ_INT(encode_palette(&state, SMK_ENC_PALETTE_GLOBAL,
                                     SMK_ENC_DITHER_NONE, 6.0, 256, path, err,
                                     sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *types = (const uint8_t *)raw + 104 + state.frame_count * 4;

        for (frame = 0; frame < state.frame_count; frame++)
            SMK_ASSERT_EQ_INT(types[frame] & 0x01, frame == 0 ? 1 : 0);
    }
    free(raw);

    SMK_ASSERT_EQ_INT(decode_and_compare(path, &state, 1), 0);
    check_palette_is_palmap(path);

    remove(path);
    pal_source_free(&state);
    free(path);
}

void test_palette_frame_each_frame(void)
{
    pal_source state;
    char *path = make_output_path("frame.smk");
    char err[256] = { 0 };
    void *raw = NULL;
    size_t raw_size = 0;
    unsigned frame;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    pal_source_init(&state, 16, 16, 4);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    fill_frame(&state, 0, colors_a, 8, 0);
    fill_frame(&state, 1, colors_b, 8, 1);
    fill_frame(&state, 2, colors_a, 8, 2);
    fill_frame(&state, 3, colors_b, 8, 3);

    SMK_ASSERT_EQ_INT(encode_palette(&state, SMK_ENC_PALETTE_FRAME,
                                     SMK_ENC_DITHER_NONE, 6.0, 256, path, err,
                                     sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *sizes = (const uint8_t *)raw + 104;
        const uint8_t *types = sizes + state.frame_count * 4;

        for (frame = 0; frame < state.frame_count; frame++) {
            SMK_ASSERT_EQ_INT(types[frame] & 0x01, 1);
            SMK_ASSERT_EQ_INT(load_u32(sizes + frame * 4) & 0x01, 1);
        }
    }
    free(raw);

    SMK_ASSERT_EQ_INT(decode_and_compare(path, &state, 1), 0);

    remove(path);
    pal_source_free(&state);
    free(path);
}

void test_palette_adaptive_threshold(void)
{
    /*
     * Both B colors sit at red level 63, so median cut separates A and B on
     * the red axis first. With three entries the shared palette then holds A's
     * two colors exactly and lumps B into one averaged entry, which is far
     * enough off to cross the threshold.
     */
    static const uint8_t adaptive_a[2][3] = {
        { 0x00, 0x00, 0x00 }, { 0x82, 0x00, 0x00 }
    };
    static const uint8_t adaptive_b[2][3] = {
        { 0xff, 0xff, 0xff }, { 0xff, 0x00, 0xff }
    };
    static const unsigned expected_palette[10] = {
        1, 0, 0, 0, 1, 1, 0, 0, 0, 0
    };
    pal_source state;
    char *path = make_output_path("adaptive.smk");
    char err[256] = { 0 };
    void *raw = NULL;
    size_t raw_size = 0;
    unsigned frame;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    /* Nine frames of A, one of B, so the global palette stays on A. */
    pal_source_init(&state, 16, 16, 10);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    for (frame = 0; frame < state.frame_count; frame++) {
        if (frame == 4)
            fill_frame(&state, frame, adaptive_b, 2, 0);
        else
            fill_frame(&state, frame, adaptive_a, 2, frame);
    }

    SMK_ASSERT_EQ_INT(encode_palette(&state, SMK_ENC_PALETTE_ADAPTIVE,
                                     SMK_ENC_DITHER_NONE, 6.0, 3, path, err,
                                     sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *sizes = (const uint8_t *)raw + 104;
        const uint8_t *types = sizes + state.frame_count * 4;

        for (frame = 0; frame < state.frame_count; frame++) {
            SMK_ASSERT_EQ_INT(types[frame] & 0x01, expected_palette[frame]);
            SMK_ASSERT_EQ_INT(load_u32(sizes + frame * 4) & 0x01,
                              expected_palette[frame]);
        }
    }
    free(raw);

    /* A frames use the shared palette; the B frame uses its own. */
    SMK_ASSERT_EQ_INT(decode_and_compare(path, &state, 1), 0);

    /* A threshold above any possible error never switches. */
    raw = NULL;
    raw_size = 0;
    SMK_ASSERT_EQ_INT(encode_palette(&state, SMK_ENC_PALETTE_ADAPTIVE,
                                     SMK_ENC_DITHER_NONE, 255.0, 3, path, err,
                                     sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *sizes = (const uint8_t *)raw + 104;
        const uint8_t *types = sizes + state.frame_count * 4;

        for (frame = 0; frame < state.frame_count; frame++) {
            SMK_ASSERT_EQ_INT(types[frame] & 0x01, frame == 0 ? 1 : 0);
            SMK_ASSERT_EQ_INT(load_u32(sizes + frame * 4) & 0x01,
                              frame == 0 ? 1 : 0);
        }
    }
    free(raw);

    /* Zero threshold still leaves error-free frames on the shared palette. */
    raw = NULL;
    raw_size = 0;
    SMK_ASSERT_EQ_INT(encode_palette(&state, SMK_ENC_PALETTE_ADAPTIVE,
                                     SMK_ENC_DITHER_NONE, 0.0, 3, path, err,
                                     sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *sizes = (const uint8_t *)raw + 104;
        const uint8_t *types = sizes + state.frame_count * 4;

        for (frame = 0; frame < state.frame_count; frame++)
            SMK_ASSERT_EQ_INT(types[frame] & 0x01, expected_palette[frame]);
    }
    free(raw);

    remove(path);
    pal_source_free(&state);
    free(path);
}

void test_palette_dither_modes(void)
{
    static const smk_enc_dither_mode modes[] = {
        SMK_ENC_DITHER_NONE, SMK_ENC_DITHER_ORDERED, SMK_ENC_DITHER_FS
    };
    pal_source state;
    char *path = make_output_path("dither.smk");
    char err[256] = { 0 };
    unsigned frame;
    unsigned m;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    pal_source_init(&state, 16, 16, 3);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    for (frame = 0; frame < state.frame_count; frame++)
        fill_gradient(&state, frame);

    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        double error;

        SMK_ASSERT_EQ_INT(encode_palette(&state, SMK_ENC_PALETTE_GLOBAL,
                                         modes[m], 6.0, 256, path, err,
                                         sizeof(err)), SMK_ENC_OK);
        error = decode_and_compare(path, &state, 0);
        SMK_ASSERT(error <= 10.0);
    }

    remove(path);
    pal_source_free(&state);
    free(path);
}
