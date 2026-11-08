#ifndef SMK_ENCODER_H
#define SMK_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMK_ENC_MAX_AUDIO_TRACKS 7

/* Status values double as process exit codes. */
typedef enum {
    SMK_ENC_OK = 0,
    SMK_ENC_ERR_USAGE = 1,
    SMK_ENC_ERR_INPUT = 2,
    SMK_ENC_ERR_ENCODE = 3,
    SMK_ENC_ERR_VERIFY = 4
} smk_enc_status;

typedef enum {
    SMK_ENC_PALETTE_ADAPTIVE = 0,
    SMK_ENC_PALETTE_GLOBAL,
    SMK_ENC_PALETTE_FRAME
} smk_enc_palette_mode;

typedef enum {
    SMK_ENC_DITHER_NONE = 0,
    SMK_ENC_DITHER_ORDERED,
    SMK_ENC_DITHER_FS
} smk_enc_dither_mode;

typedef struct {
    unsigned rate;
    unsigned channels; /* 1 or 2 */
    unsigned bits;     /* 8 or 16 */
} smk_enc_audio_info;

/*
 * Input adapter. The encoder calls read_frame() and read_audio() during each
 * pass, so inputs may stay on disk.
 *
 * read_frame fills width * height * 3 bytes of RGB.
 * read_audio fills interleaved PCM for [start_sample, start_sample + count),
 * returning the number of bytes written. Samples past the end are silence.
 */
typedef struct smk_enc_source {
    void *user;
    unsigned width;
    unsigned height;
    unsigned frame_count;
    unsigned fps_num;
    unsigned fps_den;
    unsigned audio_track_count;
    smk_enc_audio_info audio[SMK_ENC_MAX_AUDIO_TRACKS];
    int (*read_frame)(void *user, unsigned index, uint8_t *rgb);
    size_t (*read_audio)(void *user, unsigned track, size_t start_sample,
                         size_t sample_count, void *dst);
} smk_enc_source;

typedef struct {
    smk_enc_palette_mode palette;
    smk_enc_dither_mode dither;
    unsigned colors;             /* 2..256 */
    unsigned keyframe_interval;  /* 0 = only frame 0 */
    unsigned fps_num;
    unsigned fps_den;
    int ring;
    uint8_t background[3];
    int strict;
    int verify;
    int verbose;
} smk_enc_options;

void smk_enc_options_default(smk_enc_options *options);

smk_enc_status smk_enc_encode(const smk_enc_source *source,
                              const smk_enc_options *options,
                              const char *output_path,
                              char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
