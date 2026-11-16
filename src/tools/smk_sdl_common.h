#ifndef SMK_SDL_COMMON_H
#define SMK_SDL_COMMON_H

#include "smk_player_common.h"
#include "smacker.h"

#ifdef SMK_USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

struct smk_sdl_player {
    SDL_Window *window;
};

struct smk_sdl_render_ops {
    /* 0 presented, 1 recoverable skip, -1 fatal. */
    int (*present)(void *ctx, const unsigned char *frame,
                   const unsigned char *palette, int drawable_w,
                   int drawable_h, int y_double);
    void (*shutdown)(void *ctx);
};

/* Create the SDL window for a renderer. Returns 0 on success. */
int smk_sdl_open(struct smk_sdl_player *p, const char *title,
                 unsigned long w, unsigned long h, int y_double,
                 Uint32 extra_flags);

/* Run playback to the end, the frame limit, or quit. Owns audio and input.
 * Calls ops->shutdown exactly once before returning. */
int smk_sdl_run(struct smk_sdl_player *p, smk s,
                const struct smk_player_options *opt,
                const struct smk_sdl_render_ops *ops, void *ctx);

void smk_sdl_close(struct smk_sdl_player *p);

#endif
