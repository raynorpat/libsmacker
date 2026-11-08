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

static unsigned mask_component(uint32_t value, uint32_t mask)
{
    unsigned shift = 0;
    uint32_t max;

    if (mask == 0)
        return 0;
    while (shift < 32 && ((mask >> shift) & 1u) == 0)
        shift++;
    if (shift >= 32)
        return 0;
    max = mask >> shift;
    return (unsigned)((((value & mask) >> shift) * 255u + max / 2) / max);
}

static smk_enc_status load_bmp(FILE *fp, const uint8_t background[3],
                               uint8_t **rgb, unsigned *width, unsigned *height,
                               char *err, size_t errlen)
{
    uint8_t file_header[14];
    uint8_t size_bytes[4];
    uint8_t *dib = NULL;
    uint32_t pixel_offset;
    uint32_t dib_size;
    int32_t signed_width;
    int32_t signed_height;
    unsigned bitcount;
    uint32_t compression;
    uint32_t colors_used;
    uint32_t masks[4] = { 0, 0, 0, 0 };
    unsigned mask_count = 0;
    unsigned w;
    unsigned h;
    int top_down;
    size_t row_size;
    size_t total;
    uint8_t *row = NULL;
    uint8_t *pixels = NULL;
    uint8_t *palette = NULL;
    unsigned palette_entries = 0;
    smk_enc_status status = SMK_ENC_ERR_INPUT;
    unsigned y;

    if (read_exact(fp, file_header, sizeof(file_header)) != 0)
        return fail(err, errlen, "truncated BMP header");
    if (file_header[0] != 'B' || file_header[1] != 'M')
        return fail(err, errlen, "not a BMP file");
    pixel_offset = (uint32_t)file_header[10] |
                   ((uint32_t)file_header[11] << 8) |
                   ((uint32_t)file_header[12] << 16) |
                   ((uint32_t)file_header[13] << 24);

    if (read_exact(fp, size_bytes, 4) != 0)
        return fail(err, errlen, "truncated BMP header");
    dib_size = (uint32_t)size_bytes[0] | ((uint32_t)size_bytes[1] << 8) |
               ((uint32_t)size_bytes[2] << 16) |
               ((uint32_t)size_bytes[3] << 24);
    if (dib_size < 40 || dib_size > 4096)
        return fail(err, errlen, "unsupported BMP header size");

    dib = (uint8_t *)malloc(dib_size - 4);
    if (dib == NULL)
        return fail(err, errlen, "out of memory reading BMP header");
    if (read_exact(fp, dib, dib_size - 4) != 0) {
        free(dib);
        return fail(err, errlen, "truncated BMP header");
    }

    signed_width = (int32_t)((uint32_t)dib[0] | ((uint32_t)dib[1] << 8) |
                             ((uint32_t)dib[2] << 16) |
                             ((uint32_t)dib[3] << 24));
    signed_height = (int32_t)((uint32_t)dib[4] | ((uint32_t)dib[5] << 8) |
                              ((uint32_t)dib[6] << 16) |
                              ((uint32_t)dib[7] << 24));
    bitcount = (unsigned)dib[10] | ((unsigned)dib[11] << 8);
    compression = (uint32_t)dib[12] | ((uint32_t)dib[13] << 8) |
                  ((uint32_t)dib[14] << 16) | ((uint32_t)dib[15] << 24);
    colors_used = (uint32_t)dib[28] | ((uint32_t)dib[29] << 8) |
                  ((uint32_t)dib[30] << 16) | ((uint32_t)dib[31] << 24);

    if (signed_width <= 0 || signed_height == 0) {
        free(dib);
        return fail(err, errlen, "BMP image has invalid dimensions");
    }
    if (((unsigned)dib[8] | ((unsigned)dib[9] << 8)) != 1) {
        free(dib);
        return fail(err, errlen, "BMP image must have one plane");
    }
    if (bitcount != 8 && bitcount != 16 && bitcount != 24 && bitcount != 32) {
        free(dib);
        return fail(err, errlen, "unsupported BMP color depth");
    }
    if (compression != 0 && compression != 3) {
        free(dib);
        return fail(err, errlen, "compressed BMP images are not supported");
    }
    if (compression == 3 && bitcount != 16 && bitcount != 32) {
        free(dib);
        return fail(err, errlen, "BMP bitfields need 16- or 32-bit pixels");
    }

    if (compression == 3) {
        if (dib_size >= 56) {
            unsigned i;
            for (i = 0; i < 4; i++) {
                size_t at = 36 + (size_t)i * 4;
                masks[i] = (uint32_t)dib[at] |
                           ((uint32_t)dib[at + 1] << 8) |
                           ((uint32_t)dib[at + 2] << 16) |
                           ((uint32_t)dib[at + 3] << 24);
            }
            mask_count = 4;
        } else {
            uint8_t raw[12];
            unsigned i;
            if (read_exact(fp, raw, sizeof(raw)) != 0) {
                free(dib);
                return fail(err, errlen, "truncated BMP bitfields");
            }
            for (i = 0; i < 3; i++) {
                masks[i] = (uint32_t)raw[i * 4] |
                           ((uint32_t)raw[i * 4 + 1] << 8) |
                           ((uint32_t)raw[i * 4 + 2] << 16) |
                           ((uint32_t)raw[i * 4 + 3] << 24);
            }
            mask_count = 3;
        }
        if (masks[0] == 0 || masks[1] == 0 || masks[2] == 0) {
            free(dib);
            return fail(err, errlen, "BMP bitfields are incomplete");
        }
    }
    (void)mask_count;

    w = (unsigned)signed_width;
    h = signed_height < 0 ? (unsigned)(-signed_height)
                          : (unsigned)signed_height;
    top_down = signed_height < 0;

    total = (size_t)w * h;
    if (total > IMAGE_MAX_PIXELS) {
        free(dib);
        return fail(err, errlen, "BMP image is too large");
    }
    row_size = (((size_t)w * bitcount + 31) / 32) * 4;

    if (bitcount == 8) {
        unsigned i;
        uint8_t entry[4];

        palette_entries = colors_used == 0 ? 256 : colors_used;
        if (palette_entries > 256) {
            free(dib);
            return fail(err, errlen, "BMP palette is too large");
        }
        if (fseek(fp, (long)(14 + dib_size), SEEK_SET) != 0) {
            free(dib);
            return fail(err, errlen, "truncated BMP palette");
        }
        palette = (uint8_t *)malloc((size_t)palette_entries * 3);
        if (palette == NULL) {
            free(dib);
            return fail(err, errlen, "out of memory reading BMP palette");
        }
        for (i = 0; i < palette_entries; i++) {
            if (read_exact(fp, entry, 4) != 0) {
                free(palette);
                free(dib);
                return fail(err, errlen, "truncated BMP palette");
            }
            palette[i * 3] = entry[2];
            palette[i * 3 + 1] = entry[1];
            palette[i * 3 + 2] = entry[0];
        }
    }

    row = (uint8_t *)malloc(row_size == 0 ? 1 : row_size);
    pixels = (uint8_t *)malloc(total * 3);
    if (row == NULL || pixels == NULL) {
        free(row);
        free(pixels);
        free(palette);
        free(dib);
        return fail(err, errlen, "out of memory reading BMP image");
    }

    if (fseek(fp, (long)pixel_offset, SEEK_SET) != 0) {
        status = fail(err, errlen, "truncated BMP pixel data");
        goto done;
    }

    for (y = 0; y < h; y++) {
        unsigned x;
        size_t out_y = top_down ? y : (h - 1 - y);

        if (read_exact(fp, row, row_size) != 0) {
            status = fail(err, errlen, "truncated BMP pixel data");
            goto done;
        }
        for (x = 0; x < w; x++) {
            uint8_t *out = pixels + (out_y * w + x) * 3;

            if (bitcount == 8) {
                unsigned index = row[x];
                if (index >= palette_entries) {
                    status = fail(err, errlen, "BMP pixel index out of range");
                    goto done;
                }
                memcpy(out, palette + (size_t)index * 3, 3);
            } else if (bitcount == 24) {
                const uint8_t *src = row + (size_t)x * 3;
                out[0] = src[2];
                out[1] = src[1];
                out[2] = src[0];
            } else {
                uint32_t value;
                if (bitcount == 16)
                    value = (uint32_t)row[(size_t)x * 2] |
                            ((uint32_t)row[(size_t)x * 2 + 1] << 8);
                else
                    value = (uint32_t)row[(size_t)x * 4] |
                            ((uint32_t)row[(size_t)x * 4 + 1] << 8) |
                            ((uint32_t)row[(size_t)x * 4 + 2] << 16) |
                            ((uint32_t)row[(size_t)x * 4 + 3] << 24);

                if (compression == 0) {
                    if (bitcount == 16) {
                        out[0] = (uint8_t)expand5((value >> 10) & 31u);
                        out[1] = (uint8_t)expand5((value >> 5) & 31u);
                        out[2] = (uint8_t)expand5(value & 31u);
                    } else {
                        out[0] = (uint8_t)(value >> 16);
                        out[1] = (uint8_t)(value >> 8);
                        out[2] = (uint8_t)value;
                    }
                } else {
                    unsigned r = mask_component(value, masks[0]);
                    unsigned g = mask_component(value, masks[1]);
                    unsigned b = mask_component(value, masks[2]);
                    unsigned a = masks[3] == 0
                        ? 255u : mask_component(value, masks[3]);
                    blend(out, r, g, b, a, background);
                }
            }
        }
    }

    status = SMK_ENC_OK;
    *rgb = pixels;
    *width = w;
    *height = h;
    pixels = NULL;

done:
    free(pixels);
    free(row);
    free(palette);
    free(dib);
    return status;
}

static int pcx_decode_rle(FILE *fp, uint8_t *dst, size_t size)
{
    size_t pos = 0;

    while (pos < size) {
        int byte = fgetc(fp);
        unsigned count;

        if (byte == EOF)
            return -1;
        if ((byte & 0xc0) != 0xc0) {
            dst[pos++] = (uint8_t)byte;
            continue;
        }
        count = (unsigned)(byte & 0x3f);
        byte = fgetc(fp);
        if (byte == EOF || count == 0 || count > size - pos)
            return -1;
        memset(dst + pos, byte, count);
        pos += count;
    }
    return 0;
}

static smk_enc_status load_pcx(FILE *fp, const uint8_t background[3],
                               uint8_t **rgb, unsigned *width, unsigned *height,
                               char *err, size_t errlen)
{
    uint8_t header[128];
    unsigned bpp;
    unsigned planes;
    unsigned bytes_per_line;
    unsigned w;
    unsigned h;
    size_t pixel_bytes;
    size_t total;
    uint8_t *planes_data = NULL;
    uint8_t *pixels = NULL;
    uint8_t palette[256 * 3];
    smk_enc_status status = SMK_ENC_ERR_INPUT;
    unsigned y;

    if (read_exact(fp, header, sizeof(header)) != 0)
        return fail(err, errlen, "truncated PCX header");
    if (header[0] != 0x0a)
        return fail(err, errlen, "not a PCX file");
    if (header[2] != 1)
        return fail(err, errlen, "unsupported PCX encoding");

    bpp = header[3];
    planes = header[65];
    bytes_per_line = (unsigned)header[66] | ((unsigned)header[67] << 8);
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8)
        return fail(err, errlen, "unsupported PCX bit depth");
    if (planes == 0)
        return fail(err, errlen, "PCX image has no planes");
    if (bpp == 8) {
        if (planes != 1 && planes != 3 && planes != 4)
            return fail(err, errlen, "unsupported PCX plane count");
    } else if (planes != 1) {
        return fail(err, errlen, "unsupported PCX plane count");
    }

    {
        unsigned xmin = (unsigned)header[4] | ((unsigned)header[5] << 8);
        unsigned ymin = (unsigned)header[6] | ((unsigned)header[7] << 8);
        unsigned xmax = (unsigned)header[8] | ((unsigned)header[9] << 8);
        unsigned ymax = (unsigned)header[10] | ((unsigned)header[11] << 8);

        if (xmax < xmin || ymax < ymin)
            return fail(err, errlen, "PCX image has invalid dimensions");
        w = xmax - xmin + 1;
        h = ymax - ymin + 1;
    }
    if (bytes_per_line == 0 ||
        (size_t)bytes_per_line * 8 < (size_t)w * bpp)
        return fail(err, errlen, "PCX scanline is too short");

    total = (size_t)w * h;
    if (total > IMAGE_MAX_PIXELS)
        return fail(err, errlen, "PCX image is too large");
    pixel_bytes = (size_t)h * planes * bytes_per_line;
    if (pixel_bytes > IMAGE_MAX_PIXELS * 4)
        return fail(err, errlen, "PCX image is too large");

    planes_data = (uint8_t *)malloc(pixel_bytes);
    pixels = (uint8_t *)malloc(total * 3);
    if (planes_data == NULL || pixels == NULL) {
        status = fail(err, errlen, "out of memory reading PCX image");
        goto done;
    }
    if (pcx_decode_rle(fp, planes_data, pixel_bytes) != 0) {
        status = fail(err, errlen, "truncated PCX image data");
        goto done;
    }

    if (bpp == 8) {
        if (planes == 1) {
            uint8_t tail[769];

            if (read_exact(fp, tail, sizeof(tail)) != 0 || tail[0] != 0x0c) {
                status = fail(err, errlen,
                              "PCX image has no 256-color palette");
                goto done;
            }
            memcpy(palette, tail + 1, 768);
        } else {
            unsigned i;
            for (i = 0; i < 16; i++) {
                palette[i * 3] = header[16 + i * 3];
                palette[i * 3 + 1] = header[16 + i * 3 + 1];
                palette[i * 3 + 2] = header[16 + i * 3 + 2];
            }
        }
    } else {
        unsigned i;
        for (i = 0; i < 16; i++) {
            palette[i * 3] = header[16 + i * 3];
            palette[i * 3 + 1] = header[16 + i * 3 + 1];
            palette[i * 3 + 2] = header[16 + i * 3 + 2];
        }
    }

    for (y = 0; y < h; y++) {
        const uint8_t *row = planes_data + (size_t)y * planes * bytes_per_line;
        unsigned x;

        for (x = 0; x < w; x++) {
            uint8_t *out = pixels + ((size_t)y * w + x) * 3;

            if (bpp == 8) {
                if (planes == 1) {
                    unsigned index = row[x];
                    memcpy(out, palette + (size_t)index * 3, 3);
                } else if (planes == 3) {
                    out[0] = row[x];
                    out[1] = row[bytes_per_line + x];
                    out[2] = row[(size_t)bytes_per_line * 2 + x];
                } else {
                    blend(out, row[x], row[bytes_per_line + x],
                          row[(size_t)bytes_per_line * 2 + x],
                          row[(size_t)bytes_per_line * 3 + x], background);
                }
            } else {
                unsigned per_byte = 8 / bpp;
                unsigned shift = 8 - bpp - (x % per_byte) * bpp;
                unsigned index = (row[x / per_byte] >> shift) &
                                 ((1u << bpp) - 1u);
                memcpy(out, palette + (size_t)index * 3, 3);
            }
        }
    }

    status = SMK_ENC_OK;
    *rgb = pixels;
    *width = w;
    *height = h;
    pixels = NULL;

done:
    free(pixels);
    free(planes_data);
    return status;
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
    if (extension_is(ext, "bmp")) {
        fp = fopen(path, "rb");
        if (fp == NULL) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot open image: %s", path);
            return SMK_ENC_ERR_INPUT;
        }
        status = load_bmp(fp, background, rgb, width, height, err, errlen);
        fclose(fp);
        return status;
    }
    if (extension_is(ext, "pcx")) {
        fp = fopen(path, "rb");
        if (fp == NULL) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "cannot open image: %s", path);
            return SMK_ENC_ERR_INPUT;
        }
        status = load_pcx(fp, background, rgb, width, height, err, errlen);
        fclose(fp);
        return status;
    }
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
