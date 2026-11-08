#include "test.h"
#include "format/bitio.h"
#include "format/huffman.h"

#include <string.h>

static void init_reader(smk_bitreader *bs, const smk_bitwriter *bw)
{
    static const uint8_t empty = 0;
    const uint8_t *data = smk_bw_data(bw);

    if (data == NULL)
        smk_bs_init(bs, &empty, 0);
    else
        smk_bs_init(bs, data, smk_bw_byte_count(bw));
}

void test_huffman_absent(void)
{
    smk_huff h;
    smk_bitwriter bw;
    smk_bitreader bs;
    smk_huff16 walker;
    static const uint8_t expected[1] = { 0 };

    smk_huff_init(&h);
    SMK_ASSERT_EQ_INT(smk_huff_build(&h), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&h), 0);
    smk_bw_init(&bw);
    SMK_ASSERT_EQ_INT(smk_huff_serialize(&h, &bw), 0);
    SMK_ASSERT_EQ_INT(smk_bw_byte_count(&bw), 1);
    SMK_ASSERT_MEM_EQ(smk_bw_data(&bw), expected, 1);

    memset(&walker, 0, sizeof(walker));
    init_reader(&bs, &bw);
    SMK_ASSERT_EQ_INT(smk_huff16_build(&walker, &bs, 0), 1);
    SMK_ASSERT_EQ_INT(smk_huff16_lookup(&walker, &bs), 0);
    SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), (long long)bw.bit_count);
    smk_huff16_free(&walker);
    smk_bw_free(&bw);
    smk_huff_free(&h);
}

void test_huffman_single_symbol(void)
{
    smk_huff h;
    smk_bitwriter tree;
    smk_bitwriter data;
    smk_bitreader bs;
    smk_huff16 walker;
    int i;

    smk_huff_init(&h);
    smk_huff_count(&h, 0x1234);
    SMK_ASSERT_EQ_INT(smk_huff_build(&h), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&h), 1);
    smk_bw_init(&tree);
    SMK_ASSERT_EQ_INT(smk_huff_serialize(&h, &tree), 0);

    memset(&walker, 0, sizeof(walker));
    init_reader(&bs, &tree);
    SMK_ASSERT_EQ_INT(smk_huff16_build(&walker, &bs,
                                       12u + 4u * smk_huff_packed_nodes(&h)),
                      1);
    SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), (long long)tree.bit_count);
    for (i = 0; i < 3; i++)
        SMK_ASSERT(walker.cache[i] != 0x1234);

    smk_bw_init(&data);
    SMK_ASSERT_EQ_INT(smk_huff_encode(&h, 0x1234, &data), 0);
    SMK_ASSERT_EQ_INT(data.bit_count, 0);
    init_reader(&bs, &data);
    SMK_ASSERT_EQ_INT(smk_huff16_lookup(&walker, &bs), 0x1234);

    smk_huff16_free(&walker);
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
    smk_bitreader bs;
    smk_huff16 walker;
    unsigned i;
    unsigned j;

    smk_huff_init(&h);
    for (i = 0; i < symbol_count; i++) {
        for (j = 0; j < frequencies[i]; j++)
            smk_huff_count(&h, symbols[i]);
    }
    SMK_ASSERT_EQ_INT(smk_huff_build(&h), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&h), symbol_count * 2 - 1);

    smk_bw_init(&tree);
    SMK_ASSERT_EQ_INT(smk_huff_serialize(&h, &tree), 0);
    memset(&walker, 0, sizeof(walker));
    init_reader(&bs, &tree);
    SMK_ASSERT_EQ_INT(smk_huff16_build(&walker, &bs,
                                       12u + 4u * smk_huff_packed_nodes(&h)),
                      1);
    SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), (long long)tree.bit_count);

    for (i = 0; i < 3; i++) {
        for (j = 0; j < symbol_count; j++)
            SMK_ASSERT(walker.cache[i] != symbols[j]);
    }

    smk_bw_init(&data);
    for (i = 0; i < symbol_count; i++)
        SMK_ASSERT_EQ_INT(smk_huff_encode(&h, symbols[i], &data), 0);
    init_reader(&bs, &data);
    for (i = 0; i < symbol_count; i++)
        SMK_ASSERT_EQ_INT(smk_huff16_lookup(&walker, &bs), symbols[i]);
    SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), (long long)data.bit_count);

    smk_huff16_free(&walker);
    smk_bw_free(&data);
    smk_bw_free(&tree);
    smk_huff_free(&h);
}

void test_huffman_cache_lookup(void)
{
    static const uint16_t sequence[] = { 1, 2, 1 };
    smk_huff h;
    smk_bitwriter tree;
    smk_bitwriter data;
    smk_bitreader bs;
    smk_huff16 walker;
    unsigned i;

    smk_huff_init(&h);
    smk_huff_count(&h, 1);
    smk_huff_count(&h, 2);
    smk_huff_count(&h, 3);
    SMK_ASSERT_EQ_INT(smk_huff_build(&h), 0);

    smk_bw_init(&tree);
    SMK_ASSERT_EQ_INT(smk_huff_serialize(&h, &tree), 0);
    memset(&walker, 0, sizeof(walker));
    init_reader(&bs, &tree);
    SMK_ASSERT_EQ_INT(smk_huff16_build(&walker, &bs,
                                       12u + 4u * smk_huff_packed_nodes(&h)),
                      1);

    smk_bw_init(&data);
    for (i = 0; i < 3; i++)
        SMK_ASSERT_EQ_INT(smk_huff_encode(&h, sequence[i], &data), 0);
    init_reader(&bs, &data);
    for (i = 0; i < 3; i++)
        SMK_ASSERT_EQ_INT(smk_huff16_lookup(&walker, &bs), sequence[i]);

    smk_huff16_free(&walker);
    smk_bw_free(&data);
    smk_bw_free(&tree);
    smk_huff_free(&h);
}
