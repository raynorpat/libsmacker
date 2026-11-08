#include "test.h"

int smk_test_failures = 0;
int smk_test_checks = 0;

void test_cli_defaults(void);
void test_encode_null_source(void);
void test_encode_null_output(void);
void test_encode_missing_input(void);
void test_bitwriter_lsb_example(void);
void test_bitwriter_put_bit(void);
void test_bitwriter_grows(void);
void test_huffman_absent(void);
void test_huffman_single_symbol(void);
void test_huffman_roundtrip(void);
void test_tga_24_uncompressed_bottom_up(void);
void test_tga_32_rle_top_down_alpha(void);
void test_tga_8_colormapped(void);
void test_tga_truncated(void);
void test_image_unsupported_format(void);
void test_bmp_24_bottom_up(void);
void test_bmp_32_top_down(void);
void test_bmp_8_palette(void);
void test_bmp_odd_width_padding(void);
void test_bmp_truncated(void);
void test_pcx_8bit_1plane_palette(void);
void test_pcx_8bit_3plane_rgb(void);
void test_pcx_1bit(void);
void test_pcx_truncated(void);

static const smk_test_case tests[] = {
    { "cli_defaults", test_cli_defaults },
    { "encode_null_source", test_encode_null_source },
    { "encode_null_output", test_encode_null_output },
    { "encode_missing_input", test_encode_missing_input },
    { "bitwriter_lsb_example", test_bitwriter_lsb_example },
    { "bitwriter_put_bit", test_bitwriter_put_bit },
    { "bitwriter_grows", test_bitwriter_grows },
    { "huffman_absent", test_huffman_absent },
    { "huffman_single_symbol", test_huffman_single_symbol },
    { "huffman_roundtrip", test_huffman_roundtrip },
    { "tga_24_uncompressed_bottom_up", test_tga_24_uncompressed_bottom_up },
    { "tga_32_rle_top_down_alpha", test_tga_32_rle_top_down_alpha },
    { "tga_8_colormapped", test_tga_8_colormapped },
    { "tga_truncated", test_tga_truncated },
    { "image_unsupported_format", test_image_unsupported_format },
    { "bmp_24_bottom_up", test_bmp_24_bottom_up },
    { "bmp_32_top_down", test_bmp_32_top_down },
    { "bmp_8_palette", test_bmp_8_palette },
    { "bmp_odd_width_padding", test_bmp_odd_width_padding },
    { "bmp_truncated", test_bmp_truncated },
    { "pcx_8bit_1plane_palette", test_pcx_8bit_1plane_palette },
    { "pcx_8bit_3plane_rgb", test_pcx_8bit_3plane_rgb },
    { "pcx_1bit", test_pcx_1bit },
    { "pcx_truncated", test_pcx_truncated },
    { NULL, NULL }
};

int main(void)
{
    int i;
    int failed_tests = 0;

    for (i = 0; tests[i].name != NULL; i++) {
        smk_test_failures = 0;
        smk_test_checks = 0;
        tests[i].fn();
        if (smk_test_failures == 0) {
            printf("ok %d - %s (%d checks)\n", i + 1, tests[i].name,
                   smk_test_checks);
        } else {
            printf("not ok %d - %s (%d failures)\n", i + 1, tests[i].name,
                   smk_test_failures);
            failed_tests++;
        }
    }

    printf("%d tests, %d failed\n", i, failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
