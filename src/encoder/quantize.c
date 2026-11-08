#include "quantize.h"

#include <stdlib.h>
#include <string.h>

/* The format's fixed 6-bit to 8-bit expansion table (see docs/smacker.md). */
static const uint8_t smk_palmap[64] = {
    0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
    0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
    0x41, 0x45, 0x49, 0x4d, 0x51, 0x55, 0x59, 0x5d,
    0x61, 0x65, 0x69, 0x6d, 0x71, 0x75, 0x79, 0x7d,
    0x82, 0x86, 0x8a, 0x8e, 0x92, 0x96, 0x9a, 0x9e,
    0xa2, 0xa6, 0xaa, 0xae, 0xb2, 0xb6, 0xba, 0xbe,
    0xc3, 0xc7, 0xcb, 0xcf, 0xd3, 0xd7, 0xdb, 0xdf,
    0xe3, 0xe7, 0xeb, 0xef, 0xf3, 0xf7, 0xfb, 0xff
};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t weight;
} qbin;

typedef struct {
    unsigned start;
    unsigned end;
    uint64_t weight;
    uint8_t lo[3];
    uint8_t hi[3];
} qbox;

void smk_histogram_init(smk_histogram *hist)
{
    memset(hist, 0, sizeof(*hist));
}

/* Bin order is r << 12 | g << 6 | b over 6-bit levels. */
static unsigned bin_of(unsigned r, unsigned g, unsigned b)
{
    return (r << 12) | (g << 6) | b;
}

void smk_histogram_add(smk_histogram *hist, const uint8_t *rgb, size_t pixels)
{
    size_t i;

    for (i = 0; i < pixels; i++) {
        const uint8_t *p = rgb + i * 3;
        hist->bins[bin_of(p[0] >> 2, p[1] >> 2, p[2] >> 2)]++;
    }
}

static unsigned collect_bins(const smk_histogram *hist, qbin **out)
{
    unsigned count = 0;
    unsigned bin;
    qbin *bins;

    for (bin = 0; bin < SMK_QUANT_BINS; bin++) {
        if (hist->bins[bin] != 0)
            count++;
    }
    if (count == 0) {
        *out = NULL;
        return 0;
    }
    bins = (qbin *)malloc((size_t)count * sizeof(*bins));
    if (bins == NULL) {
        *out = NULL;
        return 0;
    }
    count = 0;
    for (bin = 0; bin < SMK_QUANT_BINS; bin++) {
        if (hist->bins[bin] == 0)
            continue;
        bins[count].r = (uint8_t)(bin >> 12);
        bins[count].g = (uint8_t)((bin >> 6) & 63u);
        bins[count].b = (uint8_t)(bin & 63u);
        bins[count].weight = hist->bins[bin];
        count++;
    }
    *out = bins;
    return count;
}

static unsigned channel_of(const qbin *bin, int axis)
{
    return axis == 0 ? bin->r : axis == 1 ? bin->g : bin->b;
}

/* Stable counting sort of a bin range on one channel (levels are 0..63). */
static void sort_range(qbin *bins, unsigned start, unsigned end, int axis,
                       qbin *scratch)
{
    unsigned counts[SMK_QUANT_LEVELS] = { 0 };
    unsigned offsets[SMK_QUANT_LEVELS];
    unsigned i;

    for (i = start; i < end; i++)
        counts[channel_of(&bins[i], axis)]++;
    offsets[0] = start;
    for (i = 1; i < SMK_QUANT_LEVELS; i++)
        offsets[i] = offsets[i - 1] + counts[i - 1];
    for (i = start; i < end; i++) {
        unsigned level = channel_of(&bins[i], axis);
        scratch[offsets[level]++] = bins[i];
    }
    for (i = start; i < end; i++)
        bins[i] = scratch[i];
}

static void measure_box(qbin *bins, qbox *box)
{
    unsigned i;

    box->weight = 0;
    box->lo[0] = box->lo[1] = box->lo[2] = 255;
    box->hi[0] = box->hi[1] = box->hi[2] = 0;
    for (i = box->start; i < box->end; i++) {
        unsigned c;

        box->weight += bins[i].weight;
        for (c = 0; c < 3; c++) {
            unsigned level = channel_of(&bins[i], (int)c);

            if (level < box->lo[c])
                box->lo[c] = (uint8_t)level;
            if (level > box->hi[c])
                box->hi[c] = (uint8_t)level;
        }
    }
}

static long span_of(const qbox *box, int axis)
{
    return (long)box->hi[axis] - (long)box->lo[axis];
}

static int split_box(qbin *bins, qbox *box, qbin *scratch,
                     qbox *low, qbox *high)
{
    int axis = 0;
    long best;
    uint64_t cumulative = 0;
    unsigned split;
    unsigned start = box->start;
    unsigned end = box->end;
    unsigned i;

    if (end - start < 2)
        return -1;

    best = span_of(box, 0);
    if (span_of(box, 1) > best) {
        best = span_of(box, 1);
        axis = 1;
    }
    if (span_of(box, 2) > best)
        axis = 2;

    /* All bins identical on every axis: only weight can be split. */
    sort_range(bins, start, end, axis, scratch);

    split = start;
    for (i = start; i + 1 < end; i++) {
        cumulative += bins[i].weight;
        if (cumulative * 2 >= box->weight) {
            split = i + 1;
            break;
        }
    }
    if (split <= start)
        split = start + 1;
    if (split >= end)
        split = end - 1;

    low->start = start;
    low->end = split;
    high->start = split;
    high->end = end;
    measure_box(bins, low);
    measure_box(bins, high);
    return 0;
}

/* Squared distance between a bin and a 6-bit palette entry. */
static unsigned distance6(const qbin *bin, const smk_rgb6 *entry)
{
    int dr = (int)bin->r - (int)entry->r;
    int dg = (int)bin->g - (int)entry->g;
    int db = (int)bin->b - (int)entry->b;

    return (unsigned)(dr * dr + dg * dg + db * db);
}

static unsigned nearest_entry(const smk_palette *palette, unsigned bin)
{
    unsigned r = bin >> 12;
    unsigned g = (bin >> 6) & 63u;
    unsigned b = bin & 63u;
    unsigned best = 0;
    unsigned best_distance = ~0u;
    unsigned i;

    for (i = 0; i < palette->count; i++) {
        int dr = (int)r - (int)palette->entry[i].r;
        int dg = (int)g - (int)palette->entry[i].g;
        int db = (int)b - (int)palette->entry[i].b;
        unsigned distance = (unsigned)(dr * dr + dg * dg + db * db);

        if (distance < best_distance) {
            best_distance = distance;
            best = i;
            if (distance == 0)
                break;
        }
    }
    return best;
}

static void palette_display(smk_palette *palette)
{
    unsigned i;

    for (i = 0; i < palette->count; i++) {
        palette->display[i * 3] = smk_palmap[palette->entry[i].r];
        palette->display[i * 3 + 1] = smk_palmap[palette->entry[i].g];
        palette->display[i * 3 + 2] = smk_palmap[palette->entry[i].b];
    }
}

static void refine_palette(qbin *bins, unsigned count, smk_palette *palette,
                           unsigned iterations)
{
    unsigned iteration;

    for (iteration = 0; iteration < iterations; iteration++) {
        uint64_t sum[SMK_QUANT_MAX_COLORS][3];
        uint64_t weight[SMK_QUANT_MAX_COLORS];
        unsigned changed = 0;
        unsigned i;

        memset(sum, 0, sizeof(sum));
        memset(weight, 0, sizeof(weight));
        for (i = 0; i < count; i++) {
            unsigned best = 0;
            unsigned best_distance = ~0u;
            unsigned j;

            for (j = 0; j < palette->count; j++) {
                unsigned distance = distance6(&bins[i], &palette->entry[j]);

                if (distance < best_distance) {
                    best_distance = distance;
                    best = j;
                }
            }
            sum[best][0] += (uint64_t)bins[i].r * bins[i].weight;
            sum[best][1] += (uint64_t)bins[i].g * bins[i].weight;
            sum[best][2] += (uint64_t)bins[i].b * bins[i].weight;
            weight[best] += bins[i].weight;
        }

        for (i = 0; i < palette->count; i++) {
            unsigned c;

            if (weight[i] == 0)
                continue;
            for (c = 0; c < 3; c++) {
                unsigned level = (unsigned)((sum[i][c] + weight[i] / 2) /
                                            weight[i]);
                uint8_t *target = c == 0 ? &palette->entry[i].r
                                : c == 1 ? &palette->entry[i].g
                                         : &palette->entry[i].b;

                if (level > 63)
                    level = 63;
                if (*target != (uint8_t)level) {
                    *target = (uint8_t)level;
                    changed = 1;
                }
            }
        }
        if (!changed)
            break;
    }
}

static unsigned build_palette(const smk_histogram *hist, unsigned max_colors,
                              smk_palette *palette, unsigned iterations)
{
    qbin *bins = NULL;
    qbin *scratch = NULL;
    qbox *boxes = NULL;
    unsigned count;

    memset(palette, 0, sizeof(*palette));
    if (max_colors == 0)
        max_colors = 1;
    if (max_colors > SMK_QUANT_MAX_COLORS)
        max_colors = SMK_QUANT_MAX_COLORS;

    if (hist != NULL) {
        count = collect_bins(hist, &bins);
    } else {
        count = 0;
    }

    if (count == 0) {
        free(bins);
        palette->entry[0].r = 0;
        palette->entry[0].g = 0;
        palette->entry[0].b = 0;
        palette->count = 1;
        palette_display(palette);
        return 1;
    }

    if (count <= max_colors) {
        unsigned i;

        for (i = 0; i < count; i++) {
            palette->entry[i].r = bins[i].r;
            palette->entry[i].g = bins[i].g;
            palette->entry[i].b = bins[i].b;
        }
        palette->count = count;
        palette_display(palette);
        free(bins);
        return count;
    }

    scratch = (qbin *)malloc((size_t)count * sizeof(*scratch));
    boxes = (qbox *)malloc((size_t)max_colors * sizeof(*boxes));
    if (scratch == NULL || boxes == NULL) {
        free(scratch);
        free(boxes);
        free(bins);
        palette->entry[0].r = 0;
        palette->entry[0].g = 0;
        palette->entry[0].b = 0;
        palette->count = 1;
        palette_display(palette);
        return 1;
    }

    boxes[0].start = 0;
    boxes[0].end = count;
    measure_box(bins, &boxes[0]);
    {
        unsigned box_count = 1;

        while (box_count < max_colors) {
            unsigned best = box_count;
            unsigned i;

            for (i = 0; i < box_count; i++) {
                if (boxes[i].end - boxes[i].start < 2)
                    continue;
                if (best == box_count || boxes[i].weight > boxes[best].weight)
                    best = i;
            }
            if (best == box_count)
                break;
            if (split_box(bins, &boxes[best], scratch, &boxes[best],
                          &boxes[box_count]) != 0)
                break;
            box_count++;
        }

        palette->count = box_count;
    }

    {
        unsigned i;

        for (i = 0; i < palette->count; i++) {
            qbox *box = &boxes[i];
            uint64_t sum[3] = { 0, 0, 0 };
            unsigned j;
            unsigned c;

            for (j = box->start; j < box->end; j++) {
                sum[0] += (uint64_t)bins[j].r * bins[j].weight;
                sum[1] += (uint64_t)bins[j].g * bins[j].weight;
                sum[2] += (uint64_t)bins[j].b * bins[j].weight;
            }
            for (c = 0; c < 3; c++) {
                unsigned level = (unsigned)((sum[c] + box->weight / 2) /
                                            box->weight);
                uint8_t *target = c == 0 ? &palette->entry[i].r
                                : c == 1 ? &palette->entry[i].g
                                         : &palette->entry[i].b;

                if (level > 63)
                    level = 63;
                *target = (uint8_t)level;
            }
        }
    }

    refine_palette(bins, count, palette, iterations);
    palette_display(palette);
    free(scratch);
    free(boxes);
    free(bins);
    return palette->count;
}

unsigned smk_quant_build_global(const smk_histogram *hist, unsigned max_colors,
                                smk_palette *palette)
{
    return build_palette(hist, max_colors, palette, 4);
}

unsigned smk_quant_build_frame(const uint8_t *rgb, size_t pixels,
                               unsigned max_colors, smk_palette *palette)
{
    smk_histogram *hist;
    unsigned count;

    if (rgb == NULL || pixels == 0) {
        palette->entry[0].r = 0;
        palette->entry[0].g = 0;
        palette->entry[0].b = 0;
        palette->count = 1;
        palette_display(palette);
        return 1;
    }
    hist = (smk_histogram *)malloc(sizeof(*hist));
    if (hist == NULL) {
        palette->entry[0].r = 0;
        palette->entry[0].g = 0;
        palette->entry[0].b = 0;
        palette->count = 1;
        palette_display(palette);
        return 1;
    }
    smk_histogram_init(hist);
    smk_histogram_add(hist, rgb, pixels);
    count = smk_quant_build_global(hist, max_colors, palette);
    free(hist);
    return count;
}

void smk_quant_lut_init(smk_quant_lut *lut)
{
    if (lut != NULL)
        memset(lut->known, 0, sizeof(lut->known));
}

static unsigned lookup_index(const smk_palette *palette, unsigned bin,
                             smk_quant_lut *lut)
{
    if (lut == NULL)
        return nearest_entry(palette, bin);
    if (!lut->known[bin]) {
        lut->index[bin] = (uint8_t)nearest_entry(palette, bin);
        lut->known[bin] = 1;
    }
    return lut->index[bin];
}

static unsigned level_of(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255u >> 2;
    return (unsigned)value >> 2;
}

static unsigned map_value(const smk_palette *palette, int r, int g, int b,
                          smk_quant_lut *lut)
{
    return lookup_index(palette, bin_of(level_of(r), level_of(g), level_of(b)),
                        lut);
}

/* Standard 8x8 Bayer matrix, values 0..63. */
static const uint8_t bayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21
};

static void map_ordered(const smk_palette *palette, const uint8_t *rgb,
                        unsigned width, unsigned height, uint8_t *indices,
                        smk_quant_lut *lut)
{
    unsigned y;

    for (y = 0; y < height; y++) {
        unsigned x;

        for (x = 0; x < width; x++) {
            size_t at = (size_t)y * width + x;
            const uint8_t *src = rgb + at * 3;
            int offset = ((int)bayer8[(y & 7) * 8 + (x & 7)] - 32) / 8;

            indices[at] = (uint8_t)map_value(palette,
                                             (int)src[0] + offset,
                                             (int)src[1] + offset,
                                             (int)src[2] + offset, lut);
        }
    }
}

static void map_fs(const smk_palette *palette, const uint8_t *rgb,
                   unsigned width, unsigned height, uint8_t *indices,
                   smk_quant_lut *lut)
{
    size_t row_size = ((size_t)width + 2) * 3;
    int *current = (int *)calloc(row_size, sizeof(int));
    int *next = (int *)calloc(row_size, sizeof(int));
    unsigned y;

    if (current == NULL || next == NULL) {
        free(current);
        free(next);
        /* Fall back to nearest colors rather than failing the frame. */
        {
            size_t i;
            for (i = 0; i < (size_t)width * height; i++)
                indices[i] = (uint8_t)map_value(
                    palette, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2], lut);
        }
        return;
    }

    for (y = 0; y < height; y++) {
        unsigned x;

        for (x = 0; x < width; x++) {
            size_t at = (size_t)y * width + x;
            const uint8_t *src = rgb + at * 3;
            int value[3];
            unsigned index;
            unsigned c;

            for (c = 0; c < 3; c++) {
                value[c] = (int)src[c] + current[(x + 1) * 3 + c];
                if (value[c] < 0)
                    value[c] = 0;
                if (value[c] > 255)
                    value[c] = 255;
            }
            index = map_value(palette, value[0], value[1], value[2], lut);
            indices[at] = (uint8_t)index;

            for (c = 0; c < 3; c++) {
                int error = value[c] - (int)palette->display[index * 3 + c];

                current[(x + 2) * 3 + c] += error * 7 / 16;
                next[x * 3 + c] += error * 3 / 16;
                next[(x + 1) * 3 + c] += error * 5 / 16;
                next[(x + 2) * 3 + c] += error / 16;
            }
        }
        {
            int *swap = current;
            current = next;
            next = swap;
            memset(next, 0, row_size * sizeof(int));
        }
    }
    free(current);
    free(next);
}

void smk_quant_map(const smk_palette *palette, const uint8_t *rgb,
                   unsigned width, unsigned height,
                   smk_enc_dither_mode dither, uint8_t *indices,
                   smk_quant_lut *lut)
{
    smk_quant_lut *temporary = NULL;
    size_t pixels = (size_t)width * height;
    size_t i;

    if (palette->count == 0 || pixels == 0)
        return;
    if (palette->count == 1) {
        memset(indices, 0, pixels);
        return;
    }
    if (lut == NULL) {
        temporary = (smk_quant_lut *)malloc(sizeof(*temporary));
        if (temporary != NULL) {
            smk_quant_lut_init(temporary);
            lut = temporary;
        }
    }

    if (dither == SMK_ENC_DITHER_ORDERED) {
        map_ordered(palette, rgb, width, height, indices, lut);
    } else if (dither == SMK_ENC_DITHER_FS) {
        map_fs(palette, rgb, width, height, indices, lut);
    } else {
        for (i = 0; i < pixels; i++) {
            const uint8_t *src = rgb + i * 3;

            indices[i] = (uint8_t)map_value(palette, src[0], src[1], src[2],
                                            lut);
        }
    }

    free(temporary);
}

double smk_quant_mean_error(const smk_palette *palette, const uint8_t *rgb,
                            size_t pixels, const uint8_t *indices)
{
    uint64_t sum = 0;
    size_t i;

    if (pixels == 0 || palette->count == 0)
        return 0.0;
    for (i = 0; i < pixels; i++) {
        unsigned index = indices[i];
        const uint8_t *src = rgb + i * 3;

        if (index >= palette->count)
            index = 0;
        sum += (uint64_t)abs((int)src[0] - (int)palette->display[index * 3]);
        sum += (uint64_t)abs((int)src[1] - (int)palette->display[index * 3 + 1]);
        sum += (uint64_t)abs((int)src[2] - (int)palette->display[index * 3 + 2]);
    }
    return (double)sum / ((double)pixels * 3.0);
}
