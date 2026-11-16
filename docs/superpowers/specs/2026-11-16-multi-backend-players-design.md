# Vulkan, Metal, D3D9, and D3D11 players

## Intent

`smk_sdl_player` shows CPU palette expansion and `smk_gl_player` shows the
GPU path through OpenGL. This adds four more example players so the same
palettized-frame idea is demonstrated on the APIs people actually reach for
on each platform:

- `smk_vk_player` — SDL window and audio, Vulkan rendering. Vulkan on macOS
  runs through MoltenVK.
- `smk_metal_player` — SDL window and audio, Metal rendering through
  `SDL_Metal`.
- `smk_d3d9_player` — Windows-only, plain Win32 window and `waveOut` audio,
  Direct3D 9 rendering.
- `smk_d3d11_player` — Windows-only, plain Win32 window and `waveOut` audio,
  Direct3D 11 rendering.

All four keep the frame palettized end to end: the decoded `w*h` index bytes
and the 768-byte palette go up as GPU textures, and the shader resolves the
palette. The y-scale hint is honoured by displaying at twice the height and
letting nearest sampling duplicate rows.

## Why now

The decoder already produces exactly what a GPU palette shader wants. Each
player is a small, self-contained proof that the upload is `w*h` bytes, not
ARGB, and that scaling and color live on the GPU. The Windows pair also
gives the library examples that do not depend on SDL at all.

## Goals

- Four new tools with the same command line and the same playback behaviour:
  `[--frames N] file.smk`, Esc/q quit, Space pause, Left/Right seek one
  second, resizable window, aspect-correct letterboxing, first audio track.
- Palette lookup, y-doubling, and scaling performed by the GPU in every
  player.
- Shared portable helpers (options, letterbox, palette-change detection) and
  two shared playback modules (SDL, Win32), so the loop/audio/pacing policy
  has one owner per platform family instead of four copies.
- Vulkan and Metal players build and run on macOS and link against SDL.
- D3D9 and D3D11 players are SDL-free, compile with the mingw-w64 toolchain
  available here, and are ready to run on Windows.
- Documentation covers all four and their build requirements.

## Non-goals

- Changing the decoder or the public `smacker.h` API.
- Refactoring `smk_sdl_player.c` or `smk_gl_player.c`; they stay as they are.
- A cross-API renderer abstraction. The shared layer is playback policy and
  small value helpers, not a graphics engine.
- D3D12, Vulkan on Windows, Metal on iOS, or any backend beyond the four
  named.
- Exclusive fullscreen, vsync, HDR, color management, MSAA, or shader hot
  reload.
- Multiple audio tracks, mixing, device hotplug, or resampling.
- Validation layers as a shipped requirement; they may be enabled by hand.

## Anti-goals

- Copy-pasting the audio/event/pacing loop into four files.
- A shared loop that knows about Vulkan, Metal, or D3D types.
- A GPU path that round-trips the frame through ARGB on the CPU.
- An MSL or SPIR-V blob checked in with no source and no way to regenerate
  it.
- Windows code that only compiles under MSVC because it used C++ COM syntax.
  The D3D players are C99 with `COBJMACROS`.

## Constraints

- C99 and `-Wall -Wextra`, matching the tree. The Metal player uses an
  Objective-C `.m` file; everything else stays C.
- The existing public API is untouched.
- `smk_get_video()` returns `w*h` index bytes and `smk_get_palette()` returns
  `256*3` RGB bytes; both y-scale flags mean "display at twice the height".
- Vulkan shaders are GLSL compiled to SPIR-V at build time with `glslc`.
  If `glslc`, Vulkan headers, or the Vulkan loader is missing, skip only the
  Vulkan player with a warning.
- Metal shaders are MSL compiled at runtime with
  `newLibraryWithSource:options:error:`. The Metal Toolchain is not installed
  on the development machine, so an offline `.metallib` is not available.
- D3D9/D3D11 HLSL is compiled at runtime by loading `d3dcompiler_47.dll` with
  `LoadLibrary`/`GetProcAddress`, so no `fxc` is needed at build time.
- The D3D players use Win32 for the window, `waveOut` for audio, and
  `GetTickCount64` for pacing. They do not link SDL.
- Windows-only sources are added to CMake only under `if(WIN32)`; the SDL and
  Vulkan/Metal players only under their platforms.
- Every player keeps the same viewport rules: black clear, largest centred
  rect with the source aspect, nearest filtering.

## Approaches considered

1. **D3D players: SDL-based vs Win32-native.** The user picked Win32-native.
   SDL would be more consistent but cannot be built or linked on this
   machine (no Windows SDL development package, no Wine); Win32-native can
   be cross-compiled and linked here with mingw-w64.
2. **Shared playback code vs per-player copies.** Picked shared. Four more
   copies of audio, events, pacing, and seeking would be the largest source
   of drift in the tree. Two platform modules (SDL and Win32) cover the four
   new players.
3. **Vulkan shaders: build-time `glslc` vs committed SPIR-V.** Picked
   build-time `glslc`, with the player skipped when the tool is absent. Keeps
   shader source readable and avoids opaque blobs.
4. **Metal shaders: runtime MSL vs offline `.metallib`.** Picked runtime
   compilation because the Metal Toolchain is missing here. If runtime
   compilation fails on the development machine, that is a blocking finding
   to surface before claiming the Metal player works.

## Layout

| File | Responsibility |
| --- | --- |
| `src/tools/smk_player_common.h`, `.c` | Portable: CLI parsing, letterbox computation, palette-change cache, info printing |
| `src/tools/smk_sdl_common.h`, `.c` | SDL2/SDL3: window creation, audio device, event/pause/seek/pacing loop, teardown |
| `src/tools/smk_win32_common.h`, `.c` | Win32: window class/message pump, `waveOut` audio pool, event/pause/seek/pacing loop, teardown |
| `src/tools/smk_vk_player.c` | Vulkan instance/device/swapchain/pipeline/textures/upload/present |
| `src/tools/smk_metal_player.m` | Metal device/layer/pipeline/textures/upload/present |
| `src/tools/smk_d3d9_player.c` | D3D9 device/swap chain, L8 + A8R8G8B8 textures, pixel shader, present, device-lost reset |
| `src/tools/smk_d3d11_player.c` | D3D11 device/swap chain, R8 + RGBA8 textures, VS/PS, viewport, resize, present |
| `src/tools/shaders/smk_vk.vert`, `smk_vk.frag` | Vulkan GLSL sources |
| `CMakeLists.txt` | Options, platform guards, shader generation, cross-compile linkage |
| `README.md`, `docs/usage.md`, `INSTALL.md` | Tool list, options, build requirements |

## Shared modules

### `smk_player_common`

Pure C, no platform headers:

```c
struct smk_player_options {
    const char *path;
    unsigned long frame_limit;   /* 0 = play to the end */
};

int smk_player_parse_options(int argc, char *argv[],
                             struct smk_player_options *out);
/* 0 success, 1 help printed, -1 error printed. */

void smk_player_letterbox(int win_w, int win_h,
                          unsigned long src_w, unsigned long src_h,
                          int *x, int *y, int *w, int *h);

struct smk_palette_cache {
    unsigned char last[256 * 3];
    int valid;
};
int smk_palette_changed(struct smk_palette_cache *cache,
                        const unsigned char *palette);
void smk_palette_store(struct smk_palette_cache *cache,
                       const unsigned char *palette);
```

The letterbox rules are the ones already proven in `smk_gl_player`:
zero-size drawables return a zero rect; otherwise scale to fit, round, clamp
to at least 1, and centre.

### `smk_sdl_common`

Owns the SDL surface of the Vulkan and Metal players. Exposes:

```c
struct smk_sdl_player {
    SDL_Window *window;
    /* audio handle and format, paused flag, palette cache */
};

struct smk_sdl_render_ops {
    int  (*present)(void *ctx, const unsigned char *frame,
                    const unsigned char *palette, int drawable_w,
                    int drawable_h, int y_double);
    void (*shutdown)(void *ctx);
};

int  smk_sdl_window_open(struct smk_sdl_player *p, const char *title,
                         unsigned long w, unsigned long h, int y_double,
                         Uint32 extra_flags);
int  smk_sdl_play(struct smk_sdl_player *p, smk s,
                  const struct smk_player_options *opt,
                  const struct smk_sdl_render_ops *ops, void *ctx);
void smk_sdl_close(struct smk_sdl_player *p);
```

`smk_sdl_window_open` calls `SDL_Init(SDL_INIT_VIDEO)`, creates a resizable
window at `w × (y_double ? 2h : h)` with `extra_flags` (Vulkan passes
`SDL_WINDOW_VULKAN`), and keeps the window for the renderer to attach its
surface. `smk_sdl_play` prints the standard header, selects and enables the
first audio track, decodes frame 0, and runs the loop: poll events, redraw on
resize/expose while paused, Space pause, Left/Right seek, `--frames`, audio
queueing, and `wait_until` pacing with the one-interval hold for `SMK_LAST`.
`present` receives the current frame, palette, drawable size, and y-scale
flag.

The SDL2/SDL3 compatibility macros are copied from the existing players, with
the `SDL_GL_DestroyContext` analogue (`SDL_Vulkan_DestroySurface` on SDL3) and
the audio default-device fallback where needed.

Both render-ops structs share one return-code contract: `0` means the call
succeeded; `1` means recoverable — skip this frame and keep playing (D3D9
device lost, Vulkan out-of-date swapchain); `-1` means fatal, the loop stops
and the player exits non-zero. A non-zero `init` is always fatal.

### `smk_win32_common`

Owns the Windows surface of the D3D9 and D3D11 players:

```c
struct smk_win32_render_ops {
    int  (*init)(void *ctx, HWND hwnd, unsigned long w, unsigned long h);
    int  (*resize)(void *ctx, int client_w, int client_h);
    int  (*present)(void *ctx, const unsigned char *frame,
                    const unsigned char *palette, const RECT *viewport,
                    int y_double);
    void (*shutdown)(void *ctx);
};

int smk_win32_play(smk s, const struct smk_player_options *opt,
                   const char *title, unsigned long w, unsigned long h,
                   int y_double, const struct smk_win32_render_ops *ops,
                   void *ctx);
```

It registers a window class, creates a resizable window, and runs a PeekMessage
loop. `WM_KEYDOWN` sets Esc/q/Space/Left/Right actions, `WM_SIZE` sets a resize
flag and calls `ops->resize`, `WM_PAINT` sets a redraw flag, and close messages
set quit. It also owns the `waveOut` audio pool (a small set of prepared
`WAVEHDR`s; write to a free one, poll for done ones, reset on seek, pause and
restart the device) and `GetTickCount64` pacing. `present` receives the
letterbox rect in client coordinates; renderers draw into it and present.

The module raises timer resolution with `timeBeginPeriod(1)` for the session
and restores it with `timeEndPeriod(1)` on teardown, so `Sleep` pacing matches
the SDL players' granularity. It opts into per-monitor DPI awareness v2 by
loading `SetProcessDpiAwarenessContext` from user32 at runtime, falling back to
`SetProcessDPIAware` on systems that predate it.

## Player designs

### Vulkan (`smk_vk_player`)

- Instance extensions come from SDL, plus
  `VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME` on Apple with
  `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`; enable
  `VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME` on the device if advertised.
  MoltenVK needs this to enumerate.
- Surface via SDL. Pick a physical device with a graphics queue that can
  present; request `VK_KHR_SWAPCHAIN_EXTENSION_NAME`. Prefer
  `VK_FORMAT_B8G8R8A8_UNORM`, then `VK_FORMAT_R8G8B8A8_UNORM`. Do not select
  `_SRGB` swapchain formats: they would gamma-encode the palette bytes and
  diverge from the OpenGL player. If neither UNORM format is offered, fail
  with a clear message.
- Swapchain: FIFO present mode (always supported); one image in flight with a
  fence. Recreate on zero-size or `VK_ERROR_OUT_OF_DATE_KHR`/
  `VK_SUBOPTIMAL_KHR`.
- Frame image: `VK_FORMAT_R8_UNORM`, `w × h`, `VK_IMAGE_USAGE_TRANSFER_DST |
  SAMPLED`; palette image: `VK_FORMAT_R8G8B8A8_UNORM`, `256 × 1`, same usage.
  A host-visible, coherent staging buffer holds the index frame; the palette
  region is appended and reused only when the palette changes.
- Per frame: wait fence, write staging, buffer barrier host→transfer, transition
  frame to `TRANSFER_DST`, `vkCmdCopyBufferToImage`, transition to
  `SHADER_READ_ONLY_OPTIMAL`, begin render pass (black clear), set the
  letterbox viewport, bind pipeline and one descriptor set with two combined
  image samplers, `vkCmdDraw(4, 1, 0, 0)`, end, submit with fence, present.
  Palette updates use the same copy path with their own layout transition.
- Shaders, `#version 450`:
  - Vertex: build the oversized strip from `gl_VertexID`; Vulkan clip-space y
    points down, so `v_uv = p` and `gl_Position = vec4(p * 2.0 - 1.0, 0, 1)`.
  - Fragment: sample the index texture at `v_uv`, convert to `0..255` with
    rounding, `texelFetch` the palette at that column, output RGB with alpha 1.
- CMake compiles `smk_vk.vert`/`smk_vk.frag` with `glslc --target-env=vulkan1.1
  -mfmt=c` into generated C sources; the player includes those. The target is
  skipped with a warning if `find_package(Vulkan)` or `glslc` fails.

### Metal (`smk_metal_player`)

- `SDL_Metal_CreateView(window)` and `SDL_Metal_GetLayer(view)` to obtain the
  `CAMetalLayer`; set its `contentsScale` from the window's backing scale and
  read `drawableSize` for the viewport.
- One `MTLDevice`, one `MTLCommandQueue`, pipeline state built once from an
  MSL source string with `newLibraryWithSource:options:error:`. Compile errors
  print the NSError description and exit non-zero.
- Textures: `MTLPixelFormatR8Unorm` for the frame and
  `MTLPixelFormatRGBA8Unorm` for the palette, both shared/managed with
  `replaceRegion:` uploads; palette uploaded only when it changes.
- Per frame: command buffer, render pass descriptor from the drawable's
  texture (black clear), viewport set to the letterbox rect, bind pipeline and
  textures (frame at index 0, palette at index 1), `drawPrimitives:` with a
  four-vertex triangle strip, present the drawable.
- MSL shaders: vertex uses `vertex_id` for the strip and `uv = (p.x, 1 - p.y)`
  (Metal NDC y is up, texture row 0 is the top); fragment samples the index
  with a nearest/clamp sampler and reads the palette with `read(uint2(slot, 0))`.
- CMake enables Objective-C on Apple, links `SDL`, `Metal`, and `QuartzCore`,
  and builds only on Apple with SDL present.

### Direct3D 9 (`smk_d3d9_player`)

- `Direct3DCreate9`, then a windowed device on the default adapter with
  `D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE`,
  `D3DSWAPEFFECT_DISCARD`, `D3DPRESENT_INTERVAL_IMMEDIATE`, back buffer
  format `D3DFMT_UNKNOWN`.
- Frame texture `D3DFMT_L8` and palette `D3DFMT_A8R8G8B8` (256×1), both
  `D3DPOOL_MANAGED`, uploaded with `LockRect`/`UnlockRect`; the palette only
  when it changes. The palette cache stores RGB; upload converts to
  `0x00RRGGBB`.
- Pixel shader `ps_2_0` compiled at runtime with `D3DCompile` loaded from
  `d3dcompiler_47.dll`: sample the frame with nearest/clamp, sample the
  palette at `(index + 0.5/256, 0.5)` with nearest/clamp, output the color.
  No vertex shader: four transformed (`XYZRHW | TEX1`) vertices are drawn with
  `DrawPrimitiveUP` into the letterbox rect.
- Present with `Present(NULL, NULL, NULL, NULL)`. On `D3DERR_DEVICELOST`,
  poll `TestCooperativeLevel`; on `D3DERR_DEVICENOTRESET`, `Reset`, recreate
  the textures and pixel shader, and mark the palette dirty.
- `resize` resets the device to the new client size, reusing the device-lost
  rebuild path, so the back buffer always matches the client area and the
  letterbox rect maps 1:1. A `Reset` that returns `D3DERR_DEVICELOST` leaves
  the old back buffer alone and the next `present` returns the recoverable
  code so playback continues.

### Direct3D 11 (`smk_d3d11_player`)

- `D3D11CreateDeviceAndSwapChain` with a windowed DXGI swap chain
  (`DXGI_SWAP_EFFECT_DISCARD`, `DXGI_FORMAT_R8G8B8A8_UNORM`,
  `DXGI_PRESENT_TEST` unused). Feature level `D3D_FEATURE_LEVEL_11_0` down to
  `10_0`.
- Frame texture `DXGI_FORMAT_R8_UNORM` and palette `DXGI_FORMAT_R8G8B8A8_UNORM`,
  `D3D11_USAGE_DYNAMIC` with `CPU_ACCESS_WRITE` and `SHADER_RESOURCE`; each
  frame `Map(WRITE_DISCARD)`/`Unmap` the frame, and the palette only when it
  changes.
- Shaders compiled at runtime from HLSL with `vs_4_0`/`ps_4_0` through the same
  dynamically loaded `D3DCompile`. Vertex shader builds the strip from
  `SV_VertexID`; pixel shader samples `Texture2D` resources with
  `SampleLevel`/`Load`.
- `OMSetRenderTargets` with the back buffer view, clear black, `RSSetViewports`
  with the letterbox rect, `Draw(4, 0)`, `Present(0, 0)`. `WM_SIZE` triggers
  `ResizeBuffers` and recreated render-target views.

## Build

New options, all default `ON` under `SMACKER_BUILD_TOOLS=ON`:

| Option | Built when |
| --- | --- |
| `SMACKER_BUILD_VK_PLAYER` | not Windows, SDL found, Vulkan found, `glslc` found |
| `SMACKER_BUILD_METAL_PLAYER` | Apple, SDL found |
| `SMACKER_BUILD_D3D_PLAYERS` | Windows target (`WIN32`) |

Each is skipped with a warning when its prerequisites are missing, never a
configure failure. All link `smacker_static` like the existing players.

`src/tools/smk_player_common.c` is compiled into all four. `smk_sdl_common.c`
into the Vulkan and Metal players; `smk_win32_common.c` into the D3D pair.

The Windows pair is cross-compiled here:

```sh
cmake -S . -B build-win \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DSMACKER_BUILD_TOOLS=ON -DSMACKER_BUILD_SHARED=OFF
cmake --build build-win --target smk_d3d9_player smk_d3d11_player
```

Only those two targets are built; the encoder tool's POSIX directory reading
is irrelevant to this check.

## Verification

- Vulkan and Metal: build, run against a `smk_build`-generated quadrant file,
  screenshot, and confirm colours and orientation; repeat with the y-double
  header patch and with a per-frame-palette file; check `--frames` bounds and
  a paused resize.
- On macOS the Vulkan loader needs
  `VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json` in the
  run environment.
- Runtime MSL compilation was validated on the development machine with a
  probe library, so the Metal player's shader path is expected to run here.
- D3D9/D3D11: cross-compile and link with mingw under `-Wall -Wextra`; inspect
  the produced `.exe` and the CMake target. Runtime verification is not
  possible here (no Windows host, no Wine), and the final report says so.
- Existing tests remain green; existing players unchanged.
- GLSL sources are validated by `glslc` during the build. MSL and HLSL errors
  surface at runtime only, which is why the first-run screenshot checks are
  required.

## Decisions made in this design

- D3D players are SDL-free, Win32 + `waveOut`, per the user's choice.
- Four players share portable helpers plus one SDL and one Win32 playback
  module; existing players are not migrated.
- Vulkan shaders are built with `glslc`; Metal and D3D shaders compile at
  runtime.
- No vsync; pacing stays timer-driven so `--frames` and seeking behave like
  the existing players.
- Windows players stay windowed-only.
- D3D9 resize resets the device to the client size and reuses the device-lost
  rebuild path; the back buffer never stretches independently of the window.
- Render-ops return `0`/`1`/`-1` for success/recoverable/fatal.
- Vulkan swapchain formats are UNORM only; no `_SRGB` fallback.
- The Win32 module raises timer resolution and opts into per-monitor DPI
  awareness v2 (with a `SetProcessDPIAware` fallback).

## Left to the builder

- Exact Vulkan resource counts, barrier stages/access masks, and swapchain
  format preference order.
- Exact `waveOut` pool size and drop policy when all headers are busy.
- HLSL/MSL shader spelling beyond the behaviour above.
- Whether the Metal layer needs an explicit `contentsScale` assignment for
  sharp Retina output.
- Error-message wording and logging style, matching the existing players.
