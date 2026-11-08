#include "test.h"
#include "test_util.h"
#include "avi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guard callbacks so a failed open reports a test failure instead of crashing. */
static int call_read_frame(const smk_enc_source *source, unsigned index,
                           uint8_t *rgb)
{
    return source->read_frame != NULL
        ? source->read_frame(source->user, index, rgb) : -1;
}

static size_t call_read_audio(const smk_enc_source *source, unsigned track,
                              size_t start, size_t count, void *dst)
{
    return source->read_audio != NULL
        ? source->read_audio(source->user, track, start, count, dst) : 0;
}

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} abuf;

static void ab_reserve(abuf *b, size_t extra)
{
    size_t needed = b->size + extra;
    size_t capacity;
    uint8_t *grown;

    if (needed <= b->capacity)
        return;
    capacity = b->capacity == 0 ? 1024 : b->capacity;
    while (capacity < needed)
        capacity *= 2;
    grown = (uint8_t *)realloc(b->data, capacity);
    if (grown == NULL)
        abort();
    b->data = grown;
    b->capacity = capacity;
}

static void ab_raw(abuf *b, const void *data, size_t size)
{
    ab_reserve(b, size);
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

static void ab_u8(abuf *b, unsigned value)
{
    uint8_t byte = (uint8_t)value;
    ab_raw(b, &byte, 1);
}

static void ab_u16(abuf *b, unsigned value)
{
    ab_u8(b, value & 0xff);
    ab_u8(b, (value >> 8) & 0xff);
}

static void ab_u32(abuf *b, uint32_t value)
{
    ab_u8(b, value & 0xff);
    ab_u8(b, (value >> 8) & 0xff);
    ab_u8(b, (value >> 16) & 0xff);
    ab_u8(b, (value >> 24) & 0xff);
}

static size_t ab_size_placeholder(abuf *b)
{
    size_t pos = b->size;
    ab_u32(b, 0);
    return pos;
}

static void ab_patch_size(abuf *b, size_t pos)
{
    uint32_t size = (uint32_t)(b->size - pos - 4);
    b->data[pos] = (uint8_t)(size & 0xff);
    b->data[pos + 1] = (uint8_t)((size >> 8) & 0xff);
    b->data[pos + 2] = (uint8_t)((size >> 16) & 0xff);
    b->data[pos + 3] = (uint8_t)((size >> 24) & 0xff);
}

static size_t ab_chunk_begin(abuf *b, const char *id)
{
    ab_raw(b, id, 4);
    return ab_size_placeholder(b);
}

static size_t ab_container_begin(abuf *b, const char *fourcc, const char *type)
{
    ab_raw(b, fourcc, 4);
    {
        size_t pos = ab_size_placeholder(b);
        ab_raw(b, type, 4);
        return pos;
    }
}

static void ab_chunk_end(abuf *b, size_t pos)
{
    ab_patch_size(b, pos);
}

static void avi_begin(abuf *b, size_t *riff)
{
    *riff = ab_container_begin(b, "RIFF", "AVI ");
}

static void avi_avih(abuf *b, unsigned usf, unsigned frames, unsigned streams,
                     unsigned width, unsigned height)
{
    size_t chunk = ab_chunk_begin(b, "avih");

    ab_u32(b, usf);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, frames);
    ab_u32(b, 0);
    ab_u32(b, streams);
    ab_u32(b, 0);
    ab_u32(b, width);
    ab_u32(b, height);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_chunk_end(b, chunk);
}

static void avi_video_strl(abuf *b, unsigned width, unsigned height,
                           unsigned bpp, unsigned compression,
                           const char *handler, unsigned scale, unsigned rate)
{
    size_t list = ab_container_begin(b, "LIST", "strl");
    size_t strh = ab_chunk_begin(b, "strh");
    unsigned frame_size = width * height * (bpp / 8);

    ab_raw(b, "vids", 4);
    ab_raw(b, handler, 4);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, scale);
    ab_u32(b, rate);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, frame_size);
    ab_u32(b, 0xffffffff);
    ab_u32(b, frame_size);
    ab_u32(b, 0);
    ab_u16(b, width);
    ab_u16(b, height);
    ab_chunk_end(b, strh);

    {
        size_t strf = ab_chunk_begin(b, "strf");
        ab_u32(b, 40);
        ab_u32(b, width);
        ab_u32(b, height);
        ab_u16(b, 1);
        ab_u16(b, bpp);
        ab_u32(b, compression);
        ab_u32(b, frame_size);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_chunk_end(b, strf);
    }
    ab_chunk_end(b, list);
}

static void avi_audio_strl(abuf *b, unsigned channels, unsigned rate,
                           unsigned bits)
{
    size_t list = ab_container_begin(b, "LIST", "strl");
    size_t strh = ab_chunk_begin(b, "strh");
    unsigned block = channels * (bits / 8);

    ab_raw(b, "auds", 4);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, channels);
    ab_u32(b, rate * channels);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_u32(b, block);
    ab_u32(b, 0xffffffff);
    ab_u32(b, block);
    ab_u32(b, 0);
    ab_u32(b, 0);
    ab_chunk_end(b, strh);

    {
        size_t strf = ab_chunk_begin(b, "strf");
        ab_u16(b, 1);
        ab_u16(b, channels);
        ab_u32(b, rate);
        ab_u32(b, rate * block);
        ab_u16(b, block);
        ab_u16(b, bits);
        ab_u16(b, 0);
        ab_chunk_end(b, strf);
    }
    ab_chunk_end(b, list);
}

static void avi_video_chunk(abuf *b, unsigned stream, int compressed,
                            const uint8_t *rgb, unsigned width, unsigned height,
                            unsigned bpp, int padded_rows)
{
    char id[9];
    size_t chunk;
    unsigned row_bytes = width * (bpp / 8);
    unsigned stride = padded_rows ? (row_bytes + 3) & ~3u : row_bytes;
    int y;

    snprintf(id, sizeof(id), "%02u%c%c", stream, 'd', compressed ? 'c' : 'b');
    chunk = ab_chunk_begin(b, id);
    for (y = (int)height - 1; y >= 0; y--) {
        unsigned x;
        unsigned written = 0;

        for (x = 0; x < width; x++) {
            const uint8_t *src = rgb + ((size_t)y * width + x) * 3;
            ab_u8(b, src[2]);
            ab_u8(b, src[1]);
            ab_u8(b, src[0]);
            if (bpp == 32)
                ab_u8(b, 0);
            written += bpp / 8;
        }
        while (written < stride) {
            ab_u8(b, 0);
            written++;
        }
    }
    ab_chunk_end(b, chunk);
}

static void avi_audio_chunk(abuf *b, unsigned stream, const void *data,
                            size_t size)
{
    char id[9];
    size_t chunk;

    snprintf(id, sizeof(id), "%02uwb", stream);
    chunk = ab_chunk_begin(b, id);
    ab_raw(b, data, size);
    ab_chunk_end(b, chunk);
}


static char *write_temp_avi(const char *name, abuf *b)
{
    char *dir = test_tempdir();
    char *path;
    size_t length;

    if (dir == NULL)
        return NULL;
    length = strlen(dir) + strlen(name) + 2;
    path = (char *)malloc(length);
    if (path == NULL) {
        free(dir);
        return NULL;
    }
    snprintf(path, length, "%s/%s", dir, name);
    if (test_write_file(path, b->data, b->size) != 0) {
        free(path);
        free(dir);
        return NULL;
    }
    free(dir);
    return path;
}

static void fill_pattern(uint8_t *rgb, unsigned width, unsigned height)
{
    unsigned x;
    unsigned y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint8_t *p = rgb + ((size_t)y * width + x) * 3;
            p[0] = (uint8_t)(10 + x * 20);
            p[1] = (uint8_t)(20 + y * 20);
            p[2] = (uint8_t)(30 + x + y);
        }
    }
}

static void default_options(smk_enc_options *options)
{
    smk_enc_options_default(options);
}

void test_avi_smk2avi_layout(void)
{
    abuf b;
    size_t riff;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t *rgb;
    uint8_t frame[48];
    static const int16_t samples[3] = { 100, -200, 300 };
    int16_t out[3];

    memset(&b, 0, sizeof(b));
    avi_begin(&b, &riff);
    {
        size_t hdrl = ab_container_begin(&b, "LIST", "hdrl");
        avi_avih(&b, 40000, 2, 2, 4, 4);
        avi_video_strl(&b, 4, 4, 24, 0, "DIB ", 40000, 1000000);
        avi_audio_strl(&b, 1, 44100, 16);
        ab_chunk_end(&b, hdrl);
    }
    {
        size_t movi = ab_container_begin(&b, "LIST", "movi");
        rgb = (uint8_t *)malloc(48);
        SMK_ASSERT(rgb != NULL);
        if (rgb == NULL)
            return;
        fill_pattern(rgb, 4, 4);
        avi_video_chunk(&b, 0, 0, rgb, 4, 4, 24, 0);
        memset(rgb, 0, 48);
        fill_pattern(rgb, 4, 4);
        rgb[0] = 99;
        avi_video_chunk(&b, 0, 0, rgb, 4, 4, 24, 0);
        free(rgb);
        avi_audio_chunk(&b, 1, samples, sizeof(samples));
        ab_chunk_end(&b, movi);
    }
    ab_chunk_end(&b, riff);

    path = write_temp_avi("layout.avi", &b);
    SMK_ASSERT(path != NULL);
    default_options(&options);
    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.width, 4);
    SMK_ASSERT_EQ_INT(source.height, 4);
    SMK_ASSERT_EQ_INT(source.frame_count, 2);
    SMK_ASSERT_EQ_INT(source.fps_num, 25);
    SMK_ASSERT_EQ_INT(source.fps_den, 1);
    SMK_ASSERT_EQ_INT(source.audio_track_count, 1);
    SMK_ASSERT_EQ_INT(source.audio[0].channels, 1);
    SMK_ASSERT_EQ_INT(source.audio[0].rate, 44100);
    SMK_ASSERT_EQ_INT(source.audio[0].bits, 16);

    fill_pattern(frame, 4, 4);
    {
        uint8_t decoded[48];
        SMK_ASSERT_EQ_INT(call_read_frame(&source, 0, decoded), 0);
        SMK_ASSERT_MEM_EQ(decoded, frame, 48);
        SMK_ASSERT_EQ_INT(call_read_frame(&source, 1, decoded), 0);
        SMK_ASSERT_EQ_INT(decoded[0], 99);
    }
    SMK_ASSERT_EQ_INT(call_read_audio(&source, 0, 0, 3, out), 6);
    SMK_ASSERT_MEM_EQ(out, samples, sizeof(samples));
    memset(out, 0x7f, sizeof(out));
    SMK_ASSERT_EQ_INT(call_read_audio(&source, 0, 2, 2, out), 4);
    SMK_ASSERT_EQ_INT(out[0], 300);
    SMK_ASSERT_EQ_INT(out[1], 0);

    smk_avi_close(&source);
    remove(path);
    free(path);
    free(b.data);
}

void test_avi_two_audio_tracks(void)
{
    abuf b;
    size_t riff;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t rgb[48];
    static const int16_t mono[2] = { 11, 22 };
    static const int16_t stereo[4] = { 1, 2, 3, 4 };
    int16_t out[4];

    memset(&b, 0, sizeof(b));
    avi_begin(&b, &riff);
    {
        size_t hdrl = ab_container_begin(&b, "LIST", "hdrl");
        avi_avih(&b, 50000, 1, 3, 4, 4);
        avi_video_strl(&b, 4, 4, 24, 0, "DIB ", 50000, 1000000);
        avi_audio_strl(&b, 1, 8000, 16);
        avi_audio_strl(&b, 2, 16000, 16);
        ab_chunk_end(&b, hdrl);
    }
    fill_pattern(rgb, 4, 4);
    {
        size_t movi = ab_container_begin(&b, "LIST", "movi");
        avi_video_chunk(&b, 0, 0, rgb, 4, 4, 24, 0);
        avi_audio_chunk(&b, 1, mono, sizeof(mono));
        avi_audio_chunk(&b, 2, stereo, sizeof(stereo));
        ab_chunk_end(&b, movi);
    }
    ab_chunk_end(&b, riff);

    path = write_temp_avi("tracks.avi", &b);
    SMK_ASSERT(path != NULL);
    default_options(&options);
    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.fps_num, 20);
    SMK_ASSERT_EQ_INT(source.audio_track_count, 2);
    SMK_ASSERT_EQ_INT(source.audio[0].rate, 8000);
    SMK_ASSERT_EQ_INT(source.audio[0].channels, 1);
    SMK_ASSERT_EQ_INT(source.audio[1].rate, 16000);
    SMK_ASSERT_EQ_INT(source.audio[1].channels, 2);

    SMK_ASSERT_EQ_INT(call_read_audio(&source, 0, 0, 2, out), 4);
    SMK_ASSERT_MEM_EQ(out, mono, sizeof(mono));
    SMK_ASSERT_EQ_INT(call_read_audio(&source, 1, 0, 2, out), 8);
    SMK_ASSERT_MEM_EQ(out, stereo, sizeof(stereo));

    smk_avi_close(&source);
    remove(path);
    free(path);
    free(b.data);
}

void test_avi_interleaved_chunks(void)
{
    abuf b;
    size_t riff;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t rgb[48];
    static const int16_t first[2] = { 5, 6 };
    static const int16_t second[2] = { 7, 8 };
    static const int16_t expected[4] = { 5, 6, 7, 8 };
    int16_t out[4];

    memset(&b, 0, sizeof(b));
    avi_begin(&b, &riff);
    {
        size_t hdrl = ab_container_begin(&b, "LIST", "hdrl");
        avi_avih(&b, 40000, 2, 2, 4, 4);
        avi_video_strl(&b, 4, 4, 24, 0, "DIB ", 40000, 1000000);
        avi_audio_strl(&b, 1, 22050, 16);
        ab_chunk_end(&b, hdrl);
    }
    fill_pattern(rgb, 4, 4);
    {
        size_t movi = ab_container_begin(&b, "LIST", "movi");
        avi_video_chunk(&b, 0, 0, rgb, 4, 4, 24, 0);
        avi_audio_chunk(&b, 1, first, sizeof(first));
        avi_video_chunk(&b, 0, 0, rgb, 4, 4, 24, 0);
        avi_audio_chunk(&b, 1, second, sizeof(second));
        ab_chunk_end(&b, movi);
    }
    ab_chunk_end(&b, riff);

    path = write_temp_avi("interleaved.avi", &b);
    SMK_ASSERT(path != NULL);
    default_options(&options);
    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.frame_count, 2);
    SMK_ASSERT_EQ_INT(call_read_audio(&source, 0, 0, 4, out), 8);
    SMK_ASSERT_MEM_EQ(out, expected, sizeof(expected));

    smk_avi_close(&source);
    remove(path);
    free(path);
    free(b.data);
}

void test_avi_32bit_video(void)
{
    abuf b;
    size_t riff;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t rgb[48];
    uint8_t decoded[48];

    memset(&b, 0, sizeof(b));
    avi_begin(&b, &riff);
    {
        size_t hdrl = ab_container_begin(&b, "LIST", "hdrl");
        avi_avih(&b, 40000, 1, 1, 4, 4);
        avi_video_strl(&b, 4, 4, 32, 0, "DIB ", 40000, 1000000);
        ab_chunk_end(&b, hdrl);
    }
    fill_pattern(rgb, 4, 4);
    {
        size_t movi = ab_container_begin(&b, "LIST", "movi");
        avi_video_chunk(&b, 0, 0, rgb, 4, 4, 32, 0);
        ab_chunk_end(&b, movi);
    }
    ab_chunk_end(&b, riff);

    path = write_temp_avi("rgba.avi", &b);
    SMK_ASSERT(path != NULL);
    default_options(&options);
    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(call_read_frame(&source, 0, decoded), 0);
    SMK_ASSERT_MEM_EQ(decoded, rgb, 48);

    smk_avi_close(&source);
    remove(path);
    free(path);
    free(b.data);
}

void test_avi_odd_width_stride(void)
{
    abuf b;
    size_t riff;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t rgb[18];
    uint8_t decoded[48];

    memset(&b, 0, sizeof(b));
    avi_begin(&b, &riff);
    {
        size_t hdrl = ab_container_begin(&b, "LIST", "hdrl");
        avi_avih(&b, 40000, 1, 1, 3, 2);
        avi_video_strl(&b, 3, 2, 24, 0, "DIB ", 40000, 1000000);
        ab_chunk_end(&b, hdrl);
    }
    fill_pattern(rgb, 3, 2);
    {
        size_t movi = ab_container_begin(&b, "LIST", "movi");
        avi_video_chunk(&b, 0, 0, rgb, 3, 2, 24, 1);
        ab_chunk_end(&b, movi);
    }
    ab_chunk_end(&b, riff);

    path = write_temp_avi("odd.avi", &b);
    SMK_ASSERT(path != NULL);
    default_options(&options);
    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.width, 4);
    SMK_ASSERT_EQ_INT(source.height, 4);
    SMK_ASSERT_EQ_INT(call_read_frame(&source, 0, decoded), 0);
    /* Padded column 3 replicates source column 2. */
    SMK_ASSERT_EQ_INT(decoded[3 * 3 + 0], 10 + 2 * 20);
    SMK_ASSERT_EQ_INT(decoded[3 * 3 + 1], 20);
    SMK_ASSERT_EQ_INT(decoded[3 * 3 + 2], 30 + 2);
    /* Padded row 2 replicates source row 1. */
    SMK_ASSERT_EQ_INT(decoded[(2 * 4) * 3 + 0], 10);
    SMK_ASSERT_EQ_INT(decoded[(2 * 4) * 3 + 1], 20 + 20);
    SMK_ASSERT_EQ_INT(decoded[(3 * 4) * 3 + 1], 20 + 20);

    smk_avi_close(&source);
    remove(path);
    free(path);
    free(b.data);
}

void test_avi_rejects_unsupported_codec(void)
{
    abuf b;
    size_t riff;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t rgb[48];

    memset(&b, 0, sizeof(b));
    avi_begin(&b, &riff);
    {
        size_t hdrl = ab_container_begin(&b, "LIST", "hdrl");
        avi_avih(&b, 40000, 1, 1, 4, 4);
        avi_video_strl(&b, 4, 4, 24, 0, "MJPG", 40000, 1000000);
        ab_chunk_end(&b, hdrl);
    }
    fill_pattern(rgb, 4, 4);
    {
        size_t movi = ab_container_begin(&b, "LIST", "movi");
        avi_video_chunk(&b, 0, 0, rgb, 4, 4, 24, 0);
        ab_chunk_end(&b, movi);
    }
    ab_chunk_end(&b, riff);

    path = write_temp_avi("mjpg.avi", &b);
    SMK_ASSERT(path != NULL);
    default_options(&options);
    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(err[0] != '\0');

    remove(path);
    free(path);
    free(b.data);
}
