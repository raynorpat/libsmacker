#ifndef SMK_HUFFMAN_H
#define SMK_HUFFMAN_H

#include "bitio.h"

#include <stdint.h>

#define SMK_HUFF_MAX_SYMBOLS 65536

typedef struct {
    uint32_t freq[SMK_HUFF_MAX_SYMBOLS];
    uint32_t code[SMK_HUFF_MAX_SYMBOLS];
    uint8_t len[SMK_HUFF_MAX_SYMBOLS];
    void *nodes;
    int node_count;
    int root;
    unsigned used;
    int built;
} smk_huff;

void smk_huff_init(smk_huff *h);
void smk_huff_free(smk_huff *h);
void smk_huff_clear(smk_huff *h);
void smk_huff_count(smk_huff *h, uint16_t symbol);
int smk_huff_build(smk_huff *h);
int smk_huff_encode(const smk_huff *h, uint16_t symbol, smk_bitwriter *bw);
unsigned smk_huff_packed_nodes(const smk_huff *h);
int smk_huff_serialize(const smk_huff *h, smk_bitwriter *bw);

#endif
