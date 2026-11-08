#include "test.h"
#include "test_util.h"
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[8192];
    size_t size;
} wav_buf;

static void wav_u8(wav_buf *b, unsigned value)
{
    b->data[b->size++] = (uint8_t)value;
}

static void wav_u16(wav_buf *b, unsigned value)
{
    wav_u8(b, value & 0xff);
    wav_u8(b, (value >> 8) & 0xff);
}

static void wav_u32(wav_buf *b, uint32_t value)
{
    wav_u8(b, value & 0xff);
    wav_u8(b, (value >> 8) & 0xff);
    wav_u8(b, (value >> 16) & 0xff);
    wav_u8(b, (value >> 24) & 0xff);
}

static void wav_begin(wav_buf *b)
{
    b->size = 0;
    wav_u8(b, 'R'); wav_u8(b, 'I'); wav_u8(b, 'F'); wav_u8(b, 'F');
    wav_u32(b, 0); /* patched by wav_end */
    wav_u8(b, 'W'); wav_u8(b, 'A'); wav_u8(b, 'V'); wav_u8(b, 'E');
}

static void wav_end(wav_buf *b)
{
    uint32_t size = (uint32_t)(b->size - 8);
    b->data[4] = (uint8_t)(size & 0xff);
    b->data[5] = (uint8_t)((size >> 8) & 0xff);
    b->data[6] = (uint8_t)((size >> 16) & 0xff);
    b->data[7] = (uint8_t)((size >> 24) & 0xff);
}

static void wav_fmt(wav_buf *b, unsigned format, unsigned channels,
                    uint32_t rate, unsigned bits)
{
    unsigned block_align = channels * ((bits + 7) / 8);

    wav_u8(b, 'f'); wav_u8(b, 'm'); wav_u8(b, 't'); wav_u8(b, ' ');
    wav_u32(b, 16);
    wav_u16(b, format);
    wav_u16(b, channels);
    wav_u32(b, rate);
    wav_u32(b, rate * block_align);
    wav_u16(b, block_align);
    wav_u16(b, bits);
}

static void wav_data_header(wav_buf *b, uint32_t bytes)
{
    wav_u8(b, 'd'); wav_u8(b, 'a'); wav_u8(b, 't'); wav_u8(b, 'a');
    wav_u32(b, bytes);
}

static char *write_temp_wav(const char *name, const void *data, size_t size)
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
    if (test_write_file(path, data, size) != 0) {
        free(path);
        free(dir);
        return NULL;
    }
    free(dir);
    return path;
}

static int load_wav(const char *path, smk_pcm_audio *audio, char *err,
                    size_t errlen)
{
    return smk_wav_load(path, audio, err, errlen);
}

void test_wav_16bit_mono(void)
{
    wav_buf b;
    smk_pcm_audio audio;
    char err[256];
    char *path;
    static const int16_t samples[4] = { -32768, -1, 0, 32767 };

    wav_begin(&b);
    wav_fmt(&b, 1, 1, 22050, 16);
    wav_data_header(&b, 8);
    wav_u8(&b, 0x00); wav_u8(&b, 0x80);
    wav_u8(&b, 0xff); wav_u8(&b, 0xff);
    wav_u8(&b, 0x00); wav_u8(&b, 0x00);
    wav_u8(&b, 0xff); wav_u8(&b, 0x7f);
    wav_end(&b);

    path = write_temp_wav("mono16.wav", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(&audio, 0, sizeof(audio));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_wav(path, &audio, err, sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(audio.rate, 22050);
    SMK_ASSERT_EQ_INT(audio.channels, 1);
    SMK_ASSERT_EQ_INT(audio.bits, 16);
    SMK_ASSERT_EQ_INT(audio.sample_count, 4);
    SMK_ASSERT_EQ_INT(audio.reduced_from_24, 0);
    if (audio.pcm != NULL)
        SMK_ASSERT_MEM_EQ(audio.pcm, samples, sizeof(samples));
    free(audio.pcm);
    remove(path);
    free(path);
}

void test_wav_8bit_stereo(void)
{
    wav_buf b;
    smk_pcm_audio audio;
    char err[256];
    char *path;
    static const int16_t samples[4] = { -32768, 0, 32512, -16384 };

    wav_begin(&b);
    wav_fmt(&b, 1, 2, 11025, 8);
    wav_data_header(&b, 4);
    wav_u8(&b, 0);
    wav_u8(&b, 128);
    wav_u8(&b, 255);
    wav_u8(&b, 64);
    wav_end(&b);

    path = write_temp_wav("stereo8.wav", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(&audio, 0, sizeof(audio));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_wav(path, &audio, err, sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(audio.rate, 11025);
    SMK_ASSERT_EQ_INT(audio.channels, 2);
    SMK_ASSERT_EQ_INT(audio.bits, 8);
    SMK_ASSERT_EQ_INT(audio.sample_count, 2);
    if (audio.pcm != NULL)
        SMK_ASSERT_MEM_EQ(audio.pcm, samples, sizeof(samples));
    free(audio.pcm);
    remove(path);
    free(path);
}

void test_wav_24bit_reduced(void)
{
    wav_buf b;
    smk_pcm_audio audio;
    char err[256];
    char *path;
    static const int16_t samples[2] = { 16, -16 };

    wav_begin(&b);
    wav_fmt(&b, 1, 1, 8000, 24);
    wav_data_header(&b, 6);
    wav_u8(&b, 0x00); wav_u8(&b, 0x10); wav_u8(&b, 0x00); /* +4096 */
    wav_u8(&b, 0x00); wav_u8(&b, 0xf0); wav_u8(&b, 0xff); /* -4096 */
    wav_end(&b);

    path = write_temp_wav("mono24.wav", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(&audio, 0, sizeof(audio));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_wav(path, &audio, err, sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(audio.bits, 16);
    SMK_ASSERT_EQ_INT(audio.sample_count, 2);
    SMK_ASSERT_EQ_INT(audio.reduced_from_24, 1);
    if (audio.pcm != NULL)
        SMK_ASSERT_MEM_EQ(audio.pcm, samples, sizeof(samples));
    free(audio.pcm);
    remove(path);
    free(path);
}

void test_wav_skips_unknown_chunks(void)
{
    wav_buf b;
    smk_pcm_audio audio;
    char err[256];
    char *path;
    static const int16_t samples[2] = { 100, -100 };

    wav_begin(&b);
    wav_u8(&b, 'J'); wav_u8(&b, 'U'); wav_u8(&b, 'N'); wav_u8(&b, 'K');
    wav_u32(&b, 3);
    wav_u8(&b, 1); wav_u8(&b, 2); wav_u8(&b, 3);
    wav_u8(&b, 0); /* pad to even */
    wav_fmt(&b, 1, 1, 16000, 16);
    wav_u8(&b, 'L'); wav_u8(&b, 'I'); wav_u8(&b, 'S'); wav_u8(&b, 'T');
    wav_u32(&b, 4);
    wav_u32(&b, 0x12345678);
    wav_data_header(&b, 4);
    wav_u8(&b, 100); wav_u8(&b, 0);
    wav_u8(&b, 0x9c); wav_u8(&b, 0xff);
    wav_end(&b);

    path = write_temp_wav("chunks.wav", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(&audio, 0, sizeof(audio));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_wav(path, &audio, err, sizeof(err)), SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(audio.sample_count, 2);
    if (audio.pcm != NULL)
        SMK_ASSERT_MEM_EQ(audio.pcm, samples, sizeof(samples));
    free(audio.pcm);
    remove(path);
    free(path);
}

void test_wav_rejects_compressed(void)
{
    wav_buf b;
    smk_pcm_audio audio;
    char err[256];
    char *path;

    wav_begin(&b);
    wav_fmt(&b, 2, 1, 22050, 4);
    wav_data_header(&b, 4);
    wav_u8(&b, 1); wav_u8(&b, 2); wav_u8(&b, 3); wav_u8(&b, 4);
    wav_end(&b);

    path = write_temp_wav("compressed.wav", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(&audio, 0, sizeof(audio));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_wav(path, &audio, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(audio.pcm == NULL);
    SMK_ASSERT(err[0] != '\0');
    remove(path);
    free(path);
}

void test_wav_truncated(void)
{
    wav_buf b;
    smk_pcm_audio audio;
    char err[256];
    char *path;

    wav_begin(&b);
    wav_fmt(&b, 1, 1, 22050, 16);
    wav_data_header(&b, 16);
    wav_u8(&b, 0); wav_u8(&b, 0);
    wav_u8(&b, 0); wav_u8(&b, 0);
    wav_end(&b);

    path = write_temp_wav("truncated.wav", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(&audio, 0, sizeof(audio));
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_wav(path, &audio, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(audio.pcm == NULL);
    SMK_ASSERT(err[0] != '\0');
    remove(path);
    free(path);
}
