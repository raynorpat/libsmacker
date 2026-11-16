#include "smk_win32_common.h"

#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMK_WIN_CLASS "smk_player_window"
#define SMK_AUDIO_CHUNKS 4

struct win_state {
	HWND	hwnd;
	int	quit;
	int	pause_toggle;
	int	seek_seconds;
	int	resize_pending;
	int	redraw;
	int	client_w;
	int	client_h;
};

static struct win_state *g_win;

static LRESULT CALLBACK
window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (g_win != NULL) {
		switch (msg) {
		case WM_KEYDOWN:
			switch ((int) wp) {
			case VK_ESCAPE:
			case 'Q':
				g_win->quit = 1;
				return 0;
			case VK_SPACE:
				g_win->pause_toggle++;
				return 0;
			case VK_LEFT:
				g_win->seek_seconds--;
				return 0;
			case VK_RIGHT:
				g_win->seek_seconds++;
				return 0;
			}
			break;
		case WM_SIZE:
			g_win->client_w = (int) LOWORD(lp);
			g_win->client_h = (int) HIWORD(lp);
			g_win->resize_pending = 1;
			return 0;
		case WM_PAINT:
			g_win->redraw = 1;
			ValidateRect(hwnd, NULL);
			return 0;
		case WM_CLOSE:
			g_win->quit = 1;
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

typedef BOOL (WINAPI *set_dpi_context_fn)(HANDLE);
typedef BOOL (WINAPI *set_dpi_aware_fn)(void);

static void
enable_dpi_awareness(void)
{
	HMODULE user32 = GetModuleHandleA("user32.dll");
	set_dpi_context_fn set_context;
	set_dpi_aware_fn set_aware;

	if (user32 == NULL)
		return;

	set_context = (set_dpi_context_fn) (void *) GetProcAddress(user32,
		"SetProcessDpiAwarenessContext");
	if (set_context != NULL &&
		set_context((HANDLE) (LONG_PTR) -4))	/* PER_MONITOR_AWARE_V2 */
		return;

	set_aware = (set_dpi_aware_fn) (void *) GetProcAddress(user32,
		"SetProcessDPIAware");
	if (set_aware != NULL)
		set_aware();
}

struct audio_chunk {
	WAVEHDR	header;
	int	busy;
};

struct win_audio {
	HWAVEOUT	device;
	struct audio_chunk chunks[SMK_AUDIO_CHUNKS];
	int		open;
};

static void
audio_reclaim(struct win_audio *a)
{
	int i;

	for (i = 0; i < SMK_AUDIO_CHUNKS; i++) {
		if (a->chunks[i].busy &&
			(a->chunks[i].header.dwFlags & WHDR_DONE)) {
			waveOutUnprepareHeader(a->device,
				&a->chunks[i].header, sizeof(WAVEHDR));
			free(a->chunks[i].header.lpData);
			memset(&a->chunks[i], 0, sizeof(a->chunks[i]));
		}
	}
}

static int
audio_open_win(struct win_audio *a, unsigned long rate, int channels,
	int bitdepth)
{
	WAVEFORMATEX fmt;
	MMRESULT result;

	memset(a, 0, sizeof(*a));
	memset(&fmt, 0, sizeof(fmt));
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = (WORD) channels;
	fmt.nSamplesPerSec = (DWORD) rate;
	fmt.wBitsPerSample = (WORD) bitdepth;
	fmt.nBlockAlign = (WORD) (channels * bitdepth / 8);
	fmt.nAvgBytesPerSec = (DWORD) (rate * fmt.nBlockAlign);

	result = waveOutOpen(&a->device, WAVE_MAPPER, &fmt, 0, 0,
		CALLBACK_NULL);
	if (result != MMSYSERR_NOERROR)
		return -1;
	a->open = 1;
	return 0;
}

static void
audio_queue(struct win_audio *a, const unsigned char *data,
	unsigned long size)
{
	void *copy;
	int i;

	if (!a->open || size == 0)
		return;

	audio_reclaim(a);
	for (i = 0; i < SMK_AUDIO_CHUNKS; i++) {
		if (!a->chunks[i].busy)
			break;
	}
	if (i == SMK_AUDIO_CHUNKS)
		return;		/* all buffers busy: drop this chunk */

	copy = malloc(size);
	if (copy == NULL)
		return;
	memcpy(copy, data, size);

	memset(&a->chunks[i].header, 0, sizeof(WAVEHDR));
	a->chunks[i].header.lpData = (LPSTR) copy;
	a->chunks[i].header.dwBufferLength = (DWORD) size;
	if (waveOutPrepareHeader(a->device, &a->chunks[i].header,
		sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
		free(copy);
		return;
	}
	if (waveOutWrite(a->device, &a->chunks[i].header,
		sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
		waveOutUnprepareHeader(a->device, &a->chunks[i].header,
			sizeof(WAVEHDR));
		free(copy);
		return;
	}
	a->chunks[i].busy = 1;
}

static void
audio_reset(struct win_audio *a)
{
	if (!a->open)
		return;
	waveOutReset(a->device);
	audio_reclaim(a);
}

static void
audio_pause(struct win_audio *a, int pause)
{
	if (!a->open)
		return;
	if (pause)
		waveOutPause(a->device);
	else
		waveOutRestart(a->device);
}

static void
audio_close(struct win_audio *a)
{
	if (!a->open)
		return;
	waveOutReset(a->device);
	audio_reclaim(a);
	waveOutClose(a->device);
	a->open = 0;
}

static void
wait_until(ULONGLONG target)
{
	ULONGLONG now = GetTickCount64();

	if (target > now)
		Sleep((DWORD) (target - now));
}

int
smk_win32_play(smk s, const struct smk_player_options *opt,
	const char *title, unsigned long w, unsigned long h, int y_double,
	const struct smk_win32_render_ops *ops, void *ctx)
{
	WNDCLASSEXA	wc;
	RECT		rect;
	MSG		msg;
	struct win_state state;
	struct win_audio	audio;
	unsigned long	frames = 0, presented = 0;
	unsigned long	last_frame = (unsigned long) -1;
	double		usf = 0.0, frame_ms, next_ms;
	unsigned char	y_scale = SMK_FLAG_Y_NONE;
	unsigned char	track_mask = 0;
	unsigned char	channels[7], bitdepth[7];
	unsigned long	rates[7];
	int		track = -1;
	int		has_audio = 0;
	int		paused = 0;
	int		status = 0;
	int		win_w, win_h;
	int		i;
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

	win_w = (int) w;
	win_h = (int) (y_double ? h * 2 : h);

	enable_dpi_awareness();
	timeBeginPeriod(1);

	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = window_proc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = SMK_WIN_CLASS;
	if (!RegisterClassExA(&wc)) {
		fputs("RegisterClassEx failed\n", stderr);
		timeEndPeriod(1);
		return 1;
	}

	rect.left = 0;
	rect.top = 0;
	rect.right = win_w;
	rect.bottom = win_h;
	AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

	memset(&state, 0, sizeof(state));
	g_win = &state;
	state.hwnd = CreateWindowExA(0, SMK_WIN_CLASS, title,
		WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
		rect.right - rect.left, rect.bottom - rect.top,
		NULL, NULL, wc.hInstance, NULL);
	if (state.hwnd == NULL) {
		fputs("CreateWindowEx failed\n", stderr);
		g_win = NULL;
		UnregisterClassA(SMK_WIN_CLASS, wc.hInstance);
		timeEndPeriod(1);
		return 1;
	}

	GetClientRect(state.hwnd, &rect);
	state.client_w = rect.right;
	state.client_h = rect.bottom;

	if (ops->init == NULL || ops->init(ctx, state.hwnd, w, h) != 0) {
		fputs("Renderer initialization failed\n", stderr);
		status = 1;
		goto done;
	}

	/* Enable audio before the first decode so frame 0 is kept. */
	smk_info_audio(s, &track_mask, channels, bitdepth, rates);
	for (i = 0; i < 7; i++) {
		if (track_mask & (1u << i)) {
			track = i;
			break;
		}
	}
	if (track >= 0) {
		smk_enable_audio(s, (unsigned char) track, 1);
		if (audio_open_win(&audio, rates[track], channels[track],
			bitdepth[track]) == 0) {
			has_audio = 1;
			printf("Audio track %d: %u bits, %u channels, %luhz\n",
				track, (unsigned int) bitdepth[track],
				(unsigned int) channels[track], rates[track]);
		} else {
			fputs("waveOut open failed, playing silently\n", stderr);
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

	next_ms = (double) GetTickCount64();

	while (!state.quit) {
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				state.quit = 1;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		if (state.quit)
			break;

		if (state.pause_toggle) {
			paused = !paused;
			if (has_audio)
				audio_pause(&audio, paused);
			state.pause_toggle = 0;
			next_ms = (double) GetTickCount64();
		}

		while (state.seek_seconds != 0) {
			unsigned long	cur = 0;
			long		step, target;
			int		direction = state.seek_seconds > 0 ? 1 : -1;

			smk_info_all(s, &cur, &frames, NULL);
			step = (long) (1000000.0 / usf + 0.5);
			if (step < 1)
				step = 1;
			target = (long) cur + direction * step;
			if (target < 0)
				target = 0;
			if (frames > 0 && target >= (long) frames)
				target = (long) frames - 1;

			if (smk_seek_keyframe(s, (unsigned long) target) == 0) {
				smk_info_all(s, &cur, &frames, NULL);
				rc = (cur + 1 >= frames) ? SMK_LAST : SMK_MORE;
				if (has_audio)
					audio_reset(&audio);
				next_ms = (double) GetTickCount64();
				state.redraw = 1;
			}
			state.seek_seconds -= direction;
		}

		if (state.resize_pending) {
			state.resize_pending = 0;
			if (ops->resize != NULL && state.client_w > 0 &&
				state.client_h > 0) {
				int r = ops->resize(ctx, state.client_w,
					state.client_h);

				if (r < 0) {
					status = 1;
					break;
				}
			}
			state.redraw = 1;
		}

		if (!paused || state.redraw) {
			RECT	viewport;
			int	x, y, vw, vh, pr;

			smk_player_letterbox(state.client_w, state.client_h,
				w, y_double ? h * 2 : h, &x, &y, &vw, &vh);
			viewport.left = x;
			viewport.top = y;
			viewport.right = x + vw;
			viewport.bottom = y + vh;

			pr = ops->present(ctx, smk_get_video(s),
				smk_get_palette(s), &viewport, y_double);
			if (pr < 0) {
				status = 1;
				break;
			}
			if (pr > 0) {
				state.redraw = 1;
				Sleep(10);
				continue;
			}
			state.redraw = 0;

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
				unsigned long size = smk_get_audio_size(s,
					(unsigned char) track);

				if (size > 0)
					audio_queue(&audio,
						smk_get_audio(s,
						(unsigned char) track),
						size);
			}
		}

		if (paused) {
			Sleep(10);
			continue;
		}

		if (rc == SMK_LAST) {
			next_ms += frame_ms;
			wait_until((ULONGLONG) next_ms);
			break;
		}

		next_ms += frame_ms;
		wait_until((ULONGLONG) next_ms);

		rc = smk_next(s);
		if (rc == SMK_ERROR || rc == SMK_DONE)
			break;
	}

done:
	if (has_audio)
		audio_close(&audio);
	if (ops->shutdown != NULL)
		ops->shutdown(ctx);
	g_win = NULL;
	DestroyWindow(state.hwnd);
	UnregisterClassA(SMK_WIN_CLASS, wc.hInstance);
	timeEndPeriod(1);

	return status;
}
