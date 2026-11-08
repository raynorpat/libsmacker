/*
 * libsmacker - A C library for decoding .smk Smacker Video files
 * Copyright (C) 2012-2020 Greg Kennedy
 *
 * See smacker.h for more information.
 *
 * smk_sdl_player.c - SDL2/SDL3 playback example
 *
 * Build with the static libsmacker library:
 *   cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON
 *   cmake --build build
 *
 * CMake selects SDL3 when available and falls back to SDL2. Force one with
 * -DSMACKER_SDL_VERSION=3 or -DSMACKER_SDL_VERSION=2.
 *
 * Usage:
 *   smk_sdl_player file.smk
 *
 * Keys: Esc/q quit, Space pause, Left/Right seek one second.
 */

#include "smacker.h"

#ifdef SMK_USE_SDL3
#include <SDL3/SDL.h>
#ifndef SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK SDL_AUDIO_DEVICE_DEFAULT_OUTPUT
#endif
#else
#include <SDL.h>
#endif

#include <stdio.h>
#include <stdlib.h>

/* Event names and key access moved in SDL3. */
#ifdef SMK_USE_SDL3
#define SMK_EVENT_QUIT		SDL_EVENT_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_EVENT_KEY_DOWN
#define SMK_EVENT_KEY(event)	((event).key.key)
#define SMK_KEY_Q		SDLK_Q
typedef SDL_AudioStream *smk_audio_out;
#else
#define SMK_EVENT_QUIT		SDL_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_KEYDOWN
#define SMK_EVENT_KEY(event)	((event).key.keysym.sym)
#define SMK_KEY_Q		SDLK_q
typedef SDL_AudioDeviceID smk_audio_out;
#endif

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

/* Initialize the audio subsystem. Returns 0 on success. */
static int
audio_init(void)
{
#ifdef SMK_USE_SDL3
	return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 0 : -1;
#else
	return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 ? 0 : -1;
#endif
}

/* Open an audio output device matching the PCM format libsmacker produces:
 * unsigned 8-bit, or little-endian signed 16-bit. Returns 0 on success. */
static int
audio_open(smk_audio_out * out, unsigned long rate, int channels, int bitdepth)
{
#ifdef SMK_USE_SDL3
	SDL_AudioSpec	spec;

	SDL_zero(spec);
	spec.format = (bitdepth == 16) ? SDL_AUDIO_S16LE : SDL_AUDIO_U8;
	spec.channels = channels;
	spec.freq = (int) rate;

	*out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&spec, NULL, NULL);
	if (*out == NULL)
		return -1;

	SDL_ResumeAudioStreamDevice(*out);
	return 0;
#else
	SDL_AudioSpec	want;

	SDL_zero(want);
	want.freq = (int) rate;
	want.format = (bitdepth == 16) ? AUDIO_S16LSB : AUDIO_U8;
	want.channels = (Uint8) channels;
	want.samples = 1024;
	want.callback = NULL;

	*out = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if (*out == 0)
		return -1;

	SDL_PauseAudioDevice(*out, 0);
	return 0;
#endif
}

static void
audio_pause(smk_audio_out * out, int pause)
{
#ifdef SMK_USE_SDL3
	if (pause)
		SDL_PauseAudioStreamDevice(*out);
	else
		SDL_ResumeAudioStreamDevice(*out);
#else
	SDL_PauseAudioDevice(*out, pause);
#endif
}

static void
audio_clear(smk_audio_out * out)
{
#ifdef SMK_USE_SDL3
	SDL_ClearAudioStream(*out);
#else
	SDL_ClearQueuedAudio(*out);
#endif
}

static void
audio_queue(smk_audio_out * out, const unsigned char * data, unsigned long size)
{
#ifdef SMK_USE_SDL3
	SDL_PutAudioStreamData(*out, data, (int) size);
#else
	SDL_QueueAudio(*out, data, (Uint32) size);
#endif
}

static void
audio_close(smk_audio_out * out)
{
#ifdef SMK_USE_SDL3
	SDL_DestroyAudioStream(*out);
#else
	SDL_CloseAudioDevice(*out);
#endif
}

/* Convert the 8-bit palette image to a 32-bit ARGB buffer for the texture. */
static void
convert_frame(const unsigned char * pal, const unsigned char * video,
	Uint32 * pixels, unsigned long count)
{
	unsigned long	i;

	for (i = 0; i < count; i++) {
		unsigned int	c = (unsigned int) video[i] * 3;

		pixels[i] = 0xFF000000u |
			((Uint32) pal[c] << 16) |
			((Uint32) pal[c + 1] << 8) |
			(Uint32) pal[c + 2];
	}
}

/* Sleep until the given SDL tick count, if it is still in the future. */
static void
wait_until(double target_ms)
{
	double		now = (double) SDL_GetTicks();

	if (target_ms > now)
		SDL_Delay((Uint32) (target_ms - now + 0.999));
}

static void
present(SDL_Renderer * renderer, SDL_Texture * texture)
{
	SDL_RenderClear(renderer);
#ifdef SMK_USE_SDL3
	SDL_RenderTexture(renderer, texture, NULL, NULL);
#else
	SDL_RenderCopy(renderer, texture, NULL, NULL);
#endif
	SDL_RenderPresent(renderer);
}

int
main(int argc, char *argv[])
{
	const char     *path;
	smk		s;
	SDL_Window     *window;
	SDL_Renderer   *renderer;
	SDL_Texture    *texture;
	Uint32         *pixels;
	smk_audio_out	audio;
	unsigned long	w = 0, h = 0, frames = 0;
	double		usf = 0.0;
	unsigned char	track_mask = 0;
	unsigned char	channels[7], bitdepth[7];
	unsigned long	rates[7];
	int		track = -1;
	int		has_audio = 0;
	int		i;
	int		quit = 0;
	int		paused = 0;
	int		redraw = 1;
	int		status = 0;
	double		frame_ms;
	double		next_ms;
	char		rc;
	char		title[512];

	if (argc != 2) {
		fprintf(stderr, "Usage: %s file.smk\n", argv[0]);
		return 1;
	}
	path = argv[1];

	s = smk_open_file(path, SMK_MODE_DISK);
	if (s == NULL) {
		fprintf(stderr, "Errors encountered opening %s, exiting.\n", path);
		return 1;
	}

	smk_info_all(s, NULL, &frames, &usf);
	smk_info_video(s, &w, &h, NULL);
	if (w == 0 || h == 0) {
		fprintf(stderr, "%s has no video track, exiting.\n", path);
		smk_close(s);
		return 1;
	}
	if (usf <= 0.0)
		usf = 1000000.0 / 30.0;
	frame_ms = usf / 1000.0;

	printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
		path, w, h, frames, 1000000.0 / usf);

	if (video_init() != 0) {
		fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
		smk_close(s);
		return 1;
	}

	window = NULL;
	renderer = NULL;
	texture = NULL;
	pixels = NULL;

	snprintf(title, sizeof(title), "libsmacker SDL player - %s", path);
#ifdef SMK_USE_SDL3
	window = SDL_CreateWindow(title, (int) w, (int) h, SDL_WINDOW_RESIZABLE);
	if (window != NULL)
		renderer = SDL_CreateRenderer(window, NULL);
#else
	window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, (int) w, (int) h, SDL_WINDOW_RESIZABLE);
	if (window != NULL) {
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
		if (renderer == NULL)
			renderer = SDL_CreateRenderer(window, -1, 0);
	}
#endif
	if (window == NULL || renderer == NULL) {
		fprintf(stderr, "SDL window/renderer creation failed: %s\n",
			SDL_GetError());
		status = 1;
		goto done;
	}

	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING, (int) w, (int) h);
	pixels = malloc((size_t) w * h * sizeof(*pixels));
	if (texture == NULL || pixels == NULL) {
		fprintf(stderr, "Failed to allocate video buffers: %s\n",
			SDL_GetError());
		status = 1;
		goto done;
	}

	/* Keep the aspect ratio when the window is resized. */
#ifdef SMK_USE_SDL3
	SDL_SetRenderLogicalPresentation(renderer, (int) w, (int) h,
		SDL_LOGICAL_PRESENTATION_LETTERBOX);
#else
	SDL_RenderSetLogicalSize(renderer, (int) w, (int) h);
#endif

	/* Play the first audio track that exists (normally track 0). */
	smk_info_audio(s, &track_mask, channels, bitdepth, rates);
	for (i = 0; i < 7; i++) {
		if (track_mask & (1u << i)) {
			track = i;
			break;
		}
	}

	if (track >= 0) {
		if (audio_init() != 0) {
			fprintf(stderr, "SDL audio init failed, playing silently: %s\n",
				SDL_GetError());
		} else {
			smk_enable_audio(s, (unsigned char) track, 1);
			if (audio_open(&audio, rates[track], channels[track],
				bitdepth[track]) == 0) {
				has_audio = 1;
				printf("Audio track %d: %u bits, %u channels, %luhz\n",
					track, (unsigned int) bitdepth[track],
					(unsigned int) channels[track],
					rates[track]);
			} else {
				fprintf(stderr, "SDL audio open failed, playing "
					"silently: %s\n", SDL_GetError());
			}
		}
	}

	/* Turn on video decoding and unpack the first frame. */
	smk_enable_video(s, 1);
	rc = smk_first(s);
	if (rc == SMK_ERROR) {
		fprintf(stderr, "Failed to decode the first frame of %s.\n", path);
		status = 1;
		goto done;
	}

	next_ms = (double) SDL_GetTicks();

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
			if (key == SDLK_ESCAPE || key == SMK_KEY_Q) {
				quit = 1;
			} else if (key == SDLK_SPACE) {
				paused = !paused;
				if (has_audio)
					audio_pause(&audio, paused);
				next_ms = (double) SDL_GetTicks();
			} else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
				unsigned long	cur = 0;
				long		step;
				long		target;

				smk_info_all(s, &cur, &frames, NULL);
				step = (long) (1000000.0 / usf + 0.5);
				if (step < 1)
					step = 1;
				target = (long) cur +
					(key == SDLK_LEFT ? -step : step);
				if (target < 0)
					target = 0;
				if (frames > 0 && target >= (long) frames)
					target = (long) frames - 1;

				if (smk_seek_keyframe(s,
					(unsigned long) target) == 0) {
					smk_info_all(s, &cur, &frames, NULL);
					rc = (cur + 1 >= frames) ? SMK_LAST
						: SMK_MORE;
					if (has_audio)
						audio_clear(&audio);
					next_ms = (double) SDL_GetTicks();
					redraw = 1;
				}
			}
		}
		if (quit)
			break;

		/* Draw and (while playing) feed the audio queue. */
		if (!paused || redraw) {
			convert_frame(smk_get_palette(s), smk_get_video(s),
				pixels, w * h);
			SDL_UpdateTexture(texture, NULL, pixels,
				(int) (w * sizeof(*pixels)));
			present(renderer, texture);
			redraw = 0;

			if (has_audio && !paused) {
				unsigned long	size = smk_get_audio_size(s,
					(unsigned char) track);

				if (size > 0)
					audio_queue(&audio,
						smk_get_audio(s,
						(unsigned char) track),
						size);
			}
		}

		if (paused) {
			SDL_Delay(10);
			continue;
		}

		/* Hold the last frame for one interval, then stop. */
		if (rc == SMK_LAST) {
			next_ms += frame_ms;
			wait_until(next_ms);
			break;
		}

		next_ms += frame_ms;
		wait_until(next_ms);

		rc = smk_next(s);
		if (rc == SMK_ERROR || rc == SMK_DONE)
			break;
	}

done:
	if (has_audio)
		audio_close(&audio);
	free(pixels);
	if (texture != NULL)
		SDL_DestroyTexture(texture);
	if (renderer != NULL)
		SDL_DestroyRenderer(renderer);
	if (window != NULL)
		SDL_DestroyWindow(window);
	SDL_Quit();
	smk_close(s);

	return status;
}
