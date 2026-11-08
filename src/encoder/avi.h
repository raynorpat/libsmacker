#ifndef SMK_AVI_H
#define SMK_AVI_H

#include "smk_encoder.h"

#include <stddef.h>

/*
 * Open an AVI written in the smk2avi style: RIFF AVI with a DIB video stream
 * (24 or 32 bit BI_RGB) and up to seven PCM audio streams. Interleaved and
 * non-interleaved chunk layouts are both accepted.
 *
 * On success the caller owns the source and must call smk_avi_close().
 */
smk_enc_status smk_avi_open(const char *path,
                            const smk_enc_options *options,
                            smk_enc_source *source,
                            char *err, size_t errlen);

void smk_avi_close(smk_enc_source *source);

#endif
