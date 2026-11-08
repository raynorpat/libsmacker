#include "container.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define SMK_HEADER_SIZE 104
#define SMK_MAX_TRACKS SMK_MAX_AUDIO_TRACKS

static int fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return -1;
}

static int write_bytes(FILE *fp, const void *data, size_t size)
{
    if (size == 0)
        return 0;
    return fwrite(data, 1, size, fp) == size ? 0 : -1;
}

static int write_u32(FILE *fp, uint32_t value)
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
    return write_bytes(fp, bytes, sizeof(bytes));
}

/*
 * The frame rate field holds either milliseconds per frame (positive) or
 * units of 10 microseconds per frame (negative). Pick whichever is closer.
 */
static int32_t frame_rate_field(unsigned num, unsigned den)
{
    double us;
    long ms;
    long tens;
    double ms_error;
    double tens_error;

    if (num == 0 || den == 0)
        return 0;
    us = 1000000.0 * (double)den / (double)num;

    ms = (long)(us / 1000.0 + 0.5);
    tens = (long)(us / 10.0 + 0.5);
    if (tens < 1)
        tens = 1;
    ms_error = (double)ms * 1000.0 - us;
    tens_error = (double)tens * 10.0 - us;
    if (ms_error < 0.0)
        ms_error = -ms_error;
    if (tens_error < 0.0)
        tens_error = -tens_error;

    if (ms >= 1 && ms <= INT32_MAX &&
        (tens > INT32_MAX || ms_error <= tens_error))
        return (int32_t)ms;
    if (tens <= INT32_MAX)
        return (int32_t)-tens;
    return 0;
}

static void discard(smk_container *container)
{
    if (container == NULL)
        return;
    if (container->file != NULL) {
        fclose(container->file);
        container->file = NULL;
    }
    if (container->path != NULL)
        remove(container->path);
    free(container->path);
    free(container->sizes);
    free(container->types);
    container->path = NULL;
    container->sizes = NULL;
    container->types = NULL;
    container->open = 0;
}

int smk_container_open(smk_container *container,
                       const char *path,
                       const smk_container_info *info,
                       char *err, size_t errlen)
{
    unsigned physical;
    unsigned i;

    if (container == NULL || path == NULL || info == NULL ||
        path[0] == '\0' || info->frames == 0)
        return fail(err, errlen, "invalid container arguments");

    memset(container, 0, sizeof(*container));

    if (info->frames > 0x3fffffffu)
        return fail(err, errlen, "too many frames");
    physical = info->frames + (info->ring ? 1u : 0u);

    container->path = (char *)malloc(strlen(path) + 1);
    container->sizes = (uint32_t *)calloc(physical, sizeof(uint32_t));
    container->types = (uint8_t *)calloc(physical, sizeof(uint8_t));
    if (container->path == NULL || container->sizes == NULL ||
        container->types == NULL) {
        discard(container);
        return fail(err, errlen, "out of memory creating container");
    }
    memcpy(container->path, path, strlen(path) + 1);
    container->physical_frames = physical;
    container->size_table_offset = SMK_HEADER_SIZE;
    container->type_table_offset = SMK_HEADER_SIZE + (long)physical * 4;

    container->file = fopen(path, "wb");
    if (container->file == NULL) {
        discard(container);
        return fail(err, errlen, "cannot create output file");
    }
    container->open = 1;

    if (write_bytes(container->file, "SMK2", 4) != 0 ||
        write_u32(container->file, info->width) != 0 ||
        write_u32(container->file, info->height) != 0 ||
        write_u32(container->file, info->frames) != 0 ||
        write_u32(container->file,
                  (uint32_t)frame_rate_field(info->fps_num,
                                             info->fps_den)) != 0 ||
        write_u32(container->file, info->ring ? 1u : 0u) != 0)
        goto write_error;

    for (i = 0; i < SMK_MAX_TRACKS; i++) {
        if (write_u32(container->file, info->audio_size[i]) != 0)
            goto write_error;
    }

    if (write_u32(container->file, (uint32_t)info->trees_size) != 0)
        goto write_error;
    for (i = 0; i < 4; i++) {
        if (write_u32(container->file, info->tree_sizes[i]) != 0)
            goto write_error;
    }
    for (i = 0; i < SMK_MAX_TRACKS; i++) {
        if (write_u32(container->file, info->audio_rate[i]) != 0)
            goto write_error;
    }
    if (write_u32(container->file, 0) != 0)
        goto write_error;

    if (write_bytes(container->file, container->sizes,
                    (size_t)physical * sizeof(uint32_t)) != 0 ||
        write_bytes(container->file, container->types, (size_t)physical) != 0)
        goto write_error;

    if (write_bytes(container->file, info->trees, info->trees_size) != 0)
        goto write_error;

    if (fflush(container->file) != 0 || ferror(container->file))
        goto write_error;
    return 0;

write_error:
    discard(container);
    return fail(err, errlen, "failed to write output file");
}

int smk_container_add_frame(smk_container *container, int keyframe,
                            unsigned type, const uint8_t *payload,
                            size_t size, char *err, size_t errlen)
{
    static const uint8_t padding[3] = { 0, 0, 0 };
    size_t padded;
    unsigned pad;

    if (container == NULL || !container->open || container->file == NULL)
        return fail(err, errlen, "container is not open");
    if (container->next_frame >= container->physical_frames)
        return fail(err, errlen, "too many frames for container");
    if (size > 0xfffffffcu)
        return fail(err, errlen, "frame is too large");
    if (size > 0 && payload == NULL)
        return fail(err, errlen, "missing frame payload");

    pad = (unsigned)((4 - (size & 3u)) & 3u);
    padded = size + pad;

    if (write_bytes(container->file, payload, size) != 0 ||
        write_bytes(container->file, padding, pad) != 0)
        return fail(err, errlen, "failed to write frame");

    container->sizes[container->next_frame] =
        ((uint32_t)padded & 0xfffffffcu) | (keyframe ? 1u : 0u);
    container->types[container->next_frame] = (uint8_t)(type & 0xffu);
    container->next_frame++;
    return 0;
}

int smk_container_finish(smk_container *container, char *err, size_t errlen)
{
    unsigned i;

    if (container == NULL || !container->open || container->file == NULL)
        return fail(err, errlen, "container is not open");
    if (container->next_frame != container->physical_frames) {
        discard(container);
        return fail(err, errlen, "not all frames were written");
    }

    if (fseek(container->file, container->size_table_offset, SEEK_SET) != 0)
        goto write_error;
    for (i = 0; i < container->physical_frames; i++) {
        if (write_u32(container->file, container->sizes[i]) != 0)
            goto write_error;
    }

    if (fseek(container->file, container->type_table_offset, SEEK_SET) != 0)
        goto write_error;
    if (write_bytes(container->file, container->types,
                    (size_t)container->physical_frames) != 0)
        goto write_error;

    if (fflush(container->file) != 0 || ferror(container->file))
        goto write_error;
    if (fclose(container->file) != 0) {
        container->file = NULL;
        discard(container);
        return fail(err, errlen, "failed to close output file");
    }
    container->file = NULL;
    container->open = 0;
    free(container->path);
    free(container->sizes);
    free(container->types);
    container->path = NULL;
    container->sizes = NULL;
    container->types = NULL;
    return 0;

write_error:
    discard(container);
    return fail(err, errlen, "failed to write frame tables");
}

void smk_container_abort(smk_container *container)
{
    discard(container);
}
