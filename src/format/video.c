/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2021 Greg Kennedy

	See smacker.h for more information.

	smacker.c
		Main implementation file of libsmacker.
		Open, close, query, render, advance and seek an smk
*/

#include "video.h"

#include <stdio.h>
#include <string.h>

const uint16_t smk_video_run_length[64] = {
     1,   2,   3,   4,   5,   6,   7,   8,
     9,  10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,
    25,  26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  47,  48,
    49,  50,  51,  52,  53,  54,  55,  56,
    57,  58,  59, 128, 256, 512, 1024, 2048
};

unsigned smk_video_run_index(unsigned remaining)
{
    unsigned index = 0;
    unsigned i;

    for (i = 0; i < 64; i++) {
        if (smk_video_run_length[i] <= remaining)
            index = i;
        else
            break;
    }
    return index;
}

uint16_t smk_video_pack_type(unsigned type, unsigned run_index, unsigned extra)
{
    return (uint16_t)(type | (run_index << 2) | (extra << 8));
}

int smk_video_emit_mono(const smk_symbol_sink *sink, uint16_t colors,
                        uint16_t map)
{
    if (sink->emit(sink->user, SMK_VIDEO_TREE_MCLR, colors) != 0)
        return -1;
    return sink->emit(sink->user, SMK_VIDEO_TREE_MMAP, map);
}

int smk_video_emit_full(const smk_symbol_sink *sink, const uint8_t *pixels,
                        unsigned stride)
{
    unsigned row;

    for (row = 0; row < 4; row++) {
        uint16_t right = (uint16_t)(pixels[2] | (pixels[3] << 8));
        uint16_t left = (uint16_t)(pixels[0] | (pixels[1] << 8));

        if (sink->emit(sink->user, SMK_VIDEO_TREE_FULL, right) != 0)
            return -1;
        if (sink->emit(sink->user, SMK_VIDEO_TREE_FULL, left) != 0)
            return -1;
        pixels += stride;
    }
    return 0;
}

int smk_video_decode(smk_bitreader *bs, smk_huff16 trees[4], uint8_t *frame,
                     unsigned width, unsigned height, char version)
{
	uint8_t *t = frame;
	unsigned char s1, s2;
	unsigned short temp;
	unsigned long i, j, k, row, col, skip;
	int unpack;
	unsigned char type;
	unsigned char blocklen;
	unsigned char typedata;
	char bit;

	row = 0;
	col = 0;

	/* Reset the cache on all bigtrees */
	for (i = 0; i < 4; i++)
		memset(&trees[i].cache, 0, 3 * sizeof(unsigned short));

	while (row < height) {
		if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_TYPE], bs)) < 0) {
			fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from TYPE tree.\n", stderr);
			return -1;
		}

		type = ((unpack & 0x0003));
		blocklen = ((unpack & 0x00FC) >> 2);
		typedata = ((unpack & 0xFF00) >> 8);

		/* support for v4 full-blocks */
		if (type == 1 && version == '4') {
			bit = smk_bs_read_1(bs);

			if (bit)
				type = 4;
			else {
				bit = smk_bs_read_1(bs);

				if (bit)
					type = 5;
			}
		}

		for (j = 0; (j < smk_video_run_length[blocklen]) && (row < height); j ++) {
			skip = (row * width) + col;

			switch (type) {
			case 0:
				if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_MCLR], bs)) < 0) {
					fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from MCLR tree.\n", stderr);
					return -1;
				}

				s1 = (unpack & 0xFF00) >> 8;
				s2 = (unpack & 0x00FF);

				if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_MMAP], bs)) < 0) {
					fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from MMAP tree.\n", stderr);
					return -1;
				}

				temp = 0x01;

				for (k = 0; k < 4; k ++) {
					for (i = 0; i < 4; i ++) {
						if (unpack & temp)
							t[skip + i] = s1;
						else
							t[skip + i] = s2;

						temp = temp << 1;
					}

					skip += width;
				}

				break;

			case 1: /* FULL BLOCK */
				for (k = 0; k < 4; k ++) {
					if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_FULL], bs)) < 0) {
						fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from FULL tree.\n", stderr);
						return -1;
					}

					t[skip + 3] = ((unpack & 0xFF00) >> 8);
					t[skip + 2] = (unpack & 0x00FF);

					if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_FULL], bs)) < 0) {
						fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from FULL tree.\n", stderr);
						return -1;
					}

					t[skip + 1] = ((unpack & 0xFF00) >> 8);
					t[skip] = (unpack & 0x00FF);
					skip += width;
				}

				break;

			case 2: /* VOID BLOCK */
				/* break;
				if (s->frame)
				{
					memcpy(&t[skip], &s->frame[skip], 4);
					skip += s->w;
					memcpy(&t[skip], &s->frame[skip], 4);
					skip += s->w;
					memcpy(&t[skip], &s->frame[skip], 4);
					skip += s->w;
					memcpy(&t[skip], &s->frame[skip], 4);
				} */
				break;

			case 3: /* SOLID BLOCK */
				memset(&t[skip], typedata, 4);
				skip += width;
				memset(&t[skip], typedata, 4);
				skip += width;
				memset(&t[skip], typedata, 4);
				skip += width;
				memset(&t[skip], typedata, 4);
				break;

			case 4: /* V4 DOUBLE BLOCK */
				for (k = 0; k < 2; k ++) {
					if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_FULL], bs)) < 0) {
						fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from FULL tree.\n", stderr);
						return -1;
					}

					for (i = 0; i < 2; i ++) {
						memset(&t[skip + 2], (unpack & 0xFF00) >> 8, 2);
						memset(&t[skip], (unpack & 0x00FF), 2);
						skip += width;
					}
				}

				break;

			case 5: /* V4 HALF BLOCK */
				for (k = 0; k < 2; k ++) {
					if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_FULL], bs)) < 0) {
						fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from FULL tree.\n", stderr);
						return -1;
					}

					t[skip + 3] = ((unpack & 0xFF00) >> 8);
					t[skip + 2] = (unpack & 0x00FF);
					t[skip + width + 3] = ((unpack & 0xFF00) >> 8);
					t[skip + width + 2] = (unpack & 0x00FF);

					if ((unpack = smk_huff16_lookup(&trees[SMK_VIDEO_TREE_FULL], bs)) < 0) {
						fputs("libsmacker::smk_render_video() - ERROR: failed to lookup from FULL tree.\n", stderr);
						return -1;
					}

					t[skip + 1] = ((unpack & 0xFF00) >> 8);
					t[skip] = (unpack & 0x00FF);
					t[skip + width + 1] = ((unpack & 0xFF00) >> 8);
					t[skip + width] = (unpack & 0x00FF);
					skip += (width << 1);
				}

				break;
			}

			col += 4;

			if (col >= width) {
				col = 0;
				row += 4;
			}
		}
	}

	return 0;
}
