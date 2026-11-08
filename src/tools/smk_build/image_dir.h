#ifndef SMK_IMAGE_DIR_H
#define SMK_IMAGE_DIR_H

#include "smk_encoder.h"

#include <stddef.h>

/*
 * Open a directory of TGA/BMP/PCX frames as an encoder source.
 *
 * Frames are ordered by natural filename comparison (frame2 before frame10)
 * and are loaded on demand by read_frame. Dimensions that are not a multiple
 * of four are padded by edge replication unless options->strict is set.
 *
 * On success the caller owns the source and must call smk_image_dir_close().
 */
smk_enc_status smk_image_dir_open(const char *path,
                                  const smk_enc_options *options,
                                  smk_enc_source *source,
                                  char *err, size_t errlen);

/* Attach a PCM WAV file as audio track 0. */
smk_enc_status smk_image_dir_attach_audio(smk_enc_source *source,
                                          const char *wav_path,
                                          char *err, size_t errlen);

/* Release the source and any attached audio. */
void smk_image_dir_close(smk_enc_source *source);

#endif
