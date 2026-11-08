#ifndef SMK_VERIFY_H
#define SMK_VERIFY_H

#include "smk_encoder.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reopen an encoded file with libsmacker and compare it against the same
 * source and options used to produce it: header metadata, each frame's palette
 * and decoded indices, and every audio track's PCM bytes. Returns SMK_ENC_OK
 * when the file is a faithful decode, SMK_ENC_ERR_VERIFY on any mismatch, and
 * an input error when the source itself cannot be read.
 */
smk_enc_status smk_verify_file(const char *path, const smk_enc_source *source,
                               const smk_enc_options *options,
                               char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
