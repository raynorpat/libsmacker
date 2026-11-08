#ifndef SMK_WAV_H
#define SMK_WAV_H

#include "smk_encoder.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned rate;
    unsigned channels; /* 1 or 2 */
    unsigned bits;     /* source width: 8 or 16 */
    size_t sample_count; /* frames, not interleaved samples */
    int16_t *pcm;        /* interleaved, normalized to int16 */
    int reduced_from_24;
} smk_pcm_audio;

/*
 * Load a PCM WAV file. 24-bit input is reduced to 16-bit with a warning on
 * stderr. The caller frees audio->pcm.
 */
smk_enc_status smk_wav_load(const char *path, smk_pcm_audio *audio,
                            char *err, size_t errlen);

#endif
