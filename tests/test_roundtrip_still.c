#include "test.h"
#include "test_util.h"

#include "quantize.h"
#include "smacker.h"
#include "smk_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *frames; /* frame_count * width * height * 3 */
    unsigned width;
    unsigned height;
    unsigned count;
    int fail_on_call; /* 1-based read_frame call to fail, or 0 */
    int calls;
} still_source;

static int still_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    still_source *source = (still_source *)user;
    size_t pixels;

    source->calls++;
    if (source->fail_on_call != 0 && source->calls >= source->fail_on_call)
        return -1;
    if (index >= source->count)
        return -1;
    pixels = (size_t)source->width * source->height;
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

/*
 * Four quadrants of exact 6-bit colors, so the expected palette is exact and
 * independent of quantization policy.
 */
static void fill_quadrants(uint8_t *rgb, unsigned width, unsigned height)
{
    static const uint8_t colors[4][3] = {
        { 0, 0, 0 },
        { 255, 0, 0 },
        { 0, 255, 0 },
        { 0, 0, 255 }
    };
    unsigned x;
    unsigned y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned index = ((x >= width / 2) ? 1u : 0u) |
                             ((y >= height / 2) ? 2u : 0u);
            uint8_t *p = rgb + ((size_t)y * width + x) * 3;
            memcpy(p, colors[index], 3);
        }
    }
}

static void still_source_free(still_source *source)
{
    free(source->frames);
    memset(source, 0, sizeof(*source));
}

static void still_source_init(still_source *source, unsigned width,
                              unsigned height, unsigned count)
{
    memset(source, 0, sizeof(*source));
    source->width = width;
    source->height = height;
    source->count = count;
    source->frames = (uint8_t *)calloc((size_t)width * height * count, 3);
}

static smk_enc_source make_source(still_source *state)
{
    smk_enc_source source;

    memset(&source, 0, sizeof(source));
    source.user = state;
    source.width = state->width;
    source.height = state->height;
    source.frame_count = state->count;
    source.fps_num = 15;
    source.fps_den = 1;
    source.audio_track_count = 0;
    source.read_frame = still_read_frame;
    source.read_audio = NULL;
    return source;
}

static void check_decoded_frame(const char *path, const uint8_t *rgb,
                                unsigned width, unsigned height)
{
    unsigned long frames = 0;
    unsigned long decoded_width = 0;
    unsigned long decoded_height = 0;
    unsigned long i;
    smk handle;
    smk_palette expected;
    smk_quant_lut *lut;
    uint8_t *indices;
    const uint8_t *palette;
    const uint8_t *video;
    size_t pixels = (size_t)width * height;

    handle = smk_open_file(path, SMK_MODE_DISK);
    SMK_ASSERT(handle != NULL);
    if (handle == NULL)
        return;

    SMK_ASSERT_EQ_INT(smk_info_all(handle, NULL, &frames, NULL), 0);
    SMK_ASSERT_EQ_INT(frames, 1);
    SMK_ASSERT_EQ_INT(smk_info_video(handle, &decoded_width, &decoded_height,
                                     NULL), 0);
    SMK_ASSERT_EQ_INT(decoded_width, width);
    SMK_ASSERT_EQ_INT(decoded_height, height);

    SMK_ASSERT_EQ_INT(smk_enable_all(handle, SMK_VIDEO_TRACK), 0);
    SMK_ASSERT(smk_first(handle) != -1);

    /* The encoder builds the same global palette from this single frame. */
    SMK_ASSERT(smk_quant_build_frame(rgb, pixels, 256, &expected) > 0);
    palette = smk_get_palette(handle);
    SMK_ASSERT(palette != NULL);
    if (palette != NULL) {
        for (i = 0; i < 256; i++) {
            unsigned c;

            for (c = 0; c < 3; c++) {
                unsigned value = i < expected.count
                    ? expected.display[i * 3 + c] : 0u;

                SMK_ASSERT_EQ_INT(palette[i * 3 + c], value);
            }
        }
    }

    lut = (smk_quant_lut *)malloc(sizeof(*lut));
    indices = (uint8_t *)malloc(pixels);
    SMK_ASSERT(lut != NULL && indices != NULL);
    if (lut != NULL && indices != NULL) {
        smk_quant_lut_init(lut);
        smk_quant_map(&expected, rgb, width, height, SMK_ENC_DITHER_NONE,
                      indices, lut);
        video = smk_get_video(handle);
        SMK_ASSERT(video != NULL);
        if (video != NULL)
            SMK_ASSERT_MEM_EQ(video, indices, pixels);
    }
    free(lut);
    free(indices);
    smk_close(handle);
}

void test_roundtrip_still_global(void)
{
    still_source state;
    smk_enc_source source;
    smk_enc_options options;
    char *path;
    char err[256] = { 0 };

    path = make_output_path("still.smk");
    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    still_source_init(&state, 8, 8, 1);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    fill_quadrants(state.frames, state.width, state.height);
    source = make_source(&state);

    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_GLOBAL;
    options.dither = SMK_ENC_DITHER_NONE;
    options.colors = 256;

    SMK_ASSERT_EQ_INT(smk_enc_encode(&source, &options, path, err, sizeof(err)),
                      SMK_ENC_OK);
    if (err[0] != '\0')
        fprintf(stderr, "  unexpected error: %s\n", err);

    check_decoded_frame(path, state.frames, state.width, state.height);

    remove(path);
    still_source_free(&state);
    free(path);
}

void test_roundtrip_still_padded(void)
{
    still_source state;
    smk_enc_source source;
    smk_enc_options options;
    char *path;
    char err[256] = { 0 };
    unsigned x;
    unsigned y;

    path = make_output_path("padded.smk");
    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    /* A 5x3 image padded to 8x4 by edge replication. */
    still_source_init(&state, 8, 4, 1);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 8; x++) {
            uint8_t *p = state.frames + ((size_t)y * 8 + x) * 3;

            p[0] = (uint8_t)((x >= 5 ? 4 : x) * 48);
            p[1] = (uint8_t)((y >= 3 ? 2 : y) * 80);
            p[2] = (uint8_t)(((x >= 5 ? 4 : x) + (y >= 3 ? 2 : y)) * 20);
        }
    }
    source = make_source(&state);

    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_GLOBAL;
    options.dither = SMK_ENC_DITHER_NONE;

    SMK_ASSERT_EQ_INT(smk_enc_encode(&source, &options, path, err, sizeof(err)),
                      SMK_ENC_OK);
    check_decoded_frame(path, state.frames, state.width, state.height);

    remove(path);
    still_source_free(&state);
    free(path);
}

void test_roundtrip_still_failure_removes_file(void)
{
    still_source state;
    smk_enc_source source;
    smk_enc_options options;
    char *path;
    char err[256] = { 0 };
    FILE *fp;
    unsigned k;

    path = make_output_path("failed.smk");
    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    still_source_init(&state, 8, 8, 1);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    fill_quadrants(state.frames, state.width, state.height);
    source = make_source(&state);
    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_GLOBAL;

    /* Fail during the write pass, after the output file was created. */
    for (k = 1; k <= 3; k++) {
        state.calls = 0;
        state.fail_on_call = (int)k;
        err[0] = '\0';
        remove(path);
        if (smk_enc_encode(&source, &options, path, err, sizeof(err)) ==
            SMK_ENC_OK) {
            SMK_ASSERT(0);
            remove(path);
            continue;
        }
        fp = fopen(path, "rb");
        SMK_ASSERT(fp == NULL);
        if (fp != NULL)
            fclose(fp);
    }

    still_source_free(&state);
    free(path);
}
