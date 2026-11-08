#ifndef SMK_BITIO_H
#define SMK_BITIO_H

#include <stddef.h>
#include <stdint.h>

typedef struct smk_bitreader smk_bitreader;
struct smk_bitreader {
    const uint8_t *buffer;
    const uint8_t *end;
    unsigned bit_num;
    size_t bits_read;
};

void smk_bs_init(smk_bitreader *bs, const uint8_t *data, size_t size);
int smk_bs_read_1(smk_bitreader *bs);
int smk_bs_read_8(smk_bitreader *bs);
size_t smk_bs_tell(const smk_bitreader *bs);

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t bit_count;
} smk_bitwriter;

void smk_bw_init(smk_bitwriter *bw);
void smk_bw_free(smk_bitwriter *bw);
int smk_bw_put_bit(smk_bitwriter *bw, unsigned bit);
int smk_bw_put_bits(smk_bitwriter *bw, uint32_t value, unsigned count);
size_t smk_bw_byte_count(const smk_bitwriter *bw);
const uint8_t *smk_bw_data(const smk_bitwriter *bw);

#endif
