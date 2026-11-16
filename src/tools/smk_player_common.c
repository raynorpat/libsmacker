#include "smk_player_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--frames N] file.smk\n",
            prog != NULL ? prog : "player");
}

int
smk_player_parse_options(int argc, char *argv[],
                         struct smk_player_options *out)
{
    int i;

    out->path = NULL;
    out->frame_limit = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 1;
        }

        if (strcmp(argv[i], "--frames") == 0) {
            char *end = NULL;
            unsigned long v;

            if (i + 1 >= argc) {
                fputs("--frames needs a value\n", stderr);
                usage(argv[0]);
                return -1;
            }
            if (argv[i + 1][0] == '-') {
                fprintf(stderr, "invalid --frames value: %s\n", argv[i + 1]);
                usage(argv[0]);
                return -1;
            }
            errno = 0;
            v = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                fprintf(stderr, "invalid --frames value: %s\n", argv[i]);
                usage(argv[0]);
                return -1;
            }
            out->frame_limit = v;
            continue;
        }

        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }

        if (out->path != NULL) {
            fputs("only one input file may be given\n", stderr);
            usage(argv[0]);
            return -1;
        }
        out->path = argv[i];
    }

    if (out->path == NULL) {
        usage(argv[0]);
        return -1;
    }

    return 0;
}

void
smk_player_letterbox(int win_w, int win_h,
                     unsigned long src_w, unsigned long src_h,
                     int *x, int *y, int *w, int *h)
{
    double scale_x, scale_y, scale;
    int rw, rh;

    *x = 0;
    *y = 0;
    *w = 0;
    *h = 0;
    if (win_w <= 0 || win_h <= 0 || src_w == 0 || src_h == 0)
        return;

    scale_x = (double) win_w / (double) src_w;
    scale_y = (double) win_h / (double) src_h;
    scale = scale_x < scale_y ? scale_x : scale_y;

    rw = (int) ((double) src_w * scale + 0.5);
    rh = (int) ((double) src_h * scale + 0.5);
    if (rw < 1)
        rw = 1;
    if (rw > win_w)
        rw = win_w;
    if (rh < 1)
        rh = 1;
    if (rh > win_h)
        rh = win_h;

    *w = rw;
    *h = rh;
    *x = (win_w - rw) / 2;
    *y = (win_h - rh) / 2;
}

int
smk_palette_changed(struct smk_palette_cache *cache,
                    const unsigned char *palette)
{
    return !cache->valid ||
           memcmp(cache->last, palette, sizeof(cache->last)) != 0;
}

void
smk_palette_store(struct smk_palette_cache *cache,
                  const unsigned char *palette)
{
    memcpy(cache->last, palette, sizeof(cache->last));
    cache->valid = 1;
}
