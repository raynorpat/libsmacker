# OpenGL/GLSL Smacker Player Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `smk_gl_player`, an SDL + OpenGL 3.3 core example that uploads the 8-bit Smacker frame once and does palette lookup, y-doubling, scaling, and letterboxing on the GPU.

**Architecture:** A new single-file tool in `src/tools/smk_gl_player.c` creates an SDL window with an OpenGL 3.3 core context, uploads the decoded frame as a `GL_R8` texture and the palette as a 256×1 `GL_RGB8` texture, and draws one fullscreen triangle strip through two shaders. CPU work per frame is the decode plus a viewport calculation; no ARGB expansion.

**Tech Stack:** C99, CMake 3.16+, SDL2 or SDL3, OpenGL 3.3 core, GLSL 330.

**Spec:** `docs/superpowers/specs/2026-11-16-opengl-player-design.md`

## Global Constraints

- C99, `-Wall -Wextra` (MSVC `/W3`); no new third-party dependencies beyond SDL and OpenGL.
- `src/smacker.h` and the library API are unchanged.
- OpenGL 3.3 core profile; GLSL `#version 330 core`.
- GL entry points are resolved with `SDL_GL_GetProcAddress`; no GLAD/GLEW/GLFW.
- No loader, no CPU ARGB expansion, no per-pixel y-scale branching.
- The tool builds only when `SMACKER_BUILD_TOOLS=ON`, `SMACKER_BUILD_STATIC=ON`, and both SDL and OpenGL are found; otherwise warn and skip.
- Both `SMK_FLAG_Y_DOUBLE` and `SMK_FLAG_Y_INTERLACE` mean "display at twice the height".
- The only user-visible option is `--frames N`; `0` means no limit.

## Review Focus

- **Minimized or zero-size drawable:** the draw path must not divide by zero or call `glViewport` with a negative size; it should clear and skip the draw.
- **Palette change mid-stream:** a later frame that carries a new palette must repaint in the new colours, not stale ones.
- **Y-scale hint:** a file with either y-scale flag must open at `w × 2h` and show source rows doubled, not squashed into the original height.
- **GL setup failure:** a missing entry point, shader compile error, or link error must print a clear message and exit non-zero, never crash.
- **`--frames` bounds:** `--frames` less than or greater than the file's length must exit cleanly in both cases, with no hang or overrun.

---

### Task 1: Build wiring, CLI, and GL bootstrap

**Files:**
- Create: `src/tools/smk_gl_player.c`
- Modify: `CMakeLists.txt:17-19` (option) and `CMakeLists.txt:109-161` (tools block)

**Interfaces:**
- Consumes: `smk_open_file`, `smk_info_all`, `smk_info_video`, `smk_enable_video`, `smk_first`, `smk_next`, `smk_close` from `smacker.h`; `SMK_USE_SDL3` compat macros from `src/tools/smk_sdl_player.c:24-49`.
- Produces:
  - `struct options { const char *path; unsigned long frame_limit; };`
  - `static int parse_options(int argc, char *argv[], struct options *out);` — returns `0` success, `1` help printed, `-1` error (message already printed).
  - `static int gl_context_create(SDL_Window *window, SDL_GLContext *context);` — returns `0` or `-1`.
  - `main(int argc, char *argv[])` opening a window sized `w × (h * 2)` when the y-scale hint is set, drawing a black frame, and exiting after `--frames`.

- [ ] **Step 1: Add the CMake option and guarded target**

Add next to the other options:

```cmake
option(SMACKER_BUILD_GL_PLAYER "Build the OpenGL smk_gl_player example" ON)
```

Inside the existing `if(SMACKER_BUILD_TOOLS)` block, after the SDL player block, add a target that requires static libsmacker, a found SDL target, and `find_package(OpenGL QUIET)` + `OpenGL::GL`. Warn and skip (do not fail) for each missing prerequisite. Mirror the SDL player's `smacker_set_compile_settings`, `SMK_USE_SDL3` definition, and `SDL2::SDL2main` handling. Link `smacker_static`, `${SMACKER_SDL_TARGET}`, and `OpenGL::GL`.

- [ ] **Step 2: Write the CLI and bootstrap source**

Create `src/tools/smk_gl_player.c`. Copy the SDL2/SDL3 compatibility macros from `src/tools/smk_sdl_player.c:24-49`. Include `<SDL_opengl.h>` (`<SDL3/SDL_opengl.h>` under `SMK_USE_SDL3`). `parse_options` accepts:

```
Usage: smk_gl_player [--frames N] file.smk
```

`-h`/`--help` prints usage and returns `1`. `--frames` takes one base-10 unsigned argument; anything else or a missing path returns `-1` with `unknown option` / `--frames needs a value` / `Usage: ...` on stderr. Reject extra positional arguments.

`main` opens the file with `smk_open_file(path, SMK_MODE_DISK)`, prints the same header lines as the SDL player (`Width`, `Height`, `Frames`, `FPS`), reports `Y-scale: doubled` when the hint is set, and then:

1. Call `SDL_Init(SDL_INIT_VIDEO)`, failing with the `SDL_GetError()` text.
2. Set `SDL_GL_CONTEXT_MAJOR_VERSION` 3, `SDL_GL_CONTEXT_MINOR_VERSION` 3, `SDL_GL_CONTEXT_PROFILE_MASK` `SDL_GL_CONTEXT_PROFILE_CORE`, `SDL_GL_DOUBLEBUFFER` 1, `SDL_GL_DEPTH_SIZE` 0.
3. Create a resizable `SDL_WINDOW_OPENGL` window titled `libsmacker OpenGL player - <path>` at `w × (y_double ? 2h : h)`.
4. Create the GL context with `SDL_GL_CreateContext`, call `SDL_GL_MakeCurrent`, `SDL_GL_SetSwapInterval(0)`, and log `GL_VERSION` / `GL_RENDERER` via `glGetString`.
5. Enable video and `smk_first`; fail cleanly on `SMK_ERROR`.
6. Loop: poll events (quit on `SDL_QUIT`, `Esc`, `q`), `glClearColor(0,0,0,1)`, `glClear(GL_COLOR_BUFFER_BIT)`, `SDL_GL_SwapWindow`, advance with `smk_next`, and stop on `SMK_DONE`/`SMK_ERROR` or when `frame_limit` frames have been presented. Use a plain `SDL_Delay` for `usf` pacing at this stage.
7. Tear down window, context, and `smk_close` on every exit path.

- [ ] **Step 3: Configure and build the SDL2 variant**

```sh
cmake -S . -B build-gl2 -DSMACKER_BUILD_TOOLS=ON -DSMACKER_SDL_VERSION=2
cmake --build build-gl2 --target smk_gl_player
```

Expected: configure reports `smk_gl_player: linking ...`, build succeeds with no warnings.

- [ ] **Step 4: Check CLI error paths**

```sh
./build-gl2/smk_gl_player            # exit 1, usage on stderr
./build-gl2/smk_gl_player --frames   # exit 1, "--frames needs a value"
./build-gl2/smk_gl_player nope.smk   # exit 1, "Errors encountered opening"
```

Expected: each message is clear and each exit status is 1.

- [ ] **Step 5: Generate a test file and smoke-run the window**

```sh
python3 - <<'PY'
import os, struct
os.makedirs("build-gl2/frames", exist_ok=True)
w, h = 320, 240
for f in range(10):
    body = bytearray()
    for y in range(h):
        for x in range(w):
            if y < h // 2 and x < w // 2:   r, g, b = 255, 0, 0
            elif y < h // 2:                r, g, b = 0, 255, 0
            elif x < w // 2:                r, g, b = 0, 0, 255
            else:                           r, g, b = 255, 255, 255
            body += bytes((b, g, r))
    header = bytes((0, 0, 2)) + b"\0" * 5 + struct.pack("<HH", 0, 0)
    header += struct.pack("<HH", w, h) + bytes((24, 0x20))
    with open(f"build-gl2/frames/frame{f:03d}.tga", "wb") as out:
        out.write(header + body)
PY
./build-gl2/smk_build build-gl2/frames -o build-gl2/test.smk --palette frame --keyframe-interval 3
./build-gl2/smk_gl_player --frames 1 build-gl2/test.smk
```

Expected: prints the header, `GL_VERSION`, and exits 0 after one frame. A black window may flash.

- [ ] **Step 6: Commit**

```sh
git add CMakeLists.txt src/tools/smk_gl_player.c
git commit -m "feat: add smk_gl_player OpenGL window bootstrap"
```

### Task 2: GPU frame path, shaders, and letterboxing

**Files:**
- Modify: `src/tools/smk_gl_player.c`

**Interfaces:**
- Consumes: `smk_get_video`, `smk_get_palette`, `smk_info_video`; the window/context and loop from Task 1.
- Produces:
  - `struct gl_api` with one function pointer per entry point listed below, and `static int gl_api_load(struct gl_api *api);` returning `0`, or `-1` after printing `Missing OpenGL entry point: <name>`.
  - `struct gl_renderer { struct gl_api api; GLuint program, vao, frame_tex, palette_tex; GLint u_frame, u_palette, u_y_scale; unsigned char last_palette[768]; int have_palette; };`
  - `static int gl_renderer_init(struct gl_renderer *r, unsigned long w, unsigned long h);`
  - `static void gl_renderer_upload(struct gl_renderer *r, const unsigned char *frame, const unsigned char *palette, unsigned long w, unsigned long h);`
  - `static void gl_renderer_draw(struct gl_renderer *r, int win_w, int win_h, unsigned long w, unsigned long h, int y_double);`
  - `static void gl_renderer_free(struct gl_renderer *r);`
  - `static void compute_viewport(int win_w, int win_h, unsigned long src_w, unsigned long src_h, int *x, int *y, int *vw, int *vh);`

- [ ] **Step 1: Load the GL entry points**

Define `struct gl_api` with exactly these pointers (typedefs in the file, so no `PFN*` dependency):

```c
active_texture, gen_vertex_arrays, bind_vertex_array, delete_vertex_arrays,
create_shader, shader_source, compile_shader, get_shaderiv,
get_shader_info_log, create_program, attach_shader, link_program,
get_programiv, get_program_info_log, use_program, get_uniform_location,
uniform1i, uniform1f, gen_textures, delete_textures, bind_texture,
tex_image_2d, tex_sub_image_2d, tex_parameteri, pixel_storei,
delete_program, delete_shader
```

`gl_api_load` resolves each with `SDL_GL_GetProcAddress`, printing `Missing OpenGL entry point: <name>` and returning `-1` on the first null. Base GL 1.1 calls (`glViewport`, `glClear`, `glClearColor`, `glDrawArrays`, `glGetString`, `glGetError`) are linked directly.

- [ ] **Step 2: Add the shaders**

Vertex shader source, compiled as `GL_VERTEX_SHADER`:

```glsl
#version 330 core
out vec2 v_uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
```

Fragment shader source, compiled as `GL_FRAGMENT_SHADER`:

```glsl
#version 330 core
uniform sampler2D u_frame;
uniform sampler2D u_palette;
uniform float u_y_scale;
in vec2 v_uv;
out vec4 frag_color;
void main()
{
    float index = texture(u_frame, vec2(v_uv.x, v_uv.y * u_y_scale)).r;
    int slot = int(index * 255.0 + 0.5);
    vec3 rgb = texelFetch(u_palette, ivec2(slot, 0), 0).rgb;
    frag_color = vec4(rgb, 1.0);
}
```

The vertex shader's `1.0 - p.y` handles the top-down frame buffer versus bottom-up GL texture origin. `u_y_scale` is `0.5` when doubled, which maps two displayed rows onto each source row.

- [ ] **Step 3: Implement `gl_renderer_init` and `gl_renderer_free`**

Init calls `gl_api_load`, compiles and links the program (printing the shader/program info log and returning `-1` on failure), generates the VAO, and creates:

- `frame_tex`: `glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL)`
- `palette_tex`: `glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL)`

Both textures get `GL_NEAREST` min/mag and `GL_CLAMP_TO_EDGE` wrap. After `glUseProgram`, set `u_frame` to unit 0 and `u_palette` to unit 1 once. `gl_renderer_free` deletes the program, VAO, and both textures.

- [ ] **Step 4: Implement upload, viewport, and draw**

`gl_renderer_upload` sets `GL_UNPACK_ALIGNMENT` to 1, always `glTexSubImage2D`s the `w × h` frame into `frame_tex`, and uploads the 768-byte palette only when `have_palette` is false or `memcmp` against `last_palette` differs; save the bytes and set `have_palette` after each palette upload.

`compute_viewport` returns `*vw = *vh = *x = *y = 0` when `win_w <= 0 || win_h <= 0 || src_w == 0 || src_h == 0`. Otherwise divide `win_w/src_w` and `win_h/src_h`, take the minimum, round `vw`/`vh` to the nearest integer, clamp each to `[1, win]`, and centre with `x = (win_w - vw) / 2`, `y = (win_h - vh) / 2`.

`gl_renderer_draw` clears to black, computes the viewport, returns early when `vw <= 0`, then binds the program, binds `frame_tex` to unit 0, `palette_tex` to unit 1, sets `u_y_scale` to `y_double ? 0.5f : 1.0f`, binds the VAO, and calls `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)`.

- [ ] **Step 5: Wire the renderer into the loop and check GL errors**

Replace the clear/swap in the main loop with `gl_renderer_upload` + `gl_renderer_draw` + `SDL_GL_SwapWindow`. Obtain the drawable size per frame with `SDL_GL_GetDrawableSize` (SDL2) or `SDL_GetWindowSizeInPixels` (SDL3). After init and once after the first draw, call `glGetError`; print `OpenGL error 0x%04X` and exit non-zero if it is not `GL_NO_ERROR`.

- [ ] **Step 6: Build and verify orientation and palette**

```sh
cmake --build build-gl2 --target smk_gl_player
./build-gl2/smk_gl_player --frames 6 build-gl2/test.smk
```

Expected: exit 0 and no GL errors. While it plays, capture the screen with `screencapture -x "$TMPDIR/smk_gl.png"` and confirm the window is red top-left, green top-right, blue bottom-left, white bottom-right (upright, not flipped). `--palette frame` means the palette may shift slightly between frames, but the quadrants must stay distinct.

- [ ] **Step 7: Verify the y-scale hint**

```sh
python3 - <<'PY'
import struct
data = bytearray(open("build-gl2/test.smk", "rb").read())
data[0x14:0x18] = struct.pack("<I", 2)  # Y-double flag
open("build-gl2/test-ydouble.smk", "wb").write(data)
PY
./build-gl2/smk_gl_player --frames 6 build-gl2/test-ydouble.smk
```

Expected: the startup line reports the doubled height, the window opens at `320 × 480`, and the screenshot shows the top half-colours and bottom half-colours each occupying twice their source height.

- [ ] **Step 8: Verify a minimized window does not crash**

Run the player without `--frames`, minimize the window, then restore it, then quit with `q`.

Expected: no crash, no stderr output, and the frame redraws correctly after restore.

- [ ] **Step 9: Commit**

```sh
git add src/tools/smk_gl_player.c
git commit -m "feat: render Smacker frames on the GPU in smk_gl_player"
```

### Task 3: Audio, seeking, pacing, and documentation

**Files:**
- Modify: `src/tools/smk_gl_player.c`
- Modify: `README.md` (example-program list, changelog)
- Modify: `docs/usage.md` (example programs, build command)
- Modify: `INSTALL.md` (options table, SDL note)

**Interfaces:**
- Consumes: Everything from Tasks 1 and 2; `smk_info_audio`, `smk_enable_audio`, `smk_get_audio`, `smk_get_audio_size`, `smk_seek_keyframe`; the audio shims from `src/tools/smk_sdl_player.c:62-153`.
- Produces: the finished `smk_gl_player`.

- [ ] **Step 1: Add audio**

Copy the `video_init`/`audio_init`/`audio_open`/`audio_pause`/`audio_clear`/`audio_queue`/`audio_close` helpers and the SDL2/SDL3 audio compat macros from `src/tools/smk_sdl_player.c:62-153` into the player. After the video check, enable the first existing audio track, open it, and queue `smk_get_audio(track)` after each presented frame. On any audio failure, print the SDL error and continue silently, matching the SDL player.

- [ ] **Step 2: Add pause, seek, and exact pacing**

Add the event handling from `src/tools/smk_sdl_player.c:340-385`: Space toggles pause (and pauses audio), Left/Right seek one second with `smk_seek_keyframe`, clears queued audio, and resets the timer. Replace the `SDL_Delay` pacing with the SDL player's `wait_until`/`next_ms` scheme, including the `SMK_LAST` one-interval hold. Keep the `--frames` counter: increment it per presented frame and break when it reaches a non-zero limit.

- [ ] **Step 3: Build both SDL variants and run the existing tests**

```sh
cmake -S . -B build-gl3 -DSMACKER_BUILD_TOOLS=ON -DSMACKER_BUILD_TESTS=ON -DSMACKER_SDL_VERSION=3
cmake --build build-gl2 --target smk_gl_player
cmake --build build-gl3
ctest --test-dir build-gl3 --output-on-failure
```

Expected: both players build, every test passes.

- [ ] **Step 4: Verify playback, bounds, and rebuild behaviour**

```sh
./build-gl2/smk_gl_player --frames 1 build-gl2/test.smk        # exit 0
./build-gl2/smk_gl_player --frames 100 build-gl2/test.smk      # exit 0, stops at end
./build-gl3/smk_gl_player --frames 20 build-gl2/test.smk       # exit 0
```

Expected: no crashes, no GL errors, clean exits; the 100-frame run stops after the file ends rather than hanging.

- [ ] **Step 5: Update the documentation**

- `README.md`: add `src/tools/smk_gl_player.c` to the example-program list and a changelog bullet under Unreleased.
- `docs/usage.md`: add the player to "Example programs", note that it keeps the frame palettized and resolves the palette in GLSL, and add `./build/smk_gl_player file.smk` to the build block.
- `INSTALL.md`: add `SMACKER_BUILD_GL_PLAYER` to the options table and a line that a missing OpenGL package skips only this example.

- [ ] **Step 6: Commit**

```sh
git add src/tools/smk_gl_player.c README.md docs/usage.md INSTALL.md
git commit -m "docs: describe the OpenGL player and finish its playback loop"
```

---

## Self-Review

- **Spec coverage:** build wiring (Task 1), index/palette textures + shaders + y-double + viewport + nearest filtering (Task 2), audio/seek/pacing (Task 3), `--frames` (Tasks 1 and 3), docs and verification (Task 3). All spec sections map to a task.
- **Step scan:** each step names the exact signature, GL call, or command; no "handle edge cases" or "add validation" placeholders.
- **Type consistency:** `gl_renderer_upload` takes `(r, frame, palette, w, h)` in both its definition and its use; `compute_viewport` returns display sizes through `vw`/`vh`; `y_double` is an `int` everywhere.
- **Review Focus tests:** zero-size drawable (Task 2 Step 8, plus the `compute_viewport` guard), palette change (Task 2 Step 6 with `--palette frame`), y-scale (Task 2 Step 7), GL setup failure (Task 1 Step 3's compile/link logging and Task 2 Step 5's `glGetError`; every loaded pointer is null-checked in Step 1), `--frames` bounds (Task 3 Step 4).
- **Proportion:** three tasks, no shader or function bodies beyond the two shader sources and the CMake requirement.
