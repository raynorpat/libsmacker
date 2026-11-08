#ifndef SMK_VIDEO_H
#define SMK_VIDEO_H

#include "format/bitio.h"
#include "format/huffman.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tree order as stored in the file. */
#define SMK_VIDEO_TREE_MMAP 0
#define SMK_VIDEO_TREE_MCLR 1
#define SMK_VIDEO_TREE_FULL 2
#define SMK_VIDEO_TREE_TYPE 3
#define SMK_VIDEO_TREE_COUNT 4

typedef struct {
    unsigned width;
    unsigned height;
    unsigned block_width;
    unsigned block_height;
    uint8_t *previous; /* last encoded frame of palette indices */
    void *blocks;      /* per-block scratch */
} smk_video;

/* Width and height must be nonzero multiples of four. */
int smk_video_init(smk_video *video, unsigned width, unsigned height);

/* Clear the delta reference frame (call between encoding passes). */
void smk_video_reset(smk_video *video);

void smk_video_free(smk_video *video);

/*
 * Encode one frame of palette indices.
 *
 * With count_only set, every symbol is counted into `trees` and no bits are
 * written; `writer` may be NULL. Otherwise the symbols are Huffman-coded to
 * `writer` using trees built from a previous counting pass. Both modes make
 * identical block decisions for identical inputs and reference frames.
 *
 * Returns 0 on success.
 */
int smk_video_encode_frame(smk_video *video, const uint8_t *indices,
                           int keyframe,
                           smk_huff trees[SMK_VIDEO_TREE_COUNT],
                           smk_bitwriter *writer, int count_only);

#ifdef __cplusplus
}
#endif

#endif
