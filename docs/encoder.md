# Encoder

This document covers `smk_build`, the libsmacker encoder frontend, and the
reusable encoder sources in `src/encoder/`. The encoder writes SMK2 files with
palette video and uncompressed PCM audio. It is intentionally the mirror image
of the decoder: every file it writes can be opened, verified, and played by
libsmacker.

## Contents

- [Building](#building)
- [Command line](#command-line)
  - [Options](#options)
  - [Exit codes](#exit-codes)
- [Inputs](#inputs)
  - [Image directories](#image-directories)
  - [WAV audio](#wav-audio)
  - [AVI files](#avi-files)
- [Output format](#output-format)
  - [Palette modes](#palette-modes)
  - [Dithering](#dithering)
  - [Keyframes and ring frames](#keyframes-and-ring-frames)
  - [Audio](#audio)
- [Verification](#verification)
- [C API](#c-api)
- [Limitations and roadmap](#limitations-and-roadmap)
- [Tests](#tests)

## Building

The encoder is built as part of the tools target. It has no separate installed
library: `src/encoder/*.c` is compiled directly into `smk_build` and the test
binary.

```sh
cmake -S . -B build -DSMACKER_BUILD_TOOLS=ON -DSMACKER_BUILD_TESTS=ON
cmake --build build
./build/smk_build --help
```

`SMACKER_BUILD_TESTS=ON` currently requires `SMACKER_BUILD_TOOLS=ON` because the
CLI smoke tests run the built tool.

## Command line

```
usage: smk_build <input> [-o output.smk] [options]
```

`<input>` is either a directory of still frames or a `.avi` file. The output
defaults to the input's base name with `.smk` appended, in the current working
directory.

Examples:

```sh
# Frames plus a separate WAV track.
smk_build frames/ --audio soundtrack.wav -o intro.smk --verify

# AVI with its own video and audio tracks.
smk_build capture.avi -o capture.smk --palette adaptive --dither ordered

# A seamless looping background with a keyframe every 60 frames.
smk_build loop/ --ring --keyframe-interval 60 -o loop.smk
```

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `-o`, `--output FILE` | `<input>.smk` | Output path. |
| `--audio FILE.wav` | none | Audio for an image-directory input only. |
| `--fps N[/D]` | `15` | Frame rate for image directories. AVI supplies its own rate. |
| `--palette MODE` | `adaptive` | `adaptive`, `global`, or `frame`. |
| `--palette-threshold N` | `6` | Adaptive switch threshold, in mean absolute error per component on the 0..255 scale. |
| `--dither MODE` | `none` | `none`, `ordered` (8x8 Bayer), or `fs` (Floyd–Steinberg). |
| `--colors N` | `256` | Palette size, 2..256. |
| `--keyframe-interval N` | `150` | Frames between forced keyframes; `0` means only frame 0. |
| `--ring` | off | Append a ring frame that restores frame 0 for seamless looping. |
| `--background RRGGBB` | `000000` | Color used to composite alpha in TGA inputs. |
| `--pad` | on | Pad frame dimensions up to a multiple of 4 by edge replication. |
| `--strict` | off | Reject dimensions that are not multiples of 4 instead of padding. |
| `--verify` | off | Decode the finished file with libsmacker and compare it. |
| `--verbose` | off | Print progress to stderr. |
| `--quiet` | on | Suppress progress (default). |
| `-h`, `--help` | | Print help. |
| `--version` | | Print the tool version. |

Padding prints a warning on stderr. With `--strict`, the same condition is an
input error instead.

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success. |
| `1` | Usage error: bad option, bad value, missing input, or conflicting arguments. |
| `2` | Input error: unreadable directory/image/WAV/AVI, unsupported input format, or invalid dimensions. |
| `3` | Encode error: cannot create the output, out of memory, or a failed write. |
| `4` | Verification error: the encoded file does not decode to what the encoder intended. |

The encoder removes a partially written output when encoding fails. A file that
fails `--verify` is left in place for inspection.

## Inputs

### Image directories

- Files ending in `.tga`, `.bmp`, or `.pcx` (case-insensitive) are collected.
- Names are ordered naturally, so `frame2` comes before `frame10`.
- All frames must have the same dimensions; the first frame defines them.
- Frames are loaded on demand, one at a time, so large directories do not have
  to fit in memory.
- Alpha is composited over `--background`.
- Dimensions are padded to a multiple of 4 by replicating the last row/column.
  The encoder itself requires multiples of 4 because Smacker paints whole 4x4
  blocks.

### WAV audio

- PCM WAV only. 8-bit unsigned, 16-bit signed little-endian, mono or stereo.
- 24-bit input is reduced to 16-bit with rounding and prints
  `warning: 24-bit WAV reduced to 16-bit`. The output audio track is 16-bit.
- The track is split across video frames by sample position, padded with
  silence if it ends early, and truncated at the end of the video.

### AVI files

The AVI reader targets files produced by the bundled `smk2avi` tool, and also
accepts the same structures written by similar tools:

- RIFF `AVI ` with `LIST hdrl` / `LIST movi` (and optional `LIST rec `).
- One uncompressed DIB video stream: 24-bit or 32-bit `BI_RGB` (`DIB ` handler,
  compression 0). Top-down and bottom-up frames are handled.
- Up to seven PCM audio streams (`auds`, format tag 1), mono or stereo, 8 or 16
  bit. Compressed audio and other video codecs are rejected with an
  `unsupported` error.
- Interleaved and non-interleaved chunks are both accepted.
- `smk2avi` declares the `hdrl` list four bytes too long; the reader detects
  this and reads the movie list from its real position instead of failing.

`--audio` cannot be combined with an AVI input because the AVI carries its own
tracks. `--fps` is ignored for AVI; the stream's `dwRate`/`dwScale` (or the
`avih` microsecond field) is used instead.

## Output format

The writer emits a standard SMK2 file:

- 104-byte header (`SMK2`, dimensions, logical frame count, frame rate, flags,
  audio sizes/descriptors, tree sizes).
- Frame size and type tables, including the optional ring frame.
- Four Huffman trees in file order: mono map, mono colors, full colors, block
  type.
- Frame payloads ordered as palette chunk, audio chunks, video chunk, then
  zero-padded to a 4-byte boundary.

Three deterministic passes make the encode reproducible:

1. Build a 64-level RGB histogram of every frame and choose the shared palette.
2. Quantize each frame and count the Huffman symbols the writer will emit.
3. Repeat the same decisions, serializing trees and frame payloads.

Because tests and `--verify` rerun the same decisions, the count and write
passes cannot silently diverge.

### Palette modes

| Mode | Behavior |
| --- | --- |
| `global` | One palette from all frames, written on frame 0. |
| `frame` | A new palette for every frame, written every frame; every frame is a keyframe. |
| `adaptive` | Start with the shared palette. If a frame's mean quantization error exceeds `--palette-threshold`, emit a palette just for that frame. When the frame is done, restore the shared palette on the next frame. Palette changes force keyframes. |

The palette builder uses weighted median cut over the 64³ histogram followed by
k-means refinement. Palette entries are stored as 6-bit components and expanded
through the format's fixed `palmap` table.

### Dithering

- `none` maps each pixel to the nearest palette entry.
- `ordered` uses an 8x8 Bayer matrix.
- `fs` uses Floyd–Steinberg with per-channel integer error rows.

All modes remain inside the format's palette; the choice affects quality and
compressed size, not validity.

### Keyframes and ring frames

- Frame 0 is always a keyframe.
- `--keyframe-interval N` forces a keyframe every N frames. `0` leaves only
  frame 0.
- Keyframes never use void blocks.
- `--ring` appends one physical frame that rebuilds frame 0, so a looping player
  can continue smoothly at frame 1. If the ring frame needs a palette change,
  it is written as a keyframe and carries the palette.

### Audio

Audio is raw PCM, not DPCM. Each logical frame carries one chunk per track. The
chunk size is `4 + PCM bytes`; the 4-byte little-endian field is the chunk
length. The header's `AudioSize` is the largest per-frame PCM chunk. Sample
counts use an accumulator over `rate * fps_den / fps_num`, so fractional
samples per frame do not drift over long files.

## Verification

`--verify` (or `smk_verify_file()` from C) reopens the finished file with
libsmacker and checks:

- Header metadata: frame count, dimensions, frame rate, ring flag, and every
  audio track's rate/channels/bit depth.
- For every frame: the emitted palette entries and the decoded palette indices
  against a fresh run of the same palette and quantization decisions.
- For every audio track and frame: the decoded PCM bytes against the source,
  including the encoder's silence padding.

Verification compares the decode against the encoder's own decisions, not the
original RGB. A lossy palette or dithering therefore verifies successfully as
long as the decoder reproduces exactly what the encoder wrote. Corrupting a
palette, video, or audio byte fails with exit code 4.

## C API

Include `smk_encoder.h` for encoding and `verify.h` for verification.

```c
#include "smk_encoder.h"
#include "verify.h"

smk_enc_source source;
smk_enc_options options;
char err[512];

memset(&source, 0, sizeof(source));
source.user = my_state;
source.width = width;              /* multiple of 4 */
source.height = height;            /* multiple of 4 */
source.frame_count = frame_count;
source.fps_num = 15;
source.fps_den = 1;
source.read_frame = my_read_frame; /* fills width * height * 3 RGB bytes */
source.read_audio = my_read_audio; /* optional; PCM bytes for a sample range */
source.audio_track_count = 1;
source.audio[0].rate = 44100;
source.audio[0].channels = 2;
source.audio[0].bits = 16;

smk_enc_options_default(&options);
options.palette = SMK_ENC_PALETTE_ADAPTIVE;
options.verify = 1;

status = smk_enc_encode(&source, &options, "out.smk", err, sizeof(err));
if (status == SMK_ENC_OK && options.verify)
    status = smk_verify_file("out.smk", &source, &options, err, sizeof(err));
```

The source callbacks keep inputs on disk: frames are requested one at a time
during each pass, and audio is requested as interleaved PCM for the sample range
the current frame needs. `read_audio` may return fewer bytes than requested at
the end of a track; the encoder pads with silence (`0x80` for 8-bit samples,
`0` for 16-bit).

All entry points return `smk_enc_status`, whose values double as the CLI exit
codes. `smk_enc_encode()` and `smk_verify_file()` write a human-readable message
to the supplied error buffer on failure.

## Limitations and roadmap

Current scope is deliberately narrow:

- SMK2 only; no SMK4 container.
- Raw PCM audio only; no Huffman DPCM encoding.
- Only 4x4 blocks; no v4 half/double blocks.
- No bitrate targeting or two-pass size control.

Phase C plans to add Huffman DPCM audio, SMK4 and v4 half/double blocks, and
bitrate targeting. The SMK2 decoder compatibility, callback API, and
verification model are expected to stay unchanged.

## Tests

`tests/` covers the bit writer, Huffman trees, every image/WAV/AVI parser,
quantization and dithering, video blocks, container writing, audio splitting,
delta frames, keyframes, ring frames, palette modes, verification, and an
end-to-end AVI → SMK → verify round trip.

```sh
ctest --test-dir build --output-on-failure
```
