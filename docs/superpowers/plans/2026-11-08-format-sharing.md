# Shared Smacker format code Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the SMK2 wire format one owner per piece, with a reader and a writer in the same file, compiled into libsmacker once.

**Architecture:** Add `src/format/` and move each shared rule there in order: bits, Huffman, palette, video blocks, header. `smacker.c` keeps the public decode lifecycle and calls the readers. The encoder keeps quantization, file readers, and block choice, and calls the writers. `smk_build` and the tests link the library instead of compiling private copies of those pieces.

**Tech Stack:** C99, libc, CMake 3.16+.

**Spec:** `docs/superpowers/specs/2026-11-08-format-sharing-design.md`

## Global Constraints

- C99, libc only. CMake 3.16 or newer. `-Wall -Wextra` (MSVC `/W3`). POSIX and Windows.
- Bitstreams are least-significant-bit first.
- Format sources compile into both `smacker_static` and `smacker_shared`. Non-static format functions are exported. Their headers are not installed and are not added to `src/smacker.h`. No SONAME or project-version bump.
- `smk_bw_*` and `smk_huff_*` keep their current names and signatures. New reader entry points use an `smk_` prefix that does not collide with `smacker.h`.
- Huffman build and lookup keep the decoder's return convention: build returns `1` on success and `0` on failure; lookup returns the symbol or `-1`. Do not flip these to the encoder's `0` / `-1`.
- Decode stderr strings move with the code and stay byte-for-byte. Write paths stay `0` / `-1` with no new error type.
- Format headers use `stdint.h`. Do not restyle the rest of `src/smacker.c`.
- Each new format `.c` starts with the copyright and LGPL-2.1 comment at the top of `src/smacker.c`.
- The builder and the walker stay two Huffman structs. Format code does not know about `struct smk_t`, `FILE` lifetime, or encoder source callbacks.
- Do not move quantization, image or audio loaders, block classification, DPCM decode, or SMK4 writing.

## Review Focus

- An SMK4 file with a double block (type 4) or a half block (type 5) still paints the documented 4×4 pixels. Task 4.
- A 16-bit tree's three escape values are not real symbols, and looking up a repeated symbol returns that symbol both times. Task 2.
- A palette skip keeps the previous entries, and a `0x40` copy places the previous source entries at the write index. Task 3.
- An audio-rate word with the DPCM bit or the Bink bits set unpacks those flags, even though the writer never sets them. Task 5.
- A frame-size word with only bit 1 set is not a keyframe, and the returned size has both low bits cleared. Task 5.

## File structure

- Create `src/format/bitio.h`, `src/format/bitio.c`. LSB reader and growing writer.
- Create `src/format/huffman.h`, `src/format/huffman.c`. Walker plus frequency builder and serializer.
- Create `src/format/palette.h`, `src/format/palette.c`. `palmap`, chunk reader, chunk writer.
- Create `src/format/video.h`, `src/format/video.c`. Run table, type symbol, mono and full order, video-chunk decode.
- Create `src/format/header.h`, `src/format/header.c`. Fixed header, audio-rate word, frame-size word, frame-type bits, little-endian `uint32`.
- Modify `src/smacker.c` to call those readers. Delete its private bit, Huffman, palette, and video-render copies.
- Modify `src/encoder/encoder.c`, `video.c`, `video.h`, `container.c`, `quantize.c` to call the writers.
- Delete `src/encoder/bitio.c`, `src/encoder/bitio.h`, `src/encoder/huffman.c`, `src/encoder/huffman.h`.
- Modify `CMakeLists.txt` so both library targets compile `src/format/*.c`, and remove the deleted files from `SMACKER_ENCODER_SOURCES`.
- Modify `tests/test_bitio.c`, `tests/test_huffman.c`, `tests/test_video.c`, `tests/test_main.c`. Add `tests/test_palette.c` and `tests/test_header.c`.

Build used by every verification step:

```bash
cmake -S . -B build-format -DSMACKER_BUILD_TESTS=ON
cmake --build build-format
./build-format/smk_tests
```

Expected success: exit 0, and the last line is `<N> tests, 0 failed`.

---

### Task 1: Bit reader and writer

**Files:**
- Create: `src/format/bitio.h`, `src/format/bitio.c`
- Modify: `CMakeLists.txt`, `src/smacker.c`, `src/encoder/huffman.h`, `src/encoder/video.h`, `tests/test_bitio.c`, `tests/test_huffman.c`, `tests/test_main.c`
- Delete: `src/encoder/bitio.c`, `src/encoder/bitio.h`
- Test: `tests/test_bitio.c`

**Interfaces:**
- Consumes: nothing
- Produces:

```c
typedef struct smk_bitreader smk_bitreader;
struct smk_bitreader {
    const uint8_t *buffer;
    const uint8_t *end;
    unsigned bit_num;
    size_t bits_read;
};

void smk_bs_init(smk_bitreader *bs, const uint8_t *data, size_t size);
int smk_bs_read_1(smk_bitreader *bs);   /* bit, or -1 */
int smk_bs_read_8(smk_bitreader *bs);   /* byte, or -1 */
size_t smk_bs_tell(const smk_bitreader *bs);

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t bit_count;
} smk_bitwriter;

void smk_bw_init(smk_bitwriter *bw);
void smk_bw_free(smk_bitwriter *bw);
int smk_bw_put_bit(smk_bitwriter *bw, unsigned bit);
int smk_bw_put_bits(smk_bitwriter *bw, uint32_t value, unsigned count);
size_t smk_bw_byte_count(const smk_bitwriter *bw);
const uint8_t *smk_bw_data(const smk_bitwriter *bw);
```

`smk_bs_read_1` and `smk_bs_read_8` keep the stderr strings now in `src/smacker.c`. `smk_bs_tell` returns `bits_read`. Both readers add the number of bits they consume to `bits_read`.

- [ ] **Step 1: Write the failing test**

In `tests/test_bitio.c`, include `format/bitio.h` instead of `bitio.h`. Add `test_bitreader_roundtrip`:

```c
static uint32_t read_with_smk_bs(smk_bitreader *bs, unsigned count)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        int bit = smk_bs_read_1(bs);
        if (bit < 0)
            return 0xffffffffu;
        value |= (uint32_t)bit << i;
    }
    return value;
}

void test_bitreader_roundtrip(void)
{
    static const uint8_t packed[3] = { 0x5c, 0x96, 0x03 };
    smk_bitreader bs;
    smk_bitwriter bw;

    smk_bs_init(&bs, packed, 3);
    SMK_ASSERT_EQ_INT(read_with_smk_bs(&bs, 5), 0x1c);
    SMK_ASSERT_EQ_INT(read_with_smk_bs(&bs, 6), 0x32);
    SMK_ASSERT_EQ_INT(read_with_smk_bs(&bs, 7), 0x72);
    SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), 18);
    SMK_ASSERT_EQ_INT(smk_bs_read_1(&bs), -1);

    smk_bw_init(&bw);
    SMK_ASSERT_EQ_INT(smk_bw_put_bits(&bw, 0xa5, 8), 0);
    smk_bs_init(&bs, smk_bw_data(&bw), 1);
    SMK_ASSERT_EQ_INT(smk_bs_read_8(&bs), 0xa5);
    SMK_ASSERT_EQ_INT(smk_bs_tell(&bs), 8);
    SMK_ASSERT_EQ_INT(smk_bs_read_8(&bs), -1);
    smk_bw_free(&bw);
}
```

Register `test_bitreader_roundtrip` in `tests/test_main.c` as `"bitreader_roundtrip"`.

- [ ] **Step 2: Run the test to verify it fails**

Run the build commands above.
Expected: compile fails because `format/bitio.h` does not exist.

- [ ] **Step 3: Implement bitio and point both sides at it**

Move `src/encoder/bitio.c` and `bitio.h` to `src/format/`, and add the reader by moving the `smk_bit_t` functions out of `src/smacker.c`. Use the struct and `smk_bs_tell` from the Interfaces block. In `smacker.c`, replace `struct smk_bit_t` with `smk_bitreader` and delete the old function bodies.

Add `src/format/bitio.c` to both library targets in `CMakeLists.txt`. Remove `src/encoder/bitio.c` from `SMACKER_ENCODER_SOURCES`. Change remaining `"bitio.h"` includes to `"format/bitio.h"`.

- [ ] **Step 4: Run the tests to verify they pass**

Run the build commands above.
Expected: exit 0 and `<N> tests, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/format/bitio.c src/format/bitio.h src/smacker.c \
  src/encoder/huffman.h src/encoder/video.h tests/test_bitio.c \
  tests/test_huffman.c tests/test_main.c
git add -u src/encoder/bitio.c src/encoder/bitio.h
git commit -m "refactor: share the Smacker bit reader and writer"
```

---

### Task 2: Huffman walker and builder

**Files:**
- Create: `src/format/huffman.h`, `src/format/huffman.c`
- Modify: `CMakeLists.txt`, `src/smacker.c`, `src/encoder/encoder.c`, `src/encoder/video.h`, `src/encoder/video.c`, `tests/test_huffman.c`, `tests/test_main.c`
- Delete: `src/encoder/huffman.c`, `src/encoder/huffman.h`
- Test: `tests/test_huffman.c`

**Interfaces:**
- Consumes: Task 1 `smk_bitreader` and `smk_bitwriter`
- Produces: the existing `smk_huff` functions, unchanged, plus:

```c
typedef struct {
    size_t size;
    uint16_t tree[511];
} smk_huff8;

typedef struct {
    uint32_t *tree;
    size_t size;
    uint16_t cache[3];
} smk_huff16;

int smk_huff8_build(smk_huff8 *t, smk_bitreader *bs);          /* 1 or 0 */
int smk_huff8_lookup(const smk_huff8 *t, smk_bitreader *bs);  /* symbol or -1 */
int smk_huff16_build(smk_huff16 *t, smk_bitreader *bs,
                     uint32_t alloc_size);                    /* 1 or 0 */
int smk_huff16_lookup(smk_huff16 *t, smk_bitreader *bs);      /* symbol or -1 */
void smk_huff16_free(smk_huff16 *t);
```

Node tags stay `0x8000` / low 15 bits and `0x80000000` / `0x40000000` / low 30 bits. `smk_huff16_free` frees `t->tree` and sets it to NULL. `smk_close` calls it instead of `free`.

- [ ] **Step 1: Write the failing test**

Rewrite `tests/test_huffman.c` so it includes `format/huffman.h` and no longer has `tbits`, `tnode`, `parse8`, `parse16`, or `decode16`.

`test_huffman_absent` keeps its byte assertion (`{ 0 }`, one byte, zero packed nodes). Then `smk_bs_init` on that byte, `smk_huff16_build(&walker, &bs, 0)` returns `1`, and `smk_huff16_lookup` returns `0`. `smk_bs_tell` equals `tree.bit_count`. Free the walker.

`test_huffman_single_symbol` still counts `0x1234`, expects one packed node, and expects `smk_huff_encode` to write `0` bits. Build the walker with `alloc_size = 12 + 4 * smk_huff_packed_nodes(&h)`. Lookup on an empty data reader returns `0x1234`. The three `cache` entries are none of them `0x1234`. Tell on the tree reader equals `tree.bit_count`.

`test_huffman_roundtrip` keeps the symbol and frequency tables. Build the walker with the same `alloc_size` formula. Each `cache[i]` differs from every symbol. Encode the symbols, then `smk_huff16_lookup` returns each symbol in order. Tell on both readers equals that writer's `bit_count`.

Add `test_huffman_cache_lookup`: count symbols `1`, `2`, and `3` once each, build, serialize, and build a walker. Encode the sequence `1, 2, 1`. The three lookups return `1`, `2`, `1`. Register it as `"huffman_cache_lookup"`.

- [ ] **Step 2: Run the test to verify it fails**

Run the build commands above.
Expected: compile fails because `format/huffman.h` does not exist.

- [ ] **Step 3: Implement huffman.c**

Move `src/encoder/huffman.c` and `huffman.h` to `src/format/`. Move `smk_huff8_build`, `smk_huff8_lookup`, `smk_huff16_build`, and `smk_huff16_lookup`, including their stderr strings, out of `src/smacker.c`. Audio DPCM and video decode call these instead of the static copies. Delete the static copies. Rename `struct smk_huff8_t` to `smk_huff8` and `struct smk_huff16_t` to `smk_huff16` at the remaining call sites in `src/smacker.c`.

Add `src/format/huffman.c` to both library targets. Remove `src/encoder/huffman.c` from `SMACKER_ENCODER_SOURCES`. Point includes at `format/huffman.h`.

- [ ] **Step 4: Run the tests to verify they pass**

Run the build commands above.
Expected: exit 0 and `<N> tests, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/format/huffman.c src/format/huffman.h src/smacker.c \
  src/encoder/encoder.c src/encoder/video.h src/encoder/video.c \
  tests/test_huffman.c tests/test_main.c
git add -u src/encoder/huffman.c src/encoder/huffman.h
git commit -m "refactor: share Smacker Huffman build and lookup"
```

---

### Task 3: Palette map, reader, and writer

**Files:**
- Create: `src/format/palette.h`, `src/format/palette.c`
- Modify: `CMakeLists.txt`, `src/smacker.c`, `src/encoder/encoder.c`, `src/encoder/quantize.c`, `tests/test_main.c`
- Test: `tests/test_palette.c`

**Interfaces:**
- Consumes: nothing from Tasks 1–2
- Produces:

```c
extern const uint8_t smk_palmap[64];

/* 0 on success, -1 on a short or illegal chunk. Fills all 256 entries. */
int smk_palette_apply(uint8_t palette[768], const uint8_t *chunk, size_t size);

/* Writes the encoder's set-and-skip chunk, including the leading length byte.
   entries is count RGB triples in 6-bit components. out holds at least 1020 bytes.
   Returns the chunk size. */
size_t smk_palette_write_chunk(const uint8_t *entries, unsigned count,
                               uint8_t *out);
```

`smk_palette_apply` copies the previous palette into a 768-byte stack buffer. There is no `static` previous palette. Stderr strings stay the strings now in `smk_render_palette`. The payload excludes the leading length byte, matching today's call `smk_render_palette(..., p + 1, size - 1)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_palette.c` and register these names in `tests/test_main.c`.

`test_palette_palmap` asserts `smk_palmap[0] == 0x00`, `smk_palmap[1] == 0x04`, `smk_palmap[16] == 0x41`, and `smk_palmap[63] == 0xff`.

`test_palette_write_one_color` calls `smk_palette_write_chunk` with one triple `{0, 0, 0}`. The chunk is `02 00 00 00 FF FE 00 00` and the size is 8.

`test_palette_skip_preserves` fills `palette[i] = 0x11`. Applies payload `01 02 03 FF FE` (set index 0 from 6-bit 1,2,3, then skip 128 and skip 127). Returns 0. `palette[0] == smk_palmap[1]`, `palette[1] == smk_palmap[2]`, `palette[2] == smk_palmap[3]`, and `palette[3] == 0x11`.

`test_palette_copy_from_previous` fills `palette[i * 3 + c] = (uint8_t)(i + c)`. Applies payload `43 0A FF FB`: copy 4 entries from source 10, then skip 128 and skip 124. Returns 0. Entry 0 equals the old entry 10 (`10, 11, 12`). Entry 4 is unchanged (`4, 5, 6`).

`test_palette_rejects_deep_color` applies payload `00 40 00` (a set-color whose green component is `0x40`) and asserts the return is `-1`.

- [ ] **Step 2: Run the test to verify it fails**

Run the build commands above.
Expected: compile fails because `format/palette.h` does not exist.

- [ ] **Step 3: Implement palette.c**

Move the `palmap` table and `smk_render_palette` into `src/format/palette.c` under the names above. `smacker.c` calls `smk_palette_apply((uint8_t *)s->video.palette, p + 1, size - 1)`.

Move `build_palette_chunk` out of `src/encoder/encoder.c`. It calls `smk_palette_write_chunk` with `&palette->entry[0].r`. `quantize.c` uses `smk_palmap` and deletes its private table.

Add `src/format/palette.c` to both library targets. Add `tests/test_palette.c` to `smk_tests`.

- [ ] **Step 4: Run the tests to verify they pass**

Run the build commands above.
Expected: exit 0 and `<N> tests, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/format/palette.c src/format/palette.h src/smacker.c \
  src/encoder/encoder.c src/encoder/quantize.c tests/test_palette.c tests/test_main.c
git commit -m "refactor: share the Smacker palette map and chunk codec"
```

---

### Task 4: Video block codec

**Files:**
- Create: `src/format/video.h`, `src/format/video.c`
- Modify: `CMakeLists.txt`, `src/smacker.c`, `src/encoder/video.h`, `src/encoder/video.c`, `tests/test_video.c`, `tests/test_main.c`
- Test: `tests/test_video.c`

**Interfaces:**
- Consumes: Task 1 bit reader, Task 2 `smk_huff16` and `smk_huff`
- Produces:

```c
#define SMK_VIDEO_TREE_MMAP 0
#define SMK_VIDEO_TREE_MCLR 1
#define SMK_VIDEO_TREE_FULL 2
#define SMK_VIDEO_TREE_TYPE 3
#define SMK_VIDEO_TREE_COUNT 4

#define SMK_BLOCK_MONO 0
#define SMK_BLOCK_FULL 1
#define SMK_BLOCK_VOID 2
#define SMK_BLOCK_SOLID 3

extern const uint16_t smk_video_run_length[64];

unsigned smk_video_run_index(unsigned remaining);
uint16_t smk_video_pack_type(unsigned type, unsigned run_index, unsigned extra);

typedef struct {
    int (*emit)(void *user, unsigned tree, uint16_t symbol);
    void *user;
} smk_symbol_sink;

int smk_video_emit_mono(const smk_symbol_sink *sink, uint16_t colors,
                        uint16_t map);
int smk_video_emit_full(const smk_symbol_sink *sink, const uint8_t *pixels,
                        unsigned stride);

/* 0 on success, -1 on a lookup failure. Zeros all four caches first. */
int smk_video_decode(smk_bitreader *bs, smk_huff16 trees[4], uint8_t *frame,
                     unsigned width, unsigned height, char version);
```

`smk_video_run_length` is the table now duplicated in `src/encoder/video.c` and `smk_render_video`: `1..59`, then `128, 256, 512, 1024, 2048`. `smk_video_run_index` returns the greatest index whose length is less than or equal to `remaining`. `smk_video_pack_type` returns `(uint16_t)(type | (run_index << 2) | (extra << 8))`.

`smk_video_emit_mono` emits MClr `colors`, then MMap `map`. `smk_video_emit_full` emits, for each of four rows, the right-hand pair `(pixels[2] | pixels[3] << 8)` and then the left-hand pair `(pixels[0] | pixels[1] << 8)`. `pixels` points at the block origin and advances by `stride` each row.

`smk_video_decode` is the body of `smk_render_video`, including SMK4 types 4 and 5. Stderr strings stay. Void blocks write nothing. `version == '4'` enables the extra bits after a full-block symbol.

Block classification, run grouping, the delta reference, and `smk_video_encode_frame` stay in `src/encoder/video.c`. The encode function calls the helpers above.

- [ ] **Step 1: Write the failing test**

In `tests/test_video.c`, include `format/video.h`. Delete the private `decode_frame`, `read_symbol`, and `run_table`. `test_video_write_roundtrip` and `test_video_delta_roundtrip` instead serialize each built `smk_huff` (alloc size `12 + 4 * packed nodes`), `smk_huff16_build` four walkers, and call `smk_video_decode` on the frame bytes with version `'2'`. Pixel assertions stay. Free the walkers.

Add `test_video_smk4_blocks` for a 4×4 frame. Build a TYPE tree containing symbol `1` (full, run length 1, extra 0) and a FULL tree containing `0x0301` and `0x0402`. The other two trees are absent (`alloc_size` 0). Write into one `smk_bitwriter`: the type symbol, then the raw bits, then the full symbols.

- Version `'4'`, raw bit `1`, full symbols `0x0301` then `0x0402`. Decoded rows are `01 01 03 03`, `01 01 03 03`, `02 02 04 04`, `02 02 04 04`.
- Version `'4'`, raw bits `0` then `1`, full symbols `0x0301`, `0x0402`, `0x0301`, `0x0402`. Every row is `02 04 01 03`.

Register `"video_smk4_blocks"`.

- [ ] **Step 2: Run the test to verify it fails**

Run the build commands above.
Expected: compile fails because `format/video.h` does not exist.

- [ ] **Step 3: Implement format/video.c**

Create the format files with the Interfaces above. Delete the run table and the render loop from `src/smacker.c`; `smk_render_video` only initializes a `smk_bitreader` and calls `smk_video_decode`. `src/encoder/video.c` deletes its copy of the run table and packs type symbols and mono/full pixels through the helpers.

`src/encoder/video.h` includes `format/video.h` and `format/huffman.h` and drops its own tree-index macros. Add `src/format/video.c` to both library targets.

- [ ] **Step 4: Run the tests to verify they pass**

Run the build commands above.
Expected: exit 0 and `<N> tests, 0 failed`. `video_write_roundtrip`, `video_delta_roundtrip`, and `video_smk4_blocks` are in the ok lines.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/format/video.c src/format/video.h src/smacker.c \
  src/encoder/video.c src/encoder/video.h tests/test_video.c tests/test_main.c
git commit -m "refactor: share the Smacker video block codec"
```

---

### Task 5: SMK2 header fields

**Files:**
- Create: `src/format/header.h`, `src/format/header.c`
- Modify: `CMakeLists.txt`, `src/smacker.c`, `src/encoder/container.c`, `src/encoder/encoder.c`, `tests/test_main.c`
- Test: `tests/test_header.c`

**Interfaces:**
- Consumes: nothing from the earlier format modules
- Produces:

```c
#define SMK_HEADER_SIZE 104
#define SMK_HEADER_FLAG_RING 0x01u
#define SMK_HEADER_FLAG_Y_DOUBLE 0x02u
#define SMK_HEADER_FLAG_Y_INTERLACE 0x04u
#define SMK_FRAME_HAS_PALETTE 0x01u
#define SMK_FRAME_AUDIO_TRACK(n) ((uint8_t)(0x02u << (n)))

typedef struct {
    char signature[4];
    uint32_t width;
    uint32_t height;
    uint32_t frames;
    int32_t frame_rate;
    uint32_t flags;
    uint32_t audio_size[7];
    uint32_t trees_size;
    uint32_t tree_size[4];
    uint32_t audio_rate[7];
} smk_file_header;

typedef struct {
    uint32_t rate;
    int exists;
    int dpcm;
    int bink;
    int bits16;
    int stereo;
} smk_audio_rate_info;

int smk_header_pack(const smk_file_header *header, uint8_t out[SMK_HEADER_SIZE]); /* 0 or -1 */
int smk_header_unpack(const uint8_t in[SMK_HEADER_SIZE], smk_file_header *header); /* 0 or -1 */
int32_t smk_header_frame_rate(unsigned fps_num, unsigned fps_den);
int smk_header_pack_audio_rate(uint32_t rate, int bits16, int stereo,
                               uint32_t *out);
void smk_header_unpack_audio_rate(uint32_t word, smk_audio_rate_info *out);
uint32_t smk_header_pack_frame_size(uint32_t padded_size, int keyframe);
void smk_header_unpack_frame_size(uint32_t word, uint32_t *size, int *keyframe);
uint32_t smk_header_read_u32(const uint8_t bytes[4]);
void smk_header_write_u32(uint8_t bytes[4], uint32_t value);
```

`smk_header_pack` and `smk_header_unpack` return `-1` when a pointer is NULL and `0` otherwise. `smk_header_frame_rate` is `frame_rate_field` from `src/encoder/container.c`, moved unchanged. `smk_header_pack_audio_rate` returns `-1` when `rate > 0x00ffffffu`. Otherwise it writes `0x40000000 | (bits16 ? 0x20000000 : 0) | (stereo ? 0x10000000 : 0) | rate`. Unpack reports `dpcm` from `0x80000000` and `bink` from `0x0C000000`. When the exists bit is clear, every other field is 0. `smk_header_pack_frame_size` is `(padded_size & 0xfffffffcu) | (keyframe ? 1u : 0u)`. Unpack sets keyframe from bit 0 only and returns `word & 0xfffffffcu`. Pack writes a zero dummy `uint32` as the last four bytes. Unpack ignores those four bytes.

`container.c` fills an `smk_file_header`, packs it, and writes those 104 bytes. Its `write_u32` stores the bytes with `smk_header_write_u32`. It packs each frame-size word and each audio-rate word with these helpers. The `FILE`, padding, and table patch-up stay there. `encoder.c` uses `SMK_FRAME_HAS_PALETTE` and `SMK_FRAME_AUDIO_TRACK`.

`smk_open_generic` reads 104 bytes through the existing `smk_read` macro, then unpacks. The "unknown version, guess 2 or 4" warning stays in `smacker.c`, using `signature[3]`. Microseconds-per-frame conversion stays in `smacker.c`, using `header.frame_rate`. Flag tests use the constants. The frame-size loop reads four raw bytes, calls `smk_header_read_u32`, then `smk_header_unpack_frame_size`. The audio-rate loop does the same and then unpacks the word. Delete `smk_read_ul` if nothing else uses it. `smk_render` uses `SMK_FRAME_HAS_PALETTE` and `SMK_FRAME_AUDIO_TRACK`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_header.c`.

`test_header_frame_rate` asserts `smk_header_frame_rate(10, 1) == 100` and `smk_header_frame_rate(15, 1) == -6667`. `smk_header_frame_rate(0, 1) == 0`.

`test_header_audio_rate` packs 22050 Hz, 16-bit, stereo and asserts the word is `0x7000561a`. Unpack of that word reports exists, bits16, stereo, rate 22050, and neither dpcm nor bink. Unpack of `0xcc002b11` reports exists, dpcm, bink, rate 11025, and neither bits16 nor stereo. Unpack of `0x00002b11` reports every field 0. Pack of `0x01000000` returns `-1`.

`test_header_frame_size` asserts `smk_header_pack_frame_size(8, 1) == 9` and `smk_header_pack_frame_size(7, 0) == 4`. Unpack of `0x00000002` reports size 0 and keyframe 0. Unpack of `0x00000003` reports size 0 and keyframe 1.

`test_header_pack_roundtrip` packs a header with signature `SMK2`, 320×200, 3 frames, frame rate `-6667`, flags `SMK_HEADER_FLAG_RING | SMK_HEADER_FLAG_Y_DOUBLE | SMK_HEADER_FLAG_Y_INTERLACE`, audio size `{12, 0, 0, 0, 0, 0, 0}`, trees size 20, tree sizes `{16, 0, 0, 0}`, and audio rate `{0x7000561a, 0, 0, 0, 0, 0, 0}`. Both pack and unpack return `0`. Unpack returns the same fields. The last four bytes are 0. The packed size is 104.

Register all four tests.

- [ ] **Step 2: Run the test to verify it fails**

Run the build commands above.
Expected: compile fails because `format/header.h` does not exist.

- [ ] **Step 3: Implement header.c and switch both callers**

Add the module and the call-site changes from the Interfaces block. Delete `SMK_HEADER_SIZE` and `frame_rate_field` from `container.c` in favor of the shared ones. Add `src/format/header.c` to both library targets and `tests/test_header.c` to `smk_tests`. Do not add any format header to the install rule.

- [ ] **Step 4: Run the tests to verify they pass**

Run the build commands above.
Expected: exit 0 and `<N> tests, 0 failed`.

Then build the static-only configuration, so a duplicated format symbol fails the link:

```bash
cmake -S . -B build-format-static -DSMACKER_BUILD_TESTS=ON \
  -DSMACKER_BUILD_SHARED=OFF -DSMACKER_BUILD_STATIC=ON
cmake --build build-format-static
./build-format-static/smk_tests
```

Expected: exit 0 and `<N> tests, 0 failed`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/format/header.c src/format/header.h src/smacker.c \
  src/encoder/container.c src/encoder/encoder.c \
  tests/test_header.c tests/test_main.c
git commit -m "refactor: share the SMK2 header field codec"
```
