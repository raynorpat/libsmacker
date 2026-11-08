#include "video.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t type;
    uint8_t extra;       /* solid color */
    uint16_t mono_colors; /* high byte: color 1, low byte: color 2 */
    uint16_t mono_map;
} block_info;

int smk_video_init(smk_video *video, unsigned width, unsigned height)
{
    if (video == NULL || width == 0 || height == 0 ||
        width % 4 != 0 || height % 4 != 0)
        return -1;

    memset(video, 0, sizeof(*video));
    video->width = width;
    video->height = height;
    video->block_width = width / 4;
    video->block_height = height / 4;
    video->previous = (uint8_t *)calloc((size_t)width * height, 1);
    video->blocks = calloc((size_t)video->block_width * video->block_height,
                           sizeof(block_info));
    if (video->previous == NULL || video->blocks == NULL) {
        smk_video_free(video);
        return -1;
    }
    return 0;
}

void smk_video_reset(smk_video *video)
{
    if (video == NULL || video->previous == NULL)
        return;
    memset(video->previous, 0, (size_t)video->width * video->height);
}

void smk_video_free(smk_video *video)
{
    if (video == NULL)
        return;
    free(video->previous);
    free(video->blocks);
    video->previous = NULL;
    video->blocks = NULL;
}

static void classify_block(const smk_video *video, const uint8_t *indices,
                           unsigned block, int keyframe, block_info *info)
{
    unsigned bx = block % video->block_width;
    unsigned by = block / video->block_width;
    unsigned x0 = bx * 4;
    unsigned y0 = by * 4;
    unsigned row;
    unsigned first;
    unsigned second = 0;
    int have_second = 0;
    int have_third = 0;

    memset(info, 0, sizeof(*info));

    if (!keyframe) {
        int same = 1;

        for (row = 0; row < 4 && same; row++) {
            size_t offset = (size_t)(y0 + row) * video->width + x0;

            if (memcmp(indices + offset, video->previous + offset, 4) != 0)
                same = 0;
        }
        if (same) {
            info->type = SMK_BLOCK_VOID;
            return;
        }
    }

    first = indices[(size_t)y0 * video->width + x0];
    for (row = 0; row < 4 && !have_third; row++) {
        unsigned column;

        for (column = 0; column < 4; column++) {
            unsigned color = indices[(size_t)(y0 + row) * video->width +
                                     x0 + column];

            if (color == first)
                continue;
            if (!have_second) {
                second = color;
                have_second = 1;
            } else if (color != second) {
                have_third = 1;
                break;
            }
        }
    }

    if (!have_second) {
        info->type = SMK_BLOCK_SOLID;
        info->extra = (uint8_t)first;
        return;
    }
    if (have_third) {
        info->type = SMK_BLOCK_FULL;
        return;
    }

    info->type = SMK_BLOCK_MONO;
    info->mono_colors = (uint16_t)((first << 8) | second);
    {
        uint16_t map = 0;
        unsigned pixel;

        for (pixel = 0; pixel < 16; pixel++) {
            unsigned color = indices[(size_t)(y0 + pixel / 4) * video->width +
                                     x0 + pixel % 4];

            if (color == first)
                map |= (uint16_t)(1u << pixel);
        }
        info->mono_map = map;
    }
}

static int emit_symbol(const smk_symbol_sink *sink, unsigned tree,
                       uint16_t symbol)
{
    return sink->emit(sink->user, tree, symbol);
}

/* Emit the per-block pixel payload (mono/full) for one block. */
static int emit_block_pixels(const smk_video *video, const uint8_t *indices,
                             const block_info *info, unsigned block,
                             const smk_symbol_sink *sink)
{
    unsigned bx = block % video->block_width;
    unsigned by = block / video->block_width;
    unsigned x0 = bx * 4;
    unsigned y0 = by * 4;

    if (info->type == SMK_BLOCK_MONO)
        return smk_video_emit_mono(sink, info->mono_colors, info->mono_map);

    if (info->type == SMK_BLOCK_FULL) {
        const uint8_t *pixels = indices + (size_t)y0 * video->width + x0;

        return smk_video_emit_full(sink, pixels, video->width);
    }
    return 0;
}

static int counters_emit(void *user, unsigned tree, uint16_t symbol)
{
    smk_huff *trees = (smk_huff *)user;

    smk_huff_count(&trees[tree], symbol);
    return 0;
}

typedef struct {
    smk_huff *trees;
    smk_bitwriter *writer;
} writer_sink;

static int writer_emit(void *user, unsigned tree, uint16_t symbol)
{
    writer_sink *sink = (writer_sink *)user;

    return smk_huff_encode(&sink->trees[tree], symbol, sink->writer);
}

int smk_video_encode_frame(smk_video *video, const uint8_t *indices,
                           int keyframe,
                           smk_huff trees[SMK_VIDEO_TREE_COUNT],
                           smk_bitwriter *writer, int count_only)
{
    block_info *blocks;
    unsigned total_blocks;
    unsigned block;
    smk_symbol_sink sink;
    writer_sink ws;

    if (video == NULL || indices == NULL || trees == NULL)
        return -1;
    if (!count_only && writer == NULL)
        return -1;

    blocks = (block_info *)video->blocks;
    total_blocks = video->block_width * video->block_height;

    for (block = 0; block < total_blocks; block++)
        classify_block(video, indices, block, keyframe, &blocks[block]);

    if (count_only) {
        sink.emit = counters_emit;
        sink.user = trees;
    } else {
        ws.trees = trees;
        ws.writer = writer;
        sink.emit = writer_emit;
        sink.user = &ws;
    }

    block = 0;
    while (block < total_blocks) {
        block_info *first = &blocks[block];
        unsigned end = block + 1;

        while (end < total_blocks && blocks[end].type == first->type &&
               (first->type != SMK_BLOCK_SOLID ||
                blocks[end].extra == first->extra))
            end++;

        {
            unsigned start = block;
            unsigned remaining = end - block;

            while (remaining > 0) {
                unsigned run_index = smk_video_run_index(remaining);
                unsigned run = smk_video_run_length[run_index];
                uint16_t descriptor = smk_video_pack_type(first->type,
                                                          run_index,
                                                          first->extra);
                unsigned k;

                if (emit_symbol(&sink, SMK_VIDEO_TREE_TYPE, descriptor) != 0)
                    return -1;
                for (k = 0; k < run; k++) {
                    if (emit_block_pixels(video, indices, &blocks[start + k],
                                          start + k, &sink) != 0)
                        return -1;
                }
                start += run;
                remaining -= run;
            }
        }
        block = end;
    }

    memcpy(video->previous, indices,
           (size_t)video->width * video->height);
    return 0;
}
