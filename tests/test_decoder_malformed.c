/*
 * The decoder must reject or survive damaged files. Each test encodes a small
 * valid file, damages one field, and decodes it. Run the suite under
 * AddressSanitizer to catch out-of-bounds access as well as wrong results.
 */
#include "test.h"
#include "test_util.h"

#include "smacker.h"
#include "smk_encoder.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAL_WIDTH 8u
#define MAL_HEIGHT 8u
#define MAL_FRAMES 3u

/* Header field offsets, see docs/smacker.md. */
#define OFF_WIDTH 4
#define OFF_HEIGHT 8
#define OFF_FRAMES 12
#define OFF_AUDIO_SIZE 24
#define OFF_TREES_SIZE 52
#define OFF_TREE_SIZE 56
#define OFF_AUDIO_RATE 72
#define HEADER_SIZE 104

static int mal_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    unsigned i;

    (void)user;
    for (i = 0; i < MAL_WIDTH * MAL_HEIGHT; i++) {
        rgb[i * 3] = (uint8_t)((i * 4 + index * 32) & 0xfc);
        rgb[i * 3 + 1] = (uint8_t)((i / MAL_WIDTH) * 32);
        rgb[i * 3 + 2] = (uint8_t)(index * 64);
    }
    return 0;
}

static size_t mal_read_audio(void *user, unsigned track, size_t start,
                             size_t count, void *dst)
{
    size_t i;

    (void)user;
    (void)track;
    for (i = 0; i < count; i++)
        ((uint8_t *)dst)[i] = (uint8_t)(start + i);
    return count;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* Encode a valid 8x8, three-frame file with one 8-bit mono PCM track. */
static uint8_t *mal_make_file(size_t *size)
{
    smk_enc_source source;
    smk_enc_options options;
    char err[256];
    char *dir = test_tempdir();
    char path[1024];
    void *data = NULL;

    if (dir == NULL)
        return NULL;
    snprintf(path, sizeof(path), "%s/malformed.smk", dir);
    test_tempdir_free(dir);

    memset(&source, 0, sizeof(source));
    source.width = MAL_WIDTH;
    source.height = MAL_HEIGHT;
    source.frame_count = MAL_FRAMES;
    source.fps_num = 10;
    source.fps_den = 1;
    source.audio_track_count = 1;
    source.audio[0].rate = 8000;
    source.audio[0].channels = 1;
    source.audio[0].bits = 8;
    source.read_frame = mal_read_frame;
    source.read_audio = mal_read_audio;
    smk_enc_options_default(&options);

    if (smk_enc_encode(&source, &options, path, err, sizeof(err)) !=
        SMK_ENC_OK)
        return NULL;
    if (test_read_file(path, &data, size) != 0)
        return NULL;
    remove(path);
    return (uint8_t *)data;
}

/* Offset of frame 0's chunk: header, size and type tables, then the trees. */
static size_t mal_frame0(const uint8_t *file)
{
    return HEADER_SIZE + 5u * MAL_FRAMES + get_u32(file + OFF_TREES_SIZE);
}

void test_decoder_truncated_file_fails_open(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    SMK_ASSERT(smk_open_memory(file, 10) == NULL);
    SMK_ASSERT(smk_open_memory(file, HEADER_SIZE + 2) == NULL);
    SMK_ASSERT(smk_open_memory(file, (unsigned long)(size - 1)) == NULL);
    free(file);
}

/* Open a damaged copy of the file and decode every frame. */
static smk mal_open(const uint8_t *file, size_t size)
{
    smk s = smk_open_memory(file, (unsigned long)size);

    if (s != NULL)
        smk_enable_all(s, 0xff);
    return s;
}

void test_decoder_audio_chunk_length_past_frame(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    size_t audio;
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    /* Frame 0 is: palette record, audio record, video. */
    audio = mal_frame0(file) + 4u * file[mal_frame0(file)];
    put_u32(file + audio, 0xfffffff0u);
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT_EQ_INT(smk_first(s), -1);
        smk_close(s);
    }

    put_u32(file + audio, 2); /* shorter than its own length field */
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT_EQ_INT(smk_first(s), -1);
        smk_close(s);
    }
    free(file);
}

void test_decoder_palette_length_past_frame(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    file[mal_frame0(file)] = 0; /* zero-length palette record */
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT_EQ_INT(smk_first(s), -1);
        smk_close(s);
    }

    file[mal_frame0(file)] = 0xff; /* 1020 bytes, more than the frame holds */
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT_EQ_INT(smk_first(s), -1);
        smk_close(s);
    }
    free(file);
}

void test_decoder_pcm_larger_than_declared_buffer(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    /* The header promises 4-byte audio chunks; the frames hold far more. */
    put_u32(file + OFF_AUDIO_SIZE, 4);
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT(smk_first(s) >= 0);
        SMK_ASSERT_EQ_INT(smk_get_audio_size(s, 0), 0);
        smk_close(s);
    }
    free(file);
}

void test_decoder_dpcm_unpacked_size_past_buffer(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    size_t audio;
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    /* Reinterpret the PCM track as DPCM claiming 256 MB of output. */
    put_u32(file + OFF_AUDIO_RATE, get_u32(file + OFF_AUDIO_RATE) | 0x80000000u);
    audio = mal_frame0(file) + 4u * file[mal_frame0(file)];
    put_u32(file + audio + 4, 0x10000000u);
    memset(file + audio + 8, 0x01, 8); /* present bit, then empty trees */
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT(smk_first(s) >= 0);
        SMK_ASSERT_EQ_INT(smk_get_audio_size(s, 0), 0);
        smk_close(s);
    }
    free(file);
}

void test_decoder_dpcm_broken_tree_yields_no_audio(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    size_t audio;
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    put_u32(file + OFF_AUDIO_RATE, get_u32(file + OFF_AUDIO_RATE) | 0x80000000u);
    audio = mal_frame0(file) + 4u * file[mal_frame0(file)];
    put_u32(file + audio + 4, 8);
    /* All ones: a tree of nothing but branches, which can never complete. */
    memset(file + audio + 8, 0xff, 200);
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT(smk_first(s) >= 0);
        SMK_ASSERT_EQ_INT(smk_get_audio_size(s, 0), 0);
        smk_close(s);
    }
    free(file);
}

void test_decoder_dpcm_truncated_data_yields_no_audio(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    size_t audio;
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    put_u32(file + OFF_AUDIO_RATE, get_u32(file + OFF_AUDIO_RATE) | 0x80000000u);
    audio = mal_frame0(file) + 4u * file[mal_frame0(file)];
    /*
     * A record of 4 + 4 + 1 bytes: the sizes, then one byte that says "data
     * present, mono, 8-bit, no tree" and leaves no room for the first sample.
     */
    put_u32(file + audio, 9);
    put_u32(file + audio + 4, 400);
    file[audio + 8] = 0x01;
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        smk_first(s); /* the video that follows is now misplaced */
        SMK_ASSERT_EQ_INT(smk_get_audio_size(s, 0), 0);
        smk_close(s);
    }
    free(file);
}

/* Append the low `count` bits of value, least significant bit first. */
static void put_bits(uint8_t *buffer, size_t *bit, uint32_t value,
                     unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++, (*bit)++) {
        if ((value >> i) & 1u)
            buffer[*bit >> 3] |= (uint8_t)(1u << (*bit & 7));
        else
            buffer[*bit >> 3] &= (uint8_t)~(1u << (*bit & 7));
    }
}

void test_decoder_dpcm_valid_stream_decodes(void)
{
    static const uint8_t expected[5] = { 0x10, 0x11, 0x12, 0x11, 0x12 };
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    size_t audio;
    size_t bit = 0;
    uint8_t *data;
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    put_u32(file + OFF_AUDIO_RATE, get_u32(file + OFF_AUDIO_RATE) | 0x80000000u);
    audio = mal_frame0(file) + 4u * file[mal_frame0(file)];
    put_u32(file + audio + 4, sizeof(expected));
    data = file + audio + 8;
    put_bits(data, &bit, 1, 1);    /* data present */
    put_bits(data, &bit, 0, 1);    /* mono */
    put_bits(data, &bit, 0, 1);    /* 8-bit */
    put_bits(data, &bit, 1, 1);    /* tree present */
    put_bits(data, &bit, 1, 1);    /* branch */
    put_bits(data, &bit, 0, 1);    /* leaf: bit 0 means +1 */
    put_bits(data, &bit, 0x01, 8);
    put_bits(data, &bit, 0, 1);    /* leaf: bit 1 means -1 */
    put_bits(data, &bit, 0xff, 8);
    put_bits(data, &bit, 0, 1);    /* end of tree */
    put_bits(data, &bit, 0x10, 8); /* initial sample */
    put_bits(data, &bit, 0x4, 4);  /* +1, +1, -1, +1 */
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT(smk_first(s) >= 0);
        SMK_ASSERT_EQ_INT(smk_get_audio_size(s, 0), sizeof(expected));
        if (smk_get_audio_size(s, 0) == sizeof(expected))
            SMK_ASSERT_MEM_EQ(smk_get_audio(s, 0), expected, sizeof(expected));
        smk_close(s);
    }
    free(file);
}

void test_decoder_dimensions_not_multiple_of_four(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    uint8_t reference[MAL_WIDTH * MAL_HEIGHT];
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned y;
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s == NULL) {
        free(file);
        return;
    }
    SMK_ASSERT(smk_first(s) >= 0);
    memcpy(reference, smk_get_video(s), sizeof(reference));
    smk_close(s);

    /* Same 2x2 blocks, but only the top-left 6x6 pixels are visible. */
    put_u32(file + OFF_WIDTH, 6);
    put_u32(file + OFF_HEIGHT, 6);
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT(smk_first(s) >= 0);
        smk_info_video(s, &width, &height, NULL);
        SMK_ASSERT_EQ_INT(width, 6);
        SMK_ASSERT_EQ_INT(height, 6);
        for (y = 0; y < 6; y++)
            SMK_ASSERT_MEM_EQ(smk_get_video(s) + y * 6,
                              reference + y * MAL_WIDTH, 6);
        smk_close(s);
    }
    free(file);
}

void test_decoder_deep_tree_fails_open(void)
{
    /* Enough branch bits to overflow the stack if each one recurses. */
    const size_t blob_size = 400000;
    const size_t size = HEADER_SIZE + 4 + 1 + blob_size + 4;
    uint8_t *file = (uint8_t *)calloc(1, size);
    uint8_t *blob;
    size_t bit = 0;
    unsigned i;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    memcpy(file, "SMK2", 4);
    put_u32(file + OFF_WIDTH, 8);
    put_u32(file + OFF_HEIGHT, 8);
    put_u32(file + OFF_FRAMES, 1);
    put_u32(file + OFF_TREES_SIZE, (uint32_t)blob_size);
    put_u32(file + OFF_TREE_SIZE, 12u + 4u * (1u << 24));
    put_u32(file + HEADER_SIZE, 4); /* one 4-byte frame */

    blob = file + HEADER_SIZE + 4 + 1;
    memset(blob, 0xff, blob_size);
    put_bits(blob, &bit, 1, 1); /* tree present */
    for (i = 0; i < 2; i++) {   /* low and high byte trees: one leaf each */
        put_bits(blob, &bit, 1, 1);
        put_bits(blob, &bit, 0, 1);
        put_bits(blob, &bit, 0x55, 8);
        put_bits(blob, &bit, 0, 1);
    }
    put_bits(blob, &bit, 0, 32); /* escape values */
    put_bits(blob, &bit, 0, 16);
    /* Everything after this is 1 bits: branch, branch, branch, ... */

    SMK_ASSERT(smk_open_memory(file, (unsigned long)size) == NULL);
    free(file);
}

void test_decoder_zero_frames_fails_open(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    size_t trees;
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    /*
     * Claim no frames and drop the frame tables, so the trees still follow
     * the header and the file is consistent apart from being empty.
     */
    trees = get_u32(file + OFF_TREES_SIZE);
    put_u32(file + OFF_FRAMES, 0);
    memmove(file + HEADER_SIZE, file + HEADER_SIZE + 5u * MAL_FRAMES, trees);
    s = smk_open_memory(file, (unsigned long)(HEADER_SIZE + trees));
    SMK_ASSERT(s == NULL);
    if (s != NULL)
        smk_close(s);
    free(file);
}

void test_decoder_rejects_out_of_range_arguments(void)
{
    size_t size = 0;
    uint8_t *file = mal_make_file(&size);
    smk s;

    SMK_ASSERT(file != NULL);
    if (file == NULL)
        return;
    s = mal_open(file, size);
    SMK_ASSERT(s != NULL);
    if (s != NULL) {
        SMK_ASSERT(smk_first(s) >= 0);
        SMK_ASSERT_EQ_INT(smk_enable_audio(s, 7, 1), -1);
        SMK_ASSERT(smk_get_audio(s, 7) == NULL);
        SMK_ASSERT_EQ_INT(smk_get_audio_size(s, 7), 0);
        SMK_ASSERT_EQ_INT(smk_seek_keyframe(s, MAL_FRAMES), -1);
        SMK_ASSERT_EQ_INT(smk_seek_keyframe(s, MAL_FRAMES - 1), 0);
        smk_close(s);
    }
    free(file);
}
