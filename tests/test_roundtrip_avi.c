#include "test.h"
#include "test_util.h"

#include "avi.h"
#include "smk_encoder.h"
#include "verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal smk2avi-style AVI writer: DIB video plus PCM audio streams. */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} avi_buf;

static void ab_reserve(avi_buf *b, size_t extra)
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

static void ab_raw(avi_buf *b, const void *data, size_t size)
{
    ab_reserve(b, size);
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

static void ab_u8(avi_buf *b, unsigned value)
{
    uint8_t byte = (uint8_t)value;

    ab_raw(b, &byte, 1);
}

static void ab_u16(avi_buf *b, unsigned value)
{
    ab_u8(b, value & 0xff);
    ab_u8(b, (value >> 8) & 0xff);
}

static void ab_u32(avi_buf *b, uint32_t value)
{
    ab_u8(b, value & 0xff);
    ab_u8(b, (value >> 8) & 0xff);
    ab_u8(b, (value >> 16) & 0xff);
    ab_u8(b, (value >> 24) & 0xff);
}

static size_t ab_placeholder(avi_buf *b)
{
    size_t pos = b->size;

    ab_u32(b, 0);
    return pos;
}

static void ab_patch_size(avi_buf *b, size_t pos)
{
    uint32_t size = (uint32_t)(b->size - pos - 4);

    b->data[pos] = (uint8_t)(size & 0xff);
    b->data[pos + 1] = (uint8_t)((size >> 8) & 0xff);
    b->data[pos + 2] = (uint8_t)((size >> 16) & 0xff);
    b->data[pos + 3] = (uint8_t)((size >> 24) & 0xff);
}

static size_t ab_chunk(avi_buf *b, const char *id)
{
    ab_raw(b, id, 4);
    return ab_placeholder(b);
}

static size_t ab_list(avi_buf *b, const char *type)
{
    size_t pos = ab_chunk(b, "LIST");

    ab_raw(b, type, 4);
    return pos;
}

static size_t ab_end(avi_buf *b, size_t pos)
{
    ab_patch_size(b, pos);
    while (b->size & 1u)
        ab_u8(b, 0);
    return b->size;
}

static void avi_header(avi_buf *b, unsigned frames, unsigned streams,
                       unsigned width, unsigned height, unsigned fps,
                       unsigned bpp, unsigned compression,
                       const char *handler,
                       const unsigned *audio_rate,
                       const unsigned *audio_channels,
                       const unsigned *audio_bits,
                       unsigned audio_count)
{
    size_t riff = ab_chunk(b, "RIFF");
    size_t hdrl;
    unsigned i;

    ab_raw(b, "AVI ", 4);
    hdrl = ab_list(b, "hdrl");
    {
        size_t avih = ab_chunk(b, "avih");
        unsigned usf = 1000000u / fps;

        ab_u32(b, usf);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, frames);
        ab_u32(b, 0);
        ab_u32(b, streams);
        ab_u32(b, width * height * (bpp / 8));
        ab_u32(b, width);
        ab_u32(b, height);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_end(b, avih);
    }
    {
        size_t strl = ab_list(b, "strl");
        size_t strh = ab_chunk(b, "strh");
        unsigned frame_size = width * height * (bpp / 8);

        ab_raw(b, "vids", 4);
        ab_raw(b, handler, 4);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 1);       /* dwScale */
        ab_u32(b, fps);     /* dwRate */
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, frame_size);
        ab_u32(b, 0xffffffffu);
        ab_u32(b, frame_size);
        ab_u32(b, 0);
        ab_u16(b, width);
        ab_u16(b, height);
        ab_end(b, strh);
        {
            size_t strf = ab_chunk(b, "strf");

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
            ab_end(b, strf);
        }
        ab_end(b, strl);
    }
    for (i = 0; i < audio_count; i++) {
        size_t strl = ab_list(b, "strl");
        size_t strh = ab_chunk(b, "strh");
        unsigned block = audio_channels[i] * (audio_bits[i] / 8);

        ab_raw(b, "auds", 4);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, audio_channels[i]);
        ab_u32(b, audio_rate[i] * audio_channels[i]);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_u32(b, block);
        ab_u32(b, 0xffffffffu);
        ab_u32(b, block);
        ab_u32(b, 0);
        ab_u32(b, 0);
        ab_end(b, strh);
        {
            size_t strf = ab_chunk(b, "strf");

            ab_u16(b, 1);
            ab_u16(b, audio_channels[i]);
            ab_u32(b, audio_rate[i]);
            ab_u32(b, audio_rate[i] * block);
            ab_u16(b, block);
            ab_u16(b, audio_bits[i]);
            ab_u16(b, 0);
            ab_end(b, strf);
        }
        ab_end(b, strl);
    }
    ab_end(b, hdrl);

    {
        size_t movi = ab_list(b, "movi");
        unsigned f;
        uint8_t *rgb = (uint8_t *)malloc((size_t)width * height * 3);

        if (rgb == NULL)
            abort();
        for (f = 0; f < frames; f++) {
            size_t chunk = ab_chunk(b, "00db");
            int y;

            for (y = (int)height - 1; y >= 0; y--) {
                unsigned x;

                for (x = 0; x < width; x++) {
                    uint8_t *p = rgb +
                        ((size_t)y * width + x) * 3;

                    p[0] = (uint8_t)((x * 16 + f * 4) & 0xff);
                    p[1] = (uint8_t)((y * 16 + f * 8) & 0xff);
                    p[2] = (uint8_t)((x * 7 + y * 5) & 0xff);
                    ab_u8(b, p[2]);
                    ab_u8(b, p[1]);
                    ab_u8(b, p[0]);
                }
            }
            ab_end(b, chunk);
        }
        free(rgb);

        for (i = 0; i < audio_count; i++) {
            size_t chunk;
            size_t bytes = (size_t)audio_channels[i] *
                           (audio_bits[i] / 8) * 2000;
            size_t j;
            char id[9];

            snprintf(id, sizeof(id), "%02uwb", i + 1);
            chunk = ab_chunk(b, id);
            for (j = 0; j < bytes; j++)
                ab_u8(b, (uint8_t)((j * 37u + i * 11u) & 0xffu));
            ab_end(b, chunk);
        }
        ab_end(b, movi);
    }
    ab_end(b, riff);
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

static char *write_temp_avi(const char *name, avi_buf *b)
{
    char *path = make_output_path(name);

    if (path == NULL)
        return NULL;
    if (test_write_file(path, b->data, b->size) != 0) {
        free(path);
        return NULL;
    }
    return path;
}

static void build_avi(avi_buf *b, unsigned frames, unsigned width,
                      unsigned height, unsigned compression,
                      const char *handler, unsigned audio_count)
{
    unsigned rate[2] = { 8000, 11025 };
    unsigned channels[2] = { 1, 2 };
    unsigned bits[2] = { 16, 8 };

    memset(b, 0, sizeof(*b));
    avi_header(b, frames, 1 + audio_count, width, height, 15, 24,
               compression, handler, rate, channels, bits, audio_count);
}

void test_roundtrip_avi_end_to_end(void)
{
    avi_buf b;
    char *path;
    char *output;
    smk_enc_options options;
    smk_enc_source source;
    char err[512] = { 0 };

    build_avi(&b, 8, 16, 16, 0, "DIB ", 2);
    path = write_temp_avi("roundtrip.avi", &b);
    output = make_output_path("roundtrip.smk");
    SMK_ASSERT(path != NULL && output != NULL);
    if (path == NULL || output == NULL) {
        free(path);
        free(output);
        free(b.data);
        return;
    }
    smk_enc_options_default(&options);
    options.palette = SMK_ENC_PALETTE_ADAPTIVE;
    options.dither = SMK_ENC_DITHER_NONE;
    options.verify = 1;
    memset(&source, 0, sizeof(source));

    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.width, 16);
    SMK_ASSERT_EQ_INT(source.height, 16);
    SMK_ASSERT_EQ_INT(source.frame_count, 8);
    SMK_ASSERT_EQ_INT(source.audio_track_count, 2);
    SMK_ASSERT_EQ_INT(source.audio[0].channels, 1);
    SMK_ASSERT_EQ_INT(source.audio[1].channels, 2);

    SMK_ASSERT_EQ_INT(smk_enc_encode(&source, &options, output, err,
                                     sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(smk_verify_file(output, &source, &options, err,
                                      sizeof(err)), SMK_ENC_OK);

    smk_avi_close(&source);
    remove(path);
    remove(output);
    free(path);
    free(output);
    free(b.data);
}

void test_roundtrip_avi_unsupported_codec(void)
{
    avi_buf b;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[512] = { 0 };

    build_avi(&b, 1, 16, 16, 1, "DIB ", 0);
    path = write_temp_avi("unsupported.avi", &b);
    SMK_ASSERT(path != NULL);
    if (path == NULL) {
        free(b.data);
        return;
    }

    smk_enc_options_default(&options);
    memset(&source, 0, sizeof(source));
    SMK_ASSERT_EQ_INT(smk_avi_open(path, &options, &source, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(strstr(err, "unsupported") != NULL);
    smk_avi_close(&source);

    remove(path);
    free(path);
    free(b.data);
}

void test_avi_truncation_matrix(void)
{
    static const unsigned divisors[] = { 0, 2, 4, 8, 16, 32 };
    avi_buf b;
    char *path;
    smk_enc_options options;
    smk_enc_source source;
    char err[512];
    size_t cut;
    unsigned i;

    build_avi(&b, 2, 16, 16, 0, "DIB ", 1);
    path = write_temp_avi("truncated.avi", &b);
    SMK_ASSERT(path != NULL);
    if (path == NULL) {
        free(b.data);
        return;
    }
    smk_enc_options_default(&options);

    for (i = 0; i < sizeof(divisors) / sizeof(divisors[0]); i++) {
        cut = divisors[i] == 0 ? b.size - 8 : b.size / divisors[i];

        memset(err, 0, sizeof(err));
        memset(&source, 0, sizeof(source));
        SMK_ASSERT_EQ_INT(test_write_file(path, b.data, cut), 0);
        SMK_ASSERT(smk_avi_open(path, &options, &source, err, sizeof(err)) !=
                   SMK_ENC_OK);
        SMK_ASSERT(err[0] != '\0');
        smk_avi_close(&source);
    }

    remove(path);
    free(path);
    free(b.data);
}
