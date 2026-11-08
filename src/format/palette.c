/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2021 Greg Kennedy

	See smacker.h for more information.

	smacker.c
		Main implementation file of libsmacker.
		Open, close, query, render, advance and seek an smk
*/

#include "palette.h"

#include <stdio.h>
#include <string.h>

const uint8_t smk_palmap[64] = {
    0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
    0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
    0x41, 0x45, 0x49, 0x4d, 0x51, 0x55, 0x59, 0x5d,
    0x61, 0x65, 0x69, 0x6d, 0x71, 0x75, 0x79, 0x7d,
    0x82, 0x86, 0x8a, 0x8e, 0x92, 0x96, 0x9a, 0x9e,
    0xa2, 0xa6, 0xaa, 0xae, 0xb2, 0xb6, 0xba, 0xbe,
    0xc3, 0xc7, 0xcb, 0xcf, 0xd3, 0xd7, 0xdb, 0xdf,
    0xe3, 0xe7, 0xeb, 0xef, 0xf3, 0xf7, 0xfb, 0xff
};

int smk_palette_apply(uint8_t palette[768], const uint8_t *chunk, size_t size)
{
    unsigned short i = 0;
    unsigned short count, src;
    uint8_t previous[768];
    const uint8_t *p = chunk;

    if (palette == NULL || p == NULL)
        return -1;
    memcpy(previous, palette, 768);

    while ((i < 256) && (size > 0)) {
        if ((*p) & 0x80) {
            count = ((*p) & 0x7F) + 1;
            p++;
            size--;

            if (i + count > 256) {
                fprintf(stderr, "libsmacker::palette_render(s,p,size) - ERROR: overflow, 0x80 attempt to skip %d entries from %d\n", count, i);
                goto error;
            }

            i += count;
        } else if ((*p) & 0x40) {
            if (size < 2) {
                fputs("libsmacker::palette_render(s,p,size) - ERROR: 0x40 ran out of bytes for copy\n", stderr);
                goto error;
            }

            count = ((*p) & 0x3F) + 1;
            p++;
            size--;
            src = *p;
            p++;
            size--;

            if (i + count > 256 || src + count > 256) {
                fprintf(stderr, "libsmacker::palette_render(s,p,size) - ERROR: overflow, 0x40 attempt to copy %d entries from %d to %d\n", count, src, i);
                goto error;
            }

            memmove(palette + (size_t)i * 3, previous + (size_t)src * 3,
                    (size_t)count * 3);
            i += count;
        } else {
            if (size < 3) {
                fprintf(stderr, "libsmacker::palette_render - ERROR: 0x3F ran out of bytes for copy, size=%lu\n", (unsigned long)size);
                goto error;
            }

            for (count = 0; count < 3; count++) {
                if (*p > 0x3F) {
                    fprintf(stderr, "libsmacker::palette_render - ERROR: palette index exceeds 0x3F (entry [%u][%u])\n", i, count);
                    goto error;
                }

                palette[(size_t)i * 3 + count] = smk_palmap[*p];
                p++;
                size--;
            }

            i++;
        }
    }

    if (i < 256) {
        fprintf(stderr, "libsmacker::palette_render - ERROR: did not completely fill palette (idx=%u)\n", i);
        goto error;
    }

    return 0;
error:
    return -1;
}

size_t smk_palette_write_chunk(const uint8_t *entries, unsigned count, uint8_t *out)
{
    size_t pos = 1;
    unsigned i = 0;

    if (entries == NULL || out == NULL)
        return 0;

    while (i < 256) {
        if (i < count) {
            out[pos++] = entries[i * 3];
            out[pos++] = entries[i * 3 + 1];
            out[pos++] = entries[i * 3 + 2];
            i++;
        } else {
            unsigned run = 0;

            while (i + run < 256 && run < 128 && !(i + run < count))
                run++;
            if (run == 0)
                run = 1;
            out[pos++] = (uint8_t)(0x80u | (run - 1));
            i += run;
        }
    }

    while (pos % 4 != 0)
        out[pos++] = 0;
    out[0] = (uint8_t)(pos / 4);
    return pos;
}
