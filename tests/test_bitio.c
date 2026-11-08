#include "test.h"
#include "bitio.h"

static uint32_t read_bits(const uint8_t *data, size_t *pos, unsigned count)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        size_t bit = (*pos)++;
        if ((data[bit >> 3] >> (bit & 7)) & 1u)
            value |= (uint32_t)1u << i;
    }
    return value;
}

void test_bitwriter_lsb_example(void)
{
    static const uint8_t spec[3] = { 0x5c, 0x96, 0xef };
    static const uint8_t expected[3] = { 0x5c, 0x96, 0x03 };
    smk_bitwriter bw;
    size_t pos = 0;

    /* Writing the spec values must produce the spec byte stream prefix. */
    smk_bw_init(&bw);
    SMK_ASSERT_EQ_INT(smk_bw_put_bits(&bw, 0x1c, 5), 0);
    SMK_ASSERT_EQ_INT(smk_bw_put_bits(&bw, 0x32, 6), 0);
    SMK_ASSERT_EQ_INT(smk_bw_put_bits(&bw, 0x72, 7), 0);
    SMK_ASSERT_EQ_INT(smk_bw_byte_count(&bw), 3);
    SMK_ASSERT_MEM_EQ(smk_bw_data(&bw), expected, 3);

    /* Reading 5, 6, 7 bits from the spec bytes yields 1C, 32, 72. */
    SMK_ASSERT_EQ_INT(read_bits(spec, &pos, 5), 0x1c);
    SMK_ASSERT_EQ_INT(read_bits(spec, &pos, 6), 0x32);
    SMK_ASSERT_EQ_INT(read_bits(spec, &pos, 7), 0x72);
    smk_bw_free(&bw);
}

void test_bitwriter_put_bit(void)
{
    smk_bitwriter bw;

    smk_bw_init(&bw);
    SMK_ASSERT_EQ_INT(smk_bw_put_bit(&bw, 1), 0);
    SMK_ASSERT_EQ_INT(smk_bw_put_bit(&bw, 0), 0);
    SMK_ASSERT_EQ_INT(smk_bw_put_bit(&bw, 1), 0);
    SMK_ASSERT_EQ_INT(smk_bw_byte_count(&bw), 1);
    SMK_ASSERT_EQ_INT(smk_bw_data(&bw)[0], 0x05);
    smk_bw_free(&bw);
}

void test_bitwriter_grows(void)
{
    smk_bitwriter bw;
    unsigned i;

    smk_bw_init(&bw);
    for (i = 0; i < 1000; i++)
        SMK_ASSERT_EQ_INT(smk_bw_put_bits(&bw, 0xa5, 8), 0);
    SMK_ASSERT_EQ_INT(smk_bw_byte_count(&bw), 1000);
    for (i = 0; i < 1000; i++)
        SMK_ASSERT_EQ_INT(smk_bw_data(&bw)[i], 0xa5);
    smk_bw_free(&bw);
}
