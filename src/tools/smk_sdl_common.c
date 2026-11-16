#include "smk_sdl_common.h"

#include <stdio.h>
#include <stdlib.h>

/* Event names and key access moved in SDL3. */
#ifdef SMK_USE_SDL3
#define SMK_EVENT_QUIT		SDL_EVENT_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_EVENT_KEY_DOWN
#define SMK_EVENT_KEY(event)	((event).key.key)
#define SMK_KEY_Q		SDLK_Q
typedef SDL_AudioStream *smk_audio_out;
#ifndef SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK SDL_AUDIO_DEVICE_DEFAULT_OUTPUT
#endif
#else
#define SMK_EVENT_QUIT		SDL_QUIT
#define SMK_EVENT_KEY_DOWN	SDL_KEYDOWN
#define SMK_EVENT_KEY(event)	((event).key.keysym.sym)
#define SMK_KEY_Q		SDLK_q
typedef SDL_AudioDeviceID smk_audio_out;
#endif

static int
audio_init(void)
{
#ifdef SMK_USE_SDL3
	return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 0 : -1;
#else
	return SDL_InitSubSystem(SDL_INIT_AUDIO) == 0 ? 0 : -1;
#endif
}

static int
audio_open(smk_audio_out *out, unsigned long rate, int channels, int bitdepth)
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
audio_pause(smk_audio_out *out, int pause)
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
audio_clear(smk_audio_out *out)
{
#ifdef SMK_USE_SDL3
	SDL_ClearAudioStream(*out);
#else
	SDL_ClearQueuedAudio(*out);
#endif
}

static void
audio_queue(smk_audio_out *out, const unsigned char *data, unsigned long size)
{
#ifdef SMK_USE_SDL3
	SDL_PutAudioStreamData(*out, data, (int) size);
#else
	SDL_QueueAudio(*out, data, (Uint32) size);
#endif
}

static void
audio_close(smk_audio_out *out)
{
#ifdef SMK_USE_SDL3
	SDL_DestroyAudioStream(*out);
#else
	SDL_CloseAudioDevice(*out);
#endif
}

static void
wait_until(double target_ms)
{
	double now = (double) SDL_GetTicks();

	if (target_ms > now)
		SDL_Delay((Uint32) (target_ms - now + 0.999));
}

static void
drawable_size(SDL_Window *window, int *w, int *h)
{
#ifdef SMK_USE_SDL3
	if (!SDL_GetWindowSizeInPixels(window, w, h)) {
		*w = 0;
		*h = 0;
	}
#else
	SDL_GL_GetDrawableSize(window, w, h);
#endif
}

static int
is_redraw_event(const SDL_Event *event)
{
#ifdef SMK_USE_SDL3
	return event->type == SDL_EVENT_WINDOW_RESIZED ||
		event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
		event->type == SDL_EVENT_WINDOW_EXPOSED;
#else
	return event->type == SDL_WINDOWEVENT &&
		(event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
		 event->window.event == SDL_WINDOWEVENT_EXPOSED);
#endif
}

int
smk_sdl_open(struct smk_sdl_player *p, const char *title,
	unsigned long w, unsigned long h, int y_double, Uint32 extra_flags)
{
	p->window = NULL;

#ifdef SMK_USE_SDL3
	if (!SDL_Init(SDL_INIT_VIDEO))
		return -1;
	p->window = SDL_CreateWindow(title, (int) w,
		(int) (y_double ? h * 2 : h),
		extra_flags | SDL_WINDOW_RESIZABLE);
#else
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		return -1;
	p->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED, (int) w,
		(int) (y_double ? h * 2 : h),
		extra_flags | SDL_WINDOW_RESIZABLE);
#endif

	return p->window != NULL ? 0 : -1;
}

void
smk_sdl_close(struct smk_sdl_player *p)
{
	if (p->window != NULL)
		SDL_DestroyWindow(p->window);
	p->window = NULL;
	SDL_Quit();
}

int
smk_sdl_run(struct smk_sdl_player *p, smk s,
	const struct smk_player_options *opt,
	const struct smk_sdl_render_ops *ops, void *ctx)
{
	unsigned long	w = 0, h = 0, frames = 0, presented = 0;
	unsigned long	last_frame = (unsigned long) -1;
	double		usf = 0.0, frame_ms, next_ms;
	unsigned char	y_scale = SMK_FLAG_Y_NONE;
	int		y_double;
	int		status = 0;
	int		quit = 0;
	int		paused = 0;
	int		redraw = 1;
	int		has_audio = 0;
	int		track = -1;
	int		i;
	int		draw_w = 0, draw_h = 0;
	smk_audio_out	audio;
	unsigned char	track_mask = 0;
	unsigned char	channels[7], bitdepth[7];
	unsigned long	rates[7];
	char		rc;

	smk_info_all(s, NULL, &frames, &usf);
	smk_info_video(s, &w, &h, &y_scale);
	if (usf <= 0.0)
		usf = 1000000.0 / 30.0;
	frame_ms = usf / 1000.0;
	y_double = y_scale != SMK_FLAG_Y_NONE;

	printf("Opened file %s\nWidth: %lu\nHeight: %lu\nFrames: %lu\nFPS: %f\n",
		opt->path, w, h, frames, 1000000.0 / usf);
	if (y_double)
		printf("Y-scale: doubled\n");

	/* Enable audio before the first decode so frame 0's audio is kept. */
	smk_info_audio(s, &track_mask, channels, bitdepth, rates);
	for (i = 0; i < 7; i++) {
		if (track_mask & (1u << i)) {
			track = i;
			break;
		}
	}

	if (track >= 0) {
		if (audio_init() != 0) {
			fprintf(stderr,
				"SDL audio init failed, playing silently: %s\n",
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
				fprintf(stderr,
					"SDL audio open failed, playing silently: %s\n",
					SDL_GetError());
			}
		}
	}

	smk_enable_video(s, 1);
	rc = smk_first(s);
	if (rc == SMK_ERROR) {
		fprintf(stderr, "Failed to decode the first frame of %s.\n",
			opt->path);
		status = 1;
		goto done;
	}

	next_ms = (double) SDL_GetTicks();

	while (!quit) {
		SDL_Event	event;

		while (SDL_PollEvent(&event)) {
			int	key;

			if (event.type == SMK_EVENT_QUIT) {
				quit = 1;
				break;
			}
			if (is_redraw_event(&event)) {
				redraw = 1;
				continue;
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

		if (!paused || redraw) {
			int		pr;

			draw_w = 0;
			draw_h = 0;
			drawable_size(p->window, &draw_w, &draw_h);
			pr = ops->present(ctx, smk_get_video(s),
				smk_get_palette(s), draw_w, draw_h, y_double);
			if (pr < 0) {
				status = 1;
				break;
			}
			if (pr > 0) {
				/* Recoverable: retry the same frame shortly. */
				redraw = 1;
				SDL_Delay(10);
				continue;
			}
			redraw = 0;

			{
				unsigned long	shown = 0;

				smk_info_all(s, &shown, NULL, NULL);
				if (shown != last_frame) {
					last_frame = shown;
					presented++;
				}
			}
			if (opt->frame_limit != 0 &&
				presented >= opt->frame_limit)
				break;

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
	if (ops->shutdown != NULL)
		ops->shutdown(ctx);

	return status;
}
