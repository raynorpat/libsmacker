# The Smacker file format

This document describes the Smacker container and the video and audio codecs
decoded by libsmacker. It combines the format description on the
[MultimediaWiki Smacker page](https://wiki.multimedia.cx/index.php?title=Smacker)
with the behavior of `src/smacker.c` and, where useful, the FFmpeg reference
implementation. Differences between the wiki prose and the decoders are called
out explicitly.

For the library API, see [usage.md](usage.md).

## Contents

- [Overview](#overview)
- [Conventions](#conventions)
- [File layout](#file-layout)
- [Header](#header)
  - [Signature and version](#signature-and-version)
  - [Dimensions and frame count](#dimensions-and-frame-count)
  - [Frame rate](#frame-rate)
  - [Flags](#flags)
  - [Audio buffer sizes](#audio-buffer-sizes)
  - [Huffman tree sizes](#huffman-tree-sizes)
  - [Audio descriptors](#audio-descriptors)
- [Frame size table](#frame-size-table)
- [Frame type table](#frame-type-table)
- [Huffman trees](#huffman-trees)
  - [Bitstream order](#bitstream-order)
  - [Packed 8-bit trees](#packed-8-bit-trees)
  - [Packed 16-bit trees](#packed-16-bit-trees)
  - [The move-to-front cache](#the-move-to-front-cache)
  - [Tree order and sizes](#tree-order-and-sizes)
- [Frame data](#frame-data)
  - [Palette chunk](#palette-chunk)
  - [Audio track chunk](#audio-track-chunk)
  - [Video chunk](#video-chunk)
- [Audio codec](#audio-codec)
  - [Raw PCM](#raw-pcm)
  - [Huffman DPCM](#huffman-dpcm)
- [Video codec](#video-codec)
  - [Block grid](#block-grid)
  - [Type descriptors](#type-descriptors)
  - [Mono block (type 0)](#mono-block-type-0)
  - [Full block (type 1)](#full-block-type-1)
  - [Void block (type 2)](#void-block-type-2)
  - [Solid block (type 3)](#solid-block-type-3)
  - [v4 double block (type 4)](#v4-double-block-type-4)
  - [v4 half block (type 5)](#v4-half-block-type-5)
  - [Block placement and runs](#block-placement-and-runs)
- [Ring frames](#ring-frames)
- [Keyframes and seeking](#keyframes-and-seeking)
- [Version differences](#version-differences)
- [Discrepancies with the wiki](#discrepancies-with-the-wiki)
- [Implementation limitations](#implementation-limitations)
- [References](#references)

## Overview

A Smacker file contains:

- one palette video stream, normally 256 colors and 8 bits per pixel;
- up to seven independent audio tracks;
- a 104-byte header;
- per-frame size and content-type tables;
- a set of Huffman trees for video;
- one compressed chunk per frame.

Video is coded as a sequence of 4x4 pixel blocks, with several block types
(mono, full, solid, unchanged, and two v4-specific full-block variants). Audio
is either raw PCM or Huffman-coded differential PCM (DPCM). There is no Bink
Audio support in libsmacker.

## Conventions

- All multi-byte integers are little-endian.
- `byte` is 8 bits, `word` is 16 bits, `dword` is 32 bits.
- Unless stated otherwise, all values are unsigned.
- Bitstreams read bits starting at the **least significant bit** of each byte,
  then continue with the next byte.
- "Physical frame" means one chunk in the file. A file may have one more
  physical frame than its logical frame count: the ring frame.
- Palette indices are 8 bits. Palette output is 256 entries of red, green, blue,
  each 8 bits.

## File layout

```
struct SmackerFile
    byte    Header[104];
    dword   FrameSizes[FrameCount + RingFrame];
    byte    FrameTypes[FrameCount + RingFrame];
    byte    HuffmanTrees[TreesSize];
    byte    FramesData[];
```

`FrameCount` is the logical frame count from the header. `RingFrame` is 1 when
the header flag bit 0 is set, otherwise 0. `FramesData` contains the physical
frame chunks back to back; the size table gives each chunk's length.

## Header

The header is exactly 104 bytes (`0x68`).

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| `0x00` | 4 | Signature | `"SMK2"` or `"SMK4"` |
| `0x04` | 4 | Width | Frame width in pixels |
| `0x08` | 4 | Height | Frame height in pixels |
| `0x0C` | 4 | Frames | Logical frame count |
| `0x10` | 4 | FrameRate | Signed rate field (see below) |
| `0x14` | 4 | Flags | Container and Y-scaling flags |
| `0x18` | 28 | AudioSize[7] | Largest unpacked byte size per audio track |
| `0x34` | 4 | TreesSize | Size of the Huffman tree blob in bytes |
| `0x38` | 4 | MMap_Size | Allocation size of the MMap tree |
| `0x3C` | 4 | MClr_Size | Allocation size of the MClr tree |
| `0x40` | 4 | Full_Size | Allocation size of the Full tree |
| `0x44` | 4 | Type_Size | Allocation size of the Type tree |
| `0x48` | 28 | AudioRate[7] | Format and rate descriptor per track |
| `0x64` | 4 | Dummy | Unused |

### Signature and version

The first four bytes are `"SMK2"` for the original format or `"SMK4"` for the
later revision. libsmacker checks the first three bytes for `"SMK"`, then reads
the fourth byte as the version. If the version is neither `'2'` nor `'4'`, it
prints a warning and guesses: characters below `'4'` are treated as version 2,
all others as version 4.

### Dimensions and frame count

`Width` and `Height` are the logical frame size in pixels. They are normally
multiples of 4 because video is coded in 4x4 blocks. `Frames` counts logical
frames only; a ring frame, if present, is not included.

An audio-only file normally has zero width and height but still has frames and
audio chunks.

### Frame rate

`FrameRate` is a signed 32-bit value interpreted as follows:

| Value | Meaning | Microseconds per frame | Frames per second |
| --- | --- | ---: | ---: |
| `> 0` | Milliseconds per frame | `value * 1000` | `1000 / value` |
| `< 0` | Units of 10 microseconds per frame | `-value * 10` | `100000 / -value` |
| `0` | Default | `100000` | `10` |

For example, `FrameRate = 71` gives `1000 / 71 ≈ 14.0845` fps.

### Flags

Only the low three bits are defined:

| Bit | Mask | Meaning |
| ---: | ---: | --- |
| 0 | `0x01` | File contains a ring frame |
| 1 | `0x02` | Y-interlaced; the image should be displayed at twice its height |
| 2 | `0x04` | Y-doubled; the image should be displayed at twice its height |

The exact distinction between "interlaced" and "doubled" is historical and
mostly a display hint; both indicate that the stored height is half the
intended display height of an interlaced source.

> **libsmacker quirk:** `src/smacker.c` maps `0x02` to `SMK_FLAG_Y_DOUBLE` and
> `0x04` to `SMK_FLAG_Y_INTERLACE`, the reverse of the table above and of
> FFmpeg's demuxer. The API still reports one of the two constants, but code
> that compares the flag bit directly should account for this swap.

### Audio buffer sizes

`AudioSize[i]` is the largest unpacked audio buffer, in bytes, for track `i`.
libsmacker allocates this much space per existing track up front. It is zero
for tracks that do not exist.

### Huffman tree sizes

`TreesSize` is the length in bytes of the Huffman tree blob that follows the
frame type table.

`MMap_Size`, `MClr_Size`, `Full_Size`, and `Type_Size` describe the four video
Huffman tables. For a present tree, the value is the size in bytes of the
unpacked table, including three 4-byte cache slots. libsmacker uses:

```
node_count = (tree_size - 12) / 4
```

A present tree's size must be at least 12 and divisible by 4; the decoder
requires the reconstructed tree to fill exactly `node_count` entries. An absent
tree may have size 0.

The tree names correspond to the four tables:

| Name | Used by |
| --- | --- |
| `MMap` | Mono-block pixel maps |
| `MClr` | Mono-block color pairs |
| `Full` | Full, v4 double, and v4 half blocks |
| `Type` | Block type/run descriptors |

### Audio descriptors

Each `AudioRate[i]` is a 32-bit field:

| Bit(s) | Mask | Meaning |
| ---: | ---: | --- |
| 31 | `0x80000000` | Compressed (0 = raw PCM, 1 = Huffman DPCM) |
| 30 | `0x40000000` | Track is present |
| 29 | `0x20000000` | 16-bit samples (0 = 8-bit) |
| 28 | `0x10000000` | Stereo (0 = mono) |
| 27 | `0x08000000` | Bink Audio flag |
| 26 | `0x04000000` | Bink Audio / DCT flag |
| 25–24 | `0x03000000` | Unused |
| 23–0 | `0x00FFFFFF` | Sample rate in Hz |

If bit 27 or bit 26 is set, the track uses a Bink Audio variant. libsmacker
marks such a track as unsupported: it is still reported by `smk_info_audio`,
but no samples are produced.

The wiki describes bits 27–26 as "if both are zero, use v2 sound
decompression", while FFmpeg names them `BINKAUD` and `USEDCT`. In practice,
any nonzero value in either bit means the track is not Huffman DPCM.

## Frame size table

Immediately after the header is an array of `FrameCount + RingFrame` 32-bit
sizes, one per physical frame. Each entry has two low flag bits:

| Bit | Mask | Meaning |
| ---: | ---: | --- |
| 0 | `0x00000001` | Frame is a keyframe |
| 1 | `0x00000002` | Used, but purpose unknown |
| 2–31 | `0xFFFFFFFC` | Chunk length in bytes |

The chunk length is obtained with `size & 0xFFFFFFFC`; both low bits must be
cleared. The length includes the whole frame chunk (palette, audio chunks, and
video data).

## Frame type table

Next is an array of `FrameCount + RingFrame` bytes, one per physical frame.
Each bit says what the frame chunk contains:

| Bit | Mask | Meaning |
| ---: | ---: | --- |
| 0 | `0x01` | Contains a palette update |
| 1 | `0x02` | Contains audio track 0 |
| 2 | `0x04` | Contains audio track 1 |
| 3 | `0x08` | Contains audio track 2 |
| 4 | `0x10` | Contains audio track 3 |
| 5 | `0x20` | Contains audio track 4 |
| 6 | `0x40` | Contains audio track 5 |
| 7 | `0x80` | Contains audio track 6 |

The bits are independent; a frame can contain a palette update, several audio
tracks, and video data at the same time.

## Huffman trees

The `HuffmanTrees` blob is a single bitstream containing four packed trees in
the order `MMap`, `MClr`, `Full`, `Type`. There is no padding or byte alignment
between trees.

### Bitstream order

All bits are read least-significant-bit first within each byte. For example,
for the bytes `5C 96 EF`, reading 5, 6, and 7 bits in sequence yields `1C`,
`32`, and `72`:

```
5C = 0101 1100   first 5 bits (LSB first): 0 0 1 1 1  -> 0x1C
96 = 1001 0110   next 6 bits:              0 1 0 0 1 1 -> 0x32
EF = 1110 1111   next 7 bits:              0 1 1 1 0 0 1 -> 0x72
```

A "read byte" reads 8 bits in this order and may start at any bit position, so
symbols can cross byte boundaries. libsmacker implements this with an unaligned
bit reader for both single bits and bytes.

### Packed 8-bit trees

An 8-bit tree encodes values from 0 to 255. Its packed form is:

```
tag
if tag == 1:
    node
    ...
terminator
```

- `tag` — one bit. 0 means the tree is absent and has the single value 0.
- `node` — one bit:
  - `1` = branch: recursively decode the left subtree, then the right subtree.
  - `0` = leaf: read 8 bits as the leaf value.
- `terminator` — one bit, expected to be 0. libsmacker expects this bit even
  when `tag` is 0.

The recursive structure is the standard uncompressed binary tree: a branch's
left child is stored immediately after the branch marker, and the right child
follows the entire left subtree.

### Packed 16-bit trees

A 16-bit tree encodes values from 0 to 65535 and is built from two 8-bit trees:

```
tag
if tag == 1:
    low-byte tree      (packed 8-bit tree)
    high-byte tree     (packed 8-bit tree)
    escape[0]          (16 bits, low byte then high byte)
    escape[1]          (16 bits, low byte then high byte)
    escape[2]          (16 bits, low byte then high byte)
    16-bit tree        (see below)
terminator             (one bit, expected to be 0)
```

The 16-bit tree body uses the same recursive shape, but a leaf is not followed
by 16 raw bits. Instead, a leaf's low byte is decoded with the low-byte tree,
and its high byte is decoded with the high-byte tree:

```
node = read 1 bit
if node == 1:
    decode left subtree
    decode right subtree
else:
    low  = decode_symbol(low-byte tree)
    high = decode_symbol(high-byte tree)
    value = low | (high << 8)
```

The three escape values are stored as raw 16-bit little-endian values (low byte
first, then high byte), not through the byte trees. They initialize the
move-to-front cache described below.

### The move-to-front cache

The three escape values are markers for a small cache of recently used 16-bit
values. During tree construction:

1. The three cached values are initialized from the raw escape fields.
2. When a leaf's 16-bit value equals `escape[0]`, `escape[1]`, or `escape[2]`,
   that leaf is stored as a reference to the corresponding cache slot instead
   of as a literal value.

During decoding:

1. At the start of every video frame, all three cache slots are reset to 0.
2. When a leaf is a cache reference, its value is the current contents of that
   slot.
3. After any value is decoded, if it differs from `cache[0]`, the cache shifts:

```
cache[2] = cache[1]
cache[1] = cache[0]
cache[0] = value
```

This is the "semi-dynamic tree" optimization described by the wiki: the most
recently used values occupy the shortest codes. Because the cache is reset per
frame, a cache reference near the start of a frame can legitimately produce 0
until the slot has been populated.

The three cache slots are why the header's tree size includes 12 extra bytes:
three 4-byte slots in the table allocation.

### Tree order and sizes

The four trees are read in this order, matching the four header size fields:

1. `MMap`
2. `MClr`
3. `Full`
4. `Type`

libsmacker stores them as `tree[0]` through `tree[3]` in that order.

## Frame data

Each physical frame chunk contains, in this order:

1. a palette update, if frame type bit 0 is set;
2. audio chunks for tracks 0 through 6, in ascending track order, for each
   frame type bit 1–7 that is set;
3. video data for the remainder of the chunk.

The palette update and audio chunks each carry their own length, so the video
data begins at the first byte after the last audio chunk. A frame with no
video data can still have a palette update and audio.

### Palette chunk

The palette chunk starts with a length byte:

```
byte Length;
byte Blocks[...];
```

`Length` is the total chunk size **including the length byte**, divided by 4.
The actual chunk length is `4 * Length` bytes. The decoder processes
`Blocks` until all 256 palette entries have been produced or the chunk is
exhausted.

Each block starts with a control byte:

| Control bits | Format | Meaning |
| --- | --- | --- |
| `1ccccccc` | 1 byte | Skip/preserve `(c + 1)` entries |
| `01cccccc` | 2 bytes | Copy `(c + 1)` entries from old palette entry `s` |
| `00bbbbbb` | 3 bytes | Set one entry from three 6-bit components |

Details:

- **Skip** (`1ccccccc`): advance the destination index by `(c + 1)` entries
  without writing. Since the palette is updated in place, those entries keep
  their previous colors.
- **Copy** (`01cccccc ssssssss`): copy `(c + 1)` entries starting at source
  entry `s` of the palette **as it was before this chunk**. libsmacker snapshots
  the palette before processing, so a copy cannot see entries written earlier in
  the same chunk. The second byte is the source start index.
- **Set** (`00bbbbbb 00gggggg 00rrrrrr`): write one entry. Each component is a
  6-bit value in the low 6 bits of its byte; the top two bits must be zero.
  libsmacker rejects a component above `0x3F`.

The three component bytes are labelled blue, green, red by the wiki, but the
implementations treat the first byte as **red**, the second as green, and the
third as blue. libsmacker's palette buffer is therefore R, G, B. FFmpeg's
demuxer likewise reads the three bytes as a big-endian 24-bit RGB value.

Six-bit components are expanded to 8 bits with this fixed table:

```c
static const unsigned char palmap[64] = {
    0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
    0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
    0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D,
    0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
    0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E,
    0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
    0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF,
    0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF
};
```

The palette persists across frames that have no palette chunk. Frame 0 usually
contains a full palette update, but the decoder starts with an all-zero palette
and does not require one.

### Audio track chunk

An audio chunk for one track is:

```
dword Length;
dword UnpackedLength;   /* only when the track is compressed */
byte  Data[];
```

- `Length` includes the 4-byte length field itself.
- `UnpackedLength` is present only when the track descriptor's bit 31 is set.
  It is the size in bytes of the decoded PCM data, including all channels.
- `Data` is either raw PCM or a Huffman DPCM bitstream.

For raw PCM, the data length is `Length - 4`. For compressed audio, the
compressed payload length is `Length - 8`.

A frame can contain a chunk for each track whose frame type bit is set. Tracks
without a chunk simply have no audio for that frame; the decoder reports a
decoded size of 0.

### Video chunk

The video data is the remainder of the frame chunk after the palette and audio
chunks. It is a single least-significant-bit-first bitstream containing block
type descriptors and block pixel data. See [Video codec](#video-codec).

## Audio codec

Each track is described by its `AudioRate` field. The track can be mono or
stereo, 8-bit or 16-bit, PCM or DPCM, at any rate up to 16,777,215 Hz (the
practical limit is much lower).

### Raw PCM

When bit 31 is clear, `Data` is uncompressed PCM and `UnpackedLength` is absent.
The byte layout follows the channel and bit-depth flags:

- 8-bit samples are **unsigned**, with `0x80` as silence.
- 16-bit samples are **signed**, little-endian in the file.
- Stereo samples are interleaved left, right.
- There is no per-chunk sample count beyond `Length - 4`; the number of samples
  is `(Length - 4) / (channels * bytes_per_sample)`.

### Huffman DPCM

When bit 31 is set, the chunk carries a 4-byte little-endian `UnpackedLength`
followed by the DPCM bitstream in `Data`. The bitstream is read
least-significant-bit first.

```
DataPresent   (1 bit)
IsStereo      (1 bit)
Is16Bits      (1 bit)
Trees[]       (1-4 packed 8-bit Huffman trees)
Bases[]       (1-4 raw bytes)
Deltas[]      (Huffman-coded 8-bit deltas)
```

1. **DataPresent** must be 1. libsmacker reports an error if it is 0; FFmpeg
   treats a 0 as an empty chunk.
2. **IsStereo** must match the track descriptor's stereo flag.
3. **Is16Bits** must match the track descriptor's bit depth.
4. **Trees** — one packed 8-bit tree per byte lane:
   - mono 8-bit: 1 tree (left low byte);
   - mono 16-bit: 2 trees (left low, left high);
   - stereo 8-bit: 2 trees (left low, right low);
   - stereo 16-bit: 4 trees (left low, left high, right low, right high).
5. **Bases** — the first sample values, read as raw bytes (not through the
   trees). The order is:
   - right channel before left channel, when stereo;
   - high byte before low byte, when 16-bit.

   So the four possible orders are:

   | Format | Base byte order |
   | --- | --- |
   | Mono 8-bit | L |
   | Stereo 8-bit | R, L |
   | Mono 16-bit | L high, L low |
   | Stereo 16-bit | R high, R low, L high, L low |

6. **Deltas** — for every subsequent sample, decode an 8-bit delta for each
   byte lane from its tree, then add it to the previous value of the same
   channel. For 16-bit samples, the low-byte delta is decoded first, then the
   high-byte delta; the combined 16-bit delta is added to the previous 16-bit
   sample. For stereo, left is processed before right in each sample pair.
   Addition wraps modulo 256 or 65536; the codec relies on wraparound rather
   than clipping.
7. Decoding stops after exactly `UnpackedLength` bytes of output have been
   produced.

The deltas are signed 8-bit values. For 8-bit output, the result is still
unsigned PCM (0x80 is silence); the signed delta is added modulo 256. For
16-bit output, the result is signed 16-bit PCM.

libsmacker writes DPCM 16-bit samples into a native `short`, so that output is
native-endian. Raw 16-bit PCM is copied from the file and remains
little-endian.

## Video codec

### Block grid

The frame is divided into 4x4 pixel blocks, decoded left to right and top to
bottom. Each block covers four rows of four pixels. A frame is expected to be
a whole number of blocks wide and high; the stored width and height are usually
multiples of 4. libsmacker's frame buffer is exactly `width * height` bytes and
does not add padding, so a partial final block is not handled safely (see
[limitations](#implementation-limitations)).

Initially the frame buffer is all zeros. A frame is built by painting blocks;
blocks that are not painted keep their previous contents. This is how delta
frames work.

### Type descriptors

Each run of blocks begins with a 16-bit symbol decoded from the `Type` tree:

```
bits 0-1   block type (0-3, and 4-5 as v4 extensions)
bits 2-7   run length index (0-63)
bits 8-15  extra data, used as the fill color for solid blocks
```

The low two bits select the block type. The six-bit run length index is looked
up in this table:

```c
static const unsigned short sizetable[64] = {
    1,   2,   3,   4,   5,   6,   7,   8,
    9,  10,  11,  12,  13,  14,  15,  16,
   17,  18,  19,  20,  21,  22,  23,  24,
   25,  26,  27,  28,  29,  30,  31,  32,
   33,  34,  35,  36,  37,  38,  39,  40,
   41,  42,  43,  44,  45,  46,  47,  48,
   49,  50,  51,  52,  53,  54,  55,  56,
   57,  58,  59, 128, 256, 512, 1024, 2048
};
```

The resulting count is the number of consecutive 4x4 blocks that use the same
type and extra data. Note the jump from 59 to 128: indices 60–63 encode the
large runs.

The descriptor's type field is 2 bits, so only types 0–3 can appear directly.
Types 4 and 5 are v4 sub-types derived from type 1 as described below.

### Mono block (type 0)

A mono block contains only two colors and a 16-bit map selecting between them.

1. Decode one 16-bit symbol from the `MClr` tree.
   - High byte = color 1.
   - Low byte = color 2.
2. Decode one 16-bit symbol from the `MMap` tree. This is the pixel map.
3. For each of the 16 pixels in row-major order (top-left to bottom-right),
   consume one bit of the map, starting with the least significant bit:
   - bit set → color 1 (high byte);
   - bit clear → color 2 (low byte).

Bit 0 of the map is the top-left pixel; bit 15 is the bottom-right pixel.

### Full block (type 1)

A full block stores every pixel through the `Full` tree.

For version 2 files, each of the four rows is decoded with two 16-bit symbols:

1. Decode symbol A:
   - low byte → column 2;
   - high byte → column 3.
2. Decode symbol B:
   - low byte → column 0;
   - high byte → column 1.

In other words, the right pair of pixels is stored first, then the left pair.
Repeat for each of the four rows.

For version 4 files, type 1 is a prefix for a run of full-like blocks. Read the
subtype immediately after the type descriptor:

```
read bit
if bit == 1:
    subtype = 4 (double)
else:
    read bit
    if bit == 1:
        subtype = 5 (half)
    else:
        subtype = 1 (v2 full)
```

The subtype applies to the entire run selected by the descriptor; it is read
once per descriptor, not once per block.

### Void block (type 2)

A void block carries no data. All 16 pixels are unchanged from the previous
frame. This is the main delta-compression mechanism.

### Solid block (type 3)

A solid block fills all 16 pixels with the descriptor's extra data byte:

```
color = (descriptor >> 8) & 0xFF
```

No pixel data follows from a tree.

### v4 double block (type 4)

This subtype uses two `Full` symbols per 4x4 block. Each symbol paints a
2-row by 4-column strip:

1. Decode symbol A:
   - low byte fills columns 0–1 of rows 0 and 1;
   - high byte fills columns 2–3 of rows 0 and 1.
2. Decode symbol B:
   - low byte fills columns 0–1 of rows 2 and 3;
   - high byte fills columns 2–3 of rows 2 and 3.

So each symbol fills two 2x2 sub-blocks side by side, and the two rows it
covers are identical.

### v4 half block (type 5)

This subtype uses four `Full` symbols per 4x4 block, two per pair of rows.
Each symbol fills one 2x2 sub-block, and the two rows are identical:

For rows 0 and 1:

1. Decode symbol A:
   - low byte fills column 2 of both rows;
   - high byte fills column 3 of both rows.
2. Decode symbol B:
   - low byte fills column 0 of both rows;
   - high byte fills column 1 of both rows.

For rows 2 and 3, repeat with two more symbols. In short, the right 2x2
sub-block is stored before the left 2x2 sub-block, and each pair of rows is
duplicated vertically.

### Block placement and runs

The decoder maintains a block cursor `(row, col)`, both starting at 0. For each
descriptor:

1. Compute `run = sizetable[(descriptor >> 2) & 0x3F]`.
2. Repeat `run` times, or until `row >= height`:
   - paint the 4x4 block at `(row, col)`;
   - advance `col` by 4;
   - if `col >= width`, set `col = 0` and advance `row` by 4.
3. Read the next descriptor.

The block's pixel offset is `row * width + col`; each subsequent pixel row is
`width` bytes later. A run can cross from one block row to the next, so the
same descriptor can paint a partial row and continue on the next row.

The outer loop ends when `row >= height`. There is no separate terminator.

## Ring frames

If header flag bit 0 is set, the file contains one extra physical frame at the
end, after the logical frames. The frame size and frame type tables include
this ring frame.

The ring frame is a delta frame that brings the last logical frame back to the
state needed to display the first frame again, allowing seamless looping. It is
not counted in `Frames`.

libsmacker's behavior:

- `smk_info_all` reports `cur_frame % frame_count`, so while the ring frame is
  current the reported frame number is 0.
- After decoding the ring frame, `smk_next` wraps to physical frame 1, not frame
  0. Frame 0 is the initial state and has already been displayed; the ring
  frame has made the image ready for the next pass.
- A ringing file never returns `SMK_DONE`; `smk_next` keeps looping.

A player that wants exactly one pass should stop after `frame_count` logical
frames rather than waiting for `SMK_DONE`.

## Keyframes and seeking

The low bit of each frame size marks a keyframe. Keyframes are typically frame
0 and periodic intra frames. Non-keyframes are delta-coded and must be decoded
after their predecessor.

There is no seek index. To seek to frame N:

1. Start at N and walk backwards in the frame size table until a keyframe is
   found (or frame 0).
2. Decode that keyframe.
3. Decode forward with `smk_next` until the desired frame is reached.

`smk_seek_keyframe` performs steps 1 and 2 and leaves the handle on the
keyframe. It does not decode the target frame.

## Version differences

The container layout, palette coding, audio coding, and most video coding are
the same in `SMK2` and `SMK4`.

The main difference is in full blocks:

- **SMK2:** type 1 is always the v2 full-block layout (two 16-bit symbols per
  row).
- **SMK4:** type 1 may be followed by subtype bits selecting the v2 full
  layout, the double layout, or the half layout.

libsmacker also accepts an unknown version byte, warning and guessing based on
whether it is below `'4'`. Files with other signatures are rejected.

## Discrepancies with the wiki

The MultimediaWiki page is the best public description of the format, but a few
details differ from what libsmacker and FFmpeg actually implement. This
document follows the implementations.

1. **Y-scale flag bits are swapped in libsmacker.** The wiki and FFmpeg use
   bit 1 (`0x02`) for Y-interlace and bit 2 (`0x04`) for Y-double. libsmacker
   maps `0x02` to `SMK_FLAG_Y_DOUBLE` and `0x04` to `SMK_FLAG_Y_INTERLACE`.
2. **Palette component order.** The wiki labels the set-color block
   `00bbbbbb, 00gggggg, 00rrrrrr` (blue, green, red). libsmacker and FFmpeg
   treat the first byte as red, then green, then blue.
3. **v4 full-block subtype bits.** The wiki prose says a first bit of 0 means
   v2 full, and a first bit of 1 followed by 0/1 means double/half. libsmacker
   and FFmpeg instead use: `1` → double, `01` → half, `00` → v2 full.
4. **Audio descriptor bits 27–26.** The wiki describes them as a v2 audio codec
   selector when both are zero. FFmpeg names them `BINKAUD` and `USEDCT`.
   libsmacker treats any nonzero value in either bit as unsupported Bink Audio.
5. **Palette component width.** The wiki introduction mentions an 8-bit RGB
   palette; the palette *chunk* actually stores 6-bit components that are
   expanded to 8 bits with `palmap`. The decoded palette is 8-bit.
6. **Mono map polarity naming.** The wiki's mono-block prose mixes
   "color1/color2" with "color0/color1". The implementation is unambiguous: a
   set map bit uses the high byte of the `MClr` symbol, and a clear bit uses
   the low byte.
7. **Terminator after an absent tree.** libsmacker expects the final 0 bit
   after every packed tree, even one whose presence tag is 0. FFmpeg consumes
   that bit only after a present tree. This only matters for files that omit a
   tree; normal files with all four trees present are unaffected.

## Implementation limitations

- **SMK2 and SMK4 only.** Other signatures are rejected.
- **Bink Audio is not decoded.** Tracks flagged as Bink produce no samples.
- **No Y scaling or deinterlacing.** The Y flags are informational; the frame
  is always returned at its stored size.
- **Dimensions are assumed to be multiples of 4.** libsmacker paints whole 4x4
  blocks and does not guard the frame buffer at the right and bottom edges. A
  width or height that is not a multiple of 4 can corrupt neighboring pixels,
  and a partial final block row can write past the end of the `width * height`
  buffer.
- **Corrupt input is only partially validated.** Huffman trees, palette blocks,
  and chunk lengths are checked in many places, but several paths trust header
  values. Treat decoding of untrusted files as unsafe.
- **No encoder.** libsmacker is decode-only.
- **Single-threaded.** The palette decoder uses a file-static scratch buffer;
  do not decode two handles concurrently in different threads.

## References

- MultimediaWiki, "Smacker":
  <https://wiki.multimedia.cx/index.php?title=Smacker>
- libsmacker source, `src/smacker.c` and `src/smacker.h`.
- FFmpeg, `libavcodec/smacker.c` (video and audio decoders) and
  `libavformat/smacker.c` (demuxer):
  <https://github.com/FFmpeg/FFmpeg>
- RAD Game Tools, Smacker:
  <http://www.radgametools.com/smkmain.htm>
