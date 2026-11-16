#include "test.h"
#include "smk_player_common.h"

void test_letterbox_fit_width(void)
{
    int x, y, w, h;
    smk_player_letterbox(500, 300, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 50);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 400);
    SMK_ASSERT_EQ_INT(h, 300);
}

void test_letterbox_exact(void)
{
    int x, y, w, h;
    smk_player_letterbox(640, 480, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 640);
    SMK_ASSERT_EQ_INT(h, 480);
}

void test_letterbox_fit_height(void)
{
    int x, y, w, h;
    smk_player_letterbox(100, 100, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 12);
    SMK_ASSERT_EQ_INT(w, 100);
    SMK_ASSERT_EQ_INT(h, 75);
}

void test_letterbox_zero_size(void)
{
    int x = -1, y = -1, w = -1, h = -1;
    smk_player_letterbox(0, 0, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 0);
    SMK_ASSERT_EQ_INT(h, 0);
}

void test_letterbox_y_double(void)
{
    int x, y, w, h;
    smk_player_letterbox(640, 480, 320, 480, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 160);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 320);
    SMK_ASSERT_EQ_INT(h, 480);
}

void test_letterbox_rounding(void)
{
    int x, y, w, h;
    smk_player_letterbox(801, 600, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 800);
    SMK_ASSERT_EQ_INT(h, 600);
}

void test_palette_cache(void)
{
    struct smk_palette_cache cache;
    unsigned char palette[256 * 3];

    memset(&cache, 0, sizeof(cache));
    memset(palette, 0x11, sizeof(palette));
    SMK_ASSERT_EQ_INT(smk_palette_changed(&cache, palette), 1);
    smk_palette_store(&cache, palette);
    SMK_ASSERT_EQ_INT(smk_palette_changed(&cache, palette), 0);
    palette[17] ^= 0xff;
    SMK_ASSERT_EQ_INT(smk_palette_changed(&cache, palette), 1);
}

void test_parse_options_success(void)
{
    char *argv1[] = { (char *)"prog", (char *)"--frames", (char *)"7",
                      (char *)"clip.smk" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(4, argv1, &opt), 0);
    SMK_ASSERT_STR_EQ(opt.path, "clip.smk");
    SMK_ASSERT_EQ_INT(opt.frame_limit, 7);
}

void test_parse_options_default_limit(void)
{
    char *argv1[] = { (char *)"prog", (char *)"clip.smk" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(2, argv1, &opt), 0);
    SMK_ASSERT_STR_EQ(opt.path, "clip.smk");
    SMK_ASSERT_EQ_INT(opt.frame_limit, 0);
}

void test_parse_options_rejects_extra_path(void)
{
    char *argv1[] = { (char *)"prog", (char *)"a.smk", (char *)"b.smk" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(3, argv1, &opt), -1);
}

void test_parse_options_frames_missing(void)
{
    char *argv1[] = { (char *)"prog", (char *)"--frames" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(2, argv1, &opt), -1);
}
