#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Refuse absurd allocations from corrupt headers (16384 x 16384 pixels). */
#define IMAGE_MAX_PIXELS ((size_t)1 << 28)

static smk_enc_status fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return SMK_ENC_ERR_INPUT;
}

static const char *extension_of(const char *path)
{
    const char *base = path;
    const char *dot = NULL;
    size_t i;

    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/' || path[i] == '\\')
            base = path + i + 1;
    }
    for (i = 0; base[i] != '\0'; i++) {
        if (base[i] == '.')
            dot = base + i;
    }
    return dot == NULL ? NULL : dot + 1;
}

static int extension_is(const char *ext, const char *name)
{
    size_t i;

    for (i = 0; name[i] != '\0'; i++) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != name[i])
            return 0;
    }
    return ext[i] == '\0';
}

static int read_exact(FILE *fp, void *dst, size_t size)
{
    return size == 0 || fread(dst, 1, size, fp) == size ? 0 : -1;
}

static unsigned expand5(unsigned value)
{
    return (value << 3) | (value >> 2);
}

/* Blend one source color with alpha over background. */
static void blend(uint8_t out[3], unsigned r, unsigned g, unsigned b,
                  unsigned alpha, const uint8_t background[3])
{
    unsigned inverse = 255 - alpha;

    out[0] = (uint8_t)((r * alpha + background[0] * inverse + 127) / 255);
    out[1] = (uint8_t)((g * alpha + background[1] * inverse + 127) / 255);
    out[2] = (uint8_t)((b * alpha + background[2] * inverse + 127) / 255);
}

/*
 * Expand one stored color of bpp bits into RGB, compositing alpha. Returns 0
 * on success or -1 for an unsupported depth.
 */
static int expand_color(const uint8_t *src, unsigned bpp,
                        const uint8_t background[3], uint8_t out[3])
{
    switch (bpp) {
    case 15:
    case 16: {
        unsigned value = (unsigned)src[0] | ((unsigned)src[1] << 8);
        unsigned r = expand5((value >> 10) & 31u);
        unsigned g = expand5((value >> 5) & 31u);
        unsigned b = expand5(value & 31u);

        if (bpp == 16 && (value & 0x8000u) == 0) {
            out[0] = background[0];
            out[1] = background[1];
            out[2] = background[2];
        } else {
            out[0] = (uint8_t)r;
            out[1] = (uint8_t)g;
            out[2] = (uint8_t)b;
        }
        return 0;
    }
    case 24:
        out[0] = src[2];
        out[1] = src[1];
        out[2] = src[0];
        return 0;
    case 32:
        blend(out, src[2], src[1], src[0], src[3], background);
        return 0;
    default:
        return -1;
    }
}

typedef struct {
    FILE *fp;
    uint8_t *rgb;
    unsigned width;
    unsigned height;
    unsigned type;
    unsigned bpp;
    int bottom_up;
    int right_to_left;
    const uint8_t *background;
    uint8_t *palette; /* palette_count * 3, already composited */
    unsigned palette_count;
    unsigned palette_first;
} tga_reader;

static int tga_read_pixel(tga_reader *t, uint8_t out[3])
{
    unsigned bytes = (t->bpp + 7) / 8;
    uint8_t raw[4];

    if (read_exact(t->fp, raw, bytes) != 0)
        return -1;

    if (t->type == 3 || t->type == 11) {
        out[0] = raw[0];
        out[1] = raw[0];
        out[2] = raw[0];
        return 0;
    }
    if (t->type == 1 || t->type == 9) {
        unsigned index = bytes == 2
            ? ((unsigned)raw[0] | ((unsigned)raw[1] << 8))
            : (unsigned)raw[0];

        if (index < t->palette_first ||
            index - t->palette_first >= t->palette_count)
            return -2;
        memcpy(out, t->palette + (index - t->palette_first) * 3, 3);
        return 0;
    }
    return expand_color(raw, t->bpp, t->background, out);
}

static void tga_store(tga_reader *t, size_t file_index, const uint8_t rgb[3])
{
    size_t row = file_index / t->width;
    size_t column = file_index % t->width;
    size_t x = t->right_to_left ? t->width - 1 - column : column;
    size_t y = t->bottom_up ? t->height - 1 - row : row;

    memcpy(t->rgb + (y * t->width + x) * 3, rgb, 3);
}

static smk_enc_status tga_decode_body(tga_reader *t, size_t total,
                                      char *err, size_t errlen)
{
    int rle = t->type >= 9;
    size_t index = 0;

    while (index < total) {
        size_t count = 1;
        int repeat = 0;
        uint8_t rgb[3];

        if (rle) {
            int header = fgetc(t->fp);
            size_t k;

            if (header == EOF)
                return fail(err, errlen, "truncated TGA image data");
            count = (size_t)(header & 0x7f) + 1;
            repeat = (header & 0x80) != 0;
            if (count > total - index)
                return fail(err, errlen, "corrupt TGA RLE packet");

            if (repeat) {
                if (tga_read_pixel(t, rgb) != 0)
                    return fail(err, errlen, "truncated TGA image data");
                for (k = 0; k < count; k++)
                    tga_store(t, index++, rgb);
                continue;
            }
        }

        while (count-- > 0) {
            if (tga_read_pixel(t, rgb) != 0)
                return fail(err, errlen, "truncated TGA image data");
            tga_store(t, index++, rgb);
        }
    }
    return SMK_ENC_OK;
}

static smk_enc_status load_tga(FILE *fp, const uint8_t background[3],
                               uint8_t **rgb, unsigned *width, unsigned *height,
                               char *err, size_t errlen)
{
    uint8_t header[18];
    unsigned id_length;
    unsigned colormap_type;
    unsigned type;
    unsigned colormap_first;
    unsigned colormap_length;
    unsigned colormap_bits;
    tga_reader reader;
    smk_enc_status status;
    uint8_t *pixels;
    size_t total;
    unsigned i;

    if (read_exact(fp, header, sizeof(header)) != 0)
        return fail(err, errlen, "truncated TGA header");

    id_length = header[0];
    colormap_type = header[1];
    type = header[2];
    colormap_first = (unsigned)header[3] | ((unsigned)header[4] << 8);
    colormap_length = (unsigned)header[5] | ((unsigned)header[6] << 8);
    colormap_bits = header[7];
    reader.width = (unsigned)header[12] | ((unsigned)header[13] << 8);
    reader.height = (unsigned)header[14] | ((unsigned)header[15] << 8);
    reader.bpp = header[16];

    if (type != 1 && type != 2 && type != 3 &&
        type != 9 && type != 10 && type != 11)
        return fail(err, errlen, "unsupported TGA image type");
    if (reader.width == 0 || reader.height == 0)
        return fail(err, errlen, "TGA image has zero dimensions");

    if (type == 2 || type == 10) {
        if (reader.bpp != 15 && reader.bpp != 16 &&
            reader.bpp != 24 && reader.bpp != 32)
            return fail(err, errlen, "unsupported TGA color depth");
        if (colormap_type != 0)
            return fail(err, errlen, "true-color TGA must not have a colormap");
    } else if (type == 3 || type == 11) {
        if (reader.bpp != 8)
            return fail(err, errlen, "unsupported TGA grayscale depth");
        if (colormap_type != 0)
            return fail(err, errlen, "grayscale TGA must not have a colormap");
    } else {
        if (reader.bpp != 8 && reader.bpp != 16)
            return fail(err, errlen, "unsupported TGA colormap index size");
        if (colormap_type != 1 || colormap_length == 0)
            return fail(err, errlen, "colormapped TGA has no colormap");
        if (colormap_bits != 15 && colormap_bits != 16 &&
            colormap_bits != 24 && colormap_bits != 32)
            return fail(err, errlen, "unsupported TGA colormap depth");
    }

    total = (size_t)reader.width * reader.height;
    if (total > IMAGE_MAX_PIXELS)
        return fail(err, errlen, "TGA image is too large");
    if (id_length > 0) {
        uint8_t scratch[255];
        if (read_exact(fp, scratch, id_length) != 0)
            return fail(err, errlen, "truncated TGA image ID");
    }

    reader.fp = fp;
    reader.type = type;
    reader.background = background;
    reader.bottom_up = (header[17] & 0x20) == 0;
    reader.right_to_left = (header[17] & 0x10) != 0;
    reader.palette = NULL;
    reader.palette_count = 0;
    reader.palette_first = colormap_first;

    if (colormap_type == 1) {
        unsigned entry_bytes = (colormap_bits + 7) / 8;
        uint8_t entry[4];

        if (colormap_length > 65536)
            return fail(err, errlen, "TGA colormap is too large");
        reader.palette_count = colormap_length;
        reader.palette = (uint8_t *)malloc((size_t)colormap_length * 3);
        if (reader.palette == NULL)
            return fail(err, errlen, "out of memory reading TGA colormap");
        for (i = 0; i < colormap_length; i++) {
            if (read_exact(fp, entry, entry_bytes) != 0 ||
                expand_color(entry, colormap_bits, background,
                             reader.palette + (size_t)i * 3) != 0) {
                free(reader.palette);
                return fail(err, errlen, "truncated TGA colormap");
            }
        }
    }

    pixels = (uint8_t *)malloc(total * 3);
    if (pixels == NULL) {
        free(reader.palette);
        return fail(err, errlen, "out of memory reading TGA image");
    }
    reader.rgb = pixels;

    status = tga_decode_body(&reader, total, err, errlen);
    free(reader.palette);
    if (status != SMK_ENC_OK) {
        free(pixels);
        return status;
    }

    *rgb = pixels;
    *width = reader.width;
    *height = reader.height;
    return SMK_ENC_OK;
}

smk_enc_status smk_image_load(const char *path, const uint8_t background[3],
                              uint8_t **rgb, unsigned *width, unsigned *height,
                              char *err, size_t errlen)
{
    const char *ext;
    FILE *fp;
    smk_enc_status status;

    if (path == NULL || background == NULL || rgb == NULL ||
        width == NULL || height == NULL)
        return fail(err, errlen, "missing image load argument");

    *rgb = NULL;
    *width = 0;
    *height = 0;

    ext = extension_of(path);
    if (ext == NULL)
        return fail(err, errlen, "image file has no extension");
    if (extension_is(ext, "bmp"))
        return fail(err, errlen, "BMP images are not supported yet");
    if (extension_is(ext, "pcx"))
        return fail(err, errlen, "PCX images are not supported yet");
    if (!extension_is(ext, "tga"))
        return fail(err, errlen, "unsupported image format");

    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "cannot open image: %s", path);
        return SMK_ENC_ERR_INPUT;
    }
    status = load_tga(fp, background, rgb, width, height, err, errlen);
    fclose(fp);
    return status;
}
