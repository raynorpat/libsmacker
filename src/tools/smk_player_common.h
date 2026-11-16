#ifndef SMK_PLAYER_COMMON_H
#define SMK_PLAYER_COMMON_H

struct smk_player_options {
    const char *path;
    unsigned long frame_limit; /* 0 means play to the end */
};

/* Parse "[--frames N] file.smk". Returns 0 on success, 1 after printing
 * help, and -1 after printing an error. */
int smk_player_parse_options(int argc, char *argv[],
                             struct smk_player_options *out);

/* Largest centred rect inside the window with the source aspect ratio.
 * Returns an all-zero rect for a zero-size window or source. */
void smk_player_letterbox(int win_w, int win_h,
                          unsigned long src_w, unsigned long src_h,
                          int *x, int *y, int *w, int *h);

struct smk_palette_cache {
    unsigned char last[256 * 3];
    int valid;
};

/* 1 when the palette differs from the cache (or the cache is empty). */
int smk_palette_changed(struct smk_palette_cache *cache,
                        const unsigned char *palette);
void smk_palette_store(struct smk_palette_cache *cache,
                       const unsigned char *palette);

#endif
