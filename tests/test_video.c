#include "test.h"
#include "video.h"

#include <stdlib.h>
#include <string.h>

static const unsigned run_table[64] = {
     1,   2,   3,   4,   5,   6,   7,   8,
     9,  10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,
    25,  26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  47,  48,
    49,  50,  51,  52,  53,  54,  55,  56,
    57,  58,  59, 128, 256, 512, 1024, 2048
};

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

typedef struct {
    const uint8_t *data;
    size_t bit;
} video_reader;

static unsigned read_one_bit(video_reader *reader)
{
    unsigned bit = (reader->data[reader->bit >> 3] >>
                    (reader->bit & 7)) & 1u;

    reader->bit++;
    return bit;
}

static int read_symbol(video_reader *reader, const smk_huff *tree)
{
    /* Tests are single-threaded; avoid 128 KB on every call stack. */
    static uint16_t used[SMK_HUFF_MAX_SYMBOLS];
    unsigned used_count = 0;
    unsigned i;
    uint32_t value = 0;
    unsigned length;

    for (i = 0; i < SMK_HUFF_MAX_SYMBOLS; i++) {
        if (tree->freq[i] != 0)
            used[used_count++] = (uint16_t)i;
    }

    /* A single-symbol tree reads with no bits, as libsmacker does. */
    if (used_count == 1 && tree->len[used[0]] == 0)
        return (int)used[0];

    for (length = 1; length <= 31; length++) {
        value = (value << 1) | read_one_bit(reader);
        for (i = 0; i < used_count; i++) {
            uint16_t symbol = used[i];

            if (tree->len[symbol] == length && tree->code[symbol] == value)
                return (int)symbol;
        }
    }
    return -1;
}

/*
 * Decode one frame starting at start_bit, exactly as the documented codec
 * does. `frame` must hold the reference state (zeros for a keyframe).
 */
static int decode_frame(const smk_huff trees[SMK_VIDEO_TREE_COUNT],
                        const uint8_t *data, size_t end_bit, size_t start_bit,
                        unsigned width, unsigned height, uint8_t *frame)
{
    video_reader reader;
    unsigned block_width = width / 4;
    unsigned block_height = height / 4;
    unsigned total = block_width * block_height;
    unsigned block = 0;

    reader.data = data;
    reader.bit = start_bit;

    while (block < total) {
        int symbol = read_symbol(&reader, &trees[SMK_VIDEO_TREE_TYPE]);
        unsigned type;
        unsigned run;
        unsigned extra;
        unsigned k;

        if (symbol < 0)
            return -1;
        type = (unsigned)symbol & 3u;
        run = run_table[((unsigned)symbol >> 2) & 0x3fu];
        extra = ((unsigned)symbol >> 8) & 0xffu;

        for (k = 0; k < run && block < total; k++, block++) {
            unsigned bx = block % block_width;
            unsigned by = block / block_width;
            unsigned x0 = bx * 4;
            unsigned y0 = by * 4;

            if (type == 3) {
                unsigned row;

                for (row = 0; row < 4; row++)
                    memset(frame + (size_t)(y0 + row) * width + x0, (int)extra,
                           4);
            } else if (type == 0) {
                int colors = read_symbol(&reader, &trees[SMK_VIDEO_TREE_MCLR]);
                int map = read_symbol(&reader, &trees[SMK_VIDEO_TREE_MMAP]);
                unsigned pixel;

                if (colors < 0 || map < 0)
                    return -1;
                for (pixel = 0; pixel < 16; pixel++) {
                    unsigned color = ((unsigned)map >> pixel) & 1u
                        ? (unsigned)colors >> 8 : (unsigned)colors & 0xffu;

                    frame[(size_t)(y0 + pixel / 4) * width + x0 + pixel % 4] =
                        (uint8_t)color;
                }
            } else if (type == 1) {
                unsigned row;

                for (row = 0; row < 4; row++) {
                    int right = read_symbol(&reader, &trees[SMK_VIDEO_TREE_FULL]);
                    int left = read_symbol(&reader, &trees[SMK_VIDEO_TREE_FULL]);
                    uint8_t *row_data = frame + (size_t)(y0 + row) * width + x0;

                    if (right < 0 || left < 0)
                        return -1;
                    row_data[0] = (uint8_t)((unsigned)left & 0xffu);
                    row_data[1] = (uint8_t)(((unsigned)left >> 8) & 0xffu);
                    row_data[2] = (uint8_t)((unsigned)right & 0xffu);
                    row_data[3] = (uint8_t)(((unsigned)right >> 8) & 0xffu);
                }
            }
        }
    }

    return reader.bit == end_bit ? 0 : -1;
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
    SMK_ASSERT_EQ_INT(decode_frame(trees, smk_bw_data(&writer),
                                   writer.bit_count, 0, width, height, decoded),
                      0);
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
    SMK_ASSERT_EQ_INT(decode_frame(trees, smk_bw_data(&writer),
                                   key_writer.bit_count, 0, width, height,
                                   decoded),
                      0);
    SMK_ASSERT_MEM_EQ(decoded, first, pixels);
    SMK_ASSERT_EQ_INT(decode_frame(trees, smk_bw_data(&writer),
                                   writer.bit_count, key_writer.bit_count,
                                   width, height, decoded),
                      0);
    SMK_ASSERT_MEM_EQ(decoded, second, pixels);

    smk_bw_free(&key_writer);
    smk_bw_free(&writer);
    free_trees(trees);
    smk_video_free(&video);
    free(first);
    free(second);
    free(decoded);
}
