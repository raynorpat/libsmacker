#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_READ_BUFFER 4096

static smk_enc_status fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return SMK_ENC_ERR_INPUT;
}

static int read_exact(FILE *fp, void *dst, size_t size)
{
    return size == 0 || fread(dst, 1, size, fp) == size ? 0 : -1;
}

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t load_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int skip_bytes(FILE *fp, uint32_t size)
{
    if (size > 0x7fffffffUL)
        return -1;
    return fseek(fp, (long)size, SEEK_CUR) == 0 ? 0 : -1;
}

/* Reduce one 24-bit little-endian sample to 16 bits with rounding. */
static int16_t reduce_24(const uint8_t *src)
{
    int32_t value = (int32_t)((uint32_t)src[0] |
                              ((uint32_t)src[1] << 8) |
                              ((uint32_t)src[2] << 16));

    if (value & 0x800000)
        value -= 0x1000000;
    value = value >= 0 ? (value + 128) / 256 : -((-value + 128) / 256);
    if (value > 32767)
        value = 32767;
    if (value < -32768)
        value = -32768;
    return (int16_t)value;
}

smk_enc_status smk_wav_load(const char *path, smk_pcm_audio *audio,
                            char *err, size_t errlen)
{
    FILE *fp;
    uint8_t riff[12];
    uint8_t chunk[8];
    uint8_t fmt[16];
    unsigned format = 0;
    unsigned channels = 0;
    unsigned bits = 0;
    uint32_t rate = 0;
    int have_fmt = 0;
    int have_data = 0;
    long data_offset = 0;
    uint32_t data_size = 0;
    unsigned bytes_per_sample;
    unsigned block_align;
    size_t sample_count;
    size_t total_samples;
    int16_t *pcm = NULL;
    smk_enc_status status = SMK_ENC_ERR_INPUT;

    if (path == NULL || audio == NULL)
        return fail(err, errlen, "missing WAV load argument");

    memset(audio, 0, sizeof(*audio));

    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "cannot open WAV file: %s", path);
        return SMK_ENC_ERR_INPUT;
    }

    if (read_exact(fp, riff, sizeof(riff)) != 0 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        status = fail(err, errlen, "not a RIFF WAVE file");
        goto done;
    }

    for (;;) {
        uint32_t size;

        if (fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk))
            break; /* end of chunk list */
        size = load_u32(chunk + 4);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16) {
                status = fail(err, errlen, "WAV fmt chunk is too short");
                goto done;
            }
            if (read_exact(fp, fmt, sizeof(fmt)) != 0) {
                status = fail(err, errlen, "truncated WAV fmt chunk");
                goto done;
            }
            format = load_u16(fmt);
            channels = load_u16(fmt + 2);
            rate = load_u32(fmt + 4);
            bits = load_u16(fmt + 14);
            have_fmt = 1;
            if (size > 16 && skip_bytes(fp, size - 16) != 0) {
                status = fail(err, errlen, "truncated WAV fmt chunk");
                goto done;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_offset = ftell(fp);
            data_size = size;
            have_data = 1;
            if (data_offset < 0 ||
                skip_bytes(fp, size) != 0) {
                status = fail(err, errlen, "truncated WAV data chunk");
                goto done;
            }
            /* Do not stop: tolerate trailing chunks. */
        } else if (skip_bytes(fp, size) != 0) {
            status = fail(err, errlen, "truncated WAV chunk");
            goto done;
        }

        if (size & 1u) {
            if (fgetc(fp) == EOF)
                break;
        }
    }

    if (!have_fmt) {
        status = fail(err, errlen, "WAV file has no fmt chunk");
        goto done;
    }
    if (!have_data) {
        status = fail(err, errlen, "WAV file has no data chunk");
        goto done;
    }
    if (format != 1) {
        status = fail(err, errlen,
                      "compressed WAV audio is not supported");
        goto done;
    }
    if (channels != 1 && channels != 2) {
        status = fail(err, errlen, "WAV audio must be mono or stereo");
        goto done;
    }
    if (rate == 0) {
        status = fail(err, errlen, "WAV sample rate is zero");
        goto done;
    }
    if (bits != 8 && bits != 16 && bits != 24) {
        status = fail(err, errlen, "unsupported WAV sample width");
        goto done;
    }

    bytes_per_sample = bits / 8;
    block_align = channels * bytes_per_sample;
    sample_count = data_size / block_align;

    if (sample_count == 0) {
        audio->rate = rate;
        audio->channels = channels;
        audio->bits = bits == 24 ? 16 : bits;
        audio->sample_count = 0;
        audio->pcm = NULL;
        audio->reduced_from_24 = bits == 24;
        status = SMK_ENC_OK;
        goto done;
    }

    total_samples = sample_count * channels;
    if (total_samples > ((size_t)-1) / sizeof(int16_t)) {
        status = fail(err, errlen, "WAV audio is too large");
        goto done;
    }
    pcm = (int16_t *)malloc(total_samples * sizeof(int16_t));
    if (pcm == NULL) {
        status = fail(err, errlen, "out of memory reading WAV audio");
        goto done;
    }

    if (fseek(fp, data_offset, SEEK_SET) != 0) {
        status = fail(err, errlen, "truncated WAV data chunk");
        goto done;
    }

    {
        uint8_t buffer[WAV_READ_BUFFER];
        size_t frames_done = 0;
        size_t chunk_frames = sizeof(buffer) / block_align;
        size_t out_index = 0;

        if (chunk_frames == 0)
            chunk_frames = 1;

        while (frames_done < sample_count) {
            size_t frames = sample_count - frames_done;
            size_t bytes;
            size_t sample;
            size_t samples_here;

            if (frames > chunk_frames)
                frames = chunk_frames;
            bytes = frames * block_align;
            if (read_exact(fp, buffer, bytes) != 0) {
                status = fail(err, errlen, "truncated WAV data chunk");
                goto done;
            }
            samples_here = frames * channels;
            for (sample = 0; sample < samples_here; sample++) {
                const uint8_t *src = buffer + sample * bytes_per_sample;

                if (bytes_per_sample == 1)
                    pcm[out_index++] = (int16_t)(((int)src[0] - 128) * 256);
                else if (bytes_per_sample == 2)
                    pcm[out_index++] = (int16_t)load_u16(src);
                else
                    pcm[out_index++] = reduce_24(src);
            }
            frames_done += frames;
        }
    }

    if (bits == 24)
        fprintf(stderr, "warning: 24-bit WAV reduced to 16-bit\n");

    audio->rate = rate;
    audio->channels = channels;
    audio->bits = bits == 24 ? 16 : bits;
    audio->sample_count = sample_count;
    audio->pcm = pcm;
    audio->reduced_from_24 = bits == 24;
    pcm = NULL;
    status = SMK_ENC_OK;

done:
    free(pcm);
    fclose(fp);
    return status;
}
