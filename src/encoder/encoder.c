#include "smk_encoder.h"

#include <stdio.h>
#include <string.h>

static smk_enc_status fail(char *err, size_t errlen, smk_enc_status status,
                           const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return status;
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
    if (source == NULL || options == NULL ||
        output_path == NULL || output_path[0] == '\0')
        return fail(err, errlen, SMK_ENC_ERR_USAGE,
                    "missing source or output path");

    if (source->read_frame == NULL || source->frame_count == 0 ||
        source->width == 0 || source->height == 0)
        return fail(err, errlen, SMK_ENC_ERR_INPUT,
                    "source has no frames or no dimensions");

    return fail(err, errlen, SMK_ENC_ERR_ENCODE,
                "encoder pipeline is not implemented yet");
}
