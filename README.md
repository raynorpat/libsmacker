# libsmacker

A C library for decoding `.smk` Smacker Video files.

- Version 1.2.0
- 2021-06-02
- (c) Greg Kennedy 2013-2021
- <http://libsmacker.sourceforge.net>

## Introduction

libsmacker is a cross-platform C library which can be used for decoding Smacker
Video files produced by RAD Game Tools. Smacker Video was the king of video
middleware in the 1990s, and its 256-color compressed video format was used in
over 2600 software titles.

libsmacker implements the minimum feature set required from `smackw32.dll` to
get an `.smk` file off a disk and the frames / audio into a buffer in the
correct order.

The source tree also includes `smk_build`, an encoder that writes SMK2 files
from image directories, WAV audio, and smk2avi-style AVI files. See
[docs/encoder.md](docs/encoder.md) for its CLI, C API, and format details.

## License

libsmacker is released under the Lesser GNU Public License, v2.1. See the file
[LICENSE](LICENSE) for more information.

## Usage

See [docs/usage.md](docs/usage.md) for the API and a complete example, or the
[webpage](http://libsmacker.sourceforge.net) for the original documentation.
The source package additionally includes nine example programs:

- `src/tools/driver.c` - dumps all frames of a file to a `bmp/` subdirectory, and
  all audio as raw streams to `out_*.raw` files in the current working directory.
- `src/tools/smk2avi.c` - converts `.smk` file(s) to AVI files: uncompressed
  24-bit color and a PCM audio stream.
- `src/tools/smk_sdl_player.c` - plays a `.smk` file in a window with sound
  using SDL3 (or SDL2). Links against the static libsmacker library.
- `src/tools/smk_gl_player.c` - plays a `.smk` file with OpenGL 3.3. The frame
  stays palettized: an 8-bit index texture and a palette texture go to the
  GPU, and a GLSL shader resolves the palette and scales the image.
- `src/tools/smk_vk_player.c` - the same GPU path through Vulkan, with SDL
  providing the window, surface, and audio. Shaders are GLSL compiled to
  SPIR-V at build time.
- `src/tools/smk_metal_player.m` - the same GPU path through Metal and
  `SDL_Metal`, with MSL compiled at runtime.
- `src/tools/smk_d3d11_player.c` - Windows-only Direct3D 11 player with a
  plain Win32 window and `waveOut` audio; no SDL.
- `src/tools/smk_d3d9_player.c` - Windows-only Direct3D 9 player with the same
  Win32 and `waveOut` playback core.
- `src/tools/smk_build/` - encodes image directories or AVI files into SMK2,
  with adaptive palettes, delta frames, PCM audio, and optional verification.

Though the libraries are "bulletproofed" the sample apps are not: be cautious
if you plan to implement in some critical environment.

## Documentation

- [docs/usage.md](docs/usage.md) - the public API, build and link instructions,
  a complete example, ownership and return-code rules, audio and seeking notes,
  and limitations.
- [docs/smacker.md](docs/smacker.md) - the Smacker container, palette, audio,
  Huffman tree, and 4x4 block video formats in detail, including differences
  between the MultimediaWiki description and the actual decoders.
- [docs/encoder.md](docs/encoder.md) - the `smk_build` CLI and C API, supported
  inputs and output modes, verification, and the encoder roadmap.

## Building

libsmacker uses CMake (3.16 or newer) and requires a C99 compiler. The library
sources live in `src/`, and the example programs live in `src/tools/`.

```sh
cmake -S . -B build
cmake --build build
```

This builds both static and shared libraries by default. To also build the
example programs, configure with:

```sh
cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON
cmake --build build
```

The `driver`, `smk2avi`, and `smk_build` programs are always built by that
option. The SDL playback example is added when SDL3 or SDL2 is found and the
static library is enabled. CMake prefers SDL3; pass `-DSMACKER_SDL_VERSION=2`
or `-DSMACKER_SDL_VERSION=3` to force a version. The OpenGL playback example
is added when an OpenGL package is found as well.

The Vulkan player needs SDL, Vulkan, and `glslc`; the Metal player needs SDL
and the Apple Metal frameworks; the two Direct3D players are built only for a
Windows target. Each is skipped with a warning when its prerequisites are
missing, so the default build still succeeds.

To build and run the encoder test suite as well:

```sh
cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON -DSMACKER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

See [INSTALL.md](INSTALL.md) for the full list of build options and
installation instructions.

## Changelog

### Unreleased

- Replaced the autotools and IDE project files with a CMake build.
- Install `smacker.h` and the libsmacker libraries with `cmake --install`.
- Added optional `driver` and `smk2avi` build targets.
- Added `smk_sdl_player`, an SDL2/SDL3 playback example.
- Added `smk_gl_player`, an OpenGL 3.3 playback example that keeps frames
  palettized and resolves the palette in GLSL.
- Added `smk_vk_player`, `smk_metal_player`, `smk_d3d11_player`, and
  `smk_d3d9_player`, which apply the same GPU palette path to Vulkan, Metal,
  Direct3D 11, and Direct3D 9. The D3D players use Win32 and `waveOut`
  directly instead of SDL.
- Added `smk_build`, an SMK2 encoder for image directories, WAV audio, and
  smk2avi-style AVI files, with adaptive palettes, delta frames, keyframes,
  ring frames, PCM audio, and round-trip verification.
- Converted the documentation to Markdown.

### 1.2.0

- Major refactor of data structures for performance.
- Merge all files into a single unified `.c` source file.
- Support audio-only `.smk` files.

### 1.1.1

- Re-license under LGPL 2.1.

### 1.1

- Switch to autotools-based build.
- Incorporates patches from Dalerank Slim, Gennady Trafimenkov, and Bianca van
  Schaik.
- Performance improvements and code cleanup / safety.

### 1.0

- Initial revision.

## Contact

Questions/comments:

- by email: <kennedy.greg@gmail.com>
- by website: <http://libsmacker.sourceforge.net>

Enjoy!
