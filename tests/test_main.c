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
void test_wav_16bit_mono(void);
void test_wav_8bit_stereo(void);
void test_wav_24bit_reduced(void);
void test_wav_skips_unknown_chunks(void);
void test_wav_rejects_compressed(void);
void test_wav_truncated(void);
void test_image_dir_natural_sort_and_filter(void);
void test_image_dir_mixed_sizes(void);
void test_image_dir_pads_and_replicates(void);
void test_image_dir_strict_rejects_padding(void);
void test_image_dir_empty(void);
void test_image_dir_audio_pads_silence(void);
void test_avi_smk2avi_layout(void);
void test_avi_two_audio_tracks(void);
void test_avi_interleaved_chunks(void);
void test_avi_32bit_video(void);
void test_avi_odd_width_stride(void);
void test_avi_rejects_unsupported_codec(void);
void test_quantize_histogram_accumulates(void);
void test_quantize_frame_palette_limit(void);
void test_quantize_global_from_histogram(void);
void test_quantize_exact_256(void);
void test_quantize_deterministic(void);
void test_quantize_dither_error(void);
void test_quantize_global_spans_frames(void);
void test_quantize_empty_histogram(void);
void test_video_solid_and_keyframe(void);
void test_video_mono(void);
void test_video_full(void);
void test_video_void_on_delta(void);
void test_video_run_grouping(void);
void test_video_write_roundtrip(void);
void test_video_delta_roundtrip(void);
void test_roundtrip_still_global(void);
void test_roundtrip_still_padded(void);
void test_roundtrip_still_failure_removes_file(void);
void test_roundtrip_audio_pcm_formats(void);
void test_roundtrip_audio_short_silence(void);
void test_roundtrip_audio_long_truncated(void);
void test_roundtrip_audio_two_tracks(void);
void test_roundtrip_delta_identical(void);
void test_roundtrip_keyframe_interval(void);
void test_roundtrip_solid_still(void);
void test_roundtrip_ring_frame(void);
void test_palette_global_one_chunk(void);
void test_palette_frame_each_frame(void);
void test_palette_adaptive_threshold(void);
void test_palette_dither_modes(void);
void test_verify_clean_file(void);
void test_verify_corrupted_video(void);
void test_verify_metadata_mismatch(void);
void test_verify_every_audio_track(void);

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
    { "wav_16bit_mono", test_wav_16bit_mono },
    { "wav_8bit_stereo", test_wav_8bit_stereo },
    { "wav_24bit_reduced", test_wav_24bit_reduced },
    { "wav_skips_unknown_chunks", test_wav_skips_unknown_chunks },
    { "wav_rejects_compressed", test_wav_rejects_compressed },
    { "wav_truncated", test_wav_truncated },
    { "image_dir_natural_sort_and_filter",
      test_image_dir_natural_sort_and_filter },
    { "image_dir_mixed_sizes", test_image_dir_mixed_sizes },
    { "image_dir_pads_and_replicates", test_image_dir_pads_and_replicates },
    { "image_dir_strict_rejects_padding",
      test_image_dir_strict_rejects_padding },
    { "image_dir_empty", test_image_dir_empty },
    { "image_dir_audio_pads_silence", test_image_dir_audio_pads_silence },
    { "avi_smk2avi_layout", test_avi_smk2avi_layout },
    { "avi_two_audio_tracks", test_avi_two_audio_tracks },
    { "avi_interleaved_chunks", test_avi_interleaved_chunks },
    { "avi_32bit_video", test_avi_32bit_video },
    { "avi_odd_width_stride", test_avi_odd_width_stride },
    { "avi_rejects_unsupported_codec", test_avi_rejects_unsupported_codec },
    { "quantize_histogram_accumulates", test_quantize_histogram_accumulates },
    { "quantize_frame_palette_limit", test_quantize_frame_palette_limit },
    { "quantize_global_from_histogram", test_quantize_global_from_histogram },
    { "quantize_exact_256", test_quantize_exact_256 },
    { "quantize_deterministic", test_quantize_deterministic },
    { "quantize_dither_error", test_quantize_dither_error },
    { "quantize_global_spans_frames", test_quantize_global_spans_frames },
    { "quantize_empty_histogram", test_quantize_empty_histogram },
    { "video_solid_and_keyframe", test_video_solid_and_keyframe },
    { "video_mono", test_video_mono },
    { "video_full", test_video_full },
    { "video_void_on_delta", test_video_void_on_delta },
    { "video_run_grouping", test_video_run_grouping },
    { "video_write_roundtrip", test_video_write_roundtrip },
    { "video_delta_roundtrip", test_video_delta_roundtrip },
    { "roundtrip_still_global", test_roundtrip_still_global },
    { "roundtrip_still_padded", test_roundtrip_still_padded },
    { "roundtrip_still_failure_removes_file",
      test_roundtrip_still_failure_removes_file },
    { "roundtrip_audio_pcm_formats", test_roundtrip_audio_pcm_formats },
    { "roundtrip_audio_short_silence", test_roundtrip_audio_short_silence },
    { "roundtrip_audio_long_truncated", test_roundtrip_audio_long_truncated },
    { "roundtrip_audio_two_tracks", test_roundtrip_audio_two_tracks },
    { "roundtrip_delta_identical", test_roundtrip_delta_identical },
    { "roundtrip_keyframe_interval", test_roundtrip_keyframe_interval },
    { "roundtrip_solid_still", test_roundtrip_solid_still },
    { "roundtrip_ring_frame", test_roundtrip_ring_frame },
    { "palette_global_one_chunk", test_palette_global_one_chunk },
    { "palette_frame_each_frame", test_palette_frame_each_frame },
    { "palette_adaptive_threshold", test_palette_adaptive_threshold },
    { "palette_dither_modes", test_palette_dither_modes },
    { "verify_clean_file", test_verify_clean_file },
    { "verify_corrupted_video", test_verify_corrupted_video },
    { "verify_metadata_mismatch", test_verify_metadata_mismatch },
    { "verify_every_audio_track", test_verify_every_audio_track },
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
