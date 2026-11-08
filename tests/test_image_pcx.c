#include "test.h"
#include "test_util.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[8192];
    size_t size;
} pcx_buf;

static void pcx_u8(pcx_buf *b, unsigned value)
{
    b->data[b->size++] = (uint8_t)value;
}

static void pcx_u16(pcx_buf *b, unsigned value)
{
    pcx_u8(b, value & 0xff);
    pcx_u8(b, (value >> 8) & 0xff);
}

/* Write a 128-byte PCX header with a zeroed body afterwards. */
static void pcx_header(pcx_buf *b, unsigned version, unsigned bpp,
                       unsigned planes, unsigned bytes_per_line,
                       unsigned width, unsigned height)
{
    unsigned i;

    memset(b->data, 0, sizeof(b->data));
    b->size = 0;
    pcx_u8(b, 0x0a);
    pcx_u8(b, version);
    pcx_u8(b, 1);
    pcx_u8(b, bpp);
    pcx_u16(b, 0);
    pcx_u16(b, 0);
    pcx_u16(b, width - 1);
    pcx_u16(b, height - 1);
    pcx_u16(b, 72);
    pcx_u16(b, 72);
    for (i = 0; i < 48; i++)
        pcx_u8(b, 0);
    pcx_u8(b, 0);
    pcx_u8(b, planes);
    pcx_u16(b, bytes_per_line);
    pcx_u16(b, 1);
    pcx_u16(b, 0);
    pcx_u16(b, 0);
    for (i = 0; i < 54; i++)
        pcx_u8(b, 0);
}

static char *write_temp_pcx(const char *name, const void *data, size_t size)
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

static int load_pcx(const char *path, uint8_t **rgb, unsigned *w,
                    unsigned *h, char *err, size_t errlen)
{
    static const uint8_t black[3] = { 0, 0, 0 };

    return smk_image_load(path, black, rgb, w, h, err, errlen);
}

void test_pcx_8bit_1plane_palette(void)
{
    pcx_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    unsigned i;

    pcx_header(&b, 5, 8, 1, 2, 2, 2);
    /* One run of two 0x00 crossing the scanline, then 1 and 2. */
    pcx_u8(&b, 0xc2);
    pcx_u8(&b, 0x00);
    pcx_u8(&b, 0x01);
    pcx_u8(&b, 0x02);
    /* 256-color tail: entry 0 red, 1 green, 2 blue. */
    pcx_u8(&b, 0x0c);
    for (i = 0; i < 256; i++) {
        pcx_u8(&b, i == 0 ? 255 : 0);
        pcx_u8(&b, i == 1 ? 255 : 0);
        pcx_u8(&b, i == 2 ? 255 : 0);
    }

    path = write_temp_pcx("oneplane.pcx", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_pcx(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 2);
    SMK_ASSERT_EQ_INT(h, 2);
    if (rgb != NULL) {
        static const uint8_t expected[12] = {
            255, 0, 0,  255, 0, 0,
            0, 255, 0,  0, 0, 255
        };
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    }
    free(rgb);
    remove(path);
    free(path);
}

void test_pcx_8bit_3plane_rgb(void)
{
    pcx_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    static const uint8_t expected[6] = { 10, 30, 50, 20, 40, 60 };

    pcx_header(&b, 5, 8, 3, 2, 2, 1);
    /* R plane, then G plane, then B plane. */
    pcx_u8(&b, 10);
    pcx_u8(&b, 20);
    pcx_u8(&b, 30);
    pcx_u8(&b, 40);
    pcx_u8(&b, 50);
    pcx_u8(&b, 60);

    path = write_temp_pcx("threeplane.pcx", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_pcx(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 2);
    SMK_ASSERT_EQ_INT(h, 1);
    if (rgb != NULL)
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    free(rgb);
    remove(path);
    free(path);
}

void test_pcx_1bit(void)
{
    pcx_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;
    static const uint8_t expected[24] = {
        255, 255, 255,  0, 0, 0,  255, 255, 255,  0, 0, 0,
        0, 0, 0,        0, 0, 0,  0, 0, 0,        0, 0, 0
    };

    pcx_header(&b, 5, 1, 1, 1, 8, 1);
    /* Header palette: entry 0 black, entry 1 white. */
    b.data[16 + 0] = 0;
    b.data[16 + 1] = 0;
    b.data[16 + 2] = 0;
    b.data[16 + 3] = 255;
    b.data[16 + 4] = 255;
    b.data[16 + 5] = 255;
    /* Bits are MSB first: 1010 0000. */
    pcx_u8(&b, 0xa0);

    path = write_temp_pcx("onebit.pcx", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_pcx(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_OK);
    SMK_ASSERT_EQ_INT(w, 8);
    SMK_ASSERT_EQ_INT(h, 1);
    if (rgb != NULL)
        SMK_ASSERT_MEM_EQ(rgb, expected, sizeof(expected));
    free(rgb);
    remove(path);
    free(path);
}

void test_pcx_truncated(void)
{
    pcx_buf b;
    uint8_t *rgb = NULL;
    unsigned w = 0;
    unsigned h = 0;
    char err[256];
    char *path;

    pcx_header(&b, 5, 8, 1, 16, 16, 16);
    pcx_u8(&b, 0x01);

    path = write_temp_pcx("truncated.pcx", b.data, b.size);
    SMK_ASSERT(path != NULL);
    memset(err, 0, sizeof(err));
    SMK_ASSERT_EQ_INT(load_pcx(path, &rgb, &w, &h, err, sizeof(err)),
                      SMK_ENC_ERR_INPUT);
    SMK_ASSERT(rgb == NULL);
    SMK_ASSERT(err[0] != '\0');
    remove(path);
    free(path);
}
