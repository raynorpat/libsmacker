/*
 * libsmacker - Direct3D 11 playback example.
 *
 * Windows-only: a Win32 window and waveOut audio come from smk_win32_common,
 * while the decoded frame goes to the GPU as an R8 texture plus a 256x1 RGBA
 * palette texture. HLSL is compiled at runtime through d3dcompiler_47.dll.
 */

#define COBJMACROS

#include "smk_win32_common.h"

#include <d3d11.h>
#include <dxgi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *d3d_compile_fn)(LPCVOID pSrcData,
	SIZE_T SrcDataSize, LPCSTR pSourceName,
	const D3D_SHADER_MACRO *pDefines, ID3DInclude *pInclude,
	LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
	ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

static const char *const vertex_source =
	"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
	"VSOut main(uint vid : SV_VertexID)\n"
	"{\n"
	"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
	"    VSOut o;\n"
	"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"    o.uv = float2(p.x, 1.0 - p.y);\n"
	"    return o;\n"
	"}\n";

static const char *const pixel_source =
	"Texture2D<float> frameTex : register(t0);\n"
	"Texture2D<float4> paletteTex : register(t1);\n"
	"SamplerState smp : register(s0);\n"
	"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0)\n"
	"    : SV_TARGET\n"
	"{\n"
	"    float index = frameTex.SampleLevel(smp, uv, 0).r;\n"
	"    int slot = (int) (index * 255.0 + 0.5);\n"
	"    return float4(paletteTex.Load(int3(slot, 0, 0)).rgb, 1.0);\n"
	"}\n";

struct d3d11_state {
	ID3D11Device		*device;
	ID3D11DeviceContext	*context;
	IDXGISwapChain		*swapchain;
	ID3D11RenderTargetView	*rtv;
	ID3D11VertexShader	*vs;
	ID3D11PixelShader	*ps;
	ID3D11Texture2D		*frame_tex;
	ID3D11Texture2D		*palette_tex;
	ID3D11ShaderResourceView *frame_srv;
	ID3D11ShaderResourceView *palette_srv;
	ID3D11SamplerState	*sampler;
	ID3D11RasterizerState	*raster;
	HMODULE			compiler;
	d3d_compile_fn		compile;
	unsigned long		w;
	unsigned long		h;
	struct smk_palette_cache palettes;
};

static ID3DBlob *
compile_shader(struct d3d11_state *st, const char *source, const char *target)
{
	ID3DBlob *code = NULL, *errors = NULL;
	HRESULT hr;

	hr = st->compile(source, strlen(source), NULL, NULL, NULL, "main",
		target, 0, 0, &code, &errors);
	if (FAILED(hr) || code == NULL) {
		if (errors != NULL) {
			fprintf(stderr, "HLSL compile failed (%s): %s\n", target,
				(const char *) ID3D10Blob_GetBufferPointer(errors));
			ID3D10Blob_Release(errors);
		} else {
			fprintf(stderr, "HLSL compile failed (%s)\n", target);
		}
		return NULL;
	}
	if (errors != NULL)
		ID3D10Blob_Release(errors);
	return code;
}

static int
create_render_target(struct d3d11_state *st)
{
	ID3D11Texture2D *back = NULL;
	HRESULT hr;

	hr = IDXGISwapChain_GetBuffer(st->swapchain, 0,
		&IID_ID3D11Texture2D, (void **) &back);
	if (FAILED(hr))
		return -1;
	hr = ID3D11Device_CreateRenderTargetView(st->device,
		(ID3D11Resource *) back, NULL, &st->rtv);
	ID3D11Texture2D_Release(back);
	return FAILED(hr) ? -1 : 0;
}

static int
d3d11_init(void *ctx, HWND hwnd, unsigned long w, unsigned long h)
{
	struct d3d11_state	*st = ctx;
	DXGI_SWAP_CHAIN_DESC	swap_desc;
	D3D_FEATURE_LEVEL	levels[3];
	D3D_FEATURE_LEVEL	level;
	D3D11_TEXTURE2D_DESC	tex_desc;
	D3D11_SAMPLER_DESC	sampler_desc;
	ID3DBlob		*vs_code, *ps_code;
	HRESULT			hr;
	UINT			flags = 0;

	memset(st, 0, sizeof(*st));
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

	memset(&swap_desc, 0, sizeof(swap_desc));
	swap_desc.BufferCount = 1;
	swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_desc.OutputWindow = hwnd;
	swap_desc.SampleDesc.Count = 1;
	swap_desc.Windowed = TRUE;
	swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	levels[0] = D3D_FEATURE_LEVEL_11_0;
	levels[1] = D3D_FEATURE_LEVEL_10_1;
	levels[2] = D3D_FEATURE_LEVEL_10_0;
	hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE,
		NULL, flags, levels, 3, D3D11_SDK_VERSION, &swap_desc,
		&st->swapchain, &st->device, &level, &st->context);
	if (FAILED(hr)) {
		hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP,
			NULL, flags, levels, 3, D3D11_SDK_VERSION, &swap_desc,
			&st->swapchain, &st->device, &level, &st->context);
	}
	if (FAILED(hr)) {
		fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed: 0x%08lX\n",
			(unsigned long) hr);
		return -1;
	}

	if (create_render_target(st) != 0) {
		fputs("D3D11 render target creation failed\n", stderr);
		return -1;
	}

	vs_code = compile_shader(st, vertex_source, "vs_4_0");
	ps_code = compile_shader(st, pixel_source, "ps_4_0");
	if (vs_code == NULL || ps_code == NULL) {
		if (vs_code != NULL)
			ID3D10Blob_Release(vs_code);
		if (ps_code != NULL)
			ID3D10Blob_Release(ps_code);
		return -1;
	}
	hr = ID3D11Device_CreateVertexShader(st->device,
		ID3D10Blob_GetBufferPointer(vs_code),
		ID3D10Blob_GetBufferSize(vs_code), NULL, &st->vs);
	if (FAILED(hr))
		return -1;
	hr = ID3D11Device_CreatePixelShader(st->device,
		ID3D10Blob_GetBufferPointer(ps_code),
		ID3D10Blob_GetBufferSize(ps_code), NULL, &st->ps);
	ID3D10Blob_Release(vs_code);
	ID3D10Blob_Release(ps_code);
	if (FAILED(hr))
		return -1;

	memset(&tex_desc, 0, sizeof(tex_desc));
	tex_desc.Width = (UINT) w;
	tex_desc.Height = (UINT) h;
	tex_desc.MipLevels = 1;
	tex_desc.ArraySize = 1;
	tex_desc.Format = DXGI_FORMAT_R8_UNORM;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.Usage = D3D11_USAGE_DYNAMIC;
	tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = ID3D11Device_CreateTexture2D(st->device, &tex_desc, NULL,
		&st->frame_tex);
	if (FAILED(hr))
		return -1;
	hr = ID3D11Device_CreateShaderResourceView(st->device,
		(ID3D11Resource *) st->frame_tex, NULL, &st->frame_srv);
	if (FAILED(hr))
		return -1;

	memset(&tex_desc, 0, sizeof(tex_desc));
	tex_desc.Width = 256;
	tex_desc.Height = 1;
	tex_desc.MipLevels = 1;
	tex_desc.ArraySize = 1;
	tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	tex_desc.SampleDesc.Count = 1;
	tex_desc.Usage = D3D11_USAGE_DYNAMIC;
	tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = ID3D11Device_CreateTexture2D(st->device, &tex_desc, NULL,
		&st->palette_tex);
	if (FAILED(hr))
		return -1;
	hr = ID3D11Device_CreateShaderResourceView(st->device,
		(ID3D11Resource *) st->palette_tex, NULL, &st->palette_srv);
	if (FAILED(hr))
		return -1;

	memset(&sampler_desc, 0, sizeof(sampler_desc));
	sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	sampler_desc.MaxLOD = 0.0f;
	hr = ID3D11Device_CreateSamplerState(st->device, &sampler_desc,
		&st->sampler);
	if (FAILED(hr))
		return -1;

	{
		D3D11_RASTERIZER_DESC raster_desc;

		memset(&raster_desc, 0, sizeof(raster_desc));
		raster_desc.FillMode = D3D11_FILL_SOLID;
		raster_desc.CullMode = D3D11_CULL_NONE;
		raster_desc.FrontCounterClockwise = FALSE;
		raster_desc.DepthClipEnable = TRUE;
		hr = ID3D11Device_CreateRasterizerState(st->device,
			&raster_desc, &st->raster);
		if (FAILED(hr))
			return -1;
	}

	return 0;
}

static int
d3d11_resize(void *ctx, int client_w, int client_h)
{
	struct d3d11_state *st = ctx;
	HRESULT hr;

	if (client_w <= 0 || client_h <= 0)
		return 1;

	if (st->rtv != NULL) {
		ID3D11RenderTargetView_Release(st->rtv);
		st->rtv = NULL;
	}
	ID3D11DeviceContext_OMSetRenderTargets(st->context, 0, NULL, NULL);
	hr = IDXGISwapChain_ResizeBuffers(st->swapchain, 0,
		(UINT) client_w, (UINT) client_h, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr)) {
		fprintf(stderr, "ResizeBuffers failed: 0x%08lX\n",
			(unsigned long) hr);
		return -1;
	}
	return create_render_target(st) == 0 ? 0 : -1;
}

static int
d3d11_present(void *ctx, const unsigned char *frame,
	const unsigned char *palette, const RECT *viewport, int y_double)
{
	struct d3d11_state	*st = ctx;
	D3D11_MAPPED_SUBRESOURCE map;
	D3D11_VIEWPORT		vp;
	ID3D11ShaderResourceView *views[2];
	float			clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	HRESULT			hr;
	unsigned long		y;

	(void) y_double;	/* the RECT already has the doubled height */
	if (viewport->right <= viewport->left ||
		viewport->bottom <= viewport->top)
		return 1;

	hr = ID3D11DeviceContext_Map(st->context,
		(ID3D11Resource *) st->frame_tex, 0, D3D11_MAP_WRITE_DISCARD, 0,
		&map);
	if (FAILED(hr))
		return -1;
	for (y = 0; y < st->h; y++)
		memcpy((unsigned char *) map.pData + y * map.RowPitch,
			frame + y * st->w, st->w);
	ID3D11DeviceContext_Unmap(st->context,
		(ID3D11Resource *) st->frame_tex, 0);

	if (smk_palette_changed(&st->palettes, palette)) {
		unsigned char	rgba[256 * 4];
		unsigned int	i;

		for (i = 0; i < 256; i++) {
			rgba[i * 4 + 0] = palette[i * 3 + 0];
			rgba[i * 4 + 1] = palette[i * 3 + 1];
			rgba[i * 4 + 2] = palette[i * 3 + 2];
			rgba[i * 4 + 3] = 255;
		}
		hr = ID3D11DeviceContext_Map(st->context,
			(ID3D11Resource *) st->palette_tex, 0,
			D3D11_MAP_WRITE_DISCARD, 0, &map);
		if (FAILED(hr))
			return -1;
		memcpy(map.pData, rgba, sizeof(rgba));
		ID3D11DeviceContext_Unmap(st->context,
			(ID3D11Resource *) st->palette_tex, 0);
		smk_palette_store(&st->palettes, palette);
	}

	ID3D11DeviceContext_OMSetRenderTargets(st->context, 1, &st->rtv, NULL);
	ID3D11DeviceContext_ClearRenderTargetView(st->context, st->rtv, clear);

	vp.TopLeftX = (FLOAT) viewport->left;
	vp.TopLeftY = (FLOAT) viewport->top;
	vp.Width = (FLOAT) (viewport->right - viewport->left);
	vp.Height = (FLOAT) (viewport->bottom - viewport->top);
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(st->context, 1, &vp);
	ID3D11DeviceContext_RSSetState(st->context, st->raster);
	ID3D11DeviceContext_IASetPrimitiveTopology(st->context,
		D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ID3D11DeviceContext_VSSetShader(st->context, st->vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(st->context, st->ps, NULL, 0);
	views[0] = st->frame_srv;
	views[1] = st->palette_srv;
	ID3D11DeviceContext_PSSetShaderResources(st->context, 0, 2, views);
	ID3D11DeviceContext_PSSetSamplers(st->context, 0, 1, &st->sampler);
	ID3D11DeviceContext_Draw(st->context, 4, 0);

	hr = IDXGISwapChain_Present(st->swapchain, 0, 0);
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		fprintf(stderr, "D3D11 device removed/reset (0x%08lX)\n",
			(unsigned long) hr);
		return -1;
	}
	return FAILED(hr) ? -1 : 0;
}

static void
d3d11_shutdown(void *ctx)
{
	struct d3d11_state *st = ctx;

#define SMK_RELEASE(p) \
	do { \
		if ((p) != NULL) { \
			IUnknown_Release((IUnknown *) (p)); \
			(p) = NULL; \
		} \
	} while (0)
	SMK_RELEASE(st->raster);
	SMK_RELEASE(st->sampler);
	SMK_RELEASE(st->palette_srv);
	SMK_RELEASE(st->frame_srv);
	SMK_RELEASE(st->palette_tex);
	SMK_RELEASE(st->frame_tex);
	SMK_RELEASE(st->ps);
	SMK_RELEASE(st->vs);
	SMK_RELEASE(st->rtv);
	SMK_RELEASE(st->swapchain);
	SMK_RELEASE(st->context);
	SMK_RELEASE(st->device);
#undef SMK_RELEASE
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
	struct d3d11_state		state;
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

	snprintf(title, sizeof(title), "libsmacker D3D11 player - %s",
		opt.path);

	ops.init = d3d11_init;
	ops.resize = d3d11_resize;
	ops.present = d3d11_present;
	ops.shutdown = d3d11_shutdown;

	rc = smk_win32_play(s, &opt, title, w, h, y_double, &ops, &state);
	smk_close(s);
	return rc;
}
