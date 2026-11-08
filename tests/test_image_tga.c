#include "test.h"
#include "test_util.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_MAX 4096

typedef struct {
    uint8_t data[IMG_MAX];
    size_t size;
} img_buf;

static void img_u8(img_buf *b, unsigned value)
{
    b->data[b->size++] = (uint8_t)value;
}

static void img_u16(img_buf *b, unsigned value)
{
    img_u8(b, value & 0xff);
    img_u8(b, (value >> 8) & 0xff);
}

/* Write the 18-byte TGA header. */
static void tga_header(img_buf *b, unsigned id_length, unsigned cmap_type,
                       unsigned cmap_first, unsigned cmap_length,
                       unsigned cmap_bits, unsigned type, unsigned width,
                       unsigned height, unsigned bpp, unsigned descriptor)
{
    img_u8(b, id_length);
    img_u8(b, cmap_type);
    img_u8(b, type);
    img_u16(b, cmap_first);
    img_u16(b, cmap_length);
    img_u8(b, cmap_bits);
    img_u16(b, 0);
    img_u16(b, 0);
    img_u16(b, width);
    img_u16(b, height);
    img_u8(b, bpp);
    img_u8(b, descriptor);
}

static char *write_temp_image(const char *name, const void *data, size_t size)
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

static int load_image(const char *path, uint8_t **rgb, unsigned *w,
                      unsigned *h, char *err, size_t errlen)
{
    static const uint8_t black[3] = { 0, 0, 0 };

    return smk_image_load(path, black, rgb, w, h, err, errlen);
}

void test_tga_24_uncompressed_bottom_up(void)
{
    img_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    /* Bottom row: red, green. Top row: blue, white. Stored B, G, R. */
    static const uint8_t pixels[12] = {
        0, 0, 255,   0, 255, 0,
        255, 0, 0,   255, 255, 255
    };
    static const uint8_t expected[12] = {
        0, 0, 255,   255, 255, 255,
        255, 0, 0,   0, 255, 0
    };

    b.size = 0;
    tga_header(&b, 0, 0, 0, 0, 0, 2, 2, 2, 24, 0);
    memcpy(b.data + b.size, pixels, sizeof(pixels));
    b.size += sizeof(pixels);

    path = write_temp_image("bottom.tga", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_image(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 2);
    SMK_ASSERT_EQ_INT(h, 2);
    SMK_ASSERT(rgb != NULL);
    if (rgb != NULL)
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    free(rgb);
    remove(path);
    free(path);
}

void test_tga_32_rle_top_down_alpha(void)
{
    img_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    /* One raw packet of four BGRA pixels; descriptor 0x28 = top-down + alpha.
       Pixel 0: alpha 0 -> black. Pixel 1: opaque blue. Pixel 2: alpha 128,
       red 100 -> 50. Pixel 3: opaque white. */
    static const uint8_t packet[17] = {
        0x03,
        30, 20, 10, 0,
        255, 0, 0, 255,
        0, 0, 100, 128,
        255, 255, 255, 255
    };
    static const uint8_t expected[12] = {
        0, 0, 0,      0, 0, 255,
        50, 0, 0,     255, 255, 255
    };

    b.size = 0;
    tga_header(&b, 0, 0, 0, 0, 0, 10, 2, 2, 32, 0x28);
    memcpy(b.data + b.size, packet, sizeof(packet));
    b.size += sizeof(packet);

    path = write_temp_image("rle.tga", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_image(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 2);
    SMK_ASSERT_EQ_INT(h, 2);
    if (rgb != NULL)
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    free(rgb);
    remove(path);
    free(path);
}

void test_tga_8_colormapped(void)
{
    img_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    /* 1x2 image, colormap first index 0, two 24-bit entries (BGR). */
    static const uint8_t body[] = { 1, 0 };

    b.size = 0;
    tga_header(&b, 0, 1, 0, 2, 24, 1, 1, 2, 8, 0x20);
    img_u8(&b, 0); img_u8(&b, 0); img_u8(&b, 255);   /* entry 0: red */
    img_u8(&b, 255); img_u8(&b, 0); img_u8(&b, 0);   /* entry 1: blue */
    memcpy(b.data + b.size, body, sizeof(body));
    b.size += sizeof(body);

    path = write_temp_image("cmap.tga", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_image(path, &rgb, &w, &h, err, sizeof(err)),
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

void test_tga_truncated(void)
{
    img_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    static const uint8_t partial[5] = { 0, 0, 255, 0, 255 };

    b.size = 0;
    tga_header(&b, 0, 0, 0, 0, 0, 2, 4, 4, 24, 0);
    memcpy(b.data + b.size, partial, sizeof(partial));
    b.size += sizeof(partial);

    path = write_temp_image("truncated.tga", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_image(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(rgb == NULL);
    SMK_ASSERT(err[0] != '\0');
    remove(path);
    free(path);
}

void test_image_unsupported_format(void)
{
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path = write_temp_image("bad.jpg", "nope", 4);

    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_image(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(rgb == NULL);
    SMK_ASSERT(err[0] != '\0');
    remove(path);
    free(path);
}
