#include "test.h"
#include "format/palette.h"

#include <string.h>

void test_palette_palmap(void)
{
    SMK_ASSERT_EQ_INT(smk_palmap[0], 0x00);
    SMK_ASSERT_EQ_INT(smk_palmap[1], 0x04);
    SMK_ASSERT_EQ_INT(smk_palmap[16], 0x41);
    SMK_ASSERT_EQ_INT(smk_palmap[63], 0xff);
}

void test_palette_write_one_color(void)
{
    static const uint8_t entries[3] = { 0, 0, 0 };
    static const uint8_t expected[8] = { 0x02, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00 };
    uint8_t out[1020];
    size_t size;

    size = smk_palette_write_chunk(entries, 1, out);
    SMK_ASSERT_EQ_INT(size, 8);
    SMK_ASSERT_MEM_EQ(out, expected, 8);
}

void test_palette_skip_preserves(void)
{
    static const uint8_t payload[] = { 0x01, 0x02, 0x03, 0xff, 0xfe };
    uint8_t palette[768];
    unsigned i;

    memset(palette, 0x11, sizeof(palette));
    SMK_ASSERT_EQ_INT(smk_palette_apply(palette, payload, sizeof(payload)), 0);
    SMK_ASSERT_EQ_INT(palette[0], smk_palmap[1]);
    SMK_ASSERT_EQ_INT(palette[1], smk_palmap[2]);
    SMK_ASSERT_EQ_INT(palette[2], smk_palmap[3]);
    SMK_ASSERT_EQ_INT(palette[3], 0x11);
    for (i = 3; i < 768; i++)
        SMK_ASSERT_EQ_INT(palette[i], 0x11);
}

void test_palette_copy_from_previous(void)
{
    static const uint8_t payload[] = { 0x43, 0x0a, 0xff, 0xfb };
    uint8_t palette[768];
    unsigned i;

    for (i = 0; i < 256; i++) {
        palette[i * 3] = (uint8_t)i;
        palette[i * 3 + 1] = (uint8_t)(i + 1);
        palette[i * 3 + 2] = (uint8_t)(i + 2);
    }
    SMK_ASSERT_EQ_INT(smk_palette_apply(palette, payload, sizeof(payload)), 0);
    SMK_ASSERT_EQ_INT(palette[0], 10);
    SMK_ASSERT_EQ_INT(palette[1], 11);
    SMK_ASSERT_EQ_INT(palette[2], 12);
    SMK_ASSERT_EQ_INT(palette[4 * 3], 4);
    SMK_ASSERT_EQ_INT(palette[4 * 3 + 1], 5);
    SMK_ASSERT_EQ_INT(palette[4 * 3 + 2], 6);
}

void test_palette_rejects_deep_color(void)
{
    static const uint8_t payload[] = { 0x00, 0x40, 0x00 };
    uint8_t palette[768];

    memset(palette, 0, sizeof(palette));
    SMK_ASSERT_EQ_INT(smk_palette_apply(palette, payload, sizeof(payload)), -1);
}
