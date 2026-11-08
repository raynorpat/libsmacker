#include "test.h"
#include "bitio.h"
#include "huffman.h"

#include <stdlib.h>

typedef struct {
    const uint8_t *data;
    size_t bits;
    size_t pos;
} tbits;

static unsigned tbit(tbits *b)
{
    if (b->pos >= b->bits)
        return 0;
    return (b->data[b->pos >> 3] >> (b->pos++ & 7)) & 1u;
}

static uint32_t tbits_n(tbits *b, unsigned n)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < n; i++)
        value |= (uint32_t)tbit(b) << i;
    return value;
}

typedef struct {
    int left;
    int right;
    uint16_t value;
} tnode;

static int parse8_rec(tbits *b, tnode *nodes, int *count)
{
    unsigned branch = tbit(b);
    int index = (*count)++;

    if (branch) {
        nodes[index].left = parse8_rec(b, nodes, count);
        nodes[index].right = parse8_rec(b, nodes, count);
    } else {
        nodes[index].left = -1;
        nodes[index].right = -1;
        nodes[index].value = (uint16_t)tbits_n(b, 8);
    }
    return index;
}

static int parse8(tbits *b, tnode *nodes, int *count, int *present)
{
    int root;

    *present = (int)tbit(b);
    if (!*present) {
        (void)tbit(b);
        return -1;
    }
    root = parse8_rec(b, nodes, count);
    (void)tbit(b);
    return root;
}

static uint16_t decode8(tbits *b, const tnode *nodes, int root, int present)
{
    int index;

    if (!present)
        return 0;
    index = root;
    while (nodes[index].left >= 0)
        index = tbit(b) ? nodes[index].right : nodes[index].left;
    return nodes[index].value;
}

static int parse16_rec(tbits *b, tnode *nodes, int *count,
                       const tnode *low, int low_root, int low_present,
                       const tnode *high, int high_root, int high_present)
{
    unsigned branch = tbit(b);
    int index = (*count)++;

    if (branch) {
        nodes[index].left = parse16_rec(b, nodes, count, low, low_root,
                                        low_present, high, high_root,
                                        high_present);
        nodes[index].right = parse16_rec(b, nodes, count, low, low_root,
                                         low_present, high, high_root,
                                         high_present);
    } else {
        uint16_t lo = decode8(b, low, low_root, low_present);
        uint16_t hi = decode8(b, high, high_root, high_present);

        nodes[index].left = -1;
        nodes[index].right = -1;
        nodes[index].value = (uint16_t)(lo | (hi << 8));
    }
    return index;
}

static int parse16(tbits *b, tnode *nodes, int *count, uint16_t escapes[3],
                   int *present)
{
    tnode low[511];
    tnode high[511];
    int low_count = 0;
    int high_count = 0;
    int low_root;
    int high_root;
    int low_present;
    int high_present;
    int root;
    int i;

    *present = (int)tbit(b);
    if (!*present) {
        (void)tbit(b);
        return -1;
    }
    low_root = parse8(b, low, &low_count, &low_present);
    high_root = parse8(b, high, &high_count, &high_present);
    for (i = 0; i < 3; i++) {
        uint16_t lo = (uint16_t)tbits_n(b, 8);
        uint16_t hi = (uint16_t)tbits_n(b, 8);
        escapes[i] = (uint16_t)(lo | (hi << 8));
    }
    root = parse16_rec(b, nodes, count, low, low_root, low_present, high,
                       high_root, high_present);
    (void)tbit(b);
    return root;
}

static uint16_t decode16(tbits *b, const tnode *nodes, int root)
{
    int index = root;

    while (nodes[index].left >= 0)
        index = tbit(b) ? nodes[index].right : nodes[index].left;
    return nodes[index].value;
}

void test_huffman_absent(void)
{
    smk_huff h;
    smk_bitwriter bw;
    static const uint8_t expected[1] = { 0 };

    smk_huff_init(&h);
    SMK_ASSERT_EQ_INT(smk_huff_build(&h), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&h), 0);
    smk_bw_init(&bw);
    SMK_ASSERT_EQ_INT(smk_huff_serialize(&h, &bw), 0);
    SMK_ASSERT_EQ_INT(smk_bw_byte_count(&bw), 1);
    SMK_ASSERT_MEM_EQ(smk_bw_data(&bw), expected, 1);
    smk_bw_free(&bw);
    smk_huff_free(&h);
}

void test_huffman_single_symbol(void)
{
    smk_huff h;
    smk_bitwriter tree;
    smk_bitwriter data;
    tnode nodes[8];
    uint16_t escapes[3];
    int present = 0;
    int count = 0;
    int root;
    tbits tb;
    tbits db;

    smk_huff_init(&h);
    smk_huff_count(&h, 0x1234);
    SMK_ASSERT_EQ_INT(smk_huff_build(&h), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&h), 1);
    smk_bw_init(&tree);
    SMK_ASSERT_EQ_INT(smk_huff_serialize(&h, &tree), 0);

    memset(&tb, 0, sizeof(tb));
    tb.data = smk_bw_data(&tree);
    tb.bits = tree.bit_count;
    root = parse16(&tb, nodes, &count, escapes, &present);
    SMK_ASSERT_EQ_INT(present, 1);
    SMK_ASSERT_EQ_INT(count, 1);
    SMK_ASSERT_EQ_INT(nodes[root].value, 0x1234);
    SMK_ASSERT_EQ_INT(tb.pos, tb.bits);

    smk_bw_init(&data);
    SMK_ASSERT_EQ_INT(smk_huff_encode(&h, 0x1234, &data), 0);
    SMK_ASSERT_EQ_INT(data.bit_count, 0);
    memset(&db, 0, sizeof(db));
    db.data = smk_bw_data(&data);
    db.bits = data.bit_count;
    SMK_ASSERT_EQ_INT(decode16(&db, nodes, root), 0x1234);

    smk_bw_free(&data);
    smk_bw_free(&tree);
    smk_huff_free(&h);
}

void test_huffman_roundtrip(void)
{
    static const uint16_t symbols[] = {
        0x0000, 0x0001, 0x0002, 0x00ff, 0x0100, 0x1234, 0xabcc, 0xfffe
    };
    static const uint32_t frequencies[] = { 100, 50, 25, 12, 6, 3, 2, 1 };
    unsigned symbol_count = sizeof(symbols) / sizeof(symbols[0]);
    smk_huff h;
    smk_bitwriter tree;
    smk_bitwriter data;
    tnode nodes[2047];
    uint16_t escapes[3];
    int present = 0;
    int count = 0;
    int root;
    unsigned i;
    unsigned j;
    tbits tb;
    tbits db;

    smk_huff_init(&h);
    for (i = 0; i < symbol_count; i++) {
        for (j = 0; j < frequencies[i]; j++)
            smk_huff_count(&h, symbols[i]);
    }
    SMK_ASSERT_EQ_INT(smk_huff_build(&h), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&h), symbol_count * 2 - 1);

    smk_bw_init(&tree);
    SMK_ASSERT_EQ_INT(smk_huff_serialize(&h, &tree), 0);
    memset(&tb, 0, sizeof(tb));
    tb.data = smk_bw_data(&tree);
    tb.bits = tree.bit_count;
    root = parse16(&tb, nodes, &count, escapes, &present);
    SMK_ASSERT_EQ_INT(present, 1);
    SMK_ASSERT_EQ_INT(count, (int)(symbol_count * 2 - 1));
    SMK_ASSERT_EQ_INT(tb.pos, tb.bits);

    for (i = 0; i < 3; i++) {
        for (j = 0; j < symbol_count; j++)
            SMK_ASSERT(escapes[i] != symbols[j]);
    }

    smk_bw_init(&data);
    for (i = 0; i < symbol_count; i++)
        SMK_ASSERT_EQ_INT(smk_huff_encode(&h, symbols[i], &data), 0);
    memset(&db, 0, sizeof(db));
    db.data = smk_bw_data(&data);
    db.bits = data.bit_count;
    for (i = 0; i < symbol_count; i++)
        SMK_ASSERT_EQ_INT(decode16(&db, nodes, root), symbols[i]);
    SMK_ASSERT_EQ_INT(db.pos, db.bits);

    smk_bw_free(&data);
    smk_bw_free(&tree);
    smk_huff_free(&h);
}
