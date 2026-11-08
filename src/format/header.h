#ifndef SMK_FORMAT_HEADER_H
#define SMK_FORMAT_HEADER_H

#include <stdint.h>

#define SMK_HEADER_SIZE 104
#define SMK_HEADER_FLAG_RING 0x01u
#define SMK_HEADER_FLAG_Y_DOUBLE 0x02u
#define SMK_HEADER_FLAG_Y_INTERLACE 0x04u
#define SMK_FRAME_HAS_PALETTE 0x01u
#define SMK_FRAME_AUDIO_TRACK(n) ((uint8_t)(0x02u << (n)))

typedef struct {
    char signature[4];
    uint32_t width;
    uint32_t height;
    uint32_t frames;
    int32_t frame_rate;
    uint32_t flags;
    uint32_t audio_size[7];
    uint32_t trees_size;
    uint32_t tree_size[4];
    uint32_t audio_rate[7];
} smk_file_header;

typedef struct {
    uint32_t rate;
    int exists;
    int dpcm;
    int bink;
    int bits16;
    int stereo;
} smk_audio_rate_info;

int smk_header_pack(const smk_file_header *header, uint8_t out[SMK_HEADER_SIZE]);
int smk_header_unpack(const uint8_t in[SMK_HEADER_SIZE], smk_file_header *header);
int32_t smk_header_frame_rate(unsigned fps_num, unsigned fps_den);
int smk_header_pack_audio_rate(uint32_t rate, int bits16, int stereo,
                               uint32_t *out);
void smk_header_unpack_audio_rate(uint32_t word, smk_audio_rate_info *out);
uint32_t smk_header_pack_frame_size(uint32_t padded_size, int keyframe);
void smk_header_unpack_frame_size(uint32_t word, uint32_t *size, int *keyframe);
uint32_t smk_header_read_u32(const uint8_t bytes[4]);
void smk_header_write_u32(uint8_t bytes[4], uint32_t value);

#endif
