#include "test.h"
#include "test_util.h"
#include "image_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)((value >> 8) & 0xff);
}

/* Write a bottom-up uncompressed 24-bit TGA from top-down RGB. */
static int write_tga(const char *dir, const char *name, unsigned width,
                     unsigned height, const uint8_t *rgb)
{
    size_t size = 18 + (size_t)width * height * 3;
    uint8_t *data = (uint8_t *)calloc(1, size);
    char path[4096];
    unsigned x;
    unsigned y;
    int result;

    if (data == NULL)
        return -1;
    data[2] = 2; /* uncompressed true-color */
    put_u16(data + 12, width);
    put_u16(data + 14, height);
    data[16] = 24;

    for (y = 0; y < height; y++) {
        size_t row = 18 + (size_t)(height - 1 - y) * width * 3;
        for (x = 0; x < width; x++) {
            const uint8_t *src = rgb + ((size_t)y * width + x) * 3;
            uint8_t *dst = data + row + (size_t)x * 3;
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
    }

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    result = test_write_file(path, data, size);
    free(data);
    return result;
}

static int write_wav_16(const char *dir, const char *name,
                        const int16_t *samples, size_t count)
{
    size_t size = 44 + count * 2;
    uint8_t *data = (uint8_t *)calloc(1, size);
    char path[4096];
    size_t i;
    uint32_t data_size = (uint32_t)(count * 2);
    int result;

    if (data == NULL)
        return -1;
    memcpy(data, "RIFF", 4);
    data[4] = (uint8_t)((36 + data_size) & 0xff);
    data[5] = (uint8_t)(((36 + data_size) >> 8) & 0xff);
    data[6] = (uint8_t)(((36 + data_size) >> 16) & 0xff);
    data[7] = (uint8_t)(((36 + data_size) >> 24) & 0xff);
    memcpy(data + 8, "WAVEfmt ", 8);
    data[16] = 16;
    data[20] = 1; /* PCM */
    data[22] = 1; /* mono */
    data[24] = 0x44; data[25] = 0xac; /* 44100 */
    data[28] = 0x88; data[29] = 0x58; data[30] = 0x01; /* byte rate */
    data[32] = 2;
    data[34] = 16;
    memcpy(data + 36, "data", 4);
    data[40] = (uint8_t)(data_size & 0xff);
    data[41] = (uint8_t)((data_size >> 8) & 0xff);
    data[42] = (uint8_t)((data_size >> 16) & 0xff);
    data[43] = (uint8_t)((data_size >> 24) & 0xff);
    for (i = 0; i < count; i++) {
        data[44 + i * 2] = (uint8_t)((uint16_t)samples[i] & 0xff);
        data[45 + i * 2] = (uint8_t)(((uint16_t)samples[i] >> 8) & 0xff);
    }

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    result = test_write_file(path, data, size);
    free(data);
    return result;
}

static void default_options(smk_enc_options *options)
{
    smk_enc_options_default(options);
    options->fps_num = 15;
    options->fps_den = 1;
}

static void fill_rgb(uint8_t *dst, unsigned width, unsigned height,
                     uint8_t r, uint8_t g, uint8_t b)
{
    size_t i;
    for (i = 0; i < (size_t)width * height; i++) {
        dst[i * 3] = r;
        dst[i * 3 + 1] = g;
        dst[i * 3 + 2] = b;
    }
}

void test_image_dir_natural_sort_and_filter(void)
{
    char *dir = test_tempdir();
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t frame[48];
    uint8_t red[48];
    uint8_t green[48];
    uint8_t blue[48];

    SMK_ASSERT(dir != NULL);
    default_options(&options);
    fill_rgb(red, 4, 4, 255, 0, 0);
    fill_rgb(green, 4, 4, 0, 255, 0);
    fill_rgb(blue, 4, 4, 0, 0, 255);
    SMK_ASSERT_EQ_INT(write_tga(dir, "frame10.tga", 4, 4, blue), 0);
    SMK_ASSERT_EQ_INT(write_tga(dir, "frame2.tga", 4, 4, green), 0);
    SMK_ASSERT_EQ_INT(write_tga(dir, "frame1.tga", 4, 4, red), 0);

    {
        char path[4096];
        snprintf(path, sizeof(path), "%s/notes.txt", dir);
        SMK_ASSERT_EQ_INT(test_write_file(path, "ignore me", 9), 0);
    }

    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_image_dir_open(dir, &options, &source, err,
                                         sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.frame_count, 3);
    SMK_ASSERT_EQ_INT(source.width, 4);
    SMK_ASSERT_EQ_INT(source.height, 4);
    SMK_ASSERT_EQ_INT(source.fps_num, 15);
    SMK_ASSERT_EQ_INT(source.fps_den, 1);
    SMK_ASSERT(source.read_frame != NULL);

    SMK_ASSERT_EQ_INT(source.read_frame(source.user, 0, frame), 0);
    SMK_ASSERT_MEM_EQ(frame, red, 48);
    SMK_ASSERT_EQ_INT(source.read_frame(source.user, 1, frame), 0);
    SMK_ASSERT_MEM_EQ(frame, green, 48);
    SMK_ASSERT_EQ_INT(source.read_frame(source.user, 2, frame), 0);
    SMK_ASSERT_MEM_EQ(frame, blue, 48);
    SMK_ASSERT_EQ_INT(source.read_frame(source.user, 3, frame), -1);

    smk_image_dir_close(&source);
    free(dir);
}

void test_image_dir_mixed_sizes(void)
{
    char *dir = test_tempdir();
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    uint8_t *frame;
    uint8_t small[12];
    uint8_t large[48];

    SMK_ASSERT(dir != NULL);
    default_options(&options);
    memset(small, 10, sizeof(small));
    memset(large, 20, sizeof(large));
    SMK_ASSERT_EQ_INT(write_tga(dir, "a.tga", 2, 2, small), 0);
    SMK_ASSERT_EQ_INT(write_tga(dir, "b.tga", 4, 4, large), 0);

    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_image_dir_open(dir, &options, &source, err,
                                         sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.frame_count, 2);
    frame = (uint8_t *)malloc((size_t)source.width * source.height * 3);
    SMK_ASSERT(frame != NULL);
    if (frame != NULL) {
        SMK_ASSERT_EQ_INT(source.read_frame(source.user, 0, frame), 0);
        SMK_ASSERT_EQ_INT(source.read_frame(source.user, 1, frame), -1);
    }
    free(frame);
    smk_image_dir_close(&source);
    free(dir);
}

void test_image_dir_pads_and_replicates(void)
{
    char *dir = test_tempdir();
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    unsigned w = 321;
    unsigned h = 199;
    uint8_t *input;
    uint8_t *frame;
    unsigned x;
    unsigned y;

    SMK_ASSERT(dir != NULL);
    default_options(&options);
    input = (uint8_t *)malloc((size_t)w * h * 3);
    SMK_ASSERT(input != NULL);
    if (input == NULL) {
        free(dir);
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *p = input + ((size_t)y * w + x) * 3;
            int edge = x == w - 1 || y == h - 1;
            p[0] = edge ? 0 : 255;
            p[1] = 0;
            p[2] = edge ? 255 : 0;
        }
    }
    SMK_ASSERT_EQ_INT(write_tga(dir, "big.tga", w, h, input), 0);
    free(input);

    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_image_dir_open(dir, &options, &source, err,
                                         sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(source.width, 324);
    SMK_ASSERT_EQ_INT(source.height, 200);
    frame = (uint8_t *)malloc((size_t)source.width * source.height * 3);
    SMK_ASSERT(frame != NULL);
    if (frame != NULL) {
        SMK_ASSERT_EQ_INT(source.read_frame(source.user, 0, frame), 0);
        SMK_ASSERT_EQ_INT(frame[0], 255);           /* (0,0) red */
        SMK_ASSERT_EQ_INT(frame[1], 0);
        SMK_ASSERT_EQ_INT(frame[2], 0);
        SMK_ASSERT_EQ_INT(frame[(320 * 3) + 0], 0); /* (320,0) blue */
        SMK_ASSERT_EQ_INT(frame[(320 * 3) + 2], 255);
        SMK_ASSERT_EQ_INT(frame[(323 * 3) + 0], 0); /* replicated column */
        SMK_ASSERT_EQ_INT(frame[(323 * 3) + 2], 255);
        /* Last source row replicated into padded row 199. */
        SMK_ASSERT_EQ_INT(frame[((size_t)199 * 324 + 100) * 3 + 2], 255);
    }
    free(frame);
    smk_image_dir_close(&source);
    free(dir);
}

void test_image_dir_strict_rejects_padding(void)
{
    char *dir = test_tempdir();
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    unsigned w = 321;
    unsigned h = 199;
    uint8_t *input;

    SMK_ASSERT(dir != NULL);
    default_options(&options);
    options.strict = 1;
    input = (uint8_t *)calloc((size_t)w * h * 3, 1);
    SMK_ASSERT(input != NULL);
    if (input != NULL) {
        SMK_ASSERT_EQ_INT(write_tga(dir, "big.tga", w, h, input), 0);
        free(input);
    }

    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_image_dir_open(dir, &options, &source, err,
                                         sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(err[0] != '\0');
    free(dir);
}

void test_image_dir_empty(void)
{
    char *dir = test_tempdir();
    smk_enc_options options;
    smk_enc_source source;
    char err[256];

    SMK_ASSERT(dir != NULL);
    default_options(&options);
    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_image_dir_open(dir, &options, &source, err,
                                         sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(err[0] != '\0');
    free(dir);
}

void test_image_dir_audio_pads_silence(void)
{
    char *dir = test_tempdir();
    smk_enc_options options;
    smk_enc_source source;
    char err[256];
    static const int16_t samples[3] = { 100, 200, 300 };
    static const int16_t expected[4] = { 200, 300, 0, 0 };
    int16_t out[4];
    uint8_t image[3] = { 9, 9, 9 };

    SMK_ASSERT(dir != NULL);
    default_options(&options);
    SMK_ASSERT_EQ_INT(write_tga(dir, "f.tga", 1, 1, image), 0);
    SMK_ASSERT_EQ_INT(write_wav_16(dir, "a.wav", samples, 3), 0);

    memset(&source, 0, sizeof(source));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(smk_image_dir_open(dir, &options, &source, err,
                                         sizeof(err)),
                      SMK_ENC_OK);
    {
        char path[4096];
        snprintf(path, sizeof(path), "%s/a.wav", dir);
        SMK_ASSERT_EQ_INT(smk_image_dir_attach_audio(&source, path, err,
                                                     sizeof(err)),
                          SMK_ENC_OK);
    }
    SMK_ASSERT_EQ_INT(source.audio_track_count, 1);
    SMK_ASSERT_EQ_INT(source.audio[0].rate, 44100);
    SMK_ASSERT_EQ_INT(source.audio[0].channels, 1);
    SMK_ASSERT_EQ_INT(source.audio[0].bits, 16);
    SMK_ASSERT(source.read_audio != NULL);
    memset(out, 0x7f, sizeof(out));
    SMK_ASSERT_EQ_INT(source.read_audio(source.user, 0, 1, 4, out), 8);
    SMK_ASSERT_MEM_EQ(out, expected, sizeof(expected));

    smk_image_dir_close(&source);
    free(dir);
}
