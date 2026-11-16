/*
 * libsmacker - Metal playback example.
 *
 * The decoded frame stays palettized: an R8 index texture and a 256x1 RGBA
 * palette texture go to the GPU, and a fragment shader resolves the palette.
 * SDL provides the window and audio; the Metal view comes from SDL_Metal.
 */

#include "smk_sdl_common.h"
#include "smk_player_common.h"

#ifdef SMK_USE_SDL3
#include <SDL3/SDL_metal.h>
#else
#include <SDL_metal.h>
#endif

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *const kMetalShader =
	@"#include <metal_stdlib>\n"
	@"using namespace metal;\n"
	@"struct VOut { float4 pos [[position]]; float2 uv; };\n"
	@"vertex VOut smk_vertex(uint vid [[vertex_id]]) {\n"
	@"    float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
	@"    VOut o;\n"
	@"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	@"    o.uv = float2(p.x, 1.0 - p.y);\n"
	@"    return o;\n"
	@"}\n"
	@"fragment float4 smk_fragment(VOut in [[stage_in]],\n"
	@"        texture2d<float> frame [[texture(0)]],\n"
	@"        texture2d<float> palette [[texture(1)]]) {\n"
	@"    constexpr sampler s(coord::normalized, filter::nearest,\n"
	@"                        address::clamp_to_edge);\n"
	@"    float index = frame.sample(s, in.uv).r;\n"
	@"    int slot = int(index * 255.0 + 0.5);\n"
	@"    float3 rgb = palette.read(uint2(slot, 0)).rgb;\n"
	@"    return float4(rgb, 1.0);\n"
	@"}\n";

struct metal_state {
	SDL_MetalView	view;
	CAMetalLayer   *layer;
	id<MTLDevice>	device;
	id<MTLCommandQueue> queue;
	id<MTLRenderPipelineState> pipeline;
	id<MTLTexture>	frame_tex;
	id<MTLTexture>	palette_tex;
	struct smk_palette_cache palettes;
	unsigned long	w;
	unsigned long	h;
};

static void
metal_shutdown(void *ctx)
{
	struct metal_state *st = ctx;

	[st->frame_tex release];
	[st->palette_tex release];
	[st->pipeline release];
	[st->queue release];
	[st->device release];
	st->frame_tex = nil;
	st->palette_tex = nil;
	st->pipeline = nil;
	st->queue = nil;
	st->device = nil;
	if (st->view != NULL) {
		SDL_Metal_DestroyView(st->view);
		st->view = NULL;
	}
	st->layer = nil;
}

static int
metal_init(struct metal_state *st, SDL_Window *window, unsigned long w,
	unsigned long h)
{
	NSError		*error = nil;
	id<MTLLibrary>	library;
	id<MTLFunction>	vertex_fn, fragment_fn;
	MTLRenderPipelineDescriptor *pipeline_desc;
	MTLTextureDescriptor *desc;

	memset(st, 0, sizeof(*st));
	st->w = w;
	st->h = h;

	st->view = SDL_Metal_CreateView(window);
	if (st->view == NULL) {
		fprintf(stderr, "SDL_Metal_CreateView failed: %s\n",
			SDL_GetError());
		return -1;
	}
	st->layer = (__bridge CAMetalLayer *) SDL_Metal_GetLayer(st->view);
	if (st->layer == nil) {
		fputs("SDL_Metal_GetLayer failed\n", stderr);
		return -1;
	}

	st->device = MTLCreateSystemDefaultDevice();
	if (st->device == nil) {
		fputs("No Metal device available\n", stderr);
		return -1;
	}
	st->queue = [st->device newCommandQueue];
	if (st->queue == nil) {
		fputs("Failed to create Metal command queue\n", stderr);
		return -1;
	}

	library = [st->device newLibraryWithSource:kMetalShader
		options:nil error:&error];
	if (library == nil) {
		fprintf(stderr, "Metal shader compile failed: %s\n",
			error != nil ?
			[[error localizedDescription] UTF8String] : "unknown");
		return -1;
	}
	vertex_fn = [library newFunctionWithName:@"smk_vertex"];
	fragment_fn = [library newFunctionWithName:@"smk_fragment"];
	if (vertex_fn == nil || fragment_fn == nil) {
		fputs("Metal shader function missing\n", stderr);
		[vertex_fn release];
		[fragment_fn release];
		[library release];
		return -1;
	}

	pipeline_desc = [MTLRenderPipelineDescriptor new];
	pipeline_desc.vertexFunction = vertex_fn;
	pipeline_desc.fragmentFunction = fragment_fn;
	pipeline_desc.colorAttachments[0].pixelFormat =
		MTLPixelFormatBGRA8Unorm;
	st->pipeline = [st->device
		newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
	if (st->pipeline == nil) {
		fprintf(stderr, "Metal pipeline creation failed: %s\n",
			error != nil ?
			[[error localizedDescription] UTF8String] : "unknown");
	}
	[pipeline_desc release];
	[vertex_fn release];
	[fragment_fn release];
	[library release];
	if (st->pipeline == nil)
		return -1;

	desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
		width:w height:h mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	st->frame_tex = [st->device newTextureWithDescriptor:desc];
	if (st->frame_tex == nil) {
		fputs("Failed to create Metal frame texture\n", stderr);
		return -1;
	}

	desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		width:256 height:1 mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	st->palette_tex = [st->device newTextureWithDescriptor:desc];
	if (st->palette_tex == nil) {
		fputs("Failed to create Metal palette texture\n", stderr);
		return -1;
	}

	st->layer.device = st->device;
	st->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	st->layer.framebufferOnly = YES;

	return 0;
}

static int
metal_present(void *ctx, const unsigned char *frame,
	const unsigned char *palette, int drawable_w, int drawable_h,
	int y_double)
{
	struct metal_state *st = ctx;
	id<CAMetalDrawable> drawable;
	MTLRenderPassDescriptor *pass;
	id<MTLCommandBuffer> command;
	id<MTLRenderCommandEncoder> encoder;
	int x, y, vw, vh;

	(void) drawable_w;
	(void) drawable_h;

	@autoreleasepool {
		drawable = [st->layer nextDrawable];
		if (drawable == nil)
			return 1;

		[st->frame_tex
			replaceRegion:MTLRegionMake2D(0, 0,
				(NSUInteger) st->w, (NSUInteger) st->h)
			mipmapLevel:0
			withBytes:frame
			bytesPerRow:st->w];

		if (smk_palette_changed(&st->palettes, palette)) {
			unsigned char	rgba[256 * 4];
			unsigned int	i;

			for (i = 0; i < 256; i++) {
				rgba[i * 4 + 0] = palette[i * 3 + 0];
				rgba[i * 4 + 1] = palette[i * 3 + 1];
				rgba[i * 4 + 2] = palette[i * 3 + 2];
				rgba[i * 4 + 3] = 255;
			}
			[st->palette_tex
				replaceRegion:MTLRegionMake2D(0, 0, 256, 1)
				mipmapLevel:0
				withBytes:rgba
				bytesPerRow:256 * 4];
			smk_palette_store(&st->palettes, palette);
		}

		pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = drawable.texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor =
			MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

		command = [st->queue commandBuffer];
		encoder = [command renderCommandEncoderWithDescriptor:pass];

		smk_player_letterbox((int) drawable.texture.width,
			(int) drawable.texture.height, st->w,
			y_double ? st->h * 2 : st->h,
			&x, &y, &vw, &vh);
		if (vw > 0 && vh > 0) {
			MTLViewport viewport = {
				(double) x, (double) y,
				(double) vw, (double) vh, 0.0, 1.0
			};

			[encoder setViewport:viewport];
			[encoder setRenderPipelineState:st->pipeline];
			[encoder setFragmentTexture:st->frame_tex atIndex:0];
			[encoder setFragmentTexture:st->palette_tex atIndex:1];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
				vertexStart:0 vertexCount:4];
		}

		[encoder endEncoding];
		[command presentDrawable:drawable];
		[command commit];
		[command waitUntilCompleted];
	}

	return 0;
}

int
main(int argc, char *argv[])
{
	struct smk_player_options	opt;
	struct smk_sdl_player		player;
	struct metal_state		state;
	struct smk_sdl_render_ops	ops;
	smk				s;
	unsigned long			w = 0, h = 0;
	double				usf = 0.0;
	unsigned char			y_scale = SMK_FLAG_Y_NONE;
	int				y_double;
	int				status;
	int				rc;
	char				title[512];

	@autoreleasepool {
		rc = smk_player_parse_options(argc, argv, &opt);
		if (rc != 0)
			return rc > 0 ? 0 : 1;

		s = smk_open_file(opt.path, SMK_MODE_DISK);
		if (s == NULL) {
			fprintf(stderr, "Errors encountered opening %s, exiting.\n",
				opt.path);
			return 1;
		}

		smk_info_all(s, NULL, NULL, &usf);
		smk_info_video(s, &w, &h, &y_scale);
		if (w == 0 || h == 0) {
			fprintf(stderr, "%s has no video track, exiting.\n",
				opt.path);
			smk_close(s);
			return 1;
		}
		y_double = y_scale != SMK_FLAG_Y_NONE;
		if (w > (unsigned long) INT_MAX ||
			h > (unsigned long) (INT_MAX / (y_double ? 2 : 1))) {
			fprintf(stderr,
				"%s has unsupported video dimensions: %lu x %lu\n",
				opt.path, w, h);
			smk_close(s);
			return 1;
		}

		snprintf(title, sizeof(title), "libsmacker Metal player - %s",
			opt.path);
		if (smk_sdl_open(&player, title, w, h, y_double, 0) != 0) {
			fprintf(stderr, "SDL window creation failed: %s\n",
				SDL_GetError());
			smk_close(s);
			return 1;
		}

		if (metal_init(&state, player.window, w, h) != 0) {
			metal_shutdown(&state);
			smk_sdl_close(&player);
			smk_close(s);
			return 1;
		}

		ops.present = metal_present;
		ops.shutdown = metal_shutdown;
		status = smk_sdl_run(&player, s, &opt, &ops, &state);

		smk_sdl_close(&player);
		smk_close(s);
	}

	return status;
}
