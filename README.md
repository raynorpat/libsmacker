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

## License

libsmacker is released under the Lesser GNU Public License, v2.1. See the file
[LICENSE](LICENSE) for more information.

## Usage

See the [webpage](http://libsmacker.sourceforge.net) for sample code and
function documentation. The source package additionally includes a pair of
driver programs:

- `src/tools/driver.c` - dumps all frames of a file to a `bmp/` subdirectory, and
  all audio as raw streams to `out_*.raw` files in the current working directory.
- `src/tools/smk2avi.c` - converts `.smk` file(s) to AVI files: uncompressed
  24-bit color and a PCM audio stream.

Though the libraries are "bulletproofed" the sample apps are not: be cautious
if you plan to implement in some critical environment.

## Building

libsmacker uses CMake (3.16 or newer) and requires a C99 compiler. The library
sources live in `src/`, and the example programs live in `src/tools/`.

```sh
cmake -S . -B build
cmake --build build
```

This builds both static and shared libraries by default. To also build the
`driver` and `smk2avi` example programs, configure with:

```sh
cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON
cmake --build build
```

See [INSTALL.md](INSTALL.md) for the full list of build options and
installation instructions.

## Changelog

### Unreleased

- Replaced the autotools and IDE project files with a CMake build.
- Install `smacker.h` and the libsmacker libraries with `cmake --install`.
- Added optional `driver` and `smk2avi` build targets.
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
