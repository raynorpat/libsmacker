#include "test.h"
#include "smk_encoder.h"

void test_cli_defaults(void)
{
    smk_enc_options options;

    memset(&options, 0xff, sizeof(options));
    smk_enc_options_default(&options);

    SMK_ASSERT_EQ_INT(options.palette, SMK_ENC_PALETTE_ADAPTIVE);
    SMK_ASSERT_EQ_INT(options.dither, SMK_ENC_DITHER_NONE);
    SMK_ASSERT_EQ_INT(options.colors, 256);
    SMK_ASSERT_EQ_INT(options.keyframe_interval, 150);
    SMK_ASSERT_EQ_INT(options.fps_num, 15);
    SMK_ASSERT_EQ_INT(options.fps_den, 1);
    SMK_ASSERT_EQ_INT(options.background[0], 0);
    SMK_ASSERT_EQ_INT(options.background[1], 0);
    SMK_ASSERT_EQ_INT(options.background[2], 0);
    SMK_ASSERT_EQ_INT(options.strict, 0);
    SMK_ASSERT_EQ_INT(options.ring, 0);
    SMK_ASSERT_EQ_INT(options.verify, 0);
}

void test_encode_null_source(void)
{
    smk_enc_options options;
    char err[128];

    smk_enc_options_default(&options);
    SMK_ASSERT_EQ_INT(smk_enc_encode(NULL, &options, "out.smk", err, sizeof(err)),
                      SMK_ENC_ERR_USAGE);
}

void test_encode_null_output(void)
{
    smk_enc_source source;
    smk_enc_options options;
    char err[128];

    memset(&source, 0, sizeof(source));
    smk_enc_options_default(&options);
    SMK_ASSERT_EQ_INT(smk_enc_encode(&source, &options, NULL, err, sizeof(err)),
                      SMK_ENC_ERR_USAGE);
}

void test_encode_missing_input(void)
{
    smk_enc_source source;
    smk_enc_options options;
    char err[128];

    memset(&source, 0, sizeof(source));
    smk_enc_options_default(&options);
    SMK_ASSERT_EQ_INT(smk_enc_encode(&source, &options, "out.smk", err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
}
