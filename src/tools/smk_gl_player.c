/*
 * libsmacker - A C library for decoding .smk Smacker Video files
 * Copyright (C) 2012-2020 Greg Kennedy
 *
 * See smacker.h for more information.
 *
 * smk_gl_player.c - OpenGL 3.3 playback example
 *
 * The frame stays palettized: an 8-bit index texture and a 256x1 palette
 * texture go to the GPU, and the shaders resolve the palette and scale the
 * image. See smk_sdl_player.c for the CPU-side reference player.
 *
 * Build with:
 *   cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON
 *   cmake --build build
 *
 * Usage:
 *   smk_gl_player [--frames N] file.smk
 *
 * Keys: Esc/q quit. --frames 0 (the default) plays to the end.
 */

#include "smacker.h"

#ifdef SMK_USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#else
#include <SDL.h>
#include <SDL_opengl.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Event names and key access moved in SDL3. */
#ifdef SMK_USE_SDL3
#define SMK_EVENT_QUIT		SDL_EVENT_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_EVENT_KEY_DOWN
#define SMK_EVENT_KEY(event)	((event).key.key)
#define SMK_KEY_Q		SDLK_Q
#else
#define SMK_EVENT_QUIT		SDL_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_KEYDOWN
#define SMK_EVENT_KEY(event)	((event).key.keysym.sym)
#define SMK_KEY_Q		SDLK_q
#endif

struct options {
	const char     *path;
	unsigned long	frame_limit;	/* 0 means play to the end */
};

static void
usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [--frames N] file.smk\n",
		prog != NULL ? prog : "smk_gl_player");
}

/* Parse the command line. Returns 0 on success, 1 after printing help, and
 * -1 after printing an error. */
static int
parse_options(int argc, char *argv[], struct options *out)
{
	int		i;

	out->path = NULL;
	out->frame_limit = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
			strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 1;
		}

		if (strcmp(argv[i], "--frames") == 0) {
			char           *end = NULL;
			unsigned long	v;

			if (i + 1 >= argc) {
				fputs("--frames needs a value\n", stderr);
				usage(argv[0]);
				return -1;
			}
			if (argv[i + 1][0] == '-') {
				fprintf(stderr, "invalid --frames value: %s\n",
					argv[i + 1]);
				usage(argv[0]);
				return -1;
			}
			errno = 0;
			v = strtoul(argv[++i], &end, 10);
			if (errno != 0 || end == argv[i] || *end != '\0') {
				fprintf(stderr, "invalid --frames value: %s\n",
					argv[i]);
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

/* Initialize the video subsystem. Returns 0 on success. */
static int
video_init(void)
{
#ifdef SMK_USE_SDL3
	return SDL_Init(SDL_INIT_VIDEO) ? 0 : -1;
#else
	return SDL_Init(SDL_INIT_VIDEO) == 0 ? 0 : -1;
#endif
}

/* Request an OpenGL 3.3 core context and make it current. */
static int
gl_context_create(SDL_Window *window, SDL_GLContext *context)
{
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
		SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

	*context = SDL_GL_CreateContext(window);
	if (*context == NULL)
		return -1;

#ifdef SMK_USE_SDL3
	if (!SDL_GL_MakeCurrent(window, *context))
		return -1;
#else
	if (SDL_GL_MakeCurrent(window, *context) != 0)
		return -1;
#endif

	SDL_GL_SetSwapInterval(0);
	return 0;
}

static const char *
gl_string(GLenum name)
{
	const GLubyte  *value = glGetString(name);

	return value != NULL ? (const char *) value : "(unknown)";
}

int
main(int argc, char *argv[])
{
	struct options	opt;
	smk		s = NULL;
	SDL_Window     *window = NULL;
	SDL_GLContext	context = NULL;
	unsigned long	w = 0, h = 0, frames = 0;
	double		usf = 0.0;
	unsigned char	y_scale = SMK_FLAG_Y_NONE;
	int		y_double;
	int		status = 0;
	int		quit = 0;
	unsigned long	presented = 0;
	double		frame_ms;
	char		title[512];
	char		rc;
	int		parsed;

	parsed = parse_options(argc, argv, &opt);
	if (parsed != 0)
		return parsed > 0 ? 0 : 1;

	s = smk_open_file(opt.path, SMK_MODE_DISK);
	if (s == NULL) {
		fprintf(stderr, "Errors encountered opening %s, exiting.\n",
			opt.path);
		return 1;
	}

	smk_info_all(s, NULL, &frames, &usf);
	smk_info_video(s, &w, &h, &y_scale);
	if (w == 0 || h == 0) {
		fprintf(stderr, "%s has no video track, exiting.\n", opt.path);
		smk_close(s);
		return 1;
	}
	if (usf <= 0.0)
		usf = 1000000.0 / 30.0;
	frame_ms = usf / 1000.0;
	y_double = y_scale != SMK_FLAG_Y_NONE;

	printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
		opt.path, w, h, frames, 1000000.0 / usf);
	if (y_double)
		printf("Y-scale: doubled\n");

	if (video_init() != 0) {
		fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
		status = 1;
		goto done;
	}

	snprintf(title, sizeof(title), "libsmacker OpenGL player - %s",
		opt.path);
#ifdef SMK_USE_SDL3
	window = SDL_CreateWindow(title, (int) w,
		(int) (y_double ? h * 2 : h),
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
#else
	window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, (int) w,
		(int) (y_double ? h * 2 : h),
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
#endif
	if (window == NULL) {
		fprintf(stderr, "SDL window creation failed: %s\n",
			SDL_GetError());
		status = 1;
		goto done;
	}

	if (gl_context_create(window, &context) != 0) {
		fprintf(stderr, "OpenGL 3.3 core context creation failed: %s\n",
			SDL_GetError());
		status = 1;
		goto done;
	}

	printf("GL_VERSION: %s\nGL_RENDERER: %s\n",
		gl_string(GL_VERSION), gl_string(GL_RENDERER));

	smk_enable_video(s, 1);
	rc = smk_first(s);
	if (rc == SMK_ERROR) {
		fprintf(stderr, "Failed to decode the first frame of %s.\n",
			opt.path);
		status = 1;
		goto done;
	}

	while (!quit) {
		SDL_Event	event;

		while (SDL_PollEvent(&event)) {
			int		key;

			if (event.type == SMK_EVENT_QUIT) {
				quit = 1;
				break;
			}
			if (event.type != SMK_EVENT_KEY_DOWN)
				continue;

			key = SMK_EVENT_KEY(event);
			if (key == SDLK_ESCAPE || key == SMK_KEY_Q)
				quit = 1;
		}
		if (quit)
			break;

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		SDL_GL_SwapWindow(window);

		presented++;
		if (opt.frame_limit != 0 && presented >= opt.frame_limit)
			break;

		/* Hold the last frame for one interval, then stop. */
		if (rc == SMK_LAST) {
			SDL_Delay((Uint32) (frame_ms + 0.999));
			break;
		}

		SDL_Delay((Uint32) (frame_ms + 0.999));

		rc = smk_next(s);
		if (rc == SMK_ERROR || rc == SMK_DONE)
			break;
	}

done:
	if (context != NULL)
		SDL_GL_DeleteContext(context);
	if (window != NULL)
		SDL_DestroyWindow(window);
	SDL_Quit();
	if (s != NULL)
		smk_close(s);

	return status;
}
