#ifndef SMK_FORMAT_VIDEO_H
#define SMK_FORMAT_VIDEO_H

#include "format/bitio.h"
#include "format/huffman.h"

#include <stdint.h>

#define SMK_VIDEO_TREE_MMAP 0
#define SMK_VIDEO_TREE_MCLR 1
#define SMK_VIDEO_TREE_FULL 2
#define SMK_VIDEO_TREE_TYPE 3
#define SMK_VIDEO_TREE_COUNT 4

#define SMK_BLOCK_MONO 0
#define SMK_BLOCK_FULL 1
#define SMK_BLOCK_VOID 2
#define SMK_BLOCK_SOLID 3

extern const uint16_t smk_video_run_length[64];

unsigned smk_video_run_index(unsigned remaining);
uint16_t smk_video_pack_type(unsigned type, unsigned run_index, unsigned extra);

typedef struct {
    int (*emit)(void *user, unsigned tree, uint16_t symbol);
    void *user;
} smk_symbol_sink;

int smk_video_emit_mono(const smk_symbol_sink *sink, uint16_t colors,
                        uint16_t map);
int smk_video_emit_full(const smk_symbol_sink *sink, const uint8_t *pixels,
                        unsigned stride);

/* 0 on success, -1 on a lookup failure. Zeros all four caches first. */
int smk_video_decode(smk_bitreader *bs, smk_huff16 trees[4], uint8_t *frame,
                     unsigned width, unsigned height, char version);

#endif
