#include "test.h"
#include "format/header.h"

#include <string.h>

void test_header_frame_rate(void)
{
    SMK_ASSERT_EQ_INT(smk_header_frame_rate(10, 1), 100);
    SMK_ASSERT_EQ_INT(smk_header_frame_rate(15, 1), -6667);
    SMK_ASSERT_EQ_INT(smk_header_frame_rate(0, 1), 0);
}

void test_header_audio_rate(void)
{
    smk_audio_rate_info info;
    uint32_t word = 0;

    /* 22050 is 0x5622, so present|16-bit|stereo|rate is 0x70005622. */
    SMK_ASSERT_EQ_INT(smk_header_pack_audio_rate(22050, 1, 1, &word), 0);
    SMK_ASSERT_EQ_INT(word, 0x70005622u);

    smk_header_unpack_audio_rate(word, &info);
    SMK_ASSERT_EQ_INT(info.exists, 1);
    SMK_ASSERT_EQ_INT(info.bits16, 1);
    SMK_ASSERT_EQ_INT(info.stereo, 1);
    SMK_ASSERT_EQ_INT(info.rate, 22050);
    SMK_ASSERT_EQ_INT(info.dpcm, 0);
    SMK_ASSERT_EQ_INT(info.bink, 0);

    smk_header_unpack_audio_rate(0xcc002b11u, &info);
    SMK_ASSERT_EQ_INT(info.exists, 1);
    SMK_ASSERT_EQ_INT(info.dpcm, 1);
    SMK_ASSERT_EQ_INT(info.bink, 1);
    SMK_ASSERT_EQ_INT(info.rate, 11025);
    SMK_ASSERT_EQ_INT(info.bits16, 0);
    SMK_ASSERT_EQ_INT(info.stereo, 0);

    smk_header_unpack_audio_rate(0x00002b11u, &info);
    SMK_ASSERT_EQ_INT(info.exists, 0);
    SMK_ASSERT_EQ_INT(info.dpcm, 0);
    SMK_ASSERT_EQ_INT(info.bink, 0);
    SMK_ASSERT_EQ_INT(info.bits16, 0);
    SMK_ASSERT_EQ_INT(info.stereo, 0);
    SMK_ASSERT_EQ_INT(info.rate, 0);

    SMK_ASSERT_EQ_INT(smk_header_pack_audio_rate(0x01000000u, 0, 0, &word), -1);
}

void test_header_frame_size(void)
{
    uint32_t size = 99;
    int keyframe = 99;

    SMK_ASSERT_EQ_INT(smk_header_pack_frame_size(8, 1), 9);
    SMK_ASSERT_EQ_INT(smk_header_pack_frame_size(7, 0), 4);

    smk_header_unpack_frame_size(0x00000002u, &size, &keyframe);
    SMK_ASSERT_EQ_INT(size, 0);
    SMK_ASSERT_EQ_INT(keyframe, 0);

    smk_header_unpack_frame_size(0x00000003u, &size, &keyframe);
    SMK_ASSERT_EQ_INT(size, 0);
    SMK_ASSERT_EQ_INT(keyframe, 1);
}

void test_header_pack_roundtrip(void)
{
    smk_file_header header;
    smk_file_header out;
    uint8_t bytes[SMK_HEADER_SIZE];
    unsigned i;

    memset(&header, 0, sizeof(header));
    memcpy(header.signature, "SMK2", 4);
    header.width = 320;
    header.height = 200;
    header.frames = 3;
    header.frame_rate = -6667;
    header.flags = SMK_HEADER_FLAG_RING | SMK_HEADER_FLAG_Y_DOUBLE |
                   SMK_HEADER_FLAG_Y_INTERLACE;
    header.audio_size[0] = 12;
    header.trees_size = 20;
    header.tree_size[0] = 16;
    header.audio_rate[0] = 0x7000561au;

    SMK_ASSERT_EQ_INT(SMK_HEADER_SIZE, 104);
    SMK_ASSERT_EQ_INT(smk_header_pack(&header, bytes), 0);
    SMK_ASSERT_EQ_INT(bytes[100], 0);
    SMK_ASSERT_EQ_INT(bytes[101], 0);
    SMK_ASSERT_EQ_INT(bytes[102], 0);
    SMK_ASSERT_EQ_INT(bytes[103], 0);

    memset(&out, 0x5a, sizeof(out));
    SMK_ASSERT_EQ_INT(smk_header_unpack(bytes, &out), 0);
    SMK_ASSERT_MEM_EQ(out.signature, header.signature, 4);
    SMK_ASSERT_EQ_INT(out.width, header.width);
    SMK_ASSERT_EQ_INT(out.height, header.height);
    SMK_ASSERT_EQ_INT(out.frames, header.frames);
    SMK_ASSERT_EQ_INT(out.frame_rate, header.frame_rate);
    SMK_ASSERT_EQ_INT(out.flags, header.flags);
    for (i = 0; i < 7; i++)
        SMK_ASSERT_EQ_INT(out.audio_size[i], header.audio_size[i]);
    SMK_ASSERT_EQ_INT(out.trees_size, header.trees_size);
    for (i = 0; i < 4; i++)
        SMK_ASSERT_EQ_INT(out.tree_size[i], header.tree_size[i]);
    for (i = 0; i < 7; i++)
        SMK_ASSERT_EQ_INT(out.audio_rate[i], header.audio_rate[i]);
}
