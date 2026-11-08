#include "smk_encoder.h"

#include "container.h"
#include "encoder_internal.h"
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

unsigned smk_enc_clamp_colors(unsigned colors)
{
    if (colors < 2)
        return 2;
    if (colors > 256)
        return 256;
    return colors;
}

size_t smk_enc_audio_align(const smk_enc_audio_info *info)
{
    return (size_t)info->channels * (size_t)(info->bits / 8);
}

/* Build the AudioRate descriptor, or fail on an unusable track format. */
static int audio_descriptor(const smk_enc_audio_info *info, uint32_t *out)
{
    if (info->rate == 0 || info->rate > 0x00ffffffu)
        return -1;
    if (info->channels != 1 && info->channels != 2)
        return -1;
    if (info->bits != 8 && info->bits != 16)
        return -1;

    *out = 0x40000000u |
           (info->bits == 16 ? 0x20000000u : 0u) |
           (info->channels == 2 ? 0x10000000u : 0u) |
           info->rate;
    return 0;
}

/*
 * The fraction of a sample frame that belongs to each video frame is
 * rate * fps_den / fps_num. Keep the remainder so samples split exactly and
 * never drift, regardless of whether the ratio is integral.
 */
int smk_enc_audio_frame_bytes(smk_enc_audio_split *split,
                             const smk_enc_audio_info *info,
                             unsigned fps_num, unsigned fps_den,
                             size_t *samples_out, size_t *bytes_out)
{
    uint64_t step;
    uint64_t samples;
    size_t align = smk_enc_audio_align(info);

    if (fps_num == 0 || fps_den == 0 || align == 0)
        return -1;
    if ((uint64_t)info->rate > UINT64_MAX / (uint64_t)fps_den)
        return -1;

    step = (uint64_t)info->rate * (uint64_t)fps_den;
    split->rem += step;
    if (split->rem < step)
        return -1; /* overflow */
    samples = split->rem / fps_num;
    split->rem %= fps_num;
    if (samples > SIZE_MAX / align)
        return -1;

    *samples_out = (size_t)samples;
    *bytes_out = (size_t)samples * align;
    return 0;
}

static void put_u32le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static int build_frame_palette(smk_histogram *hist, const uint8_t *rgb,
                               size_t pixels, unsigned colors,
                               smk_palette *out)
{
    smk_histogram_init(hist);
    smk_histogram_add(hist, rgb, pixels);
    if (smk_quant_build_global(hist, colors, out) == 0 || out->count == 0)
        return -1;
    return 0;
}

/*
 * Pick the palette for one frame and map the frame through it. Global mode
 * always uses the shared palette. Frame mode always builds a new one.
 * Adaptive mode keeps the shared palette unless the per-frame error exceeds
 * the threshold. The same decision is made in the count and write passes.
 */
int smk_enc_choose_palette(const smk_enc_options *options,
                          const smk_palette *global, smk_histogram *hist,
                          const uint8_t *rgb, unsigned width, unsigned height,
                          unsigned colors, smk_quant_lut *lut,
                          uint8_t *indices, smk_palette *scratch,
                          smk_enc_palette_choice *choice)
{
    size_t pixels = (size_t)width * height;

    choice->palette = global;
    choice->frame_palette = 0;

    if (options->palette == SMK_ENC_PALETTE_FRAME) {
        if (build_frame_palette(hist, rgb, pixels, colors, scratch) != 0)
            return -1;
        choice->palette = scratch;
        choice->frame_palette = 1;
    } else {
        smk_quant_lut_init(lut);
        smk_quant_map(global, rgb, width, height, options->dither, indices,
                      lut);
        if (options->palette == SMK_ENC_PALETTE_ADAPTIVE &&
            smk_quant_mean_error(global, rgb, pixels, indices) >
                options->palette_threshold) {
            if (build_frame_palette(hist, rgb, pixels, colors, scratch) != 0)
                return -1;
            choice->palette = scratch;
            choice->frame_palette = 1;
        }
    }

    if (choice->frame_palette) {
        smk_quant_lut_init(lut);
        smk_quant_map(choice->palette, rgb, width, height, options->dither,
                      indices, lut);
    }
    return 0;
}

/*
 * A palette chunk is written on frame 0. Frame mode writes one on every frame;
 * adaptive mode writes one whenever a frame palette starts or ends, because
 * the previous palette must be restored and a palette change needs a keyframe.
 */
int smk_enc_palette_is_emitted(smk_enc_palette_mode mode, unsigned index,
                              int frame_palette, int previous_frame_palette)
{
    if (mode == SMK_ENC_PALETTE_FRAME)
        return 1;
    if (index == 0)
        return 1;
    if (mode == SMK_ENC_PALETTE_ADAPTIVE)
        return frame_palette || previous_frame_palette;
    return 0;
}

void smk_enc_options_default(smk_enc_options *options)
{
    if (options == NULL)
        return;

    memset(options, 0, sizeof(*options));
    options->palette = SMK_ENC_PALETTE_ADAPTIVE;
    options->dither = SMK_ENC_DITHER_NONE;
    options->colors = 256;
    options->palette_threshold = 6.0;
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
    smk_palette frame_palette;
    smk_enc_palette_choice choice;
    smk_quant_lut *lut = NULL;
    smk_huff trees[SMK_VIDEO_TREE_COUNT];
    smk_video video;
    smk_bitwriter tree_blob;
    smk_bitwriter frame_bits;
    smk_container container;
    smk_container_info info;
    uint8_t palette_chunk[SMK_PALETTE_CHUNK_MAX];
    smk_enc_audio_split splits[SMK_ENC_MAX_AUDIO_TRACKS];
    size_t audio_pos[SMK_ENC_MAX_AUDIO_TRACKS];
    size_t frame_samples[SMK_ENC_MAX_AUDIO_TRACKS];
    size_t frame_audio_bytes[SMK_ENC_MAX_AUDIO_TRACKS];
    unsigned tracks;
    unsigned track;
    unsigned colors;
    unsigned fps_num;
    unsigned fps_den;
    unsigned interval;
    unsigned i;
    size_t palette_size;
    int prev_frame_palette;
    int emit_palette;
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
    if (source->audio_track_count > SMK_ENC_MAX_AUDIO_TRACKS)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "too many audio tracks");
    if (source->audio_track_count > 0 && source->read_audio == NULL)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "audio tracks have no reader");

    pixels = (size_t)source->width * source->height;
    colors = smk_enc_clamp_colors(options->colors);
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
    memset(&frame_palette, 0, sizeof(frame_palette));
    memset(&info, 0, sizeof(info));
    for (i = 0; i < SMK_VIDEO_TREE_COUNT; i++)
        smk_huff_init(&trees[i]);

    /*
     * Audio header fields: one descriptor per track and the largest unpacked
     * chunk any frame will produce, which libsmacker uses to size its buffer.
     */
    tracks = source->audio_track_count;
    for (track = 0; track < tracks; track++) {
        smk_enc_audio_split split;
        uint64_t max_bytes = 0;

        if (audio_descriptor(&source->audio[track],
                             &info.audio_rate[track]) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_INPUT,
                          "invalid audio track format");
            goto cleanup;
        }
        split.rem = 0;
        for (i = 0; i < source->frame_count; i++) {
            size_t samples;
            size_t bytes;

            if (smk_enc_audio_frame_bytes(&split, &source->audio[track], fps_num,
                                  fps_den, &samples, &bytes) != 0) {
                status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                              "audio frame size overflow");
                goto cleanup;
            }
            if (bytes > max_bytes)
                max_bytes = bytes;
        }
        if (max_bytes > 0xffffffffu) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "audio frame is too large");
            goto cleanup;
        }
        info.audio_size[track] = (uint32_t)max_bytes;
    }

    rgb = (uint8_t *)malloc(pixels * 3);
    indices = (uint8_t *)malloc(pixels);
    hist = (smk_histogram *)malloc(sizeof(*hist));
    lut = (smk_quant_lut *)malloc(sizeof(*lut));
    if (rgb == NULL || indices == NULL || hist == NULL || lut == NULL) {
        status = fail(err, errlen, SMK_ENC_ERR_ENCODE, "out of memory");
        goto cleanup;
    }

    smk_quant_lut_init(lut);

    /* Pass 1: build the shared palette from every frame (unless per-frame). */
    if (options->palette != SMK_ENC_PALETTE_FRAME) {
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
    }

    if (smk_video_init(&video, source->width, source->height) != 0) {
        status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                      "out of memory creating video encoder");
        goto cleanup;
    }
    have_video = 1;

    /* Pass 2: count every symbol the write pass will produce. */
    smk_video_reset(&video);
    prev_frame_palette = 0;
    for (i = 0; i < source->frame_count; i++) {
        int keyframe;

        if (source->read_frame(source->user, i, rgb) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot read frame %u", i);
            status = SMK_ENC_ERR_INPUT;
            goto cleanup;
        }
        if (smk_enc_choose_palette(options, &palette, hist, rgb, source->width,
                           source->height, colors, lut, indices,
                           &frame_palette, &choice) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "could not build a palette");
            goto cleanup;
        }
        emit_palette = smk_enc_palette_is_emitted(options->palette, i,
                                          choice.frame_palette,
                                          prev_frame_palette);
        prev_frame_palette = choice.frame_palette;
        keyframe = keyframe_at(i, interval) || emit_palette;

        if (smk_video_encode_frame(&video, indices, keyframe, trees, NULL, 1) !=
            0) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "failed to count video symbols");
            goto cleanup;
        }
    }

    /*
     * A tree with one symbol encodes zero bits. For the Type tree that would
     * make an all-unchanged frame, or a single-color still, an empty chunk
     * that the decoder rejects. Add an unused symbol so every frame's
     * descriptors occupy at least one bit.
     */
    {
        smk_huff *type = &trees[SMK_VIDEO_TREE_TYPE];
        unsigned only = 0;
        unsigned used = 0;

        for (i = 0; i < SMK_HUFF_MAX_SYMBOLS; i++) {
            if (type->freq[i] != 0) {
                only = i;
                used++;
            }
        }
        if (used == 1)
            smk_huff_count(type,
                           (uint16_t)(only == 0xffffu ? 0xfffeu : 0xffffu));
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

    /* Pass 3: write the frames. */
    smk_video_reset(&video);
    prev_frame_palette = 0;
    memset(splits, 0, sizeof(splits));
    memset(audio_pos, 0, sizeof(audio_pos));
    for (i = 0; i < source->frame_count; i++) {
        size_t video_size;
        size_t payload_size;
        size_t pos;
        uint8_t *payload;
        unsigned type;
        int keyframe;

        if (source->read_frame(source->user, i, rgb) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot read frame %u", i);
            status = SMK_ENC_ERR_INPUT;
            goto cleanup;
        }
        if (smk_enc_choose_palette(options, &palette, hist, rgb, source->width,
                           source->height, colors, lut, indices,
                           &frame_palette, &choice) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "could not build a palette");
            goto cleanup;
        }
        emit_palette = smk_enc_palette_is_emitted(options->palette, i,
                                          choice.frame_palette,
                                          prev_frame_palette);
        prev_frame_palette = choice.frame_palette;
        keyframe = keyframe_at(i, interval) || emit_palette;
        palette_size = emit_palette
            ? build_palette_chunk(choice.palette, palette_chunk) : 0;

        smk_bw_init(&frame_bits);
        if (smk_video_encode_frame(&video, indices, keyframe, trees,
                                   &frame_bits, 0) != 0) {
            smk_bw_free(&frame_bits);
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "failed to encode video frame");
            goto cleanup;
        }

        type = emit_palette ? 0x01u : 0u;
        video_size = smk_bw_byte_count(&frame_bits);
        payload_size = palette_size + video_size;
        for (track = 0; track < tracks; track++) {
            if (smk_enc_audio_frame_bytes(&splits[track], &source->audio[track],
                                  fps_num, fps_den, &frame_samples[track],
                                  &frame_audio_bytes[track]) != 0) {
                smk_bw_free(&frame_bits);
                status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                              "audio frame size overflow");
                goto cleanup;
            }
            payload_size += 4 + frame_audio_bytes[track];
            type |= 0x02u << track;
        }

        payload = (uint8_t *)malloc(payload_size != 0 ? payload_size : 1);
        if (payload == NULL) {
            smk_bw_free(&frame_bits);
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE, "out of memory");
            goto cleanup;
        }

        pos = 0;
        if (palette_size != 0) {
            memcpy(payload, palette_chunk, palette_size);
            pos += palette_size;
        }
        for (track = 0; track < tracks; track++) {
            size_t bytes = frame_audio_bytes[track];

            put_u32le(payload + pos, (uint32_t)(4 + bytes));
            pos += 4;
            if (bytes != 0) {
                size_t got = source->read_audio(
                    source->user, track, audio_pos[track],
                    frame_samples[track], payload + pos);

                if (got > bytes)
                    got = bytes;
                if (got < bytes) {
                    uint8_t silence =
                        source->audio[track].bits == 8 ? 0x80 : 0x00;

                    memset(payload + pos + got, silence, bytes - got);
                }
                pos += bytes;
            }
            audio_pos[track] += frame_samples[track];
        }
        if (video_size != 0) {
            memcpy(payload + pos, smk_bw_data(&frame_bits), video_size);
            pos += video_size;
        }
        smk_bw_free(&frame_bits);

        if (smk_container_add_frame(&container, keyframe, type,
                                    payload, payload_size, err, errlen) != 0) {
            free(payload);
            status = SMK_ENC_ERR_ENCODE;
            goto cleanup;
        }
        free(payload);
    }

    /*
     * The ring frame is one extra physical frame: a delta from the last frame
     * back to frame 0, so looping playback resumes correctly at frame 1.
     */
    if (options->ring) {
        size_t video_size;
        size_t payload_size;
        uint8_t *payload;
        int ring_keyframe;

        if (source->read_frame(source->user, 0, rgb) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_INPUT,
                          "cannot read frame 0 for ring");
            goto cleanup;
        }
        if (smk_enc_choose_palette(options, &palette, hist, rgb, source->width,
                           source->height, colors, lut, indices,
                           &frame_palette, &choice) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "could not build a palette");
            goto cleanup;
        }

        /*
         * The loop must resume with frame 0's palette. Re-emit it when it
         * differs from the palette in effect after the last frame; a palette
         * change turns the ring frame into a keyframe.
         */
        emit_palette = options->palette == SMK_ENC_PALETTE_FRAME ||
            (options->palette == SMK_ENC_PALETTE_ADAPTIVE &&
             (choice.frame_palette || prev_frame_palette));
        ring_keyframe = emit_palette ? 1 : 0;
        palette_size = emit_palette
            ? build_palette_chunk(choice.palette, palette_chunk) : 0;

        smk_bw_init(&frame_bits);
        if (smk_video_encode_frame(&video, indices, ring_keyframe, trees,
                                   &frame_bits, 0) != 0) {
            smk_bw_free(&frame_bits);
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "failed to encode ring frame");
            goto cleanup;
        }
        video_size = smk_bw_byte_count(&frame_bits);
        if (video_size == 0) {
            smk_bw_free(&frame_bits);
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE,
                          "ring frame encoded to no data");
            goto cleanup;
        }
        payload_size = palette_size + video_size;
        payload = (uint8_t *)malloc(payload_size);
        if (payload == NULL) {
            smk_bw_free(&frame_bits);
            status = fail(err, errlen, SMK_ENC_ERR_ENCODE, "out of memory");
            goto cleanup;
        }
        if (palette_size != 0)
            memcpy(payload, palette_chunk, palette_size);
        memcpy(payload + palette_size, smk_bw_data(&frame_bits), video_size);
        smk_bw_free(&frame_bits);

        if (smk_container_add_frame(&container, ring_keyframe,
                                    emit_palette ? 0x01u : 0u, payload,
                                    payload_size, err, errlen) != 0) {
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
