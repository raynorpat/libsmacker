add_executable(smk_tests
  test_main.c
  test_util.c
  test_cli.c
  test_bitio.c
  test_huffman.c
  test_image_tga.c
  test_image_bmp.c
  test_image_pcx.c
  test_wav.c
  test_image_dir.c
  test_avi.c
  test_quantize.c
  test_video.c
  test_roundtrip_still.c
  test_roundtrip_audio.c
  test_roundtrip_delta.c
  test_roundtrip_palette.c
  test_verify.c
  test_roundtrip_avi.c
  ${SMACKER_ENCODER_SOURCES}
  ${SMACKER_TOOL_INPUT_SOURCES})

target_include_directories(smk_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/encoder
  ${CMAKE_SOURCE_DIR}/src/tools/build_smk
  ${CMAKE_CURRENT_SOURCE_DIR})

smacker_set_compile_settings(smk_tests)
target_link_libraries(smk_tests PRIVATE ${SMACKER_LINK_TARGET})

add_test(NAME smk_tests COMMAND smk_tests)
add_test(NAME build_smk_help COMMAND build_smk --help)
add_test(NAME build_smk_usage COMMAND build_smk)
set_tests_properties(build_smk_usage PROPERTIES WILL_FAIL TRUE)
add_test(NAME build_smk_version COMMAND build_smk --version)
set_tests_properties(build_smk_version PROPERTIES
  PASS_REGULAR_EXPRESSION "build_smk 1\\.0\\.0")
add_test(NAME build_smk_bad_option COMMAND build_smk --palette nope)
set_tests_properties(build_smk_bad_option PROPERTIES WILL_FAIL TRUE)
add_test(NAME build_smk_missing_input
  COMMAND build_smk ${CMAKE_CURRENT_BINARY_DIR}/missing-input-directory)
set_tests_properties(build_smk_missing_input PROPERTIES WILL_FAIL TRUE)
