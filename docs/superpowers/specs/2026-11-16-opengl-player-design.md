# OpenGL/GLSL Smacker player

## Intent

`smk_sdl_player` expands every decoded frame to 32-bit ARGB on the CPU, then
uploads `w*h*4` bytes and lets SDL scale it. This adds a second playback
example, `smk_gl_player`, that keeps the frame on the GPU from upload to
presentation: the 8-bit palette-index frame goes up as a single-channel
texture, and a GLSL fragment shader resolves the palette, applies the y-scale
hint, and lets the GPU scale and letterbox the result.

The point is to demonstrate the smallest, most direct GPU path for this
decoder's output, not to replace the SDL player.

## Goals

- New `src/tools/smk_gl_player.c`, built as `smk_gl_player` when SDL and
  OpenGL are available.
- Per frame, upload `w*h` index bytes (not ARGB) with `glTexSubImage2D`.
- Palette lookup, y-doubling, and nearest-neighbour scaling happen in shaders.
- Same playback behaviour as the SDL player: audio, pause, seek, quit, pacing,
  aspect-correct letterboxing, resizable window.
- Works with SDL3 and SDL2, on macOS, Linux, and Windows. No new third-party
  dependency beyond OpenGL itself.

## Non-goals

- Moving Smacker Huffman/block decoding to the GPU. The bitstream is
  sequential and CPU-bound; decoding stays in libsmacker.
- Replacing or refactoring `smk_sdl_player.c`.
- A reusable rendering library or GL abstraction layer in the library.
- Video output in `driver` or `smk2avi`.
- Colour-space conversion, gamma, shaders beyond palette lookup.

## Anti-goals

- Adding GLAD, GLEW, GLFW, or another loader/windowing dependency.
- Interleaving the index frame into ARGB on the CPU as a fallback "just in
  case".
- A shader that does per-pixel branching on y-scale when a uniform multiplier
  will do.
- A GL path that only works on one OS.

## Constraints

- C99, `-Wall -Wextra`, matching the tree.
- The library API is unchanged.
- `smk_get_video()` returns `w*h` bytes regardless of y-scale, so the shader
  owns doubling.
- Both `SMK_FLAG_Y_DOUBLE` and `SMK_FLAG_Y_INTERLACE` mean "display at twice
  the height" per `docs/smacker.md`.
- OpenGL 3.3 core profile is the target. macOS supports 3.3 core through SDL,
  and it is available on any desktop that can run the SDL player.
- GL entry points are resolved with `SDL_GL_GetProcAddress`, so no loader
  library is needed. `OpenGL::GL` is still linked for the base symbols.
- The player is built only when `SMACKER_BUILD_TOOLS=ON`, `SMACKER_BUILD_STATIC=ON`,
  SDL is found, and OpenGL is found. Absence of GL degrades to a warning, like
  the SDL player.

## Approaches considered

1. **SDL window + raw OpenGL 3.3 core, index texture + palette texture +
   shaders.** Chosen. Reuses the SDL player's audio/event/timing plumbing,
   has no new dependency, and puts the actual work in GLSL.
2. **Raw platform GL (NSGL/WGL/GLX) without SDL.** Rejected: OS-specific
   window and audio plumbing for no rendering benefit.
3. **SDL renderer with CPU palette expansion.** Rejected: keeps the CPU
   conversion and uploads ARGB, so it does not offload anything.

## Design

### Rendering pipeline

- One texture unit holds the frame: `GL_R8`, `GL_NEAREST`, `GL_CLAMP_TO_EDGE`,
  `UNPACK_ALIGNMENT 1`. Allocated once with `glTexImage2D`, updated per frame
  with `glTexSubImage2D`.
- A second unit holds the palette: `GL_RGB8`, 256×1, `GL_NEAREST`. Re-uploaded
  only when the 768 palette bytes differ from the last upload.
- A fullscreen triangle strip is drawn with `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)`
  and an empty VAO. The vertex shader derives clip-space positions from
  `gl_VertexID`; no VBO or attributes.
- Fragment shader samples `texelFetch`-free: it samples the index texture at
  `vec2(v_uv.x, v_uv.y * u_y_scale)` and uses the red channel as a coordinate
  into the palette texture. `u_y_scale` is `1.0` normally and `0.5` when the
  y-scale hint is set, which duplicates each source row over two displayed
  rows under nearest sampling.
- Letterboxing stays on the CPU: `glViewport` is set to the largest centred
  rect inside the window with the source aspect, where the source height is
  `h` or `2*h` according to the hint. `glClear` paints the bars black.
- `glTexParameteri(GL_TEXTURE_MIN_FILTER/GL_TEXTURE_MAG_FILTER, GL_NEAREST)`
  keeps Smacker's pixel art crisp. Scaling is still done by the GPU.

### Playback

The event loop is the SDL player's, with the render swap replaced by
`SDL_GL_SwapWindow`. Audio, pause, one-second seeking, first-frame decode,
`SMK_LAST` hold, and `wait_until` pacing are carried over. Swap interval is 0
so the frame timer owns pacing rather than the compositor.

The existing SDL2/SDL3 compatibility macros (`SMK_USE_SDL3`) are reused,
including the audio backends and key handling.

### Build

- New option `SMACKER_BUILD_GL_PLAYER`, default `ON`, effective only under
  `SMACKER_BUILD_TOOLS`.
- Inside the existing tools block, `find_package(OpenGL QUIET)`. Build
  `smk_gl_player` when the SDL target was found, `SMACKER_BUILD_STATIC` is on,
  and OpenGL was found; otherwise warn and skip.
- Sources: `src/tools/smk_gl_player.c`. Link `${SMACKER_LINK_TARGET}` (or
  `smacker_static` like the SDL player), the SDL target, and `OpenGL::GL`.
- The GL entry-point loader is a small table in the player file using
  `SDL_GL_GetProcAddress`; a null entry is a startup error with the missing
  name printed.

### Command line

`smk_gl_player [--frames N] file.smk`

`--frames N` stops cleanly after N frames. It exists so the player can be
smoke-tested non-interactively (build a file with `smk_build`, play 20 frames,
check the exit code); omitting it plays to the end.

### Verification

- Configure and build with `SMACKER_SDL_VERSION=2` and `3`.
- Generate a short `.smk` with `smk_build` from a few generated TGA frames,
  run `smk_gl_player --frames 20` against it, and confirm exit status 0, no
  shader/link errors, and no GL errors reported.
- Confirm the player refuses invalid usage and a missing file cleanly.

### Documentation

Update the tool list in `README.md`, the example-program section in
`docs/usage.md`, the build options in `INSTALL.md`, and the Unreleased
changelog entry.

## Decisions made in this design

- New tool rather than modifying `smk_sdl_player`.
- Raw GL 3.3 core through SDL, no loader library.
- Shaders own palette lookup and y-doubling; CPU owns viewport letterboxing.
- Nearest-neighbour filtering.
- `--frames` flag for scriptable verification.

## Left to the builder

- Exact shader source and attribute/uniform names.
- Whether the palette comparison is a `memcmp` against a saved 768-byte copy or
  a dirty flag (either is fine; `memcmp` is simplest).
- Error-message wording and logging style, matching the SDL player.
