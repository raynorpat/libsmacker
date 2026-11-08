#include "verify.h"

#include "encoder_internal.h"
#include "quantize.h"
#include "smacker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static smk_enc_status fail(char *err, size_t errlen, smk_enc_status status,
                           const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return status;
}

/* Read the video-flags word straight from the header to check the ring bit. */
static int read_ring_flag(const char *path, int *ring)
{
    uint8_t bytes[4];
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return -1;
    if (fseek(file, 0x14, SEEK_SET) != 0 ||
        fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    *ring = (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
             ((uint32_t)bytes[3] << 24)) != 0;
    return 0;
}

static int audio_format_ok(const smk_enc_audio_info *info)
{
    if (info->rate == 0 || info->rate > 0x00ffffffu)
        return 0;
    if (info->channels != 1 && info->channels != 2)
        return 0;
    return info->bits == 8 || info->bits == 16;
}

/*
 * Compare one decoded audio chunk with the source. The encoder pads a short
 * track with silence, so the verifier does the same before comparing.
 */
static int audio_matches(const smk_enc_source *source, unsigned track,
                         size_t start_sample, size_t samples, size_t bytes,
                         const unsigned char *decoded,
                         unsigned long decoded_size, uint8_t *expected)
{
    const smk_enc_audio_info *info = &source->audio[track];
    uint8_t silence = info->bits == 8 ? 0x80 : 0x00;
    size_t got;

    if ((size_t)decoded_size != bytes)
        return 0;
    if (bytes == 0)
        return 1;

    memset(expected, silence, bytes);
    got = source->read_audio(source->user, track, start_sample, samples,
                             expected);
    if (got > bytes)
        got = bytes;
    if (got < bytes)
        memset(expected + got, silence, bytes - got);
    return memcmp(expected, decoded, bytes) == 0;
}

static smk_enc_status verify_body(const char *path,
                                  const smk_enc_source *source,
                                  const smk_enc_options *options,
                                  char *err, size_t errlen);

smk_enc_status smk_verify_file(const char *path, const smk_enc_source *source,
                               const smk_enc_options *options,
                               char *err, size_t errlen)
{
    if (path == NULL || path[0] == '\0' || source == NULL ||
        options == NULL)
        return fail(err, errlen, SMK_ENC_ERR_USAGE,
                    "missing source or output path");
    return verify_body(path, source, options, err, errlen);
}

static smk_enc_status verify_body(const char *path,
                                  const smk_enc_source *source,
                                  const smk_enc_options *options,
                                  char *err, size_t errlen)
{
    size_t pixels;
    uint8_t *rgb = NULL;
    uint8_t *indices = NULL;
    uint8_t *expected_audio = NULL;
    smk_histogram hist;
    smk_palette palette;
    smk_palette frame_palette;
    smk_enc_palette_choice choice;
    smk_quant_lut *lut = NULL;
    smk handle = NULL;
    unsigned long frames = 0;
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long audio_rate[SMK_ENC_MAX_AUDIO_TRACKS];
    unsigned char audio_channels[SMK_ENC_MAX_AUDIO_TRACKS];
    unsigned char audio_bits[SMK_ENC_MAX_AUDIO_TRACKS];
    unsigned char track_mask = 0;
    unsigned char enable_mask;
    smk_enc_audio_split splits[SMK_ENC_MAX_AUDIO_TRACKS];
    size_t audio_pos[SMK_ENC_MAX_AUDIO_TRACKS];
    size_t frame_samples[SMK_ENC_MAX_AUDIO_TRACKS];
    size_t frame_bytes[SMK_ENC_MAX_AUDIO_TRACKS];
    size_t max_audio = 0;
    unsigned colors;
    unsigned fps_num;
    unsigned fps_den;
    unsigned tracks;
    unsigned track;
    unsigned i;
    int ring;
    double decoded_usf;
    smk_enc_status status = SMK_ENC_ERR_VERIFY;

    if (source->read_frame == NULL || source->frame_count == 0 ||
        source->width == 0 || source->height == 0)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "source has no frames or no dimensions");
    if (source->width % 4 != 0 || source->height % 4 != 0)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "frame dimensions must be multiples of 4");
    tracks = source->audio_track_count;
    if (tracks > SMK_ENC_MAX_AUDIO_TRACKS)
        return fail(err, errlen, SMK_ENC_ERR_INPUT, "too many audio tracks");
    if (tracks > 0 && source->read_audio == NULL)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "audio tracks have no reader");
    for (track = 0; track < tracks; track++) {
        if (!audio_format_ok(&source->audio[track]))
            return fail(err, errlen, SMK_ENC_ERR_INPUT,
                        "invalid audio track format");
    }

    pixels = (size_t)source->width * (size_t)source->height;
    colors = smk_enc_clamp_colors(options->colors);
    fps_num = source->fps_num != 0 ? source->fps_num : options->fps_num;
    fps_den = source->fps_den != 0 ? source->fps_den : options->fps_den;
    if (fps_num == 0)
        fps_num = 15;
    if (fps_den == 0)
        fps_den = 1;

    memset(&hist, 0, sizeof(hist));
    memset(&palette, 0, sizeof(palette));
    memset(&frame_palette, 0, sizeof(frame_palette));
    memset(&choice, 0, sizeof(choice));
    memset(splits, 0, sizeof(splits));
    memset(audio_pos, 0, sizeof(audio_pos));

    rgb = (uint8_t *)malloc(pixels * 3);
    indices = (uint8_t *)malloc(pixels);
    lut = (smk_quant_lut *)malloc(sizeof(*lut));
    if (rgb == NULL || indices == NULL || lut == NULL) {
        status = fail(err, errlen, SMK_ENC_ERR_ENCODE, "out of memory");
        goto cleanup;
    }
    smk_quant_lut_init(lut);

    handle = smk_open_file(path, SMK_MODE_DISK);
    if (handle == NULL) {
        status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                      "cannot decode the encoded file");
        goto cleanup;
    }

    if (smk_info_all(handle, NULL, &frames, &decoded_usf) != 0 ||
        smk_info_video(handle, &width, &height, NULL) != 0 ||
        smk_info_audio(handle, &track_mask, audio_channels, audio_bits,
                       audio_rate) != 0) {
        status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                      "cannot read encoded file metadata");
        goto cleanup;
    }

    if (frames != source->frame_count) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "frame count mismatch: expected %u, got %lu",
                     source->frame_count, frames);
        goto cleanup;
    }
    if (width != source->width || height != source->height) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen,
                     "dimensions mismatch: expected %ux%u, got %lux%lu",
                     source->width, source->height, width, height);
        goto cleanup;
    }
    if (((unsigned char)((1u << tracks) - 1u)) != track_mask) {
        status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                      "audio track count mismatch");
        goto cleanup;
    }
    for (track = 0; track < tracks; track++) {
        if (audio_channels[track] != source->audio[track].channels ||
            audio_bits[track] != source->audio[track].bits ||
            audio_rate[track] != source->audio[track].rate) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "audio track %u format mismatch",
                         track);
            goto cleanup;
        }
    }
    {
        double expected = 1000000.0 * (double)fps_den / (double)fps_num;
        double tolerance = expected * 0.005 + 1.0;

        if (fabs(decoded_usf - expected) > tolerance) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen,
                         "frame rate mismatch: expected %.3f us, got %.3f us",
                         expected, decoded_usf);
            goto cleanup;
        }
    }
    if (read_ring_flag(path, &ring) != 0 ||
        (ring != 0) != (options->ring != 0)) {
        status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                      "ring frame flag mismatch");
        goto cleanup;
    }

    /* Rebuild the shared palette exactly as the encoder did. */
    if (options->palette != SMK_ENC_PALETTE_FRAME) {
        smk_histogram_init(&hist);
        for (i = 0; i < source->frame_count; i++) {
            if (source->read_frame(source->user, i, rgb) != 0) {
                if (err != NULL && errlen > 0)
                    snprintf(err, errlen, "cannot read frame %u", i);
                status = SMK_ENC_ERR_INPUT;
                goto cleanup;
            }
            smk_histogram_add(&hist, rgb, pixels);
        }
        if (smk_quant_build_global(&hist, colors, &palette) == 0 ||
            palette.count == 0) {
            status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                          "could not rebuild the shared palette");
            goto cleanup;
        }
    }

    /*
     * Size the audio scratch buffer from the largest chunk any frame will
     * produce, so a frame can be compared without reallocating.
     */
    for (track = 0; track < tracks; track++) {
        smk_enc_audio_split split;

        split.rem = 0;
        for (i = 0; i < source->frame_count; i++) {
            size_t samples;
            size_t bytes;

            if (smk_enc_audio_frame_bytes(&split, &source->audio[track],
                                          fps_num, fps_den, &samples,
                                          &bytes) != 0) {
                status = fail(err, errlen, SMK_ENC_ERR_INPUT,
                              "audio frame size overflow");
                goto cleanup;
            }
            if (bytes > max_audio)
                max_audio = bytes;
        }
    }
    expected_audio = (uint8_t *)malloc(max_audio != 0 ? max_audio : 1);
    if (expected_audio == NULL) {
        status = fail(err, errlen, SMK_ENC_ERR_ENCODE, "out of memory");
        goto cleanup;
    }

    enable_mask = SMK_VIDEO_TRACK;
    for (track = 0; track < tracks; track++)
        enable_mask = (unsigned char)(enable_mask | (1u << track));
    if (smk_enable_all(handle, enable_mask) != 0) {
        status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                      "cannot enable decode tracks");
        goto cleanup;
    }

    if (smk_first(handle) < 0) {
        status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                      "cannot decode frame 0");
        goto cleanup;
    }

    for (i = 0; i < source->frame_count; i++) {
        const unsigned char *decoded_palette;
        const unsigned char *decoded_video;

        if (i > 0 && smk_next(handle) < 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot decode frame %u", i);
            goto cleanup;
        }

        if (source->read_frame(source->user, i, rgb) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot read frame %u", i);
            status = SMK_ENC_ERR_INPUT;
            goto cleanup;
        }
        if (smk_enc_choose_palette(options, &palette, &hist, rgb,
                                   source->width, source->height, colors, lut,
                                   indices, &frame_palette, &choice) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                          "could not rebuild a frame palette");
            goto cleanup;
        }
        decoded_palette = smk_get_palette(handle);
        decoded_video = smk_get_video(handle);
        if (decoded_palette == NULL || decoded_video == NULL) {
            status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                          "no decoded palette or video");
            goto cleanup;
        }
        if (memcmp(decoded_palette, choice.palette->display,
                   (size_t)choice.palette->count * 3) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "palette mismatch at frame %u", i);
            goto cleanup;
        }
        if (memcmp(decoded_video, indices, pixels) != 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "video mismatch at frame %u", i);
            goto cleanup;
        }

        for (track = 0; track < tracks; track++) {
            if (smk_enc_audio_frame_bytes(&splits[track],
                                          &source->audio[track], fps_num,
                                          fps_den, &frame_samples[track],
                                          &frame_bytes[track]) != 0) {
                status = fail(err, errlen, SMK_ENC_ERR_INPUT,
                              "audio frame size overflow");
                goto cleanup;
            }
            if (!audio_matches(source, track, audio_pos[track],
                               frame_samples[track], frame_bytes[track],
                               smk_get_audio(handle, (unsigned char)track),
                               smk_get_audio_size(handle,
                                                  (unsigned char)track),
                               expected_audio)) {
                if (err != NULL && errlen > 0)
                    snprintf(err, errlen,
                             "audio track %u mismatch at frame %u", track, i);
                goto cleanup;
            }
            audio_pos[track] += frame_samples[track];
        }
    }

    /*
     * A ring file has one extra physical frame that must rebuild frame 0 with
     * the palette the loop resumes on. A file without a ring must not have one.
     */
    if (options->ring) {
        if (smk_next(handle) != SMK_LAST) {
            status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                          "missing ring frame");
            goto cleanup;
        }
        if (source->read_frame(source->user, 0, rgb) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_INPUT,
                          "cannot read frame 0 for ring check");
            goto cleanup;
        }
        if (smk_enc_choose_palette(options, &palette, &hist, rgb,
                                   source->width, source->height, colors, lut,
                                   indices, &frame_palette, &choice) != 0 ||
            memcmp(smk_get_palette(handle), choice.palette->display,
                   (size_t)choice.palette->count * 3) != 0 ||
            memcmp(smk_get_video(handle), indices, pixels) != 0) {
            status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                          "ring frame mismatch");
            goto cleanup;
        }
    } else if (smk_next(handle) != SMK_DONE) {
        status = fail(err, errlen, SMK_ENC_ERR_VERIFY,
                      "unexpected extra frame");
        goto cleanup;
    }

    status = SMK_ENC_OK;

cleanup:
    if (handle != NULL)
        smk_close(handle);
    free(rgb);
    free(indices);
    free(expected_audio);
    free(lut);
    return status;
}
