#include "test.h"
#include "test_util.h"

#include "smk_encoder.h"
#include "verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_TRACKS 2
#define TEST_AUDIO_MAX (1u << 15)

static const uint8_t verify_colors[8][3] = {
    { 0x00, 0x00, 0x00 }, { 0x41, 0x00, 0x00 }, { 0x82, 0x00, 0x00 },
    { 0xc3, 0x00, 0x00 }, { 0x00, 0x41, 0x00 }, { 0x00, 0x82, 0x00 },
    { 0x00, 0xc3, 0x00 }, { 0x00, 0x00, 0xff }
};

typedef struct {
    uint8_t *video;
    unsigned width;
    unsigned height;
    unsigned frame_count;
    unsigned fps_num;
    unsigned fps_den;
    unsigned track_count;
    smk_enc_audio_info info[TEST_TRACKS];
    uint8_t *audio[TEST_TRACKS];
    size_t audio_bytes[TEST_TRACKS];
} verify_source;

static int verify_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    verify_source *source = (verify_source *)user;
    size_t pixels = (size_t)source->width * source->height;

    if (index >= source->frame_count)
        return -1;
    memcpy(rgb, source->video + (size_t)index * pixels * 3, pixels * 3);
    return 0;
}

static size_t verify_read_audio(void *user, unsigned track,
                                size_t start_sample, size_t sample_count,
                                void *dst)
{
    verify_source *source = (verify_source *)user;
    size_t align = (size_t)source->info[track].channels *
                   (size_t)(source->info[track].bits / 8);
    size_t offset = start_sample * align;
    size_t want = sample_count * align;
    size_t available;

    if (offset >= source->audio_bytes[track])
        return 0;
    available = source->audio_bytes[track] - offset;
    if (want > available)
        want = available;
    memcpy(dst, source->audio[track] + offset, want);
    return want;
}

static void verify_source_init(verify_source *source, unsigned width,
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
    source->fps_num = 15;
    source->fps_den = 1;
    source->video = (uint8_t *)malloc(pixels * 3 * frame_count);
    if (source->video == NULL)
        return;

    for (frame = 0; frame < frame_count; frame++) {
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                unsigned color =
                    ((x / 2) + 3 * (y / 2) + frame) % 8;
                uint8_t *p = source->video +
                    ((size_t)frame * pixels + (size_t)y * width + x) * 3;

                memcpy(p, verify_colors[color], 3);
            }
        }
    }
}

static void verify_source_free(verify_source *source)
{
    unsigned track;

    free(source->video);
    for (track = 0; track < TEST_TRACKS; track++)
        free(source->audio[track]);
    memset(source, 0, sizeof(*source));
}

static void add_audio(verify_source *source, unsigned track, unsigned rate,
                      unsigned channels, unsigned bits, size_t sample_frames)
{
    size_t align = (size_t)channels * (size_t)(bits / 8);
    size_t bytes = sample_frames * align;
    size_t i;

    source->info[track].rate = rate;
    source->info[track].channels = channels;
    source->info[track].bits = bits;
    source->audio[track] = (uint8_t *)malloc(bytes != 0 ? bytes : 1);
    source->audio_bytes[track] = bytes;
    if (source->audio[track] == NULL) {
        source->audio_bytes[track] = 0;
        return;
    }
    for (i = 0; i < bytes; i++)
        source->audio[track][i] = (uint8_t)((i * 31u + track * 7u) & 0xffu);
}

static void fill_source(verify_source *source)
{
    add_audio(source, 0, 8000, 2, 16, 4000);
    add_audio(source, 1, 11025, 1, 8, 2000);
    source->track_count = 2;
}

static smk_enc_status encode_verify_source(verify_source *state,
                                           const smk_enc_options *options,
                                           const char *path,
                                           char *err, size_t errlen)
{
    smk_enc_source source;

    memset(&source, 0, sizeof(source));
    source.user = state;
    source.width = state->width;
    source.height = state->height;
    source.frame_count = state->frame_count;
    source.fps_num = state->fps_num;
    source.fps_den = state->fps_den;
    source.read_frame = verify_read_frame;
    source.read_audio = verify_read_audio;
    source.audio_track_count = state->track_count;
    memcpy(source.audio, state->info, sizeof(state->info));
    return smk_enc_encode(&source, options, path, err, errlen);
}

static smk_enc_status verify_verify_source(const verify_source *state,
                                           const smk_enc_options *options,
                                           const char *path,
                                           char *err, size_t errlen)
{
    smk_enc_source source;

    memset(&source, 0, sizeof(source));
    source.user = (void *)state;
    source.width = state->width;
    source.height = state->height;
    source.frame_count = state->frame_count;
    source.fps_num = state->fps_num;
    source.fps_den = state->fps_den;
    source.read_frame = verify_read_frame;
    source.read_audio = verify_read_audio;
    source.audio_track_count = state->track_count;
    memcpy(source.audio, state->info, sizeof(state->info));
    return smk_verify_file(path, &source, options, err, errlen);
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

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void test_verify_clean_file(void)
{
    verify_source state;
    smk_enc_options options;
    char *path = make_output_path("verify-clean.smk");
    char err[256] = { 0 };

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    verify_source_init(&state, 16, 16, 6);
    SMK_ASSERT(state.video != NULL);
    if (state.video == NULL) {
        free(path);
        return;
    }
    fill_source(&state);
    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_ADAPTIVE;

    SMK_ASSERT_EQ_INT(encode_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);

    /* Ring frames and per-frame palettes must verify too. */
    options.ring = 1;
    options.palette = SMK_ENC_PALETTE_FRAME;
    SMK_ASSERT_EQ_INT(encode_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);

    /* A lossy palette still verifies: indices must match the encoder's. */
    options.palette = SMK_ENC_PALETTE_ADAPTIVE;
    options.colors = 4;
    options.dither = SMK_ENC_DITHER_ORDERED;
    SMK_ASSERT_EQ_INT(encode_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);

    /* Adaptive palette switching with a ring frame. */
    options.colors = 2;
    options.palette_threshold = 0.0;
    SMK_ASSERT_EQ_INT(encode_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);

    remove(path);
    verify_source_free(&state);
    free(path);
}

void test_verify_corrupted_video(void)
{
    verify_source state;
    smk_enc_options options;
    char *path = make_output_path("verify-corrupt.smk");
    char err[256] = { 0 };
    void *raw = NULL;
    size_t raw_size = 0;
    unsigned physical;
    size_t trees_size;
    size_t data_offset;
    size_t palette_size;
    size_t frame_size;
    size_t at;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    verify_source_init(&state, 16, 16, 4);
    SMK_ASSERT(state.video != NULL);
    if (state.video == NULL) {
        free(path);
        return;
    }
    fill_source(&state);
    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_GLOBAL;

    SMK_ASSERT_EQ_INT(encode_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    SMK_ASSERT(raw != NULL);
    if (raw == NULL) {
        remove(path);
        verify_source_free(&state);
        free(path);
        return;
    }

    physical = state.frame_count + (options.ring ? 1 : 0);
    trees_size = load_u32((const uint8_t *)raw + 0x34);
    data_offset = 104 + (size_t)physical * 4 + physical + trees_size;
    palette_size = (size_t)((const uint8_t *)raw)[data_offset] * 4;
    frame_size = load_u32((const uint8_t *)raw + 104) & 0xfffffffc;

    /* Flip one byte in the middle of frame 0's video bitstream. */
    SMK_ASSERT(data_offset + frame_size <= raw_size);
    SMK_ASSERT(frame_size > palette_size + 1);
    at = data_offset + palette_size + (frame_size - palette_size) / 2;
    ((uint8_t *)raw)[at] ^= 0xffu;
    SMK_ASSERT_EQ_INT(test_write_file(path, raw, raw_size), 0);

    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)),
                      SMK_ENC_ERR_VERIFY);

    free(raw);
    remove(path);
    verify_source_free(&state);
    free(path);
}

void test_verify_metadata_mismatch(void)
{
    verify_source state;
    smk_enc_options options;
    char *path = make_output_path("verify-meta.smk");
    char err[256] = { 0 };

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    verify_source_init(&state, 16, 16, 4);
    SMK_ASSERT(state.video != NULL);
    if (state.video == NULL) {
        free(path);
        return;
    }
    fill_source(&state);
    smk_enc_options_default(&options);

    SMK_ASSERT_EQ_INT(encode_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);

    state.width = 20;
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)),
                      SMK_ENC_ERR_VERIFY);
    state.width = 16;

    state.frame_count = 5;
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)),
                      SMK_ENC_ERR_VERIFY);
    state.frame_count = 4;

    state.fps_num = 30;
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)),
                      SMK_ENC_ERR_VERIFY);
    state.fps_num = 15;

    state.info[1].rate = 8000;
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)),
                      SMK_ENC_ERR_VERIFY);
    state.info[1].rate = 11025;

    remove(path);
    verify_source_free(&state);
    free(path);
}

void test_verify_every_audio_track(void)
{
    verify_source state;
    smk_enc_options options;
    char *path = make_output_path("verify-audio.smk");
    char err[256] = { 0 };
    unsigned track;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    verify_source_init(&state, 16, 16, 4);
    SMK_ASSERT(state.video != NULL);
    if (state.video == NULL) {
        free(path);
        return;
    }
    fill_source(&state);
    smk_enc_options_default(&options);

    SMK_ASSERT_EQ_INT(encode_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);

    /* Change one byte in each track in turn; both must be compared. */
    for (track = 0; track < state.track_count; track++) {
        uint8_t saved;

        SMK_ASSERT(state.audio_bytes[track] > 5);
        if (state.audio_bytes[track] <= 5)
            continue;
        saved = state.audio[track][5];
        state.audio[track][5] ^= 0x5au;
        SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                               sizeof(err)),
                          SMK_ENC_ERR_VERIFY);
        state.audio[track][5] = saved;
    }

    /* And the unmodified file verifies again after restoring both tracks. */
    SMK_ASSERT_EQ_INT(verify_verify_source(&state, &options, path, err,
                                           sizeof(err)), SMK_ENC_OK);

    remove(path);
    verify_source_free(&state);
    free(path);
}
