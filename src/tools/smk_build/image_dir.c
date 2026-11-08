#include "image_dir.h"

#include "image.h"
#include "wav.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

typedef struct {
    char **paths;
    unsigned count;
    unsigned width;
    unsigned height;
    unsigned padded_width;
    unsigned padded_height;
    uint8_t background[3];
    smk_pcm_audio audio;
    int has_audio;
} image_dir_state;

static smk_enc_status fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return SMK_ENC_ERR_INPUT;
}

static char *duplicate_string(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);
    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

static char *join_path(const char *dir, const char *name)
{
    size_t dir_length = strlen(dir);
    size_t name_length = strlen(name);
    int needs_slash = dir_length > 0 && dir[dir_length - 1] != '/' &&
                      dir[dir_length - 1] != '\\';
    char *path = (char *)malloc(dir_length + (size_t)needs_slash + name_length + 1);

    if (path == NULL)
        return NULL;
    memcpy(path, dir, dir_length);
    if (needs_slash)
        path[dir_length++] = '/';
    memcpy(path + dir_length, name, name_length + 1);
    return path;
}

static int has_image_extension(const char *name)
{
    const char *dot = strrchr(name, '.');

    if (dot == NULL)
        return 0;
    dot++;
    if (strlen(dot) != 3)
        return 0;
    if (tolower((unsigned char)dot[0]) == 't' &&
        tolower((unsigned char)dot[1]) == 'g' &&
        tolower((unsigned char)dot[2]) == 'a')
        return 1;
    if (tolower((unsigned char)dot[0]) == 'b' &&
        tolower((unsigned char)dot[1]) == 'm' &&
        tolower((unsigned char)dot[2]) == 'p')
        return 1;
    if (tolower((unsigned char)dot[0]) == 'p' &&
        tolower((unsigned char)dot[1]) == 'c' &&
        tolower((unsigned char)dot[2]) == 'x')
        return 1;
    return 0;
}

/* Compare strings with embedded decimal numbers, e.g. frame2 < frame10. */
static int natural_compare(const char *a, const char *b)
{
    size_t i = 0;
    size_t j = 0;

    while (a[i] != '\0' && b[j] != '\0') {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t a_start;
            size_t b_start;
            size_t a_end;
            size_t b_end;
            int cmp;

            while (a[i] == '0')
                i++;
            while (b[j] == '0')
                j++;
            a_start = i;
            b_start = j;
            while (isdigit((unsigned char)a[i]))
                i++;
            while (isdigit((unsigned char)b[j]))
                j++;
            a_end = i;
            b_end = j;
            if (a_end - a_start != b_end - b_start)
                return a_end - a_start < b_end - b_start ? -1 : 1;
            cmp = memcmp(a + a_start, b + b_start, a_end - a_start);
            if (cmp != 0)
                return cmp < 0 ? -1 : 1;
        } else {
            int ca = tolower((unsigned char)a[i]);
            int cb = tolower((unsigned char)b[j]);

            if (ca != cb)
                return ca < cb ? -1 : 1;
            i++;
            j++;
        }
    }
    if (a[i] != '\0')
        return 1;
    if (b[j] != '\0')
        return -1;
    return 0;
}

static int name_compare(const void *left, const void *right)
{
    const char *a = *(const char *const *)left;
    const char *b = *(const char *const *)right;
    int cmp = natural_compare(a, b);

    if (cmp != 0)
        return cmp;
    return strcmp(a, b);
}

static smk_enc_status collect_names(const char *dir, char ***out_names,
                                    unsigned *out_count, char *err,
                                    size_t errlen)
{
    char **names = NULL;
    unsigned count = 0;
    unsigned capacity = 0;

#if defined(_WIN32)
    {
        char pattern[4096];
        WIN32_FIND_DATAA entry;
        HANDLE handle;

        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        handle = FindFirstFileA(pattern, &entry);
        if (handle == INVALID_HANDLE_VALUE)
            return fail(err, errlen, "cannot open image directory");
        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (!has_image_extension(entry.cFileName))
                continue;
            if (count == capacity) {
                unsigned next = capacity == 0 ? 16 : capacity * 2;
                char **grown = (char **)realloc(names,
                                                (size_t)next * sizeof(char *));
                if (grown == NULL) {
                    FindClose(handle);
                    goto oom;
                }
                names = grown;
                capacity = next;
            }
            names[count] = duplicate_string(entry.cFileName);
            if (names[count] == NULL) {
                FindClose(handle);
                goto oom;
            }
            count++;
        } while (FindNextFileA(handle, &entry));
        FindClose(handle);
    }
#else
    {
        DIR *handle = opendir(dir);
        struct dirent *entry;

        if (handle == NULL)
            return fail(err, errlen, "cannot open image directory");
        while ((entry = readdir(handle)) != NULL) {
            struct stat info;
            char *full;

            if (entry->d_name[0] == '.' &&
                (entry->d_name[1] == '\0' ||
                 (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
                continue;
            if (!has_image_extension(entry->d_name))
                continue;
            full = join_path(dir, entry->d_name);
            if (full == NULL) {
                closedir(handle);
                goto oom;
            }
            if (stat(full, &info) != 0 || !S_ISREG(info.st_mode)) {
                free(full);
                continue;
            }
            free(full);
            if (count == capacity) {
                unsigned next = capacity == 0 ? 16 : capacity * 2;
                char **grown = (char **)realloc(names,
                                                (size_t)next * sizeof(char *));
                if (grown == NULL) {
                    closedir(handle);
                    goto oom;
                }
                names = grown;
                capacity = next;
            }
            names[count] = duplicate_string(entry->d_name);
            if (names[count] == NULL) {
                closedir(handle);
                goto oom;
            }
            count++;
        }
        closedir(handle);
    }
#endif

    if (count == 0) {
        free(names);
        return fail(err, errlen, "no TGA, BMP, or PCX images in directory");
    }
    qsort(names, count, sizeof(char *), name_compare);
    *out_names = names;
    *out_count = count;
    return SMK_ENC_OK;

oom:
    while (count > 0)
        free(names[--count]);
    free(names);
    return fail(err, errlen, "out of memory reading image directory");
}

static int image_dir_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    image_dir_state *state = (image_dir_state *)user;
    uint8_t *source = NULL;
    unsigned width = 0;
    unsigned height = 0;
    char message[256];
    unsigned y;

    if (state == NULL || rgb == NULL || index >= state->count)
        return -1;
    if (smk_image_load(state->paths[index], state->background, &source,
                       &width, &height, message, sizeof(message)) != SMK_ENC_OK)
        return -1;
    if (width != state->width || height != state->height) {
        free(source);
        return -1;
    }

    for (y = 0; y < state->padded_height; y++) {
        unsigned source_y = y < height ? y : height - 1;
        unsigned x;

        for (x = 0; x < state->padded_width; x++) {
            unsigned source_x = x < width ? x : width - 1;
            memcpy(rgb + ((size_t)y * state->padded_width + x) * 3,
                   source + ((size_t)source_y * width + source_x) * 3, 3);
        }
    }
    free(source);
    return 0;
}

static size_t image_dir_read_audio(void *user, unsigned track,
                                   size_t start_sample, size_t sample_count,
                                   void *dst)
{
    image_dir_state *state = (image_dir_state *)user;
    unsigned channels;
    unsigned bytes_per_sample;
    uint8_t *out = (uint8_t *)dst;
    size_t i;

    if (state == NULL || dst == NULL || !state->has_audio || track != 0)
        return 0;

    channels = state->audio.channels;
    bytes_per_sample = state->audio.bits / 8;
    for (i = 0; i < sample_count; i++) {
        size_t frame = start_sample + i;
        unsigned channel;

        for (channel = 0; channel < channels; channel++) {
            int16_t sample = frame < state->audio.sample_count
                ? state->audio.pcm[frame * channels + channel]
                : 0;

            if (bytes_per_sample == 1) {
                *out++ = (uint8_t)((sample >> 8) + 128);
            } else {
                *out++ = (uint8_t)((uint16_t)sample & 0xff);
                *out++ = (uint8_t)(((uint16_t)sample >> 8) & 0xff);
            }
        }
    }
    return sample_count * channels * bytes_per_sample;
}

smk_enc_status smk_image_dir_open(const char *path,
                                  const smk_enc_options *options,
                                  smk_enc_source *source,
                                  char *err, size_t errlen)
{
    image_dir_state *state;
    char **names = NULL;
    unsigned count = 0;
    unsigned width = 0;
    unsigned height = 0;
    uint8_t *probe = NULL;
    char message[256];
    unsigned i;

    if (path == NULL || options == NULL || source == NULL)
        return fail(err, errlen, "missing image directory argument");

    memset(source, 0, sizeof(*source));
    state = (image_dir_state *)calloc(1, sizeof(*state));
    if (state == NULL)
        return fail(err, errlen, "out of memory opening image directory");

    if (collect_names(path, &names, &count, err, errlen) != SMK_ENC_OK) {
        free(state);
        return SMK_ENC_ERR_INPUT;
    }

    state->paths = (char **)calloc(count, sizeof(char *));
    if (state->paths == NULL) {
        for (i = 0; i < count; i++)
            free(names[i]);
        free(names);
        free(state);
        return fail(err, errlen, "out of memory opening image directory");
    }
    for (i = 0; i < count; i++) {
        state->paths[i] = join_path(path, names[i]);
        free(names[i]);
        if (state->paths[i] == NULL) {
            unsigned k;

            for (k = 0; k < i; k++)
                free(state->paths[k]);
            free(state->paths);
            for (k = i + 1; k < count; k++)
                free(names[k]);
            free(names);
            free(state);
            return fail(err, errlen, "out of memory opening image directory");
        }
    }
    free(names);
    state->count = count;
    memcpy(state->background, options->background, 3);
    source->user = state;

    if (smk_image_load(state->paths[0], state->background, &probe, &width,
                       &height, message, sizeof(message)) != SMK_ENC_OK) {
        smk_image_dir_close(source);
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "%s", message);
        return SMK_ENC_ERR_INPUT;
    }
    free(probe);

    if (options->strict && (width % 4 != 0 || height % 4 != 0)) {
        smk_image_dir_close(source);
        if (err != NULL && errlen > 0)
            snprintf(err, errlen,
                     "image is %ux%u; dimensions must be multiples of 4",
                     width, height);
        return SMK_ENC_ERR_INPUT;
    }

    state->width = width;
    state->height = height;
    state->padded_width = (width + 3) / 4 * 4;
    state->padded_height = (height + 3) / 4 * 4;
    if (state->padded_width != width || state->padded_height != height)
        fprintf(stderr, "warning: padded %ux%u to %ux%u\n", width, height,
                state->padded_width, state->padded_height);

    source->width = state->padded_width;
    source->height = state->padded_height;
    source->frame_count = count;
    source->fps_num = options->fps_num != 0 ? options->fps_num : 15;
    source->fps_den = options->fps_den != 0 ? options->fps_den : 1;
    source->audio_track_count = 0;
    source->read_frame = image_dir_read_frame;
    source->read_audio = NULL;
    return SMK_ENC_OK;
}

smk_enc_status smk_image_dir_attach_audio(smk_enc_source *source,
                                          const char *wav_path,
                                          char *err, size_t errlen)
{
    image_dir_state *state;
    smk_pcm_audio audio;
    smk_enc_status status;

    if (source == NULL || source->user == NULL || wav_path == NULL)
        return fail(err, errlen, "missing image directory audio argument");

    state = (image_dir_state *)source->user;
    memset(&audio, 0, sizeof(audio));
    status = smk_wav_load(wav_path, &audio, err, errlen);
    if (status != SMK_ENC_OK)
        return status;

    free(state->audio.pcm);
    state->audio = audio;
    state->has_audio = 1;
    source->audio_track_count = 1;
    source->audio[0].rate = audio.rate;
    source->audio[0].channels = audio.channels;
    source->audio[0].bits = audio.bits;
    source->read_audio = image_dir_read_audio;
    return SMK_ENC_OK;
}

void smk_image_dir_close(smk_enc_source *source)
{
    image_dir_state *state;
    unsigned i;

    if (source == NULL)
        return;
    state = (image_dir_state *)source->user;
    if (state == NULL)
        return;
    for (i = 0; i < state->count; i++)
        free(state->paths[i]);
    free(state->paths);
    free(state->audio.pcm);
    free(state);
    source->user = NULL;
    source->read_frame = NULL;
    source->read_audio = NULL;
    source->audio_track_count = 0;
}
