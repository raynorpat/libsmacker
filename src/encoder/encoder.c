#include "smk_encoder.h"

#include "container.h"
#include "huffman.h"
#include "quantize.h"
#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A set block is three bytes (the first also carries the 00 control bits), so
 * a full 256-entry palette plus the length byte fits comfortably.
 */
#define SMK_PALETTE_CHUNK_MAX 1020u

static smk_enc_status fail(char *err, size_t errlen, smk_enc_status status,
                           const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return status;
}

static int keyframe_at(unsigned index, unsigned interval)
{
    if (index == 0)
        return 1;
    if (interval == 0)
        return 0;
    return index % interval == 0;
}

/*
 * Encode the palette as one chunk: three-byte set blocks for the entries we
 * have and skip blocks for everything else. Returns the chunk size in bytes.
 */
static size_t build_palette_chunk(const smk_palette *palette, uint8_t *out)
{
    size_t pos = 1;
    unsigned i = 0;

    while (i < 256) {
        if (i < palette->count) {
            out[pos++] = palette->entry[i].r;
            out[pos++] = palette->entry[i].g;
            out[pos++] = palette->entry[i].b;
            i++;
        } else {
            unsigned run = 0;

            while (i + run < 256 && run < 128 &&
                   !(i + run < palette->count))
                run++;
            if (run == 0)
                run = 1;
            out[pos++] = (uint8_t)(0x80u | (run - 1));
            i += run;
        }
    }

    while (pos % 4 != 0)
        out[pos++] = 0;
    out[0] = (uint8_t)(pos / 4);
    return pos;
}

static unsigned clamp_colors(unsigned colors)
{
    if (colors < 2)
        return 2;
    if (colors > 256)
        return 256;
    return colors;
}

void smk_enc_options_default(smk_enc_options *options)
{
    if (options == NULL)
        return;

    memset(options, 0, sizeof(*options));
    options->palette = SMK_ENC_PALETTE_ADAPTIVE;
    options->dither = SMK_ENC_DITHER_NONE;
    options->colors = 256;
    options->keyframe_interval = 150;
    options->fps_num = 15;
    options->fps_den = 1;
    options->background[0] = 0;
    options->background[1] = 0;
    options->background[2] = 0;
}

smk_enc_status smk_enc_encode(const smk_enc_source *source,
                              const smk_enc_options *options,
                              const char *output_path,
                              char *err, size_t errlen)
{
    size_t pixels;
    uint8_t *rgb = NULL;
    uint8_t *indices = NULL;
    smk_histogram *hist = NULL;
    smk_palette palette;
    smk_quant_lut *lut = NULL;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    smk_video video;
    smk_bitwriter tree_blob;
    smk_bitwriter frame_bits;
    smk_container container;
    smk_container_info info;
    uint8_t palette_chunk[SMK_PALETTE_CHUNK_MAX];
    unsigned colors;
    unsigned fps_num;
    unsigned fps_den;
    unsigned interval;
    unsigned i;
    size_t palette_size;
    int have_video = 0;
    int have_tree_blob = 0;
    int have_container = 0;
    smk_enc_status status = SMK_ENC_ERR_ENCODE;

    if (source == NULL || options == NULL ||
        output_path == NULL || output_path[0] == '\0')
        return fail(err, errlen, SMK_ENC_ERR_USAGE,
                    "missing source or output path");
    if (source->read_frame == NULL || source->frame_count == 0 ||
        source->width == 0 || source->height == 0)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "source has no frames or no dimensions");
    if (source->width % 4 != 0 || source->height % 4 != 0)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "frame dimensions must be multiples of 4");

    pixels = (size_t)source->width * source->height;
    colors = clamp_colors(options->colors);
    fps_num = source->fps_num != 0 ? source->fps_num : options->fps_num;
    fps_den = source->fps_den != 0 ? source->fps_den : options->fps_den;
    if (fps_num == 0)
        fps_num = 15;
    if (fps_den == 0)
        fps_den = 1;
    interval = options->keyframe_interval;

    memset(&video, 0, sizeof(video));
    memset(&container, 0, sizeof(container));
    memset(&tree_blob, 0, sizeof(tree_blob));
    memset(&frame_bits, 0, sizeof(frame_bits));
    memset(&palette, 0, sizeof(palette));
    memset(&info, 0, sizeof(info));
    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        smk_huff_init(&trees[i]);

    rgb = (uint8_t *)malloc(pixels * 3);
    indices = (uint8_t *)malloc(pixels);
    hist = (smk_histogram *)malloc(sizeof(*hist));
    lut = (smk_quant_lut *)malloc(sizeof(*lut));
    if (rgb == NULL || indices == NULL || hist == NULL || lut == NULL) {
        status = fail(err, errlen, SMK_ENC_ERR_ENCODE, "out of memory");
        goto cleanup;
    }

    /* Pass 1: build a global palette from every frame. */
    smk_histogram_init(hist);
    for (i = 0; i < source->frame_count; i++) {
        if (source->read_frame(source->user, i, rgb) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot read frame %u", i);
            status = SMK_ENC_ERR_INPUT;
            goto cleanup;
        }
        smk_histogram_add(hist, rgb, pixels);
    }
    if (smk_quant_build_global(hist, colors, &palette) == 0 ||
        palette.count == 0) {
        status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                      "could not build a palette");
        goto cleanup;
    }
    smk_quant_lut_init(lut);

    if (smk_video_init(&video, source->width, source->height) != 0) {
        status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                      "out of memory creating video encoder");
        goto cleanup;
    }
    have_video = 1;

    /* Pass 2: count every symbol the write pass will produce. */
    smk_video_reset(&video);
    for (i = 0; i < source->frame_count; i++) {
        if (source->read_frame(source->user, i, rgb) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot read frame %u", i);
            status = SMK_ENC_ERR_INPUT;
            goto cleanup;
        }
        smk_quant_map(&palette, rgb, source->width, source->height,
                      options->dither, indices, lut);
        if (smk_video_encode_frame(&video, indices,
                                   keyframe_at(i, interval), trees, NULL, 1) !=
            0) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "failed to count video symbols");
            goto cleanup;
        }
    }

    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++) {
        if (smk_huff_build(&trees[i]) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "failed to build Huffman trees");
            goto cleanup;
        }
    }

    smk_bw_init(&tree_blob);
    have_tree_blob = 1;
    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++) {
        unsigned nodes = smk_huff_packed_nodes(&trees[i]);

        info.tree_sizes[i] = nodes != 0 ? 12u + nodes * 4u : 0u;
        if (smk_huff_serialize(&trees[i], &tree_blob) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "failed to serialize Huffman trees");
            goto cleanup;
        }
    }
    info.trees = smk_bw_data(&tree_blob);
    info.trees_size = smk_bw_byte_count(&tree_blob);

    info.width = source->width;
    info.height = source->height;
    info.frames = source->frame_count;
    info.ring = options->ring ? 1 : 0;
    info.fps_num = fps_num;
    info.fps_den = fps_den;

    if (smk_container_open(&container, output_path, &info, err, errlen) != 0) {
        status = SMK_ENC_ERR_ENCODE;
        goto cleanup;
    }
    have_container = 1;

    palette_size = build_palette_chunk(&palette, palette_chunk);

    /* Pass 3: write the frames. */
    smk_video_reset(&video);
    for (i = 0; i < source->frame_count; i++) {
        size_t video_size;
        size_t payload_size;
        uint8_t *payload;
        unsigned type;

        if (source->read_frame(source->user, i, rgb) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot read frame %u", i);
            status = SMK_ENC_ERR_INPUT;
            goto cleanup;
        }
        smk_quant_map(&palette, rgb, source->width, source->height,
                      options->dither, indices, lut);

        smk_bw_init(&frame_bits);
        if (smk_video_encode_frame(&video, indices, keyframe_at(i, interval),
                                   trees, &frame_bits, 0) != 0) {
            smk_bw_free(&frame_bits);
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "failed to encode video frame");
            goto cleanup;
        }

        type = i == 0 ? 0x01u : 0u;
        video_size = smk_bw_byte_count(&frame_bits);
        payload_size = (i == 0 ? palette_size : 0) + video_size;
        payload = (uint8_t *)malloc(payload_size != 0 ? payload_size : 1);
        if (payload == NULL) {
            smk_bw_free(&frame_bits);
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE, "out of memory");
            goto cleanup;
        }
        if (i == 0)
            memcpy(payload, palette_chunk, palette_size);
        if (video_size != 0)
            memcpy(payload + (i == 0 ? palette_size : 0),
                   smk_bw_data(&frame_bits), video_size);
        smk_bw_free(&frame_bits);

        if (smk_container_add_frame(&container, keyframe_at(i, interval), type,
                                    payload, payload_size, err, errlen) != 0) {
            free(payload);
            status = SMK_ENC_ERR_ENCODE;
            goto cleanup;
        }
        free(payload);
    }

    if (smk_container_finish(&container, err, errlen) != 0) {
        have_container = 0;
        status = SMK_ENC_ERR_ENCODE;
        goto cleanup;
    }
    have_container = 0;
    status = SMK_ENC_OK;

cleanup:
    if (have_container)
        smk_container_abort(&container);
    if (have_tree_blob)
        smk_bw_free(&tree_blob);
    if (have_video)
        smk_video_free(&video);
    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        smk_huff_free(&trees[i]);
    free(rgb);
    free(indices);
    free(hist);
    free(lut);
    return status;
}
