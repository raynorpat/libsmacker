# Shared Smacker format code

## Intent

The decoder in `src/smacker.c` and the encoder under `src/encoder/` each
implement the SMK2 wire format. The implementations walk in opposite
directions, but they encode the same rules: Huffman tree bytes, bit order,
the block run-length table, palette chunks, and the SMK2 header. A change to
any of those rules has to be made twice, by hand.

Give each of those rules one owner. A reader and a writer for that piece live
in the same C file. Both the library and `smk_build` call that file.

## Goals

- One implementation of each shared format rule, compiled into libsmacker
  once. `smk_build` and the encoder tests stop compiling private copies and
  call the library.
- Five format modules, each with the reader and the writer for one piece:
  bits, Huffman trees, palette chunks, video blocks, and the SMK2 header.
- The public decode API in `src/smacker.h` stays as it is: same functions,
  same signatures, same results for a given file.
- The encoder still writes the same bytes for the same inputs. Existing
  round-trip tests are the check.
- Decoder unit-test copies of the tree parser and the video walker go away.
  Those tests call the shared readers.

## Non-goals

- Quantization, dithering, and palette selection stay in the encoder.
- TGA, BMP, PCX, WAV, and AVI readers stay in the encoder.
- Block classification (void, solid, mono, full), run grouping, the delta
  reference frame, and keyframe policy stay in the encoder.
- Disk-versus-memory open, seek, enable flags, and frame-buffer ownership
  stay in `smacker.c`.
- SMK4 half and double blocks stay read-only. The writer does not emit them.
- Smacker DPCM and Bink audio stay read-only. The encoder still writes raw PCM.
- No new public functions in `smacker.h`. No installed format headers.
- No SONAME or project-version bump. Adding in-tree symbols is compatible.

## Anti-goals

- One Huffman struct that both builds frequencies and walks a decoded tree.
  The builder and the walker stay two structs in one file.
- The format modules knowing about `struct smk_t`, `FILE` lifetime, or the
  encoder's source callbacks.
- A third copy of the wire grammar left behind in the tests.

## Constraints

- C99, libc only. CMake 3.16 or newer. `-Wall -Wextra` (MSVC `/W3`).
  POSIX and Windows, matching the rest of the tree.
- Bitstreams are least-significant-bit first.
- The library targets today are only `src/smacker.c`. `smk_build` and
  `smk_tests` compile `SMACKER_ENCODER_SOURCES` themselves and link
  `SMACKER_LINK_TARGET`, which is the shared library when both static and
  shared are enabled.
- `WINDOWS_EXPORT_ALL_SYMBOLS` is on, and Unix visibility is the default.
  Non-static functions in the library are exported. That is how `smk_build`
  will call the format writers. The supported API remains `smacker.h`.
  Format headers are not installed.
- Decode error text on stderr stays. Write paths keep their current `0` / `-1`
  returns and do not grow a new error type.
- Code that moves out of `src/smacker.c` keeps the libsmacker copyright and
  LGPL-2.1 notice. The encoder sources being absorbed have no separate
  copyright header.

## Layout

New sources, compiled into both `smacker_static` and `smacker_shared`:

| File | Owns |
| --- | --- |
| `src/format/bitio.h`, `bitio.c` | LSB bit reader and the growing bit writer |
| `src/format/huffman.h`, `huffman.c` | 8-bit and 16-bit tree walker, plus the frequency builder and serializer |
| `src/format/palette.h`, `palette.c` | `palmap`, palette-chunk reader, palette-chunk writer |
| `src/format/video.h`, `video.c` | Run-length table, type-symbol packing, mono and full pixel order, video-chunk decode |
| `src/format/header.h`, `header.c` | Fixed SMK2 header fields, audio-rate flags, frame-size word, frame-type bits |

`src/encoder/bitio.c`, `src/encoder/bitio.h`, `src/encoder/huffman.c`, and
`src/encoder/huffman.h` are removed. Encoder and test includes change from
`"bitio.h"` and `"huffman.h"` to `"format/bitio.h"` and `"format/huffman.h"`.
The library's public include directory is already `src/`, and `smk_build`
links the library, so those includes resolve. Drop the encoder directory from
the include path only where a file no longer needs an encoder-private header.

Format headers are in-tree. They are not listed in the install rules.
`src/smacker.h` remains the only installed header.

## Modules

### Bits

Move the decoder's `smk_bit_t` reader (`smk_bs_init`, `smk_bs_read_1`,
`smk_bs_read_8`) and the encoder's `smk_bitwriter` (`smk_bw_init`,
`smk_bw_free`, `smk_bw_put_bit`, `smk_bw_put_bits`, `smk_bw_byte_count`,
`smk_bw_data`) into `bitio`. Keep the existing writer signatures so
`tests/test_bitio.c` and `tests/test_huffman.c` stay source-compatible apart
from the include path.

The reader keeps its current exhausted-stream behavior: print the existing
stderr line and return `-1`. DPCM audio decode in `smacker.c` calls this
reader. It does not grow a writer.

### Huffman

Same file, two structs.

- The walker is today's `smk_huff8_t` and `smk_huff16_t`: array-of-nodes
  build from a bitstream, lookup, and the three-slot 16-bit cache. Build and
  lookup become callable from `smacker.c`. Audio DPCM keeps using the 8-bit
  walker. Video decode uses the 16-bit walker. Node tags stay
  `0x8000` / low 15 bits for 8-bit trees and `0x80000000` / `0x40000000` /
  low 30 bits for 16-bit trees.
- The builder is today's `smk_huff`: `smk_huff_init`, `smk_huff_free`,
  `smk_huff_clear`, `smk_huff_count`, `smk_huff_build`, `smk_huff_encode`,
  `smk_huff_packed_nodes`, `smk_huff_serialize`. Signatures stay. Serialize
  still writes the present bit, the low 8-bit tree, the high 8-bit tree,
  three little-endian escape values chosen from unused symbols, the 16-bit
  tree, and the terminator bit. An empty tree is still the absent tag plus
  the terminator.

Do not merge these structs. The builder holds frequencies and codes. The
walker holds the decoded array the file format requires.

`tests/test_huffman.c` currently parses trees with a private bit reader.
Point those checks at the shared walker so the test and the library cannot
drift. The assertions stay: empty tree, one symbol, and a multi-symbol
round trip through serialize then lookup.

### Palette

`palmap` is the 64-byte 6-bit-to-8-bit table now duplicated in
`src/encoder/quantize.c` and `smk_render_palette`. One table lives here.
`quantize.c` uses it when filling a palette's display RGB. The chunk reader
uses it when expanding file entries into the decoded palette.

The reader applies one palette chunk to a `uint8_t[768]` palette. It
implements skip (`0x80`), copy-from-previous (`0x40`), and set-new-colors.
The decoder's `smk_render_palette` becomes a call into this reader.
`oldPalette` is a function-local `static` today. Drop that. Use a 768-byte
stack buffer (or a scratch argument of that size) so the copy of the previous
palette is reentrant. Output colors for a given chunk stay the same.

The writer is today's `build_palette_chunk` in `src/encoder/encoder.c`:
three-byte set records for the entries in use, `0x80` skips for the rest,
padded to a multiple of 4, with the leading byte equal to the padded length
divided by 4. The encoder calls it. The writer does not have to emit `0x40`
copies.

### Video

`src/format/video.c` owns the format. `src/encoder/video.c` owns encoder
policy.

Shared, in the format file:

- The 64-entry run-length table (`1..59`, then `128, 256, 512, 1024, 2048`).
- Type-symbol packing. Low 2 bits are the block type (mono 0, full 1, void 2,
  solid 3). The next 6 bits are the run-table index. The high byte is the
  solid color or the void extra. The reader also accepts SMK4 types 4 and 5,
  which are signaled by extra bits after a full-block symbol when the file
  version is `'4'`.
- Mono pixel order: MClr symbol (high byte color 1, low byte color 2), then
  MMap, applied low bit first across each 4-wide row. Full pixel order: for
  each of 4 rows, the right-hand 16-bit symbol (bytes 2 and 3) then the
  left-hand symbol (bytes 0 and 1). Tree indexes stay MMap 0, MClr 1, Full 2,
  Type 3.
- A video-chunk decoder that takes a bit reader, four 16-bit walkers, the
  destination index frame, width, height, and the SMK version character.
  `smk_render_video` becomes that call. Width and height are the decoder's;
  SMK4 half and double blocks stay inside this function.

Stays in `src/encoder/video.c`:

- Choosing void, solid, mono, or full, including the delta against the
  previous frame and the rule that keyframes emit no void blocks.
- Grouping consecutive blocks into runs.
- `smk_video_init`, `smk_video_reset`, `smk_video_free`, and
  `smk_video_encode_frame`. The encode function calls the format helpers to
  pack each type symbol and to emit mono and full pixels, then writes those
  symbols through the existing Huffman encoder.

`tests/test_video.c` has a private `decode_frame`. Replace it with the shared
video decoder so the test walks the same code as the library.

### Header

The format header module packs and unpacks bytes. It does not open files or
allocate the decoder.

It owns:

- The fixed header fields, in file order: 4-byte signature, width, height,
  frame count, frame-rate integer, flags, seven audio max-chunk sizes, Huffman
  tree blob size, four tree sizes, seven audio-rate words, and the dummy
  `uint32`. That is 104 bytes.
- Signature check for `SMK2` and `SMK4`. The decoder's "unknown version,
  guess 2 or 4" warning stays in `smacker.c`, because it is recovery policy
  around a value this module reports.
- Flag bits: `0x01` ring, `0x02` Y-double, `0x04` Y-interlace.
- Audio-rate word: `0x40000000` track present, `0x80000000` Smacker DPCM,
  `0x20000000` 16-bit, `0x10000000` stereo, `0x0C000000` Bink, low 24 bits
  sample rate. The writer sets present, bit depth, channel count, and rate.
  The reader reports every flag, including DPCM and Bink. The encoder still
  does not set DPCM or Bink.
- The frame-rate integer formula that is `frame_rate_field` in
  `src/encoder/container.c` today (positive milliseconds, otherwise a negative
  count of 10-microsecond units). The decoder's conversion of that integer
  into microseconds-per-frame stays in `smacker.c`.
- Frame-size word: bit 0 is the keyframe flag, the stored size has the low
  two bits cleared. The writer sets bit 0 from the keyframe flag and leaves
  bit 1 clear. The reader returns the keyframe bit and the masked size.
- Frame-type bits: bit 0 means a palette chunk follows, bits 1 through 7 mean
  audio tracks 0 through 6. Helpers name those bits. Assembling a frame
  payload (palette chunk, per-track audio length, video bits) stays in
  `encoder.c` and `smk_render`.

`src/encoder/container.c` keeps the `FILE`, the temporary-file rename, zero
padding of frame chunks to a multiple of 4, and patching the size and type
tables. It calls the header helpers for the field bytes. `smk_open_generic`
keeps reading from its file or memory source and filling `struct smk_t`. It
calls the header helpers instead of decoding those bit masks inline.

Little-endian `uint32` load and store used by this module live here.
`container.c` uses them for the size table as well.

## What stays where

`smacker.c` keeps `smk_open_*`, `smk_close`, the info and enable getters,
`smk_first`, `smk_next`, `smk_seek_keyframe`, audio DPCM decode, and the
frame-chunk walker in `smk_render`. Those functions call the format readers.

The encoder keeps quantization, image and audio loaders, block choice, PCM
splitting, and container streaming. Those call the format writers.

`src/encoder/verify.c` still checks a finished file through the public
`smk_*` API.

## Builder calls

These are settled. Do not reopen them with the human partner.

- Format functions are non-static and therefore exported from the shared
  library. Their headers are not installed and they are not added to
  `smacker.h`. Names keep the existing `smk_bw_*` and `smk_huff_*` prefixes.
  New reader entry points use an `smk_` prefix that does not collide with
  `smacker.h`.
- Format code uses `stdint.h` types in its headers. `smacker.c` may adapt at
  the call site. Do not restyle the rest of `smacker.c`.
- The palette reader's previous-palette copy is a 768-byte stack buffer.
- No version bump and no change to install rules beyond not installing the
  new headers.
- Verification is the existing suite: configure with `-DSMACKER_BUILD_TESTS=ON`,
  build, and run `ctest`. That covers bit and tree round trips, video encode
  and decode, palette modes, and full-file verify. There is no checked-in
  `.smk` sample to play separately.

## Left to the builder

- Exact function names for the new readers, as long as the behavior above
  holds and the existing writer entry points keep their names and signatures.
- How large a video write helper is, as long as mono and full byte order and
  the type symbol live in `src/format/video.c`, and block choice stays in
  `src/encoder/video.c`.
- Whether the fixed header is packed into one struct or several small
  helpers, as long as both sides call them and neither re-decodes the flag
  masks.
- Order of extraction. A sensible order is bits, then Huffman, then palette,
  then video, then header, with the tests green after each step. Another
  order is fine if the suite still ends green.
