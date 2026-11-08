# Using libsmacker

libsmacker is a small C library that decodes RAD Game Tools Smacker (`.smk`)
files. It provides palette video frames, up to seven audio tracks, and the
metadata needed to play or convert a file. The library has no dependencies
beyond the C standard library.

For the on-disk format and codec details, see [smacker.md](smacker.md).

## Contents

- [Requirements](#requirements)
- [Building and linking](#building-and-linking)
- [Quick start](#quick-start)
- [Complete example](#complete-example)
- [API reference](#api-reference)
  - [Types and constants](#types-and-constants)
  - [Opening and closing](#opening-and-closing)
  - [File information](#file-information)
  - [Enabling decoders](#enabling-decoders)
  - [Decoding frames](#decoding-frames)
  - [Accessing decoded data](#accessing-decoded-data)
  - [Seeking](#seeking)
- [Return codes](#return-codes)
- [Data formats and ownership](#data-formats-and-ownership)
- [Playing audio](#playing-audio)
- [Error handling and limitations](#error-handling-and-limitations)
- [Example programs](#example-programs)

## Requirements

- A C99 compiler. The public header also works from C++ (`extern "C"` is
  provided).
- CMake 3.16 or newer if you use the supplied build system.
- No third-party libraries; only standard C headers are required.

## Building and linking

### Compile the sources directly

Copy these three files into your project and compile them with your program:

- `src/smacker.c`
- `src/smacker.h`
- `src/smk_malloc.h`

```sh
cc -std=c99 -I path/to/libsmacker/src main.c path/to/libsmacker/src/smacker.c
```

In your source, include the public header:

```c
#include "smacker.h"
```

### Build a library with CMake

```sh
cmake -S . -B build
cmake --build build
```

This produces both `libsmacker.a` and a shared `libsmacker` by default. To use
the library from another CMake project:

```cmake
add_subdirectory(path/to/libsmacker)
target_link_libraries(myapp PRIVATE Smacker::smacker)
```

`Smacker::smacker` refers to the shared library when `SMACKER_BUILD_SHARED` is
on, and to the static library otherwise. The include directory is propagated
automatically.

After `cmake --install build --prefix /usr/local`, link the installed library
directly:

```sh
cc -std=c99 main.c -lsmacker
```

See [INSTALL.md](../INSTALL.md) for all build options.

## Quick start

A decode session follows six steps:

1. Open the file with `smk_open_file`, `smk_open_filepointer`, or
   `smk_open_memory`.
2. Read metadata with `smk_info_all`, `smk_info_video`, and `smk_info_audio`.
3. Enable the video and audio tracks you want with `smk_enable_video`,
   `smk_enable_audio`, or `smk_enable_all`. **Decoding is disabled by default.**
4. Decode the first frame with `smk_first`.
5. Read the palette, video, and audio with `smk_get_palette`, `smk_get_video`,
   `smk_get_audio`, and `smk_get_audio_size`. Call `smk_next` to advance.
6. Release everything with `smk_close`.

Frame 0 is decoded by `smk_first`; call `smk_next` once per subsequent frame.
The pointers returned by the `smk_get_*` functions are owned by the library and
remain valid until `smk_close`. Their contents are replaced in place each time
a frame is decoded, so copy anything you need to keep.

## Complete example

This program opens a file, prints its metadata, decodes every logical frame,
and reports the total amount of decoded audio. It compiles as-is against the
static library.

```c
#include "smacker.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    smk s;
    unsigned long w = 0, h = 0, frames = 0, i;
    double usf = 0.0;
    unsigned char track_mask = 0;
    unsigned char channels[7], bitdepth[7];
    unsigned long rates[7];
    unsigned long long audio_bytes = 0;
    unsigned long long video_sum = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: %s file.smk\n", argv[0]);
        return 1;
    }

    /* SMK_MODE_DISK keeps the file on disk and reads frames on demand.
       SMK_MODE_MEMORY loads every frame chunk into RAM during open. */
    s = smk_open_file(argv[1], SMK_MODE_DISK);
    if (s == NULL)
        return 1;

    smk_info_all(s, NULL, &frames, &usf);
    smk_info_video(s, &w, &h, NULL);
    smk_info_audio(s, &track_mask, channels, bitdepth, rates);

    printf("%s: %lu x %lu, %lu frames, %.3f fps\n",
        argv[1], w, h, frames, usf > 0.0 ? 1000000.0 / usf : 0.0);

    for (i = 0; i < 7; i++) {
        if (track_mask & (1u << i)) {
            printf("audio %lu: %lu Hz, %u channel(s), %u-bit\n",
                i, rates[i], (unsigned) channels[i],
                (unsigned) bitdepth[i]);
        }
    }

    /* Decoding is off until enabled. */
    smk_enable_video(s, 1);
    if (track_mask & SMK_AUDIO_TRACK_0)
        smk_enable_audio(s, 0, 1);

    if (smk_first(s) == SMK_ERROR) {
        smk_close(s);
        return 1;
    }

    /* smk_first() decoded frame 0. Decode the remaining logical frames. */
    for (i = 0; i < frames; i++) {
        const unsigned char *video = smk_get_video(s);
        unsigned long j;

        for (j = 0; j < w * h; j++)
            video_sum += video[j];

        if (track_mask & SMK_AUDIO_TRACK_0)
            audio_bytes += smk_get_audio_size(s, 0);

        if (i + 1 < frames && smk_next(s) == SMK_ERROR)
            break;
    }

    printf("decoded %lu frames, video checksum %llu, audio bytes %llu\n",
        frames, video_sum, audio_bytes);

    smk_close(s);
    return 0;
}
```

Note: `SMK_AUDIO_TRACK_0` is the mask bit for track 0, not the track index.
For `smk_enable_audio` and the `smk_get_audio*` functions, pass the track
number itself (`0` through `6`).

## API reference

All functions are declared in `src/smacker.h`.

### Types and constants

```c
typedef struct smk_t *smk;
```

`smk` is an opaque handle. Treat it as a pointer; do not dereference it.

Return codes used by `smk_first` and `smk_next`:

| Constant | Value | Meaning |
| --- | ---: | --- |
| `SMK_ERROR` | `-1` | Decode or I/O error. |
| `SMK_DONE` | `0` | No more frames (`smk_next` after the last frame of a non-ringing file). |
| `SMK_MORE` | `1` | A frame was decoded and more frames remain. |
| `SMK_LAST` | `2` | The frame just decoded was the last physical chunk. |

Open modes:

| Constant | Meaning |
| --- | --- |
| `SMK_MODE_DISK` | Keep the file open and read each frame on demand with `fseek`/`fread`. |
| `SMK_MODE_MEMORY` | Read all frame chunks into memory while opening. |

Y-scale hints returned by `smk_info_video`:

| Constant | Value | Meaning |
| --- | ---: | --- |
| `SMK_FLAG_Y_NONE` | `0` | No vertical scaling. |
| `SMK_FLAG_Y_INTERLACE` | `1` | Frame is vertically interlaced; double the height when displaying. |
| `SMK_FLAG_Y_DOUBLE` | `2` | Frame is vertically doubled. |

Track mask bits:

| Constant | Value | Meaning |
| --- | ---: | --- |
| `SMK_AUDIO_TRACK_0` … `SMK_AUDIO_TRACK_6` | `0x01` … `0x40` | Audio track existence/enable bits. |
| `SMK_VIDEO_TRACK` | `0x80` | Video enable bit for `smk_enable_all`. |

### Opening and closing

```c
smk smk_open_file(const char *filename, unsigned char mode);
smk smk_open_filepointer(FILE *file, unsigned char mode);
smk smk_open_memory(const unsigned char *buffer, unsigned long size);
void smk_close(smk object);
```

**`smk_open_file`** opens `filename` in binary mode and returns a handle, or
`NULL` on failure. `mode` is `SMK_MODE_DISK` or `SMK_MODE_MEMORY`.

**`smk_open_filepointer`** uses an already-open `FILE *`. Ownership of the
stream is transferred to the library in every case:

- With `SMK_MODE_DISK`, the stream must remain open while the handle is used,
  and `smk_close` closes it.
- With `SMK_MODE_MEMORY`, the library reads all frame chunks during open and
  closes the stream immediately.
- If opening fails, the library also closes the stream.

**`smk_open_memory`** parses a complete `.smk` image already in memory. The
library copies every frame chunk into its own allocations during open, so the
caller may free or reuse `buffer` as soon as the function returns. `size` is the
total size of the image in bytes.

**`smk_close`** releases all memory and, in disk mode, closes the stream. It is
safe to call with a handle obtained from any open function. Passing `NULL`
prints a diagnostic and returns.

Do not mix handles: every handle must be closed with `smk_close`, and a
`FILE *` passed to `smk_open_filepointer` must not be closed again by the
caller.

### File information

```c
char smk_info_all(const smk object, unsigned long *frame,
                  unsigned long *frame_count, double *usf);
char smk_info_video(const smk object, unsigned long *w, unsigned long *h,
                    unsigned char *y_scale_mode);
char smk_info_audio(const smk object, unsigned char *track_mask,
                    unsigned char channels[7], unsigned char bitdepth[7],
                    unsigned long audio_rate[7]);
```

Each function returns `0` on success and `-1` if `object` is `NULL` or if every
output pointer is `NULL`. You may pass `NULL` for any output you do not need.

**`smk_info_all`**

- `frame` — current logical frame index, computed as `cur_frame % frame_count`.
  It is `0` before the first decode and after the ring frame wraps.
- `frame_count` — number of logical frames. A file may contain one extra
  "ring" frame at the end; it is **not** included in this count.
- `usf` — microseconds per frame as a `double`. Frames-per-second is
  `1000000.0 / usf`.

**`smk_info_video`**

- `w`, `h` — frame dimensions in pixels. Both may be `0` for an audio-only
  file.
- `y_scale_mode` — one of the `SMK_FLAG_Y_*` constants. This is only a hint:
  libsmacker does not scale the frame for you.

**`smk_info_audio`**

- `track_mask` — bits 0–6 are set for tracks that exist in the file.
- `channels[7]` — `1` for mono, `2` for stereo.
- `bitdepth[7]` — `8` or `16`.
- `audio_rate[7]` — sample rate in Hz.

Entries for tracks that do not exist are zero.

### Enabling decoders

```c
char smk_enable_all(smk object, unsigned char mask);
char smk_enable_video(smk object, unsigned char enable);
char smk_enable_audio(smk object, unsigned char track,
                      unsigned char enable);
```

All three return `0` on success and `-1` if `object` is `NULL`.

**`smk_enable_all`** sets video from bit `0x80` and audio track `n` from bit
`n` of `mask`. Only audio tracks that exist in the file are changed.

**`smk_enable_video`** enables video decoding when `enable` is non-zero.

**`smk_enable_audio`** enables or disables one track. `track` must be `0`
through `6`; the library does not range-check it. Enabling a track that does
not exist does not allocate a buffer, and its decoded size stays `0`.

Decoding is disabled by default. Call one of these functions **before**
`smk_first`. The palette is also only updated while video decoding is enabled.

### Decoding frames

```c
char smk_first(smk object);
char smk_next(smk object);
```

**`smk_first`** rewinds to frame 0 and decodes it. It returns:

- `SMK_MORE` if the file has more than one logical frame.
- `SMK_LAST` if the file has exactly one logical frame.
- `SMK_ERROR` on failure.

**`smk_next`** decodes the next physical chunk and returns:

- `SMK_MORE` if more frames remain.
- `SMK_LAST` when the chunk just decoded is the last physical chunk.
- `SMK_DONE` when called again after `SMK_LAST` on a non-ringing file.
- `SMK_ERROR` on failure.

For a **ringing** file (header flag bit 0 set), the last physical chunk is the
extra ring frame. `smk_next` then wraps to frame 1 rather than frame 0, and it
never returns `SMK_DONE`. If you want to play exactly one pass, stop after
`frame_count` frames instead of waiting for `SMK_DONE`.

A typical one-pass loop:

```c
char rc = smk_first(s);

for (i = 0; i < frame_count; i++) {
    /* use smk_get_palette(), smk_get_video(), smk_get_audio*() */
    if (i + 1 < frame_count)
        rc = smk_next(s);
    if (rc == SMK_ERROR)
        break;
}
```

### Accessing decoded data

```c
const unsigned char *smk_get_palette(const smk object);
const unsigned char *smk_get_video(const smk object);
const unsigned char *smk_get_audio(const smk object, unsigned char track);
unsigned long smk_get_audio_size(const smk object, unsigned char track);
```

**`smk_get_palette`** returns a pointer to `256 * 3` bytes: 256 RGB entries,
three bytes each. The byte order is red, green, blue. All returned values are
already expanded to the full 0–255 range. The palette starts black and is
updated in place when a frame contains palette data. If video decoding is
disabled, it is not updated at all.

**`smk_get_video`** returns a pointer to `w * h` bytes. Each byte is a palette
index; rows are stored top to bottom with no padding. The buffer is updated in
place for every decoded frame. It is `NULL` if `object` is `NULL`.

**`smk_get_audio`** returns the decoded audio chunk for one track. The data
format depends on the track's bit depth (see below). The pointer remains owned
by the library, and its contents are overwritten on the next decoded frame
that contains audio for that track.

**`smk_get_audio_size`** returns the number of decoded bytes for the current
frame of that track. It is `0` for a frame that has no chunk for the track, and
stays `0` for a track that was never enabled. A track disabled after decoding
can retain its last size until a frame without a chunk resets it to `0`. Always
check the size before reading the pointer: the buffer may still contain the
previous frame's data.

### Seeking

```c
char smk_seek_keyframe(smk object, unsigned long frame);
```

Smacker frames are delta-coded, so decoding an arbitrary frame requires
starting from the nearest preceding keyframe. `smk_seek_keyframe` selects
`frame`, walks backwards to the closest keyframe at or before it, decodes that
keyframe, and returns `0`. It returns `-1` if `object` is `NULL` or decoding
fails.

After a successful seek, `smk_info_all` reports the keyframe's index, which may
be lower than the requested frame. To reach the exact requested frame, keep
calling `smk_next` until the current frame matches:

```c
if (smk_seek_keyframe(s, target) == 0) {
    unsigned long cur = 0;

    smk_info_all(s, &cur, NULL, NULL);
    while (cur < target && smk_next(s) != SMK_ERROR) {
        unsigned long next;

        smk_info_all(s, &next, NULL, NULL);
        if (next <= cur)       /* wrapped or stuck */
            break;
        cur = next;
    }
}
```

Pass a frame index below the logical `frame_count`; the function does not
validate the argument against the allocated frame tables.

## Return codes

| Function group | Success | Failure |
| --- | --- | --- |
| `smk_open_*` | non-`NULL` handle | `NULL` |
| `smk_info_*` | `0` | `-1` |
| `smk_enable_*` | `0` | `-1` |
| `smk_first`, `smk_next` | `SMK_MORE` / `SMK_LAST` / `SMK_DONE` | `SMK_ERROR` (`-1`) |
| `smk_seek_keyframe` | `0` | `-1` |
| `smk_get_audio_size` | byte count (may be `0`) | `0` |
| `smk_get_*` | non-`NULL` pointer | `NULL` for a `NULL` handle |

Diagnostics are written to `stderr` with a `libsmacker::` prefix. Many internal
allocations use `smk_malloc`, which calls `exit(EXIT_FAILURE)` on failure; a
few later allocations report an error instead.

## Data formats and ownership

### Video

- The palette is 768 bytes, ordered R, G, B, with 8 bits per component.
- The frame is `w * h` bytes of palette indices, row-major, top to bottom.
- There is no row padding or stride.
- The decoder writes 4x4 pixel blocks. Dimensions are expected to be multiples
  of 4; see [limitations](#error-handling-and-limitations).

Converting a frame to packed RGB:

```c
const unsigned char *pal = smk_get_palette(s);
const unsigned char *idx = smk_get_video(s);
unsigned char *rgb = malloc(w * h * 3);
unsigned long i;

for (i = 0; i < w * h; i++) {
    unsigned int c = (unsigned int) idx[i] * 3;

    rgb[i * 3 + 0] = pal[c + 0];
    rgb[i * 3 + 1] = pal[c + 1];
    rgb[i * 3 + 2] = pal[c + 2];
}
```

The palette is the same size as the destination and is fully opaque; there is
no alpha channel.

### Audio

Each existing track produces one chunk per frame that contains data for it.
The decoded bytes are interleaved PCM:

| Bit depth | Sample format |
| --- | --- |
| 8-bit | **Unsigned** 8-bit, silence is `0x80` |
| 16-bit | **Signed** 16-bit, little-endian (see the note below) |

Stereo frames store left and right samples interleaved. The sample rate is the
value returned by `smk_info_audio`. For a mono track, `channels` is `1`; for
stereo, `2`.

The 8-bit data is unsigned even though the DPCM deltas are signed. If you feed
it to an audio API that expects signed 8-bit samples, convert with
`sample ^ 0x80` (or subtract 128), otherwise you will hear a large DC offset
and distortion.

Raw 16-bit PCM is copied from the file unchanged and is therefore little-endian.
Huffman DPCM output is assembled into a native `short`, so on big-endian hosts
that path produces host-endian bytes. Convert with a byte swap if you need
portable little-endian PCM.

### Ownership and lifetime

- The `smk` handle is owned by the caller and must be released with
  `smk_close`.
- A `FILE *` passed to `smk_open_filepointer` is owned by the library.
- The buffer passed to `smk_open_memory` is copied and remains owned by the
  caller.
- Pointers returned by `smk_get_palette`, `smk_get_video`, and `smk_get_audio`
  point into library-owned storage. Do not free them. They remain valid until
  `smk_close`, but their contents change on subsequent decodes.
- Copy any frame or audio data you need to keep beyond the next `smk_next`
  call.

## Playing audio

A minimal audio setup decision looks like this:

```c
unsigned char mask = 0, channels[7], bitdepth[7];
unsigned long rates[7];
int track;

smk_info_audio(s, &mask, channels, bitdepth, rates);

for (track = 0; track < 7; track++) {
    if (mask & (1u << track))
        break;
}

if (track < 7) {
    smk_enable_audio(s, (unsigned char) track, 1);
    /* open an output stream using rates[track], channels[track],
       bitdepth[track] and the sample format rules above */
}
```

Once per decoded frame, queue that frame's audio:

```c
unsigned long bytes = smk_get_audio_size(s, (unsigned char) track);

if (bytes > 0)
    queue_audio(smk_get_audio(s, (unsigned char) track), bytes);
```

Audio chunks are small (one per frame), so a player normally queues them into a
buffered output device. If you seek while audio is queued, clear or flush the
device first so old audio does not play after the jump.

## Error handling and limitations

- **Decoding is opt-in.** Forgetting to call `smk_enable_video`/`smk_enable_audio`
  produces zeroed or stale buffers, not an error.
- **The library is not thread-safe.** `smk_render_palette` uses a file-static
  scratch palette, and the decoder writes diagnostics to `stderr`. Do not
  decode on two handles concurrently from different threads. Separate
  processes are fine; sequential use of separate handles is fine.
- **Disk mode performs one `fseek` and one allocation per frame.** This is
  convenient for local files but poor for non-seekable streams. Use memory
  mode for pipes, network streams, or repeated random access. `smk_open_memory`
  and `SMK_MODE_MEMORY` load the entire frame data set into RAM.
- **Malformed files are only partially validated.** Several internal readers
  trust header sizes and frame tables. A corrupt file can cause decode errors,
  very large allocations, or out-of-bounds reads. Do not feed untrusted files to
  a process that must remain available.
- **Some argument checks are missing.** `smk_enable_audio` and
  `smk_seek_keyframe` do not range-check their track/frame arguments. Keep
  tracks in `0..6` and frames below `frame_count`.
- **Bink Audio is not supported.** Tracks flagged as Bink-compressed are opened
  but produce no decoded samples.
- **Video dimensions are assumed to be multiples of 4.** The decoder paints
  whole 4x4 blocks. Widths and heights that are not multiples of 4 can decode
  incorrectly, and a partial final block row can write past the end of the
  frame buffer.
- **Y-scale flags are hints only.** `smk_info_video` reports them, but the
  decoder never scales or deinterlaces the image.
- **The playback examples are not hardened.** `driver`, `smk2avi`, and
  `smk_sdl_player` demonstrate the API but do not validate untrusted input.

## Example programs

The source tree includes three programs that use the public API:

- `src/tools/driver.c` — decodes every frame to a BMP and every audio track to
  a raw file. Uses memory mode.
- `src/tools/smk2avi.c` — converts one or more `.smk` files to AVI with
  uncompressed RGB video and PCM audio. Shows multi-track buffering and AVI
  container writing.
- `src/tools/smk_sdl_player.c` — plays video and the first audio track through
  SDL2 or SDL3. Shows frame pacing, palette conversion, and audio queueing.

Build them with:

```sh
cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON
cmake --build build
./build/driver file.smk
./build/smk2avi file.smk
./build/smk_sdl_player file.smk
```

See [INSTALL.md](../INSTALL.md) for SDL selection and other build details.
