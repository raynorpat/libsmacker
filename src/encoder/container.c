#include "container.h"

#include "format/header.h"

#include <stdlib.h>
#include <string.h>

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

    smk_header_write_u32(bytes, value);
    return write_bytes(fp, bytes, sizeof(bytes));
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

    {
        smk_file_header header;
        uint8_t header_bytes[SMK_HEADER_SIZE];

        memset(&header, 0, sizeof(header));
        memcpy(header.signature, "SMK2", 4);
        header.width = info->width;
        header.height = info->height;
        header.frames = info->frames;
        header.frame_rate = smk_header_frame_rate(info->fps_num, info->fps_den);
        header.flags = info->ring ? SMK_HEADER_FLAG_RING : 0u;
        for (i = 0; i < SMK_MAX_TRACKS; i++) {
            header.audio_size[i] = info->audio_size[i];
            header.audio_rate[i] = info->audio_rate[i];
        }
        header.trees_size = (uint32_t)info->trees_size;
        for (i = 0; i < 4; i++)
            header.tree_size[i] = info->tree_sizes[i];
        if (smk_header_pack(&header, header_bytes) != 0 ||
            write_bytes(container->file, header_bytes, SMK_HEADER_SIZE) != 0)
            goto write_error;
    }

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
        smk_header_pack_frame_size((uint32_t)padded, keyframe);
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
