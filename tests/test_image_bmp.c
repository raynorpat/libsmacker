#include "test.h"
#include "test_util.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[8192];
    size_t size;
} bmp_buf;

static void bmp_u8(bmp_buf *b, unsigned value)
{
    b->data[b->size++] = (uint8_t)value;
}

static void bmp_u16(bmp_buf *b, unsigned value)
{
    bmp_u8(b, value & 0xff);
    bmp_u8(b, (value >> 8) & 0xff);
}

static void bmp_u32(bmp_buf *b, uint32_t value)
{
    bmp_u8(b, value & 0xff);
    bmp_u8(b, (value >> 8) & 0xff);
    bmp_u8(b, (value >> 16) & 0xff);
    bmp_u8(b, (value >> 24) & 0xff);
}

static void bmp_header(bmp_buf *b, uint32_t pixel_offset,
                       int32_t width, int32_t height, unsigned bitcount,
                       uint32_t compression, uint32_t colors_used)
{
    size_t file_size_field;

    bmp_u8(b, 'B');
    bmp_u8(b, 'M');
    file_size_field = b->size;
    bmp_u32(b, 0); /* patched by caller through b->size afterwards */
    (void)file_size_field;
    bmp_u32(b, 0);
    bmp_u32(b, pixel_offset);

    bmp_u32(b, 40);
    bmp_u32(b, (uint32_t)width);
    bmp_u32(b, (uint32_t)height);
    bmp_u16(b, 1);
    bmp_u16(b, bitcount);
    bmp_u32(b, compression);
    bmp_u32(b, 0);
    bmp_u32(b, 0);
    bmp_u32(b, 0);
    bmp_u32(b, colors_used);
    bmp_u32(b, 0);
}

static void bmp_finish(bmp_buf *b)
{
    b->data[2] = (uint8_t)(b->size & 0xff);
    b->data[3] = (uint8_t)((b->size >> 8) & 0xff);
    b->data[4] = (uint8_t)((b->size >> 16) & 0xff);
    b->data[5] = (uint8_t)((b->size >> 24) & 0xff);
}

static char *write_temp_bmp(const char *name, const void *data, size_t size)
{
    char *dir = test_tempdir();
    char *path;
    size_t length;

    if (dir == NULL)
        return NULL;
    length = strlen(dir) + strlen(name) + 2;
    path = (char *)malloc(length);
    if (path == NULL) {
        free(dir);
        return NULL;
    }
    snprintf(path, length, "%s/%s", dir, name);
    if (test_write_file(path, data, size) != 0) {
        free(path);
        free(dir);
        return NULL;
    }
    free(dir);
    return path;
}

static int load_bmp(const char *path, uint8_t **rgb, unsigned *w,
                    unsigned *h, char *err, size_t errlen)
{
    static const uint8_t black[3] = { 0, 0, 0 };

    return smk_image_load(path, black, rgb, w, h, err, errlen);
}

void test_bmp_24_bottom_up(void)
{
    bmp_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    /* Bottom row: red, green. Top row: blue, white. Rows pad to 8 bytes. */
    static const uint8_t pixels[16] = {
        0, 0, 255,  0, 255, 0,  0, 0,
        255, 0, 0,  255, 255, 255, 0, 0
    };
    static const uint8_t expected[12] = {
        0, 0, 255,  255, 255, 255,
        255, 0, 0,  0, 255, 0
    };

    b.size = 0;
    bmp_header(&b, 54, 2, 2, 24, 0, 0);
    memcpy(b.data + b.size, pixels, sizeof(pixels));
    b.size += sizeof(pixels);
    bmp_finish(&b);

    path = write_temp_bmp("bottom.bmp", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_bmp(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 2);
    SMK_ASSERT_EQ_INT(h, 2);
    if (rgb != NULL)
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    free(rgb);
    remove(path);
    free(path);
}

void test_bmp_32_top_down(void)
{
    bmp_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    /* Top-down 2x2; alpha bytes are 0 and must be treated as opaque. */
    static const uint8_t pixels[16] = {
        255, 0, 0, 0,    255, 255, 255, 0,
        0, 0, 255, 0,    0, 255, 0, 0
    };
    static const uint8_t expected[12] = {
        0, 0, 255,  255, 255, 255,
        255, 0, 0,  0, 255, 0
    };

    b.size = 0;
    bmp_header(&b, 54, 2, -2, 32, 0, 0);
    memcpy(b.data + b.size, pixels, sizeof(pixels));
    b.size += sizeof(pixels);
    bmp_finish(&b);

    path = write_temp_bmp("topdown.bmp", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_bmp(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 2);
    SMK_ASSERT_EQ_INT(h, 2);
    if (rgb != NULL)
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    free(rgb);
    remove(path);
    free(path);
}

void test_bmp_8_palette(void)
{
    bmp_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;

    b.size = 0;
    bmp_header(&b, 54 + 8, 1, 2, 8, 0, 2);
    /* Palette: entry 0 red, entry 1 blue; BGRA order. */
    bmp_u8(&b, 0); bmp_u8(&b, 0); bmp_u8(&b, 255); bmp_u8(&b, 0);
    bmp_u8(&b, 255); bmp_u8(&b, 0); bmp_u8(&b, 0); bmp_u8(&b, 0);
    /* Bottom-up: first stored row is red, then blue; rows pad to 4 bytes. */
    bmp_u8(&b, 0); bmp_u8(&b, 0); bmp_u8(&b, 0); bmp_u8(&b, 0);
    bmp_u8(&b, 1); bmp_u8(&b, 0); bmp_u8(&b, 0); bmp_u8(&b, 0);
    bmp_finish(&b);

    path = write_temp_bmp("palette.bmp", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_bmp(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 1);
    SMK_ASSERT_EQ_INT(h, 2);
    if (rgb != NULL) {
        SMK_ASSERT_EQ_INT(rgb[0], 0);
        SMK_ASSERT_EQ_INT(rgb[1], 0);
        SMK_ASSERT_EQ_INT(rgb[2], 255);
        SMK_ASSERT_EQ_INT(rgb[3], 255);
        SMK_ASSERT_EQ_INT(rgb[4], 0);
        SMK_ASSERT_EQ_INT(rgb[5], 0);
    }
    free(rgb);
    remove(path);
    free(path);
}

void test_bmp_odd_width_padding(void)
{
    bmp_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    /* 3x1 24-bit: 9 data bytes padded to 12. */
    static const uint8_t pixels[12] = {
        0, 0, 255,  0, 255, 0,  255, 0, 0,  0xaa, 0xbb, 0xcc
    };
    static const uint8_t expected[9] = {
        255, 0, 0,  0, 255, 0,  0, 0, 255
    };

    b.size = 0;
    bmp_header(&b, 54, 3, 1, 24, 0, 0);
    memcpy(b.data + b.size, pixels, sizeof(pixels));
    b.size += sizeof(pixels);
    bmp_finish(&b);

    path = write_temp_bmp("odd.bmp", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_bmp(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 3);
    SMK_ASSERT_EQ_INT(h, 1);
    if (rgb != NULL)
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    free(rgb);
    remove(path);
    free(path);
}

void test_bmp_truncated(void)
{
    bmp_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;

    b.size = 0;
    bmp_header(&b, 54, 4, 4, 24, 0, 0);
    bmp_finish(&b);

    path = write_temp_bmp("truncated.bmp", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_bmp(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(rgb == NULL);
    SMK_ASSERT(err[0] != '\0');
    remove(path);
    free(path);
}
