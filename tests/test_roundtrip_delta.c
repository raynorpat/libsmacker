#include "test.h"
#include "test_util.h"

#include "smacker.h"
#include "smk_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Colors chosen from the Smacker 6-bit palette expansion, so quantization maps
 * them exactly and decoded RGB can be compared byte for byte.
 */
static const uint8_t exact_colors[8][3] = {
    { 0x00, 0x00, 0x00 },
    { 0x41, 0x00, 0x00 },
    { 0x82, 0x00, 0x00 },
    { 0xc3, 0x00, 0x00 },
    { 0x00, 0x41, 0x00 },
    { 0x00, 0x82, 0x00 },
    { 0x00, 0xc3, 0x00 },
    { 0x00, 0x00, 0xff }
};

/* Frame 4 repeats frame 3 to force a frame whose blocks are all unchanged. */
static const unsigned pattern_keys[] = { 0, 1, 2, 3, 3, 4, 5, 6, 7, 0, 1, 2 };

typedef struct {
    uint8_t *frames;
    unsigned width;
    unsigned height;
    unsigned frame_count;
    unsigned calls;
    int fail_on_call;
} delta_source;

static int delta_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    delta_source *source = (delta_source *)user;
    size_t pixels = (size_t)source->width * source->height;

    source->calls++;
    if (source->fail_on_call != 0 && source->calls >= (unsigned)source->fail_on_call)
        return -1;
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

static void delta_source_init(delta_source *source, unsigned width,
                              unsigned height, unsigned frame_count)
{
    size_t pixels = (size_t)width * height;
    unsigned frame;
    unsigned y;
    unsigned x;

    memset(source, 0, sizeof(*source));
    source->width = width;
    source->height = height;
    source->frame_count = frame_count;
    source->frames = (uint8_t *)malloc(pixels * 3 * frame_count);
    if (source->frames == NULL)
        return;

    for (frame = 0; frame < frame_count; frame++) {
        unsigned key = pattern_keys[frame %
            (sizeof(pattern_keys) / sizeof(pattern_keys[0]))];

        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                unsigned color = ((x / 4) + 2 * (y / 4) + key +
                                  (x & 1) + 2 * (y & 1)) % 8;
                uint8_t *p = source->frames +
                    ((size_t)frame * pixels + (size_t)y * width + x) * 3;

                memcpy(p, exact_colors[color], 3);
            }
        }
    }
}

static void delta_source_free(delta_source *source)
{
    free(source->frames);
    memset(source, 0, sizeof(*source));
}

static smk_enc_status encode_source(delta_source *state, const char *path,
                                    int ring, unsigned interval,
                                    char *err, size_t errlen)
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
    source.read_frame = delta_read_frame;

    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_GLOBAL;
    options.dither = SMK_ENC_DITHER_NONE;
    options.keyframe_interval = interval;
    options.ring = ring;
    return smk_enc_encode(&source, &options, path, err, errlen);
}

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Compare the decoded frame (indices through the decoded palette) to RGB. */
static void check_frame(smk handle, const uint8_t *expected, size_t pixels)
{
    const unsigned char *palette = smk_get_palette(handle);
    const unsigned char *video = smk_get_video(handle);
    uint8_t *actual;
    size_t i;
    unsigned c;

    SMK_ASSERT(palette != NULL && video != NULL);
    if (palette == NULL || video == NULL)
        return;

    actual = (uint8_t *)malloc(pixels * 3);
    SMK_ASSERT(actual != NULL);
    if (actual == NULL)
        return;

    for (i = 0; i < pixels; i++) {
        unsigned index = video[i];

        for (c = 0; c < 3; c++)
            actual[i * 3 + c] = palette[index * 3 + c];
    }
    SMK_ASSERT_MEM_EQ(actual, expected, pixels * 3);
    free(actual);
}

static const uint8_t *frame_rgb(const delta_source *source, unsigned frame)
{
    return source->frames +
        (size_t)frame * source->width * source->height * 3;
}

static void check_logical_frames(const char *path, const delta_source *source)
{
    smk handle;
    unsigned long frames = 0;
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long frame;
    size_t pixels = (size_t)source->width * source->height;

    handle = smk_open_file(path, SMK_MODE_DISK);
    SMK_ASSERT(handle != NULL);
    if (handle == NULL)
        return;

    SMK_ASSERT_EQ_INT(smk_info_all(handle, NULL, &frames, NULL), 0);
    SMK_ASSERT_EQ_INT(smk_info_video(handle, &width, &height, NULL), 0);
    SMK_ASSERT_EQ_INT(frames, source->frame_count);
    SMK_ASSERT_EQ_INT(width, source->width);
    SMK_ASSERT_EQ_INT(height, source->height);

    SMK_ASSERT_EQ_INT(smk_enable_all(handle, SMK_VIDEO_TRACK), 0);
    SMK_ASSERT(smk_first(handle) != -1);
    for (frame = 0; frame < frames; frame++) {
        if (frame > 0)
            SMK_ASSERT(smk_next(handle) != -1);
        check_frame(handle, frame_rgb(source, (unsigned)frame), pixels);
    }
    smk_close(handle);
}

void test_roundtrip_delta_identical(void)
{
    delta_source state;
    char *path = make_output_path("delta.smk");
    char err[256] = { 0 };

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    delta_source_init(&state, 16, 16, 10);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }

    SMK_ASSERT_EQ_INT(encode_source(&state, path, 0, 150, err, sizeof(err)),
                      SMK_ENC_OK);
    check_logical_frames(path, &state);

    remove(path);
    delta_source_free(&state);
    free(path);
}

void test_roundtrip_keyframe_interval(void)
{
    delta_source state;
    char *path = make_output_path("keyframes.smk");
    char err[256] = { 0 };
    void *raw = NULL;
    size_t raw_size = 0;
    unsigned frame;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    delta_source_init(&state, 16, 16, 12);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }

    SMK_ASSERT_EQ_INT(encode_source(&state, path, 0, 4, err, sizeof(err)),
                      SMK_ENC_OK);
    check_logical_frames(path, &state);

    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *sizes = (const uint8_t *)raw + 104;

        for (frame = 0; frame < state.frame_count; frame++)
            SMK_ASSERT_EQ_INT(load_u32(sizes + frame * 4) & 1u,
                              frame % 4 == 0 ? 1u : 0u);
    }
    free(raw);
    raw = NULL;
    raw_size = 0;

    /* An interval of zero means only frame 0 is a keyframe. */
    SMK_ASSERT_EQ_INT(encode_source(&state, path, 0, 0, err, sizeof(err)),
                      SMK_ENC_OK);
    check_logical_frames(path, &state);
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *sizes = (const uint8_t *)raw + 104;

        for (frame = 0; frame < state.frame_count; frame++)
            SMK_ASSERT_EQ_INT(load_u32(sizes + frame * 4) & 1u,
                              frame == 0 ? 1u : 0u);
    }
    free(raw);

    remove(path);
    delta_source_free(&state);
    free(path);
}

void test_roundtrip_solid_still(void)
{
    delta_source state;
    char *path = make_output_path("solid.smk");
    char err[256] = { 0 };
    size_t pixels;
    size_t i;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    /*
     * A single all-one-color frame uses exactly one Type symbol. Without a
     * dummy tree symbol its video chunk would be empty.
     */
    delta_source_init(&state, 8, 8, 1);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    pixels = (size_t)state.width * state.height;
    for (i = 0; i < pixels; i++)
        memcpy(state.frames + i * 3, exact_colors[2], 3);

    SMK_ASSERT_EQ_INT(encode_source(&state, path, 0, 150, err, sizeof(err)),
                      SMK_ENC_OK);
    check_logical_frames(path, &state);

    remove(path);
    delta_source_free(&state);
    free(path);
}

void test_roundtrip_ring_frame(void)
{
    delta_source state;
    char *path = make_output_path("ring.smk");
    char err[256] = { 0 };
    void *raw = NULL;
    size_t raw_size = 0;
    unsigned physical;
    size_t pixels;
    smk handle;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    delta_source_init(&state, 16, 16, 8);
    SMK_ASSERT(state.frames != NULL);
    if (state.frames == NULL) {
        free(path);
        return;
    }
    pixels = (size_t)state.width * state.height;
    physical = state.frame_count + 1;

    SMK_ASSERT_EQ_INT(encode_source(&state, path, 1, 150, err, sizeof(err)),
                      SMK_ENC_OK);
    check_logical_frames(path, &state);

    /* Header flag, logical frame count, and the extra physical frame. */
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *bytes = (const uint8_t *)raw;
        const uint8_t *sizes = bytes + 104;
        const uint8_t *types = sizes + physical * 4;

        SMK_ASSERT_EQ_INT(load_u32(bytes + 0x0c), state.frame_count);
        SMK_ASSERT_EQ_INT(load_u32(bytes + 0x14), 1);
        SMK_ASSERT_EQ_INT(load_u32(sizes + (physical - 1) * 4) & 1u, 0);
        SMK_ASSERT(load_u32(sizes + (physical - 1) * 4) != 0);
        SMK_ASSERT_EQ_INT(types[physical - 1], 0);
    }
    free(raw);

    /* The ring frame rebuilds frame 0, then playback continues at frame 1. */
    handle = smk_open_file(path, SMK_MODE_DISK);
    SMK_ASSERT(handle != NULL);
    if (handle != NULL) {
        SMK_ASSERT_EQ_INT(smk_enable_all(handle, SMK_VIDEO_TRACK), 0);
        SMK_ASSERT(smk_first(handle) != -1);
        for (physical = 1; physical < state.frame_count; physical++)
            SMK_ASSERT(smk_next(handle) != -1);

        SMK_ASSERT_EQ_INT(smk_next(handle), SMK_LAST);
        check_frame(handle, frame_rgb(&state, 0), pixels);

        SMK_ASSERT_EQ_INT(smk_next(handle), SMK_MORE);
        check_frame(handle, frame_rgb(&state, 1), pixels);
        smk_close(handle);
    }

    remove(path);
    delta_source_free(&state);
    free(path);
}
