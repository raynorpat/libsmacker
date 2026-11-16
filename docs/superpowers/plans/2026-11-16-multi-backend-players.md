# Multi-backend Player Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four GPU players — Vulkan, Metal, D3D9, and D3D11 — that keep Smacker frames palettized and resolve the palette in shaders, with shared portable helpers and one shared playback module per platform family.

**Architecture:** A portable `smk_player_common` owns options, letterboxing, palette-change detection, and info output. `smk_sdl_common` owns the SDL window, audio, and playback loop for Vulkan and Metal. `smk_win32_common` owns the Win32 window, `waveOut` audio, and playback loop for D3D9 and D3D11. Each renderer implements one present callback. Existing SDL/OpenGL players are untouched.

**Tech Stack:** C99; Objective-C for the Metal player; SDL2/SDL3; Vulkan + MoltenVK + `glslc`; Metal via `SDL_Metal`; Win32, `waveOut`, D3D9, D3D11, runtime `D3DCompile`; CMake 3.16+.

**Spec:** `docs/superpowers/specs/2026-11-16-multi-backend-players-design.md`

## Global Constraints

- C99, `-Wall -Wextra` (MSVC `/W3`); `.m` only for the Metal player.
- No changes to `src/smacker.h` or decoder behaviour.
- Existing `smk_sdl_player` and `smk_gl_player` stay as they are.
- GPU path always uploads `w*h` index bytes plus a `256*3` palette, never ARGB.
- Both y-scale flags mean "display at twice the height"; nearest filtering duplicates rows.
- Letterbox rules match `smk_gl_player`: zero-size → zero rect; else fit, round, clamp to ≥1, centre.
- `[--frames N] file.smk` on every player; `0` means no limit; Esc/q quit, Space pause, Left/Right seek one second.
- Return codes from render ops: `0` success, `1` recoverable skip, `-1` fatal. A non-zero `init` is fatal.
- Vulkan builds only with SDL + `find_package(Vulkan)` + `glslc`; Metal only on Apple with SDL; D3D only under `if(WIN32)`. Missing prerequisites warn and skip, never fail configure.
- Vulkan swapchain formats are UNORM only; no `_SRGB` fallback.
- D3D players do not link SDL. D3D9 resize resets the device; D3D11 resize calls `ResizeBuffers`.
- Win32 playback calls `timeBeginPeriod(1)`/`timeEndPeriod(1)` and opts into per-monitor DPI v2 with a `SetProcessDPIAware` fallback.

## Review Focus

- **Upright image and palette colours in Vulkan and Metal:** Vulkan clip-space y points down, Metal's points up; a wrong UV flip is invisible in code review but obvious on screen. Each player gets a quadrant screenshot check.
- **D3D9 device-lost and resize recovery:** cannot run here; the `Reset` path must be reachable and the present contract must let the loop continue. The plan pins the rebuild path and compile-verifies it.
- **Y-scale aspect:** doubled height must not squash or crop. Unit-tested letterbox cases plus y-double screenshots.
- **Zero-size/minimized drawable:** no divide-by-zero or negative viewport. Unit-tested letterbox; present paths return early.
- **Missing backend prerequisites:** each player skips independently. Configure is run with each option disabled and with the default configuration.

---

### Task 1: Portable player helpers with unit tests

**Files:**
- Create: `src/tools/smk_player_common.h`, `src/tools/smk_player_common.c`
- Create: `tests/test_player_common.c`
- Modify: `tests/test_main.c:6-97` (declarations), `tests/test_main.c:99-195` (table)
- Modify: `tests/CMakeLists.txt:1-25`
- Modify: `CMakeLists.txt:77-88` (define `SMACKER_PLAYER_COMMON_SOURCES`)

**Interfaces:**
- Produces the header used by every later task:

```c
struct smk_player_options { const char *path; unsigned long frame_limit; };

int smk_player_parse_options(int argc, char *argv[],
                             struct smk_player_options *out);

void smk_player_letterbox(int win_w, int win_h,
                          unsigned long src_w, unsigned long src_h,
                          int *x, int *y, int *w, int *h);

struct smk_palette_cache { unsigned char last[256 * 3]; int valid; };
int smk_palette_changed(struct smk_palette_cache *cache,
                        const unsigned char *palette);
void smk_palette_store(struct smk_palette_cache *cache,
                       const unsigned char *palette);
```

`smk_player_parse_options` returns `0` success, `1` help printed, `-1` error printed; it accepts `-h`/`--help`, `--frames N` (base 10, no leading `-`), one positional path, and rejects extras. `smk_player_letterbox` computes the largest centred rect with the source aspect. `smk_palette_changed` returns `1` when the cache is invalid or the 768 bytes differ; `smk_palette_store` copies and validates.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_player_common.c` with these cases:

```c
#include "test.h"
#include "smk_player_common.h"

void test_letterbox_fit_width(void)
{
    int x, y, w, h;
    smk_player_letterbox(500, 300, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 50);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 400);
    SMK_ASSERT_EQ_INT(h, 300);
}

void test_letterbox_exact(void)
{
    int x, y, w, h;
    smk_player_letterbox(640, 480, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 640);
    SMK_ASSERT_EQ_INT(h, 480);
}

void test_letterbox_fit_height(void)
{
    int x, y, w, h;
    smk_player_letterbox(100, 100, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 12);
    SMK_ASSERT_EQ_INT(w, 100);
    SMK_ASSERT_EQ_INT(h, 75);
}

void test_letterbox_zero_size(void)
{
    int x = -1, y = -1, w = -1, h = -1;
    smk_player_letterbox(0, 0, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 0);
    SMK_ASSERT_EQ_INT(h, 0);
}

void test_letterbox_y_double(void)
{
    int x, y, w, h;
    smk_player_letterbox(640, 480, 320, 480, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 160);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 320);
    SMK_ASSERT_EQ_INT(h, 480);
}

void test_letterbox_rounding(void)
{
    int x, y, w, h;
    smk_player_letterbox(801, 600, 320, 240, &x, &y, &w, &h);
    SMK_ASSERT_EQ_INT(x, 0);
    SMK_ASSERT_EQ_INT(y, 0);
    SMK_ASSERT_EQ_INT(w, 800);
    SMK_ASSERT_EQ_INT(h, 600);
}

void test_palette_cache(void)
{
    struct smk_palette_cache cache;
    unsigned char palette[256 * 3];

    memset(&cache, 0, sizeof(cache));
    memset(palette, 0x11, sizeof(palette));
    SMK_ASSERT_EQ_INT(smk_palette_changed(&cache, palette), 1);
    smk_palette_store(&cache, palette);
    SMK_ASSERT_EQ_INT(smk_palette_changed(&cache, palette), 0);
    palette[17] ^= 0xff;
    SMK_ASSERT_EQ_INT(smk_palette_changed(&cache, palette), 1);
}

void test_parse_options_success(void)
{
    char *argv1[] = { (char *)"prog", (char *)"--frames", (char *)"7",
                      (char *)"clip.smk" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(4, argv1, &opt), 0);
    SMK_ASSERT_STR_EQ(opt.path, "clip.smk");
    SMK_ASSERT_EQ_INT(opt.frame_limit, 7);
}

void test_parse_options_default_limit(void)
{
    char *argv1[] = { (char *)"prog", (char *)"clip.smk" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(2, argv1, &opt), 0);
    SMK_ASSERT_STR_EQ(opt.path, "clip.smk");
    SMK_ASSERT_EQ_INT(opt.frame_limit, 0);
}

void test_parse_options_rejects_extra_path(void)
{
    char *argv1[] = { (char *)"prog", (char *)"a.smk", (char *)"b.smk" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(3, argv1, &opt), -1);
}

void test_parse_options_frames_missing(void)
{
    char *argv1[] = { (char *)"prog", (char *)"--frames" };
    struct smk_player_options opt;

    SMK_ASSERT_EQ_INT(smk_player_parse_options(2, argv1, &opt), -1);
}
```

Register them in `tests/test_main.c`: add the eleven `void test_...;` declarations before the table and eleven `{ "<name>", test_<name> }` rows before `{ NULL, NULL }`.

- [ ] **Step 2: Run the tests to verify they fail**

```sh
cmake --build build-gl3 --target smk_tests 2>&1 | tail -5
```

Expected: link/compile failure naming `smk_player_letterbox`, `smk_palette_changed`, `smk_player_parse_options` (missing `smk_player_common.c` and its include path).

- [ ] **Step 3: Add the common sources to CMake**

In the root `CMakeLists.txt`, before the tools block:

```cmake
set(SMACKER_PLAYER_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/smk_player_common.c)
```

In `tests/CMakeLists.txt`, add `test_player_common.c` and `${SMACKER_PLAYER_COMMON_SOURCES}` to `add_executable(smk_tests ...)`, and add `${CMAKE_SOURCE_DIR}/src/tools` to `target_include_directories(smk_tests PRIVATE ...)`.

- [ ] **Step 4: Implement the header and source**

`smk_player_common.h` declares exactly the interface above. `smk_player_common.c`:

- `smk_player_parse_options`: the same messages and status codes as the existing players (`Usage: %s [--frames N] file.smk`, `--frames needs a value`, `invalid --frames value`, `unknown option`, `only one input file may be given`), writing to `stderr` and using `errno`/`strtoul`.
- `smk_player_letterbox`: zero when `win_w <= 0 || win_h <= 0 || src_w == 0 || src_h == 0`; else `scale = min(win_w/(double)src_w, win_h/(double)src_h)`, round `w`/`h` with `+ 0.5`, clamp to `[1, win]`, centre with integer division.
- `smk_palette_changed`: `!cache->valid || memcmp(...)`.
- `smk_palette_store`: `memcpy` 768 bytes, set `valid = 1`.

- [ ] **Step 5: Run the tests to verify they pass**

```sh
cmake --build build-gl3 --target smk_tests 2>&1 | tail -3
./build-gl3/tests/smk_tests
```

Expected: every existing test plus the eleven new ones pass; `0 failed`. (The two rejection tests print usage lines to stderr by design.)

- [ ] **Step 6: Commit**

```sh
git add CMakeLists.txt src/tools/smk_player_common.h src/tools/smk_player_common.c tests/CMakeLists.txt tests/test_main.c tests/test_player_common.c
git commit -m "feat: share portable player helpers"
```

### Task 2: SDL playback module and Metal player

**Files:**
- Create: `src/tools/smk_sdl_common.h`, `src/tools/smk_sdl_common.c`
- Create: `src/tools/smk_metal_player.m`
- Modify: `CMakeLists.txt` (enable Objective-C on Apple, option, target)

**Interfaces:**
- Consumes: `smk_player_common.h`; `smacker.h`; SDL2/SDL3.
- Produces:

```c
struct smk_sdl_player { SDL_Window *window; };

struct smk_sdl_render_ops {
    int  (*present)(void *ctx, const unsigned char *frame,
                    const unsigned char *palette, int drawable_w,
                    int drawable_h, int y_double);
    void (*shutdown)(void *ctx);
};

int  smk_sdl_open(struct smk_sdl_player *p, const char *title,
                  unsigned long w, unsigned long h, int y_double,
                  Uint32 extra_flags);
int  smk_sdl_run(struct smk_sdl_player *p, smk s,
                 const struct smk_player_options *opt,
                 const struct smk_sdl_render_ops *ops, void *ctx);
void smk_sdl_close(struct smk_sdl_player *p);
```

`smk_sdl_open` returns `0`/`-1`, creating a resizable window at `w × (y_double ? 2h : h)`. `smk_sdl_run` returns `0` on clean end and non-zero on fatal renderer failure; it prints the standard header and audio line, decodes with `smk_first`/`smk_next`, and calls `ops->present` once per displayed frame.

- [ ] **Step 1: Add the CMake option and target**

On Apple, call `enable_language(OBJC)` before the target. Add option `SMACKER_BUILD_METAL_PLAYER` (default `ON`). Build `smk_metal_player` only when `APPLE`, `SMACKER_BUILD_STATIC`, the SDL target exists, and `SMACKER_BUILD_METAL_PLAYER`; otherwise warn and skip. Sources: `src/tools/smk_metal_player.m`, `src/tools/smk_sdl_common.c`, `${SMACKER_PLAYER_COMMON_SOURCES}`. Link `smacker_static`, the SDL target, and the `Metal` and `QuartzCore` frameworks.

- [ ] **Step 2: Implement `smk_sdl_common`**

Copy the SDL2/SDL3 compatibility macros and the audio helpers from `smk_gl_player.c` (including the `SDL_GL_*` analogues that do not apply). Implement:

- `smk_sdl_open`: `SDL_Init(SDL_INIT_VIDEO)`, create a resizable window with `extra_flags`, keep it in `p->window`.
- `smk_sdl_run`: print `Opened file`/`Width`/`Height`/`Frames`/`FPS`, `Y-scale: doubled`, then select and enable the first audio track before `smk_first`, open audio, decode frame 0, and run the existing playback loop: poll events, `is_redraw_event` sets `redraw`, Space pause with `audio_pause`, Left/Right seek with `smk_seek_keyframe` and `audio_clear`, distinct-frame counting for `--frames`, `wait_until` pacing, `SMK_LAST` hold, `audio_queue` after each drawn frame. Call `ops->present(p->window? no, ctx, smk_get_video(s), smk_get_palette(s), drawable_w, drawable_h, y_double)` inside the draw branch; `drawable_size` uses `SDL_GetWindowSizeInPixels` (SDL3) or `SDL_GL_GetDrawableSize` (SDL2).
- If `present` returns `1`, skip the frame and keep playing; on `-1`, return non-zero.
- `smk_sdl_close`: destroy audio if open, destroy the window, `SDL_Quit`.

- [ ] **Step 3: Implement the Metal player**

`smk_metal_player.m`:

- Keep `SDL_MetalView view` and `CAMetalLayer *layer` from `SDL_Metal_CreateView(window)` / `SDL_Metal_GetLayer(view)`.
- Create one `MTLDevice`, one `MTLCommandQueue`, and the pipeline from this MSL string with `newLibraryWithSource:options:error:`:

```metal
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut smk_vertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 smk_fragment(VOut in [[stage_in]],
                             texture2d<float> frame [[texture(0)]],
                             texture2d<float> palette [[texture(1)]]) {
    constexpr sampler s(coord::normalized, filter::nearest,
                        address::clamp_to_edge);
    float index = frame.sample(s, in.uv).r;
    int slot = int(index * 255.0 + 0.5);
    float3 rgb = palette.read(uint2(slot, 0)).rgb;
    return float4(rgb, 1.0);
}
```

- Textures: `MTLPixelFormatR8Unorm` at `w × h` and `MTLPixelFormatRGBA8Unorm` at `256 × 1`, both `MTLStorageModeShared`, upload with `replaceRegion`. Convert the 768-byte RGB palette to 1024-byte RGBA (`alpha = 255`) and upload only when `smk_palette_changed` reports a change.
- Present: `nextDrawable` returning nil is recoverable (`1`). Build a render pass with `loadAction = Clear`, black clear, viewport from `smk_player_letterbox(drawable_w, drawable_h, w, y_double ? 2*h : h, ...)`, set pipeline, bind textures at 0 and 1, `drawPrimitives:MTLTriangleStrip vertexStart:0 vertexCount:4`, present, commit, and `waitUntilCompleted` before returning so the next upload cannot race.
- Shutdown: destroy textures/pipeline/queue/view (`SDL_Metal_DestroyView`) and release the device.

- [ ] **Step 4: Build the SDL2 and SDL3 variants**

```sh
cmake -S . -B build-mt2 -DSMACKER_BUILD_TOOLS=ON -DSMACKER_SDL_VERSION=2
cmake --build build-mt2 --target smk_metal_player
cmake -S . -B build-mt3 -DSMACKER_BUILD_TOOLS=ON -DSMACKER_SDL_VERSION=3
cmake --build build-mt3 --target smk_metal_player
```

Expected: configure reports the Metal player, both builds are warning-free.

- [ ] **Step 5: Verify rendering, palette, y-scale, and paused resize**

```sh
./build-mt2/smk_metal_player --frames 6 build-gl2/test.smk
```

Capture the window and confirm red top-left, green top-right, blue bottom-left, white bottom-right. Then run `build-gl2/test-ydouble.smk` and confirm the window is `320 × 480`, all four quadrants visible, no squash. Then run `build-gl2/audio.smk --frames 10` and confirm the `Audio track 0` line and a clean exit. Finally pause, resize the window, and confirm the frame redraws with letterbox bars.

- [ ] **Step 6: Commit**

```sh
git add CMakeLists.txt src/tools/smk_sdl_common.h src/tools/smk_sdl_common.c src/tools/smk_metal_player.m
git commit -m "feat: add Metal player on shared SDL playback"
```

### Task 3: Vulkan player

**Files:**
- Create: `src/tools/smk_vk_player.c`, `src/tools/smk_vk_shaders.c`
- Create: `src/tools/shaders/smk_vk.vert`, `src/tools/shaders/smk_vk.frag`
- Modify: `CMakeLists.txt` (option, shader generation, target)

**Interfaces:**
- Consumes: `smk_sdl_common.h`; `smk_player_common.h`; Vulkan; generated SPIR-V arrays.
- Produces: `smk_vk_player`, built only when `find_package(Vulkan)` and `find_program(glslc)` succeed; skipped with a warning otherwise.
- Generated symbols: `smk_vk_vert_spv[]`, `smk_vk_vert_spv_len`, `smk_vk_frag_spv[]`, `smk_vk_frag_spv_len` (declared in `smk_vk_shaders.c`).

- [ ] **Step 1: Add the shader sources and generation**

`smk_vk.vert` (`#version 450`): `v_uv = p` and `gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0)` with `p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2)` — Vulkan clip-space y points down, so this is upright without a flip.

`smk_vk.frag` (`#version 450`): sample `u_frame` at `v_uv`, `slot = int(index * 255.0 + 0.5)`, `texelFetch(u_palette, ivec2(slot, 0), 0).rgb`, output `vec4(rgb, 1.0)`; bindings 0 and 1.

`smk_vk_shaders.c`:

```c
#include <stdint.h>
const uint32_t smk_vk_vert_spv[] =
#include "smk_vk_vert_spv.inc"
;
const unsigned long smk_vk_vert_spv_len = sizeof(smk_vk_vert_spv);
/* same for the fragment shader */
```

CMake runs `glslc --target-env=vulkan1.1 -mfmt=c -o <build>/smk_vk_vert_spv.inc src/tools/shaders/smk_vk.vert` (and the fragment) and adds `<build>` to the target's include directories.

- [ ] **Step 2: Implement instance, surface, and device**

Use SDL's Vulkan extension list (SDL3 returns it without a window; SDL2 takes the window). On Apple add `VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME` and set `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`. Application API version 1.1. Surface through SDL. Pick a physical device with a graphics queue that supports present, enable `VK_KHR_SWAPCHAIN_EXTENSION_NAME` and, when advertised, `VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME`.

- [ ] **Step 3: Implement swapchain and pipeline**

Choose `VK_FORMAT_B8G8R8A8_UNORM` then `VK_FORMAT_R8G8B8A8_UNORM`; fail with a message if neither is offered. `FIFO` present mode, current transform, opaque composite, `COLOR_ATTACHMENT` usage, one color attachment render pass with clear/store and `PRESENT_SRC_KHR` final layout. Empty vertex input; dynamic viewport and scissor; one descriptor set layout with two combined image samplers (fragment stage); one descriptor pool/set.

- [ ] **Step 4: Implement textures, upload, and the frame loop**

Frame image `VK_FORMAT_R8_UNORM` `w × h`, palette image `VK_FORMAT_R8G8B8A8_UNORM` `256 × 1`, both `TRANSFER_DST | SAMPLED`. Host-visible coherent staging buffer sized `w*h + 1024`; convert the palette to RGBA on change. Per frame: wait fence, write the mapped staging buffer, record a command buffer that

1. barriers the buffer host-write → transfer-read,
2. transitions the frame image to `TRANSFER_DST`, copies it, transitions to `SHADER_READ_ONLY_OPTIMAL`,
3. does the same copy path for the palette when changed,
4. begins the render pass with black clear, sets the letterbox viewport,
5. binds the pipeline and descriptor set, `vkCmdDraw(4, 1, 0, 0)`,
6. ends and submits with the fence,

then acquires, presents, and on `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR` recreates the swapchain, framebuffers, and views and returns the recoverable code. Recreate on zero drawable size too.

- [ ] **Step 5: Build and verify on macOS**

```sh
cmake -S . -B build-vk -DSMACKER_BUILD_TOOLS=ON -DSMACKER_SDL_VERSION=2
cmake --build build-vk --target smk_vk_player
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  ./build-vk/smk_vk_player --frames 6 build-gl2/test.smk
```

Expected: `GL`-free startup, exit 0, no validation/loader errors, and a screenshot matching the quadrant colours. Repeat with `test-ydouble.smk` (320×480, all four quadrants) and `audio.smk` (audio line, clean exit).

- [ ] **Step 6: Commit**

```sh
git add CMakeLists.txt src/tools/smk_vk_player.c src/tools/smk_vk_shaders.c src/tools/shaders/smk_vk.vert src/tools/shaders/smk_vk.frag
git commit -m "feat: add Vulkan player"
```

### Task 4: Win32 playback module and D3D11 player

**Files:**
- Create: `src/tools/smk_win32_common.h`, `src/tools/smk_win32_common.c`
- Create: `src/tools/smk_d3d11_player.c`
- Modify: `CMakeLists.txt` (option, Windows guard, targets, mingw link libs)

**Interfaces:**
- Consumes: `smk_player_common.h`; `smacker.h`; Windows headers.
- Produces:

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

`smk_win32_play` returns `0` clean or non-zero fatal. It creates and runs the window internally and calls `ops->init` once before decoding.

- [ ] **Step 1: Add the CMake option and Windows targets**

Add `SMACKER_BUILD_D3D_PLAYERS` (default `ON`). Inside `if(WIN32)` and under `SMACKER_BUILD_TOOLS` + `SMACKER_BUILD_STATIC`, add `smk_d3d9_player` and `smk_d3d11_player` with `src/tools/smk_win32_common.c` and `${SMACKER_PLAYER_COMMON_SOURCES}`. Link `smacker_static` plus `d3d9`/`d3d11`, `dxgi`, `winmm`, `user32`, `gdi32`. No SDL. Guard the SDL/GL/Vulkan/Metal blocks with `if(NOT WIN32)` / `if(APPLE)` so a cross-configure does not try to find host frameworks.

- [ ] **Step 2: Implement `smk_win32_common`**

- Register a class and create a resizable window sized so the client area is `w × (y_double ? 2h : h)` (`AdjustWindowRectEx`).
- `SetProcessDpiAwarenessContext` loaded from user32 with `GetProcAddress` (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), fallback `SetProcessDPIAware`.
- `timeBeginPeriod(1)` on entry, `timeEndPeriod(1)` on exit.
- Message pump with `PeekMessage`: `WM_KEYDOWN` sets quit/pause/seek actions, `WM_SIZE` updates the client size and calls `ops->resize`, `WM_PAINT` sets redraw, `WM_CLOSE`/`WM_DESTROY` quit.
- `waveOut` audio: open with the decoded track's `WAVEFORMATEX`; keep a pool of four `WAVEHDR`s; write a copy of each chunk to a free header, poll `WHDR_DONE` to reclaim; `waveOutReset` on seek and free the buffers; `waveOutPause`/`waveOutRestart` on Space. Drop a chunk when all four headers are busy.
- Pacing: `GetTickCount64`, `Sleep(remaining)`; distinct-frame `--frames` counting; `SMK_LAST` hold. Call `ops->present` with the letterbox `RECT` computed by `smk_player_letterbox`; treat `1` as skip and `-1` as fatal.

- [ ] **Step 3: Implement the D3D11 player**

- `D3D11CreateDeviceAndSwapChain` at `D3D_FEATURE_LEVEL_11_0` through `10_0`, windowed DXGI swap chain, `DXGI_SWAP_EFFECT_DISCARD`, `DXGI_FORMAT_R8G8B8A8_UNORM`, one back buffer.
- Load `D3DCompile` from `d3dcompiler_47.dll` at runtime. Compile `vs_4_0` and `ps_4_0` from strings: vertex builds the strip from `SV_VertexID` with `uv = float2(p.x, 1 - p.y)`; pixel samples `Texture2D<float>` at nearest/clamp, rounds to a slot, and `Load`s the palette.
- Textures `R8_UNORM` (`w × h`) and `RGBA8_UNORM` (`256 × 1`), `D3D11_USAGE_DYNAMIC` + `CPU_ACCESS_WRITE` + `SHADER_RESOURCE`, created with shader resource views; `Map(WRITE_DISCARD)`/`Unmap` the frame every present, the palette only on change (convert RGB→RGBA).
- Present: clear black, `RSSetViewports` with the letterbox rect, bind pipeline and SRVs, `Draw(4, 0)`, `Present(0, 0)`.
- `resize` calls `ResizeBuffers`, recreates the render-target view, and updates the viewport; return `1` while zero-sized.

- [ ] **Step 4: Cross-compile and verify the linkage**

```sh
cmake -S . -B build-win \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DSMACKER_BUILD_TOOLS=ON -DSMACKER_BUILD_SHARED=OFF
cmake --build build-win --target smk_d3d11_player
file build-win/smk_d3d11_player.exe
x86_64-w64-mingw32-objdump -p build-win/smk_d3d11_player.exe | grep -i "DLL Name"
```

Expected: warning-free compile/link, a PE32+ executable, and imports including `d3d11.dll`, `dxgi.dll`, and `winmm.dll`.

- [ ] **Step 5: Commit**

```sh
git add CMakeLists.txt src/tools/smk_win32_common.h src/tools/smk_win32_common.c src/tools/smk_d3d11_player.c
git commit -m "feat: add Win32 playback core and D3D11 player"
```

### Task 5: D3D9 player

**Files:**
- Create: `src/tools/smk_d3d9_player.c`
- Modify: `CMakeLists.txt` (source list for the existing D3D option, if not already included)

**Interfaces:**
- Consumes: `smk_win32_common.h`; D3D9; runtime `D3DCompile`.
- Produces: `smk_d3d9_player`, built under the same Windows guard as Task 4.

- [ ] **Step 1: Implement the device and resources**

- `Direct3DCreate9(D3D_SDK_VERSION)`, then a windowed device with `D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE`, `D3DSWAPEFFECT_DISCARD`, `D3DPRESENT_INTERVAL_IMMEDIATE`, `BackBufferFormat = D3DFMT_UNKNOWN`, `hDeviceWindow = hwnd`.
- Frame texture `D3DFMT_L8` `w × h` and palette `D3DFMT_A8R8G8B8` `256 × 1`, `D3DPOOL_MANAGED`; upload with `LockRect`/`UnlockRect`; convert RGB→`0x00RRGGBB` for the palette and upload only when changed.
- Compile `ps_2_0` with the runtime `D3DCompile`: sample the frame at nearest/clamp, sample the palette at `(index + 0.5/256, 0.5)` nearest/clamp, output the colour. Sampler states `D3DTEXF_POINT` and `D3DTADDRESS_CLAMP`.
- Draw four `XYZRHW | TEX1` vertices with `DrawPrimitiveUP` into the letterbox rect, black-clear the full back buffer first.

- [ ] **Step 2: Implement resize and device loss**

- `resize` calls `Reset` with the new client size using the saved present parameters, rebuilds the pixel shader and textures, and marks the palette dirty. A `Reset` returning `D3DERR_DEVICELOST` is recoverable: keep the old state, return `1`.
- `present` polls `TestCooperativeLevel` on `D3DERR_DEVICELOST`; on `D3DERR_DEVICENOTRESET`, reset and rebuild. `Present` itself returning `D3DERR_DEVICELOST` returns `1`.

- [ ] **Step 3: Cross-compile and verify the linkage**

```sh
cmake --build build-win --target smk_d3d9_player
file build-win/smk_d3d9_player.exe
x86_64-w64-mingw32-objdump -p build-win/smk_d3d9_player.exe | grep -i "DLL Name"
```

Expected: warning-free compile/link, a PE32+ executable, and imports including `d3d9.dll` and `winmm.dll`.

- [ ] **Step 4: Commit**

```sh
git add CMakeLists.txt src/tools/smk_d3d9_player.c
git commit -m "feat: add D3D9 player"
```

### Task 6: Documentation and full verification

**Files:**
- Modify: `README.md` (player list, build notes, changelog)
- Modify: `docs/usage.md` (example programs and build commands)
- Modify: `INSTALL.md` (options table, prerequisites)

**Interfaces:**
- Consumes: all four players and the shared modules.
- Produces: the documented, fully verified feature.

- [ ] **Step 1: Update the documentation**

- `README.md`: list all four new tools with one line each; note that the Vulkan/Metal players need SDL and that the D3D pair is Win32-only and SDL-free; add changelog bullets.
- `docs/usage.md`: add them to "Example programs" with the shared GPU-path description, and the build commands (`smk_vk_player file.smk`, `smk_metal_player file.smk`, `smk_d3d11_player file.smk`, `smk_d3d9_player file.smk`); mention the MoltenVK `VK_ICD_FILENAMES` requirement for running on macOS.
- `INSTALL.md`: add `SMACKER_BUILD_VK_PLAYER`, `SMACKER_BUILD_METAL_PLAYER`, `SMACKER_BUILD_D3D_PLAYERS` to the options table, state their prerequisites, and note the `glslc` requirement for Vulkan.

- [ ] **Step 2: Verify the option guards**

```sh
cmake -S . -B build-nobackends -DSMACKER_BUILD_TOOLS=ON \
  -DSMACKER_BUILD_VK_PLAYER=OFF -DSMACKER_BUILD_METAL_PLAYER=OFF \
  -DSMACKER_BUILD_D3D_PLAYERS=OFF
cmake --build build-nobackends --target smk_sdl_player smk_gl_player
```

Expected: configure succeeds with the new options off, and the existing players still build.

- [ ] **Step 3: Run the full verification**

```sh
ctest --test-dir build-gl3 --output-on-failure
cmake --build build-mt2 --target smk_metal_player
cmake --build build-vk --target smk_vk_player
cmake --build build-win --target smk_d3d9_player smk_d3d11_player
```

Then run the Metal and Vulkan players against the normal, y-double, and audio files, and confirm the screenshots and exit codes. Re-check the D3D executables with `file` and `objdump`.

Expected: all tests pass; Metal and Vulkan render correctly; both D3D executables link; the only unverified item is D3D runtime behaviour, stated in the final report.

- [ ] **Step 4: Commit**

```sh
git add README.md docs/usage.md INSTALL.md
git commit -m "docs: describe the four GPU players"
```

---

## Self-Review

- **Spec coverage:** portable helpers with tests (Task 1); SDL common + Metal (Task 2); Vulkan with glslc and MoltenVK (Task 3); Win32 common + D3D11 (Task 4); D3D9 device-lost/resize (Task 5); docs and guards (Task 6). Every design section maps to a task.
- **Step scan:** each step names signatures, exact config values, commands, and expected output. No "handle edge cases" placeholders.
- **Type consistency:** `smk_win32_render_ops.present` takes the same `RECT *viewport` in its definition and use; `smk_sdl_render_ops.present` takes drawable dimensions everywhere; `smk_player_options` is the same struct in all players; `smk_palette_cache` is passed by pointer consistently.
- **Review Focus tests:** orientation/colour (Tasks 2 and 3 screenshots), D3D9 device-lost/resize (Task 5 Steps 2-3 compile-verified; runtime explicitly unverified), y-scale (Task 1 letterbox tests, Tasks 2/3 screenshots), zero-size (Task 1 zero test, present guards), prerequisite skips (Task 6 Step 2 plus per-task configure guards).
- **Proportion:** six tasks; details are signatures, configuration values, shader sources, and commands rather than full function bodies.
