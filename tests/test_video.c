#include "test.h"
#include "video.h"
#include "format/video.h"

#include <stdlib.h>
#include <string.h>

static void init_trees(smk_huff trees[SMK_VIDEO_TREE_COUNT])
{
    unsigned i;

    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        smk_huff_init(&trees[i]);
}

static void clear_trees(smk_huff trees[SMK_VIDEO_TREE_COUNT])
{
    unsigned i;

    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        smk_huff_clear(&trees[i]);
}

static void free_trees(smk_huff trees[SMK_VIDEO_TREE_COUNT])
{
    unsigned i;

    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        smk_huff_free(&trees[i]);
}

static unsigned total_symbols(const smk_huff *tree)
{
    unsigned total = 0;
    unsigned i;

    for (i = 0; i < SMK_HUFF_MAX_SYMBOLS; i++)
        total += tree->freq[i];
    return total;
}

void test_video_solid_and_keyframe(void)
{
    smk_video video;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    uint8_t frame[16];
    unsigned i;

    SMK_ASSERT_EQ_INT(smk_video_init(&video, 4, 4), 0);
    init_trees(trees);
    memset(frame, 7, sizeof(frame));

    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 1, trees, NULL, 1),
                      0);
    /* type 3, run index 0, extra color 7 */
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_TYPE]), 1);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0703], 1);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_FULL]), 0);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_MCLR]), 0);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_MMAP]), 0);

    /* A keyframe that matches the reference must still paint, never void. */
    clear_trees(trees);
    smk_video_reset(&video);
    for (i = 0; i < sizeof(frame); i++)
        frame[i] = 0;
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 1, trees, NULL, 1),
                      0);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0003], 1);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0002], 0);

    free_trees(trees);
    smk_video_free(&video);
}

void test_video_mono(void)
{
    smk_video video;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    uint8_t frame[16];
    unsigned i;
    uint16_t expected_map = 0;

    for (i = 0; i < 16; i++) {
        frame[i] = (uint8_t)((i + (i / 4)) % 2 == 0 ? 1 : 2);
        if (frame[i] == 1)
            expected_map |= (uint16_t)(1u << i);
    }

    SMK_ASSERT_EQ_INT(smk_video_init(&video, 4, 4), 0);
    init_trees(trees);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 1, trees, NULL, 1),
                      0);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0000], 1);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_MCLR]), 1);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_MCLR].freq[0x0102], 1);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_MMAP]), 1);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_MMAP].freq[expected_map], 1);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_FULL]), 0);

    free_trees(trees);
    smk_video_free(&video);
}

void test_video_full(void)
{
    smk_video video;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    uint8_t frame[16];
    uint16_t right0;
    uint16_t left0;

    /* Every row is a distinct 4-color sequence, forcing a full block. */
    {
        unsigned x;
        unsigned y;

        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++)
                frame[y * 4 + x] = (uint8_t)(y * 4 + x + 1);
        }
    }
    right0 = (uint16_t)(frame[2] | (frame[3] << 8));
    left0 = (uint16_t)(frame[0] | (frame[1] << 8));

    SMK_ASSERT_EQ_INT(smk_video_init(&video, 4, 4), 0);
    init_trees(trees);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 1, trees, NULL, 1),
                      0);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0001], 1);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_FULL]), 8);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_FULL].freq[right0], 1);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_FULL].freq[left0], 1);

    free_trees(trees);
    smk_video_free(&video);
}

void test_video_void_on_delta(void)
{
    smk_video video;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    uint8_t frame[16];

    memset(frame, 5, sizeof(frame));
    SMK_ASSERT_EQ_INT(smk_video_init(&video, 4, 4), 0);
    init_trees(trees);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 1, trees, NULL, 1),
                      0);

    clear_trees(trees);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 0, trees, NULL, 1),
                      0);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0002], 1);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0503], 0);

    /* Changing one pixel makes the block a two-color mono block. */
    clear_trees(trees);
    frame[5] = 6;
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 0, trees, NULL, 1),
                      0);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0000], 1);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0002], 0);

    free_trees(trees);
    smk_video_free(&video);
}

static void check_run_descriptor(unsigned width, uint16_t expected,
                                 unsigned expected_total)
{
    smk_video video;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    uint8_t *frame = (uint8_t *)malloc((size_t)width * 4);

    SMK_ASSERT(frame != NULL);
    if (frame == NULL)
        return;
    memset(frame, 3, (size_t)width * 4);
    SMK_ASSERT_EQ_INT(smk_video_init(&video, width, 4), 0);
    init_trees(trees);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, frame, 1, trees, NULL, 1),
                      0);
    SMK_ASSERT_EQ_INT(trees[SMK_VIDEO_TREE_TYPE].freq[expected], 1);
    SMK_ASSERT_EQ_INT(total_symbols(&trees[SMK_VIDEO_TREE_TYPE]),
                      expected_total);
    free_trees(trees);
    smk_video_free(&video);
    free(frame);
}

void test_video_run_grouping(void)
{
    /* 60 blocks: a 59-run then a 1-run. */
    check_run_descriptor(60 * 4, 0x03eb, 2);
    /* 128 blocks use the 128 entry. */
    check_run_descriptor(128 * 4, 0x03ef, 1);
    /* 2048 blocks use the largest entry. */
    check_run_descriptor(2048 * 4, 0x03ff, 1);
}

static void init_reader(smk_bitreader *bs, const smk_bitwriter *bw)
{
    static const uint8_t empty = 0;
    const uint8_t *data = smk_bw_data(bw);

    if (data == NULL)
        smk_bs_init(bs, &empty, 0);
    else
        smk_bs_init(bs, data, smk_bw_byte_count(bw));
}

/* Serialize each built tree and open a walker. Absent trees use alloc_size 0. */
static int build_walkers(smk_huff trees[SMK_VIDEO_TREE_COUNT],
                         smk_huff16 walkers[SMK_VIDEO_TREE_COUNT])
{
    unsigned i;

    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++) {
        smk_bitwriter bw;
        smk_bitreader bs;
        unsigned packed = smk_huff_packed_nodes(&trees[i]);
        uint32_t alloc = packed == 0 ? 0u : 12u + 4u * packed;

        memset(&walkers[i], 0, sizeof(walkers[i]));
        smk_bw_init(&bw);
        if (smk_huff_serialize(&trees[i], &bw) != 0) {
            smk_bw_free(&bw);
            return -1;
        }
        init_reader(&bs, &bw);
        if (smk_huff16_build(&walkers[i], &bs, alloc) != 1) {
            smk_bw_free(&bw);
            return -1;
        }
        smk_bw_free(&bw);
    }
    return 0;
}

static void free_walkers(smk_huff16 walkers[SMK_VIDEO_TREE_COUNT])
{
    unsigned i;

    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        smk_huff16_free(&walkers[i]);
}

static void make_mixed_frame(uint8_t *frame, unsigned width, unsigned height)
{
    unsigned bx;
    unsigned by;

    memset(frame, 0, (size_t)width * height);
    for (by = 0; by < height / 4; by++) {
        for (bx = 0; bx < width / 4; bx++) {
            unsigned x;
            unsigned y;
            int mode = (int)((bx + by) % 3);

            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    uint8_t value;

                    if (mode == 0)
                        value = (uint8_t)(bx & 0xff);
                    else if (mode == 1)
                        value = (uint8_t)(((x + y) & 1) ? 1 : 2);
                    else
                        value = (uint8_t)(1 + x + y * 4 + bx);
                    frame[(size_t)(by * 4 + y) * width + bx * 4 + x] = value;
                }
            }
        }
    }
}

void test_video_write_roundtrip(void)
{
    unsigned width = 32;
    unsigned height = 16;
    size_t pixels = (size_t)width * height;
    uint8_t *source = (uint8_t *)malloc(pixels);
    uint8_t *decoded = (uint8_t *)malloc(pixels);
    smk_video video;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    smk_bitwriter writer;
    unsigned i;

    SMK_ASSERT(source != NULL && decoded != NULL);
    if (source == NULL || decoded == NULL) {
        free(source);
        free(decoded);
        return;
    }
    make_mixed_frame(source, width, height);

    SMK_ASSERT_EQ_INT(smk_video_init(&video, width, height), 0);
    init_trees(trees);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, source, 1, trees, NULL, 1),
                      0);
    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        SMK_ASSERT_EQ_INT(smk_huff_build(&trees[i]), 0);
    SMK_ASSERT(total_symbols(&trees[SMK_VIDEO_TREE_MCLR]) > 0);
    SMK_ASSERT(total_symbols(&trees[SMK_VIDEO_TREE_MMAP]) > 0);
    SMK_ASSERT(total_symbols(&trees[SMK_VIDEO_TREE_FULL]) > 0);

    smk_bw_init(&writer);
    smk_video_reset(&video);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, source, 1, trees, &writer,
                                             0),
                      0);
    SMK_ASSERT(writer.bit_count > 0);
    memset(decoded, 0, pixels);
    {
        smk_huff16 walkers[SMK_VIDEO_TREE_COUNT];
        smk_bitreader bs;

        SMK_ASSERT_EQ_INT(build_walkers(trees, walkers), 0);
        init_reader(&bs, &writer);
        SMK_ASSERT_EQ_INT(smk_video_decode(&bs, walkers, decoded, width, height,
                                           '2'),
                          0);
        SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), (long long)writer.bit_count);
        free_walkers(walkers);
    }
    SMK_ASSERT_MEM_EQ(decoded, source, pixels);

    /* A repeat write pass must produce byte-identical output. */
    {
        smk_bitwriter second;
        unsigned long long first_bits = writer.bit_count;
        size_t first_bytes = smk_bw_byte_count(&writer);
        uint8_t *first_copy = (uint8_t *)malloc(first_bytes);
        size_t second_bytes;

        SMK_ASSERT(first_copy != NULL);
        if (first_copy != NULL) {
            memcpy(first_copy, smk_bw_data(&writer), first_bytes);
            smk_bw_init(&second);
            smk_video_reset(&video);
            SMK_ASSERT_EQ_INT(
                smk_video_encode_frame(&video, source, 1, trees, &second, 0), 0);
            SMK_ASSERT_EQ_INT(second.bit_count, first_bits);
            second_bytes = smk_bw_byte_count(&second);
            SMK_ASSERT_EQ_INT(second_bytes, first_bytes);
            SMK_ASSERT_MEM_EQ(smk_bw_data(&second), first_copy, first_bytes);
            free(first_copy);
            smk_bw_free(&second);
        }
    }

    smk_bw_free(&writer);
    free_trees(trees);
    smk_video_free(&video);
    free(source);
    free(decoded);
}

void test_video_delta_roundtrip(void)
{
    unsigned width = 16;
    unsigned height = 8;
    size_t pixels = (size_t)width * height;
    uint8_t *first = (uint8_t *)malloc(pixels);
    uint8_t *second = (uint8_t *)malloc(pixels);
    uint8_t *decoded = (uint8_t *)malloc(pixels);
    smk_video video;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    smk_bitwriter key_writer;
    smk_bitwriter writer;

    SMK_ASSERT(first != NULL && second != NULL && decoded != NULL);
    if (first == NULL || second == NULL || decoded == NULL) {
        free(first);
        free(second);
        free(decoded);
        return;
    }
    make_mixed_frame(first, width, height);
    memcpy(second, first, pixels);
    second[3] = (uint8_t)(second[3] + 1);
    second[10] = (uint8_t)(second[10] + 1);

    SMK_ASSERT_EQ_INT(smk_video_init(&video, width, height), 0);
    init_trees(trees);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, first, 1, trees, NULL, 1),
                      0);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, second, 0, trees, NULL, 1),
                      0);
    {
        unsigned i;

        for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
            SMK_ASSERT_EQ_INT(smk_huff_build(&trees[i]), 0);
    }
    SMK_ASSERT(trees[SMK_VIDEO_TREE_TYPE].freq[0x0002] > 0);

    /* Keyframe alone, to learn its exact bit length. */
    smk_bw_init(&key_writer);
    smk_video_reset(&video);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, first, 1, trees,
                                             &key_writer, 0),
                      0);

    /* Keyframe followed by the delta frame in one stream. */
    smk_bw_init(&writer);
    smk_video_reset(&video);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, first, 1, trees, &writer,
                                             0),
                      0);
    SMK_ASSERT_EQ_INT(writer.bit_count, key_writer.bit_count);
    SMK_ASSERT_EQ_INT(smk_video_encode_frame(&video, second, 0, trees, &writer,
                                             0),
                      0);

    memset(decoded, 0, pixels);
    {
        smk_huff16 walkers[SMK_VIDEO_TREE_COUNT];
        smk_bitreader bs;

        SMK_ASSERT_EQ_INT(build_walkers(trees, walkers), 0);
        init_reader(&bs, &writer);
        SMK_ASSERT_EQ_INT(smk_video_decode(&bs, walkers, decoded, width, height,
                                           '2'),
                          0);
        SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), (long long)key_writer.bit_count);
        SMK_ASSERT_MEM_EQ(decoded, first, pixels);
        SMK_ASSERT_EQ_INT(smk_video_decode(&bs, walkers, decoded, width, height,
                                           '2'),
                          0);
        SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), (long long)writer.bit_count);
        SMK_ASSERT_MEM_EQ(decoded, second, pixels);
        free_walkers(walkers);
    }

    smk_bw_free(&key_writer);
    smk_bw_free(&writer);
    free_trees(trees);
    smk_video_free(&video);
    free(first);
    free(second);
    free(decoded);
}

static void decode_smk4(smk_huff trees[SMK_VIDEO_TREE_COUNT],
                        smk_huff16 walkers[SMK_VIDEO_TREE_COUNT],
                        const unsigned *raw, unsigned raw_count,
                        const uint16_t *symbols, unsigned symbol_count,
                        const uint8_t expect[16])
{
    smk_bitwriter bw;
    smk_bitreader bs;
    uint8_t frame[16];
    unsigned i;

    smk_bw_init(&bw);
    SMK_ASSERT_EQ_INT(smk_huff_encode(&trees[SMK_VIDEO_TREE_TYPE], 1, &bw), 0);
    for (i = 0; i < raw_count; i++)
        SMK_ASSERT_EQ_INT(smk_bw_put_bit(&bw, raw[i]), 0);
    for (i = 0; i < symbol_count; i++) {
        SMK_ASSERT_EQ_INT(
            smk_huff_encode(&trees[SMK_VIDEO_TREE_FULL], symbols[i], &bw), 0);
    }

    memset(frame, 0, sizeof(frame));
    init_reader(&bs, &bw);
    SMK_ASSERT_EQ_INT(smk_video_decode(&bs, walkers, frame, 4, 4, '4'), 0);
    SMK_ASSERT_MEM_EQ(frame, expect, 16);
    smk_bw_free(&bw);
}

void test_video_smk4_blocks(void)
{
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    smk_huff16 walkers[SMK_VIDEO_TREE_COUNT];
    unsigned i;
    static const unsigned double_raw[] = { 1 };
    static const uint16_t double_symbols[] = { 0x0301, 0x0402 };
    static const uint8_t double_expect[16] = {
        0x01, 0x01, 0x03, 0x03,
        0x01, 0x01, 0x03, 0x03,
        0x02, 0x02, 0x04, 0x04,
        0x02, 0x02, 0x04, 0x04
    };
    static const unsigned half_raw[] = { 0, 1 };
    static const uint16_t half_symbols[] = { 0x0301, 0x0402, 0x0301, 0x0402 };
    static const uint8_t half_expect[16] = {
        0x02, 0x04, 0x01, 0x03,
        0x02, 0x04, 0x01, 0x03,
        0x02, 0x04, 0x01, 0x03,
        0x02, 0x04, 0x01, 0x03
    };

    init_trees(trees);
    smk_huff_count(&trees[SMK_VIDEO_TREE_TYPE], 1);
    smk_huff_count(&trees[SMK_VIDEO_TREE_FULL], 0x0301);
    smk_huff_count(&trees[SMK_VIDEO_TREE_FULL], 0x0402);
    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        SMK_ASSERT_EQ_INT(smk_huff_build(&trees[i]), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&trees[SMK_VIDEO_TREE_MMAP]), 0);
    SMK_ASSERT_EQ_INT(smk_huff_packed_nodes(&trees[SMK_VIDEO_TREE_MCLR]), 0);
    SMK_ASSERT_EQ_INT(build_walkers(trees, walkers), 0);

    decode_smk4(trees, walkers, double_raw, 1, double_symbols, 2,
                double_expect);
    decode_smk4(trees, walkers, half_raw, 2, half_symbols, 4, half_expect);

    free_walkers(walkers);
    free_trees(trees);
}
