# build_smk encoder design

## Intent

`build_smk` is a production-quality generator for RAD Game Tools Smacker (`.smk`)
files. It turns either a directory of TGA/PCX/BMP frames plus an optional WAV, or
a `smk2avi`-style AVI, into a playable SMK2 file. libsmacker itself is decode-only;
this adds the missing encoder as a tool built from reusable sources.

The bar is production quality: correct output, bounded memory, deterministic
results, clean diagnostics, atomic writes, and round-trip verification.

## Goals

- Encode valid SMK2 files: 8-bit palettized video, up to seven PCM audio tracks.
- Good compression: delta frames, run-length block descriptors, frequency-built
  Huffman trees.
- Predictable image quality: 6-bit palette quantization, global/frame/adaptive
  palette modes, optional dithering.
- Robust ingestion of TGA, BMP, PCX, WAV, and the AVI subset written by
  `smk2avi` (uncompressed DIB video + PCM audio).
- Verify output by decoding it with libsmacker and comparing metadata, palette,
  every video index, and every audio byte.
- Keep the encoder reusable and input-agnostic behind a callback-based API.

## Non-goals (phase C)

- Huffman DPCM audio; SMK4 and v4 half/double blocks; bitrate targeting;
  arbitrary AVI codecs; more than two audio channels; resampling; alpha video;
  multi-threaded encoding.

## Constraints

- C99, libc only; CMake 3.16+; `-Wall -Wextra`; cross-platform (POSIX + Windows).
- SMK2 only. 256 colors max. Palette components are 6-bit and expand through the
  format's `palmap` table.
- Video dimensions should be multiples of 4. Default behavior pads to the next
  multiple by edge replication and warns; `--strict` errors instead.
- Bitstreams are least-significant-bit first.
- Frame chunks are padded to a multiple of 4 because the low two bits of the frame
  size field are flags.
- Output is deterministic; no timestamps or unordered iteration.
- Exit codes: 0 success, 1 usage, 2 input, 3 encode, 4 verify.
- Warnings and errors go to stderr. stdout stays silent unless `--verbose`.

## Approaches considered

- **A. Simple valid writer.** Per-frame palette, every frame a keyframe, only
  full/solid blocks, fixed trees, raw PCM. Easiest and very likely correct, but
  files are roughly 2-3x larger and there is no delta compression.
- **B. Production encoder (chosen).** Adaptive palette, delta blocks, runs,
  frequency-built trees, periodic keyframes, ring frame, raw PCM, verification.
- **C. Maximum fidelity/compression.** B plus DPCM audio and SMK4 v4 blocks.
  Deferred to phase C: larger test surface for modest gains.

## Architecture

Three deterministic passes over a `smk_enc_source` callback interface:

1. Pass A decodes every frame, builds a global 6-bit RGB histogram, and indexes
   AVI chunks. It also validates dimensions and audio layout.
2. Pass B builds the global palette, quantizes each frame, chooses palette mode,
   classifies blocks against the previous frame, and counts Huffman symbols.
3. Pass C repeats the exact deterministic decisions and writes the container.

The writer streams frame chunks to a temporary file and renames it only on
success. `--verify` re-decodes the finished file with libsmacker and compares it
against the encoder's own record/source.

## Reusable API

`src/encoder/smk_encoder.h` defines options, audio info, the source callbacks, and
the entry point `smk_enc_encode()`. The frontend supplies frames as RGB24 and
audio as interleaved PCM on demand, so large AVI/WAV inputs stay on disk.

## Input behavior

- **Image directory:** case-insensitive `.tga`, `.pcx`, `.bmp`; natural filename
  ordering (`frame2` before `frame10`); all images the same size; first image
  defines the size. Alpha is composited over `--background` (default black).
- **TGA:** types 1/2/3 and RLE 9/10/11, 8/15/16/24/32 bpp, colormaps, both
  origins.
- **BMP:** `BI_RGB` 8/16/24/32, optional bitfields, bottom-up and top-down, row
  padding.
- **PCX:** 8 bpp 1/3/4-plane, 1 bpp single-plane, RLE, 256-color tail.
- **WAV:** PCM 8/16/24-bit. 24-bit is reduced to 16-bit with a warning. Mono or
  stereo; unknown chunks skipped; compressed formats rejected.
- **AVI:** the `smk2avi` subset. RIFF `AVI `, `avih`, `DIB ` `BI_RGB` 24/32 video,
  PCM audio, `00db` and `##wb` chunks, no `idx1` required, bottom-up BGR,
  interleaved or non-interleaved. Up to seven audio streams map to tracks 0-6.
- Audio shorter than video is padded with silence; longer audio is truncated with
  a warning. More than two channels is an error. No resampling.

## Palette and quantization

- Quantization happens on the 64 representable 6-bit levels.
- Global palette: weighted median cut over a 64^3 histogram, refined with a few
  k-means iterations over non-empty bins.
- `global`: one palette for the whole file. `frame`: one per frame. `adaptive`
  (default): start from the global palette and switch to a frame palette when
  mean absolute error exceeds the threshold (default 6/255).
- Dithering: none (default), ordered 8x8, Floyd-Steinberg.
- Palette chunks skip unchanged entries and are padded to 4 bytes.

## Video coding

- Each 4x4 block is encoded losslessly with respect to palette indices:
  void (unchanged, delta frames only), solid (16 identical), mono (two colors),
  or full (v2 layout).
- Consecutive blocks of the same type/solid color share a descriptor; run sizes
  come from the format table (1-59, then 128, 256, 512, 1024, 2048).
- Frame 0 and every `--keyframe-interval` frames are keyframes; keyframes never
  emit void blocks.
- Four canonical Huffman trees in MMap, MClr, Full, Type order. 16-bit trees use
  byte subtrees and three escape values outside the symbol set so the
  move-to-front cache never aliases a literal. Header sizes are `12 + 4*nodes`.
  Unused trees are written as an absent tag plus terminator.
- Optional ring frame is a last-frame-to-first-frame delta without palette or
  audio.

## Audio and container

- Raw PCM only. Per-frame ranges are `round(i * rate / fps)` samples, keeping
  audio sample-accurate and contiguous. Each chunk is a 4-byte length plus data.
- 8-bit samples are unsigned; 16-bit samples are signed little-endian; stereo is
  interleaved.
- `AudioSize` stores the largest chunk per track; frame type bits mark tracks.
- SMK2 header (104 bytes), frame size/type tables, tree blob, frame data. Frame
  rate is stored as negative 10-microsecond units for precision.

## Verification and tests

- `--verify` reopens the output with libsmacker and compares metadata, palette,
  decoded indices, and audio bytes.
- CTest suite: bit/tree round-trips, parser tests with generated fixtures, and an
  integration matrix (image+WAV, AVI, 1 frame, odd dimensions, palette modes,
  dither modes, ring, keyframe intervals), all with verification enabled.
