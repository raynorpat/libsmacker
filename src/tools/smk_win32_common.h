#ifndef SMK_WIN32_COMMON_H
#define SMK_WIN32_COMMON_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>

#include "smk_player_common.h"
#include "smacker.h"

struct smk_win32_render_ops {
    int  (*init)(void *ctx, HWND hwnd, unsigned long w, unsigned long h);
    int  (*resize)(void *ctx, int client_w, int client_h);
    /* 0 presented, 1 recoverable skip, -1 fatal. */
    int  (*present)(void *ctx, const unsigned char *frame,
                    const unsigned char *palette, const RECT *viewport,
                    int y_double);
    void (*shutdown)(void *ctx);
};

/* Create the window, own audio/input/pacing, and run to completion. Calls
 * ops->init once and ops->shutdown once; returns 0 clean or non-zero fatal. */
int smk_win32_play(smk s, const struct smk_player_options *opt,
                   const char *title, unsigned long w, unsigned long h,
                   int y_double, const struct smk_win32_render_ops *ops,
                   void *ctx);

#endif
