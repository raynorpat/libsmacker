/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2021 Greg Kennedy

	See smacker.h for more information.

	smacker.c
		Main implementation file of libsmacker.
		Open, close, query, render, advance and seek an smk
*/

#include "header.h"

#include <limits.h>
#include <string.h>

uint32_t smk_header_read_u32(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

void smk_header_write_u32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

int smk_header_pack(const smk_file_header *header, uint8_t out[SMK_HEADER_SIZE])
{
    unsigned i;
    uint8_t *p;

    if (header == NULL || out == NULL)
        return -1;

    p = out;
    memcpy(p, header->signature, 4);
    p += 4;
    smk_header_write_u32(p, header->width);
    p += 4;
    smk_header_write_u32(p, header->height);
    p += 4;
    smk_header_write_u32(p, header->frames);
    p += 4;
    smk_header_write_u32(p, (uint32_t)header->frame_rate);
    p += 4;
    smk_header_write_u32(p, header->flags);
    p += 4;
    for (i = 0; i < 7; i++) {
        smk_header_write_u32(p, header->audio_size[i]);
        p += 4;
    }
    smk_header_write_u32(p, header->trees_size);
    p += 4;
    for (i = 0; i < 4; i++) {
        smk_header_write_u32(p, header->tree_size[i]);
        p += 4;
    }
    for (i = 0; i < 7; i++) {
        smk_header_write_u32(p, header->audio_rate[i]);
        p += 4;
    }
    smk_header_write_u32(p, 0);
    return 0;
}

int smk_header_unpack(const uint8_t in[SMK_HEADER_SIZE], smk_file_header *header)
{
    unsigned i;
    const uint8_t *p;

    if (in == NULL || header == NULL)
        return -1;

    p = in;
    memcpy(header->signature, p, 4);
    p += 4;
    header->width = smk_header_read_u32(p);
    p += 4;
    header->height = smk_header_read_u32(p);
    p += 4;
    header->frames = smk_header_read_u32(p);
    p += 4;
    header->frame_rate = (int32_t)smk_header_read_u32(p);
    p += 4;
    header->flags = smk_header_read_u32(p);
    p += 4;
    for (i = 0; i < 7; i++) {
        header->audio_size[i] = smk_header_read_u32(p);
        p += 4;
    }
    header->trees_size = smk_header_read_u32(p);
    p += 4;
    for (i = 0; i < 4; i++) {
        header->tree_size[i] = smk_header_read_u32(p);
        p += 4;
    }
    for (i = 0; i < 7; i++) {
        header->audio_rate[i] = smk_header_read_u32(p);
        p += 4;
    }
    return 0;
}

int32_t smk_header_frame_rate(unsigned num, unsigned den)
{
    double us;
    long ms;
    long tens;
    double ms_error;
    double tens_error;

    if (num == 0 || den == 0)
        return 0;
    us = 1000000.0 * (double)den / (double)num;

    ms = (long)(us / 1000.0 + 0.5);
    tens = (long)(us / 10.0 + 0.5);
    if (tens < 1)
        tens = 1;
    ms_error = (double)ms * 1000.0 - us;
    tens_error = (double)tens * 10.0 - us;
    if (ms_error < 0.0)
        ms_error = -ms_error;
    if (tens_error < 0.0)
        tens_error = -tens_error;

    if (ms >= 1 && ms <= INT32_MAX &&
        (tens > INT32_MAX || ms_error <= tens_error))
        return (int32_t)ms;
    if (tens <= INT32_MAX)
        return (int32_t)-tens;
    return 0;
}

int smk_header_pack_audio_rate(uint32_t rate, int bits16, int stereo,
                               uint32_t *out)
{
    if (out == NULL || rate > 0x00ffffffu)
        return -1;
    *out = 0x40000000u |
           (bits16 ? 0x20000000u : 0u) |
           (stereo ? 0x10000000u : 0u) |
           rate;
    return 0;
}

void smk_header_unpack_audio_rate(uint32_t word, smk_audio_rate_info *out)
{
    memset(out, 0, sizeof(*out));
    if ((word & 0x40000000u) == 0)
        return;
    out->exists = 1;
    out->dpcm = (word & 0x80000000u) != 0;
    out->bink = (word & 0x0c000000u) != 0;
    out->bits16 = (word & 0x20000000u) != 0;
    out->stereo = (word & 0x10000000u) != 0;
    out->rate = word & 0x00ffffffu;
}

uint32_t smk_header_pack_frame_size(uint32_t padded_size, int keyframe)
{
    return (padded_size & 0xfffffffcu) | (keyframe ? 1u : 0u);
}

void smk_header_unpack_frame_size(uint32_t word, uint32_t *size, int *keyframe)
{
    *size = word & 0xfffffffcu;
    *keyframe = (word & 0x1u) != 0;
}
