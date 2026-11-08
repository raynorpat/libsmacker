#include "test.h"
#include "test_util.h"

#include "smacker.h"
#include "smk_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_TRACKS 2

typedef struct {
    uint8_t *rgb;
    unsigned width;
    unsigned height;
    unsigned frame_count;
    unsigned fps_num;
    unsigned fps_den;
    unsigned track_count;
    smk_enc_audio_info info[TEST_MAX_TRACKS];
    uint8_t *pcm[TEST_MAX_TRACKS];
    size_t pcm_size[TEST_MAX_TRACKS];
} audio_source;

static size_t audio_align(const smk_enc_audio_info *info)
{
    return (size_t)info->channels * (size_t)(info->bits / 8);
}

static int audio_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    audio_source *source = (audio_source *)user;
    size_t pixels = (size_t)source->width * source->height;

    if (index >= source->frame_count)
        return -1;
    memcpy(rgb, source->rgb + (size_t)index * pixels * 3, pixels * 3);
    return 0;
}

/*
 * Return only the bytes that actually exist. The encoder is responsible for
 * padding the remainder of the frame with silence.
 */
static size_t audio_read_pcm(void *user, unsigned track, size_t start_sample,
                             size_t sample_count, void *dst)
{
    audio_source *source = (audio_source *)user;
    size_t align;
    size_t offset;
    size_t available;
    size_t needed;

    if (track >= source->track_count)
        return 0;
    align = audio_align(&source->info[track]);
    offset = start_sample * align;
    needed = sample_count * align;
    if (offset >= source->pcm_size[track])
        return 0;
    available = source->pcm_size[track] - offset;
    if (available > needed)
        available = needed;
    memcpy(dst, source->pcm[track] + offset, available);
    return available;
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

static void audio_source_init(audio_source *source, unsigned width,
                              unsigned height, unsigned frame_count,
                              unsigned fps_num, unsigned fps_den)
{
    size_t pixels = (size_t)width * height;
    unsigned frame;
    unsigned y;
    unsigned x;

    memset(source, 0, sizeof(*source));
    source->width = width;
    source->height = height;
    source->frame_count = frame_count;
    source->fps_num = fps_num;
    source->fps_den = fps_den;
    source->rgb = (uint8_t *)malloc(pixels * 3 * frame_count);
    if (source->rgb == NULL)
        return;
    for (frame = 0; frame < frame_count; frame++) {
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                uint8_t *p = source->rgb +
                    ((size_t)frame * pixels + (size_t)y * width + x) * 3;

                p[0] = (uint8_t)((x * 32 + frame * 17) & 0xff);
                p[1] = (uint8_t)((y * 32 + frame * 11) & 0xff);
                p[2] = (uint8_t)(((x ^ y) * 16 + frame * 7) & 0xff);
            }
        }
    }
}

static void audio_source_add_track(audio_source *source, unsigned track,
                                   const smk_enc_audio_info *info,
                                   size_t sample_frames)
{
    size_t bytes = sample_frames * audio_align(info);
    size_t i;

    source->info[track] = *info;
    source->pcm[track] = (uint8_t *)malloc(bytes != 0 ? bytes : 1);
    source->pcm_size[track] = bytes;
    if (source->pcm[track] == NULL)
        return;
    for (i = 0; i < bytes; i++)
        source->pcm[track][i] = (uint8_t)((i * 37 + track * 101 + 13) & 0xff);
    if (track + 1 > source->track_count)
        source->track_count = track + 1;
}

static void audio_source_free(audio_source *source)
{
    unsigned i;

    free(source->rgb);
    for (i = 0; i < TEST_MAX_TRACKS; i++)
        free(source->pcm[i]);
    memset(source, 0, sizeof(*source));
}

static smk_enc_status encode_source(audio_source *state, const char *path,
                                    char *err, size_t errlen)
{
    smk_enc_source source;
    smk_enc_options options;
    unsigned i;

    memset(&source, 0, sizeof(source));
    source.user = state;
    source.width = state->width;
    source.height = state->height;
    source.frame_count = state->frame_count;
    source.fps_num = state->fps_num;
    source.fps_den = state->fps_den;
    source.audio_track_count = state->track_count;
    for (i = 0; i < state->track_count; i++)
        source.audio[i] = state->info[i];
    source.read_frame = audio_read_frame;
    source.read_audio = audio_read_pcm;

    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_GLOBAL;
    options.dither = SMK_ENC_DITHER_NONE;
    return smk_enc_encode(&source, &options, path, err, errlen);
}

/* Sample boundary for a frame, computed independently of the encoder. */
static size_t audio_boundary(unsigned frame, const smk_enc_audio_info *info,
                             unsigned fps_num, unsigned fps_den)
{
    uint64_t value = (uint64_t)frame * info->rate * fps_den;

    return (size_t)(value / fps_num);
}

static void fill_expected(const audio_source *source, unsigned track,
                          size_t start_sample, size_t sample_count,
                          uint8_t *out)
{
    size_t align = audio_align(&source->info[track]);
    size_t offset = start_sample * align;
    size_t bytes = sample_count * align;
    uint8_t silence = source->info[track].bits == 8 ? 0x80 : 0x00;

    memset(out, silence, bytes);
    if (offset < source->pcm_size[track]) {
        size_t available = source->pcm_size[track] - offset;

        if (available > bytes)
            available = bytes;
        memcpy(out, source->pcm[track] + offset, available);
    }
}

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Decode every frame, compare the audio to the expected split, and validate
 * the container tables (AudioSize, frame types, four-byte padding).
 */
static void check_decoded_audio(const char *path, const audio_source *source)
{
    smk handle;
    unsigned long frames = 0;
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned char track_mask = 0;
    unsigned char channels[7];
    unsigned char bitdepth[7];
    unsigned long rates[7];
    unsigned char enable = SMK_VIDEO_TRACK;
    unsigned long frame;
    unsigned track;
    void *raw = NULL;
    size_t raw_size = 0;
    uint32_t sum_sizes = 0;
    size_t expected_max[TEST_MAX_TRACKS];
    size_t expected_total[TEST_MAX_TRACKS];
    size_t decoded_total[TEST_MAX_TRACKS];

    handle = smk_open_file(path, SMK_MODE_DISK);
    SMK_ASSERT(handle != NULL);
    if (handle == NULL)
        return;

    SMK_ASSERT_EQ_INT(smk_info_all(handle, NULL, &frames, NULL), 0);
    SMK_ASSERT_EQ_INT(smk_info_video(handle, &width, &height, NULL), 0);
    SMK_ASSERT_EQ_INT(frames, source->frame_count);
    SMK_ASSERT_EQ_INT(width, source->width);
    SMK_ASSERT_EQ_INT(height, source->height);
    SMK_ASSERT_EQ_INT(smk_info_audio(handle, &track_mask, channels, bitdepth,
                                     rates), 0);
    SMK_ASSERT_EQ_INT(track_mask,
                      (1u << source->track_count) - 1u);
    for (track = 0; track < source->track_count; track++) {
        SMK_ASSERT_EQ_INT(channels[track], source->info[track].channels);
        SMK_ASSERT_EQ_INT(bitdepth[track], source->info[track].bits);
        SMK_ASSERT_EQ_INT(rates[track], source->info[track].rate);
        enable |= (unsigned char)(1u << track);
    }

    /* Expected per-frame sizes and total sample counts. */
    for (track = 0; track < source->track_count; track++) {
        size_t align = audio_align(&source->info[track]);

        decoded_total[track] = 0;
        expected_max[track] = 0;
        for (frame = 0; frame < source->frame_count; frame++) {
            size_t start = audio_boundary((unsigned)frame, &source->info[track],
                                          source->fps_num, source->fps_den);
            size_t end = audio_boundary((unsigned)frame + 1,
                                        &source->info[track], source->fps_num,
                                        source->fps_den);
            size_t bytes = (end - start) * align;

            if (bytes > expected_max[track])
                expected_max[track] = bytes;
        }
        expected_total[track] = audio_boundary(source->frame_count,
                                               &source->info[track],
                                               source->fps_num,
                                               source->fps_den) * align;
    }

    /* Header AudioSize values, frame types, and container arithmetic. */
    SMK_ASSERT_EQ_INT(test_read_file(path, &raw, &raw_size), 0);
    if (raw != NULL) {
        const uint8_t *bytes = (const uint8_t *)raw;
        const uint8_t *sizes = bytes + 104;
        const uint8_t *types = sizes + (size_t)frames * 4;
        uint32_t trees_size = load_u32(bytes + 0x34);

        for (track = 0; track < source->track_count; track++)
            SMK_ASSERT_EQ_INT(load_u32(bytes + 0x18 + track * 4),
                              expected_max[track]);

        for (frame = 0; frame < frames; frame++) {
            unsigned expected_type = (frame == 0 ? 0x01u : 0u) |
                (((1u << source->track_count) - 1u) << 1);

            SMK_ASSERT_EQ_INT(types[frame], expected_type);
            SMK_ASSERT_EQ_INT(load_u32(sizes + frame * 4) & 1u,
                              frame == 0 ? 1u : 0u);
            sum_sizes += load_u32(sizes + frame * 4) & 0xfffffffcu;
        }
        SMK_ASSERT_EQ_INT(raw_size,
                          104 + frames * 4 + frames + trees_size + sum_sizes);
    }
    free(raw);

    SMK_ASSERT_EQ_INT(smk_enable_all(handle, enable), 0);
    SMK_ASSERT(smk_first(handle) != -1);

    for (frame = 0; frame < frames; frame++) {
        if (frame > 0)
            SMK_ASSERT(smk_next(handle) != -1);

        for (track = 0; track < source->track_count; track++) {
            size_t align = audio_align(&source->info[track]);
            size_t start = audio_boundary((unsigned)frame,
                                          &source->info[track],
                                          source->fps_num, source->fps_den);
            size_t end = audio_boundary((unsigned)frame + 1,
                                        &source->info[track],
                                        source->fps_num, source->fps_den);
            size_t samples = end - start;
            size_t expected_bytes = samples * align;
            uint8_t *expected = (uint8_t *)malloc(
                expected_bytes != 0 ? expected_bytes : 1);
            const unsigned char *decoded = smk_get_audio(handle,
                                                         (unsigned char)track);
            unsigned long decoded_size = smk_get_audio_size(
                handle, (unsigned char)track);

            SMK_ASSERT(expected != NULL);
            if (expected == NULL)
                continue;
            fill_expected(source, track, start, samples, expected);
            SMK_ASSERT_EQ_INT(decoded_size, expected_bytes);
            SMK_ASSERT_MEM_EQ(decoded, expected, expected_bytes);
            decoded_total[track] += decoded_size;
            free(expected);
        }
    }
    for (track = 0; track < source->track_count; track++)
        SMK_ASSERT_EQ_INT(decoded_total[track], expected_total[track]);

    smk_close(handle);
}

void test_roundtrip_audio_pcm_formats(void)
{
    static const struct {
        unsigned bits;
        unsigned channels;
        unsigned rate;
        unsigned fps_num;
    } cases[] = {
        { 16, 1, 8000, 30 },
        { 16, 2, 11025, 15 },
        { 8, 1, 22050, 15 },
        { 8, 2, 44100, 30 }
    };
    unsigned c;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        audio_source state;
        smk_enc_audio_info info;
        char *path = make_output_path("audio.smk");
        char err[256] = { 0 };
        size_t samples;

        SMK_ASSERT(path != NULL);
        if (path == NULL)
            return;

        audio_source_init(&state, 8, 8, 4, cases[c].fps_num, 1);
        SMK_ASSERT(state.rgb != NULL);
        if (state.rgb == NULL) {
            free(path);
            return;
        }

        info.rate = cases[c].rate;
        info.channels = cases[c].channels;
        info.bits = cases[c].bits;
        samples = audio_boundary(4, &info, cases[c].fps_num, 1);
        audio_source_add_track(&state, 0, &info, samples);
        SMK_ASSERT(state.pcm[0] != NULL);

        SMK_ASSERT_EQ_INT(encode_source(&state, path, err, sizeof(err)),
                          SMK_ENC_OK);
        check_decoded_audio(path, &state);

        remove(path);
        audio_source_free(&state);
        free(path);
    }
}

void test_roundtrip_audio_short_silence(void)
{
    audio_source state;
    smk_enc_audio_info info;
    char *path = make_output_path("short.smk");
    char err[256] = { 0 };

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    audio_source_init(&state, 8, 8, 6, 15, 1);
    SMK_ASSERT(state.rgb != NULL);
    if (state.rgb == NULL) {
        free(path);
        return;
    }

    /* Only one and a half frames of 8-bit stereo audio. */
    info.rate = 8000;
    info.channels = 2;
    info.bits = 8;
    audio_source_add_track(&state, 0, &info, 1200);
    SMK_ASSERT(state.pcm[0] != NULL);

    SMK_ASSERT_EQ_INT(encode_source(&state, path, err, sizeof(err)),
                      SMK_ENC_OK);
    check_decoded_audio(path, &state);

    remove(path);
    audio_source_free(&state);
    free(path);
}

void test_roundtrip_audio_long_truncated(void)
{
    audio_source state;
    smk_enc_audio_info info;
    char *path = make_output_path("long.smk");
    char err[256] = { 0 };
    smk handle;
    unsigned long frame;
    size_t total_decoded = 0;
    size_t expected_total;
    size_t align;

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    audio_source_init(&state, 8, 8, 3, 15, 1);
    SMK_ASSERT(state.rgb != NULL);
    if (state.rgb == NULL) {
        free(path);
        return;
    }

    /* One full second of 16-bit mono audio, far longer than the video. */
    info.rate = 8000;
    info.channels = 1;
    info.bits = 16;
    audio_source_add_track(&state, 0, &info, 8000);
    SMK_ASSERT(state.pcm[0] != NULL);

    SMK_ASSERT_EQ_INT(encode_source(&state, path, err, sizeof(err)),
                      SMK_ENC_OK);
    check_decoded_audio(path, &state);

    align = audio_align(&info);
    expected_total = audio_boundary(3, &info, 15, 1) * align;
    SMK_ASSERT(expected_total < state.pcm_size[0]);

    handle = smk_open_file(path, SMK_MODE_DISK);
    SMK_ASSERT(handle != NULL);
    if (handle != NULL) {
        SMK_ASSERT_EQ_INT(smk_enable_all(handle,
                                         SMK_VIDEO_TRACK | SMK_AUDIO_TRACK_0),
                          0);
        SMK_ASSERT(smk_first(handle) != -1);
        for (frame = 0; frame < 3; frame++) {
            if (frame > 0)
                SMK_ASSERT(smk_next(handle) != -1);
            total_decoded += smk_get_audio_size(handle, 0);
        }
        SMK_ASSERT_EQ_INT(total_decoded, expected_total);
        smk_close(handle);
    }

    remove(path);
    audio_source_free(&state);
    free(path);
}

void test_roundtrip_audio_two_tracks(void)
{
    audio_source state;
    smk_enc_audio_info first;
    smk_enc_audio_info second;
    char *path = make_output_path("tracks.smk");
    char err[256] = { 0 };

    SMK_ASSERT(path != NULL);
    if (path == NULL)
        return;

    audio_source_init(&state, 8, 8, 4, 15, 1);
    SMK_ASSERT(state.rgb != NULL);
    if (state.rgb == NULL) {
        free(path);
        return;
    }

    first.rate = 44100;
    first.channels = 2;
    first.bits = 16;
    audio_source_add_track(&state, 0, &first, audio_boundary(4, &first, 15, 1));

    second.rate = 8000;
    second.channels = 1;
    second.bits = 8;
    audio_source_add_track(&state, 1, &second,
                           audio_boundary(4, &second, 15, 1));

    SMK_ASSERT(state.pcm[0] != NULL && state.pcm[1] != NULL);

    SMK_ASSERT_EQ_INT(encode_source(&state, path, err, sizeof(err)),
                      SMK_ENC_OK);
    check_decoded_audio(path, &state);

    remove(path);
    audio_source_free(&state);
    free(path);
}
