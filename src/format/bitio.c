/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2021 Greg Kennedy

	See smacker.h for more information.

	smacker.c
		Main implementation file of libsmacker.
		Open, close, query, render, advance and seek an smk
*/

#include "bitio.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void smk_bs_init(smk_bitreader *bs, const uint8_t *data, size_t size)
{
    assert(bs);
    assert(data);
    bs->buffer = data;
    bs->end = data + size;
    bs->bit_num = 0;
    bs->bits_read = 0;
}

int smk_bs_read_1(smk_bitreader *bs)
{
    int ret;

    assert(bs);

    if (bs->buffer >= bs->end) {
        fputs("libsmacker::smk_bs_read_1(): ERROR: bitstream exhausted.\n", stderr);
        return -1;
    }

    ret = (*bs->buffer >> bs->bit_num) & 1;

    if (bs->bit_num >= 7) {
        bs->buffer++;
        bs->bit_num = 0;
    } else {
        bs->bit_num++;
    }

    bs->bits_read++;
    return ret;
}

int smk_bs_read_8(smk_bitreader *bs)
{
    assert(bs);

    if (bs->buffer + (bs->bit_num > 0) >= bs->end) {
        fputs("libsmacker::smk_bs_read_8(): ERROR: bitstream exhausted.\n", stderr);
        return -1;
    }

    bs->bits_read += 8;

    if (bs->bit_num) {
        int ret = *bs->buffer >> bs->bit_num;
        bs->buffer++;
        return ret | (*bs->buffer << (8 - bs->bit_num) & 0xFF);
    }

    return *bs->buffer++;
}

size_t smk_bs_tell(const smk_bitreader *bs)
{
    return bs == NULL ? 0 : bs->bits_read;
}

void smk_bw_init(smk_bitwriter *bw)
{
    if (bw == NULL)
        return;
    bw->data = NULL;
    bw->capacity = 0;
    bw->bit_count = 0;
}

void smk_bw_free(smk_bitwriter *bw)
{
    if (bw == NULL)
        return;
    free(bw->data);
    bw->data = NULL;
    bw->capacity = 0;
    bw->bit_count = 0;
}

static int reserve(smk_bitwriter *bw, size_t bits)
{
    size_t needed = (bits + 7) / 8;
    size_t capacity;
    uint8_t *data;

    if (needed <= bw->capacity)
        return 0;

    capacity = bw->capacity == 0 ? 64 : bw->capacity;
    while (capacity < needed) {
        if (capacity > ((size_t)-1) / 2)
            return -1;
        capacity *= 2;
    }

    data = (uint8_t *)realloc(bw->data, capacity);
    if (data == NULL)
        return -1;
    memset(data + bw->capacity, 0, capacity - bw->capacity);
    bw->data = data;
    bw->capacity = capacity;
    return 0;
}

int smk_bw_put_bits(smk_bitwriter *bw, uint32_t value, unsigned count)
{
    unsigned i;

    if (bw == NULL || count > 32)
        return -1;
    if (reserve(bw, bw->bit_count + count) != 0)
        return -1;

    for (i = 0; i < count; i++) {
        size_t bit = bw->bit_count++;
        if ((value >> i) & 1u)
            bw->data[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    }
    return 0;
}

int smk_bw_put_bit(smk_bitwriter *bw, unsigned bit)
{
    return smk_bw_put_bits(bw, bit & 1u, 1);
}

size_t smk_bw_byte_count(const smk_bitwriter *bw)
{
    return bw == NULL ? 0 : (bw->bit_count + 7) / 8;
}

const uint8_t *smk_bw_data(const smk_bitwriter *bw)
{
    return bw == NULL ? NULL : bw->data;
}
