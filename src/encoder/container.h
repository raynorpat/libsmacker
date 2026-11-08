#ifndef SMK_CONTAINER_H
#define SMK_CONTAINER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMK_MAX_AUDIO_TRACKS 7

/* Everything the header needs, plus the pre-serialized Huffman tree blob. */
typedef struct {
    unsigned width;
    unsigned height;
    unsigned frames;   /* logical frame count */
    int ring;          /* append a ring frame */
    unsigned fps_num;
    unsigned fps_den;
    uint32_t audio_size[SMK_MAX_AUDIO_TRACKS];
    uint32_t audio_rate[SMK_MAX_AUDIO_TRACKS];
    const uint8_t *trees; /* HuffmanTrees bitstream */
    size_t trees_size;
    uint32_t tree_sizes[4]; /* allocation size of each tree, or 0 */
} smk_container_info;

typedef struct {
    FILE *file;
    char *path;
    uint32_t *sizes;
    uint8_t *types;
    unsigned physical_frames;
    unsigned next_frame;
    long size_table_offset;
    long type_table_offset;
    int open;
} smk_container;

/*
 * Create the output file and write the header, empty frame tables, and the
 * Huffman trees. Frames are appended with smk_container_add_frame() and the
 * tables are patched in by smk_container_finish().
 */
int smk_container_open(smk_container *container,
                       const char *path,
                       const smk_container_info *info,
                       char *err, size_t errlen);

/*
 * Append one physical frame. `type` is the frame type byte (palette/audio
 * bits) and `payload` is the chunk contents (palette, audio, and video, in
 * that order). The chunk is padded with zeros to a multiple of four.
 */
int smk_container_add_frame(smk_container *container, int keyframe,
                            unsigned type, const uint8_t *payload,
                            size_t size, char *err, size_t errlen);

/* Patch the frame tables and close the file. */
int smk_container_finish(smk_container *container, char *err, size_t errlen);

/* Close and delete a partially written file. Safe to call at any point. */
void smk_container_abort(smk_container *container);

#ifdef __cplusplus
}
#endif

#endif
