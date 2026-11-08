#ifndef SMK_ENCODER_INTERNAL_H
#define SMK_ENCODER_INTERNAL_H

/*
 * Decisions shared by the encoder and the verifier. Both must reproduce the
 * same palette choices, frame mapping, and audio split so a decoded file can
 * be compared against what the encoder intended.
 */

#include "quantize.h"
#include "smk_encoder.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const smk_palette *palette;
    int frame_palette;
} smk_enc_palette_choice;

typedef struct {
    uint64_t rem;
} smk_enc_audio_split;

unsigned smk_enc_clamp_colors(unsigned colors);
size_t smk_enc_audio_align(const smk_enc_audio_info *info);

int smk_enc_audio_frame_bytes(smk_enc_audio_split *split,
                              const smk_enc_audio_info *info,
                              unsigned fps_num, unsigned fps_den,
                              size_t *samples_out, size_t *bytes_out);

int smk_enc_choose_palette(const smk_enc_options *options,
                           const smk_palette *global, smk_histogram *hist,
                           const uint8_t *rgb, unsigned width,
                           unsigned height, unsigned colors,
                           smk_quant_lut *lut, uint8_t *indices,
                           smk_palette *scratch,
                           smk_enc_palette_choice *choice);

int smk_enc_palette_is_emitted(smk_enc_palette_mode mode, unsigned index,
                               int frame_palette,
                               int previous_frame_palette);

#endif
