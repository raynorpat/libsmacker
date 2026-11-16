/*
 * libsmacker - Direct3D 9 playback example.
 *
 * Windows-only: a Win32 window and waveOut audio come from smk_win32_common,
 * while the decoded frame goes to the GPU as an L8 texture plus a 256x1
 * A8R8G8B8 palette texture. ps_2_0 is compiled at runtime through
 * d3dcompiler_47.dll.
 */

#define COBJMACROS

#include "smk_win32_common.h"

#include <d3d9.h>
#include <d3dcommon.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *d3d_compile_fn)(LPCVOID pSrcData,
	SIZE_T SrcDataSize, LPCSTR pSourceName,
	const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
	LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
	ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

static const char *const pixel_source =
	"sampler2D frameSmp : register(s0);\n"
	"sampler2D paletteSmp : register(s1);\n"
	"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
	"{\n"
	"    float index = tex2D(frameSmp, uv).r;\n"
	"    float2 lookup = float2((index * 255.0 + 0.5) / 256.0, 0.5);\n"
	"    return float4(tex2D(paletteSmp, lookup).rgb, 1.0);\n"
	"}\n";

struct d3d9_vertex {
	float	x, y, z, rhw;
	float	u, v;
};

#define SMK_D3D9_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

struct d3d9_state {
	IDirect3D9		*d3d;
	IDirect3DDevice9	*device;
	IDirect3DTexture9	*frame_tex;
	IDirect3DTexture9	*palette_tex;
	IDirect3DPixelShader9	*ps;
	HMODULE			compiler;
	d3d_compile_fn		compile;
	D3DPRESENT_PARAMETERS	present;
	HWND			hwnd;
	unsigned long		w;
	unsigned long		h;
	struct smk_palette_cache palettes;
};

static void
destroy_resources(struct d3d9_state *st)
{
	if (st->ps != NULL) {
		IDirect3DPixelShader9_Release(st->ps);
		st->ps = NULL;
	}
	if (st->palette_tex != NULL) {
		IDirect3DTexture9_Release(st->palette_tex);
		st->palette_tex = NULL;
	}
	if (st->frame_tex != NULL) {
		IDirect3DTexture9_Release(st->frame_tex);
		st->frame_tex = NULL;
	}
}

static int
create_resources(struct d3d9_state *st)
{
	ID3DBlob	*code = NULL, *errors = NULL;
	HRESULT		hr;

	hr = st->compile(pixel_source, strlen(pixel_source), NULL, NULL, NULL,
		"main", "ps_2_0", 0, 0, &code, &errors);
	if (FAILED(hr) || code == NULL) {
		if (errors != NULL) {
			fprintf(stderr, "HLSL compile failed (ps_2_0): %s\n",
				(const char *) ID3D10Blob_GetBufferPointer(errors));
			ID3D10Blob_Release(errors);
		} else {
			fputs("HLSL compile failed (ps_2_0)\n", stderr);
		}
		return -1;
	}
	hr = IDirect3DDevice9_CreatePixelShader(st->device,
		ID3D10Blob_GetBufferPointer(code), &st->ps);
	ID3D10Blob_Release(code);
	if (FAILED(hr)) {
		fputs("CreatePixelShader failed\n", stderr);
		return -1;
	}

	hr = IDirect3DDevice9_CreateTexture(st->device, (UINT) st->w,
		(UINT) st->h, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &st->frame_tex,
		NULL);
	if (FAILED(hr)) {
		fputs("Frame texture creation failed\n", stderr);
		return -1;
	}
	hr = IDirect3DDevice9_CreateTexture(st->device, 256, 1, 1, 0,
		D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &st->palette_tex, NULL);
	if (FAILED(hr)) {
		fputs("Palette texture creation failed\n", stderr);
		return -1;
	}

	st->palettes.valid = 0;
	return 0;
}

static void
set_sampler_state(struct d3d9_state *st)
{
	DWORD stage;

	for (stage = 0; stage < 2; stage++) {
		IDirect3DDevice9_SetSamplerState(st->device, stage,
			D3DSAMP_MINFILTER, D3DTEXF_POINT);
		IDirect3DDevice9_SetSamplerState(st->device, stage,
			D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		IDirect3DDevice9_SetSamplerState(st->device, stage,
			D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		IDirect3DDevice9_SetSamplerState(st->device, stage,
			D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		IDirect3DDevice9_SetSamplerState(st->device, stage,
			D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
}

static int
d3d9_init(void *ctx, HWND hwnd, unsigned long w, unsigned long h)
{
	struct d3d9_state	*st = ctx;
	RECT			client;
	HRESULT			hr;

	memset(st, 0, sizeof(*st));
	st->hwnd = hwnd;
	st->w = w;
	st->h = h;

	st->compiler = LoadLibraryA("d3dcompiler_47.dll");
	if (st->compiler == NULL) {
		fputs("d3dcompiler_47.dll not found\n", stderr);
		return -1;
	}
	st->compile = (d3d_compile_fn) (void *) GetProcAddress(st->compiler,
		"D3DCompile");
	if (st->compile == NULL) {
		fputs("D3DCompile entry point missing\n", stderr);
		return -1;
	}

	st->d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (st->d3d == NULL) {
		fputs("Direct3DCreate9 failed\n", stderr);
		return -1;
	}

	GetClientRect(hwnd, &client);
	memset(&st->present, 0, sizeof(st->present));
	st->present.BackBufferWidth = (UINT) client.right;
	st->present.BackBufferHeight = (UINT) client.bottom;
	st->present.BackBufferFormat = D3DFMT_UNKNOWN;
	st->present.BackBufferCount = 1;
	st->present.MultiSampleType = D3DMULTISAMPLE_NONE;
	st->present.SwapEffect = D3DSWAPEFFECT_DISCARD;
	st->present.hDeviceWindow = hwnd;
	st->present.Windowed = TRUE;
	st->present.EnableAutoDepthStencil = FALSE;
	st->present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

	hr = IDirect3D9_CreateDevice(st->d3d, D3DADAPTER_DEFAULT,
		D3DDEVTYPE_HAL, hwnd,
		D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
		&st->present, &st->device);
	if (FAILED(hr)) {
		hr = IDirect3D9_CreateDevice(st->d3d, D3DADAPTER_DEFAULT,
			D3DDEVTYPE_HAL, hwnd,
			D3DCREATE_SOFTWARE_VERTEXPROCESSING |
			D3DCREATE_FPU_PRESERVE,
			&st->present, &st->device);
	}
	if (FAILED(hr)) {
		fprintf(stderr, "CreateDevice failed: 0x%08lX\n",
			(unsigned long) hr);
		return -1;
	}

	if (create_resources(st) != 0)
		return -1;
	set_sampler_state(st);
	return 0;
}

/* Returns 0 usable, 1 still lost, -1 fatal. */
static int
recover_lost(struct d3d9_state *st)
{
	HRESULT hr = IDirect3DDevice9_TestCooperativeLevel(st->device);

	if (hr == D3DERR_DEVICELOST)
		return 1;
	if (hr != D3DERR_DEVICENOTRESET)
		return 0;

	destroy_resources(st);
	hr = IDirect3DDevice9_Reset(st->device, &st->present);
	if (hr == D3DERR_DEVICELOST)
		return 1;
	if (FAILED(hr)) {
		fprintf(stderr, "Reset failed: 0x%08lX\n", (unsigned long) hr);
		return -1;
	}
	if (create_resources(st) != 0)
		return -1;
	set_sampler_state(st);
	return 0;
}

static int
d3d9_resize(void *ctx, int client_w, int client_h)
{
	struct d3d9_state *st = ctx;
	HRESULT hr;

	if (client_w <= 0 || client_h <= 0)
		return 1;

	st->present.BackBufferWidth = (UINT) client_w;
	st->present.BackBufferHeight = (UINT) client_h;

	hr = IDirect3DDevice9_TestCooperativeLevel(st->device);
	if (hr == D3DERR_DEVICELOST)
		return 1;

	destroy_resources(st);
	hr = IDirect3DDevice9_Reset(st->device, &st->present);
	if (hr == D3DERR_DEVICELOST)
		return 1;
	if (FAILED(hr)) {
		fprintf(stderr, "Reset (resize) failed: 0x%08lX\n",
			(unsigned long) hr);
		return -1;
	}
	if (create_resources(st) != 0)
		return -1;
	set_sampler_state(st);
	return 0;
}

static int
upload_frame(struct d3d9_state *st, const unsigned char *frame)
{
	D3DLOCKED_RECT	locked;
	HRESULT		hr;
	unsigned long	y;

	hr = IDirect3DTexture9_LockRect(st->frame_tex, 0, &locked, NULL, 0);
	if (FAILED(hr))
		return -1;
	for (y = 0; y < st->h; y++)
		memcpy((unsigned char *) locked.pBits + y * locked.Pitch,
			frame + y * st->w, st->w);
	IDirect3DTexture9_UnlockRect(st->frame_tex, 0);
	return 0;
}

static int
upload_palette(struct d3d9_state *st, const unsigned char *palette)
{
	D3DLOCKED_RECT	locked;
	unsigned int	*lane;
	unsigned int	i;
	HRESULT		hr;

	hr = IDirect3DTexture9_LockRect(st->palette_tex, 0, &locked, NULL, 0);
	if (FAILED(hr))
		return -1;
	lane = (unsigned int *) locked.pBits;
	for (i = 0; i < 256; i++) {
		lane[i] = 0xFF000000u |
			((unsigned int) palette[i * 3 + 0] << 16) |
			((unsigned int) palette[i * 3 + 1] << 8) |
			(unsigned int) palette[i * 3 + 2];
	}
	IDirect3DTexture9_UnlockRect(st->palette_tex, 0);
	smk_palette_store(&st->palettes, palette);
	return 0;
}

static int
d3d9_present(void *ctx, const unsigned char *frame,
	const unsigned char *palette, const RECT *viewport, int y_double)
{
	struct d3d9_state	*st = ctx;
	struct d3d9_vertex	vertices[4];
	HRESULT			hr;
	int			recover;

	(void) y_double;	/* the RECT already has the doubled height */

	recover = recover_lost(st);
	if (recover != 0)
		return recover;

	if (viewport->right <= viewport->left ||
		viewport->bottom <= viewport->top) {
		hr = IDirect3DDevice9_Present(st->device, NULL, NULL, NULL, NULL);
		if (hr == D3DERR_DEVICELOST) {
			return 1;
		}
		return FAILED(hr) ? -1 : 0;
	}

	if (upload_frame(st, frame) != 0)
		return -1;
	if (smk_palette_changed(&st->palettes, palette) &&
		upload_palette(st, palette) != 0)
		return -1;

	IDirect3DDevice9_Clear(st->device, 0, NULL, D3DCLEAR_TARGET,
		D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	IDirect3DDevice9_SetVertexShader(st->device, NULL);
	IDirect3DDevice9_SetPixelShader(st->device, st->ps);
	IDirect3DDevice9_SetFVF(st->device, SMK_D3D9_FVF);
	IDirect3DDevice9_SetTexture(st->device, 0,
		(IDirect3DBaseTexture9 *) st->frame_tex);
	IDirect3DDevice9_SetTexture(st->device, 1,
		(IDirect3DBaseTexture9 *) st->palette_tex);

	vertices[0].x = (float) viewport->left;
	vertices[0].y = (float) viewport->top;
	vertices[1].x = (float) viewport->right;
	vertices[1].y = (float) viewport->top;
	vertices[2].x = (float) viewport->left;
	vertices[2].y = (float) viewport->bottom;
	vertices[3].x = (float) viewport->right;
	vertices[3].y = (float) viewport->bottom;
	for (int i = 0; i < 4; i++) {
		vertices[i].z = 0.0f;
		vertices[i].rhw = 1.0f;
		vertices[i].u = (i == 0 || i == 2) ? 0.0f : 1.0f;
		vertices[i].v = (i <= 1) ? 0.0f : 1.0f;
	}

	hr = IDirect3DDevice9_BeginScene(st->device);
	if (FAILED(hr))
		return -1;
	hr = IDirect3DDevice9_DrawPrimitiveUP(st->device,
		D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0]));
	IDirect3DDevice9_EndScene(st->device);
	if (FAILED(hr))
		return -1;

	hr = IDirect3DDevice9_Present(st->device, NULL, NULL, NULL, NULL);
	if (hr == D3DERR_DEVICELOST) {
		return 1;
	}
	return FAILED(hr) ? -1 : 0;
}

static void
d3d9_shutdown(void *ctx)
{
	struct d3d9_state *st = ctx;

	destroy_resources(st);
	if (st->device != NULL) {
		IDirect3DDevice9_Release(st->device);
		st->device = NULL;
	}
	if (st->d3d != NULL) {
		IDirect3D9_Release(st->d3d);
		st->d3d = NULL;
	}
	if (st->compiler != NULL) {
		FreeLibrary(st->compiler);
		st->compiler = NULL;
	}
	st->compile = NULL;
}

int
main(int argc, char *argv[])
{
	struct smk_player_options	opt;
	struct smk_win32_render_ops	ops;
	struct d3d9_state		state;
	smk				s;
	unsigned long			w = 0, h = 0;
	unsigned char			y_scale = SMK_FLAG_Y_NONE;
	int				y_double;
	int				rc;
	char				title[512];

	rc = smk_player_parse_options(argc, argv, &opt);
	if (rc != 0)
		return rc > 0 ? 0 : 1;

	s = smk_open_file(opt.path, SMK_MODE_DISK);
	if (s == NULL) {
		fprintf(stderr, "Errors encountered opening %s, exiting.\n",
			opt.path);
		return 1;
	}

	smk_info_video(s, &w, &h, &y_scale);
	if (w == 0 || h == 0) {
		fprintf(stderr, "%s has no video track, exiting.\n", opt.path);
		smk_close(s);
		return 1;
	}
	y_double = y_scale != SMK_FLAG_Y_NONE;

	snprintf(title, sizeof(title), "libsmacker D3D9 player - %s",
		opt.path);

	ops.init = d3d9_init;
	ops.resize = d3d9_resize;
	ops.present = d3d9_present;
	ops.shutdown = d3d9_shutdown;

	rc = smk_win32_play(s, &opt, title, w, h, y_double, &ops, &state);
	smk_close(s);
	return rc;
}
