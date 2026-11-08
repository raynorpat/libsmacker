#include "avi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVI_MAX_STREAMS 16
#define AVI_MAX_TRACKS 7
#define AVI_MAX_FRAME_BYTES ((size_t)1 << 30)

typedef struct {
    long offset;
    uint32_t size;
} avi_frame;

typedef struct {
    long offset;
    uint32_t size;
    uint64_t start_byte;
} avi_segment;

typedef struct {
    unsigned stream_id;
    unsigned channels;
    unsigned rate;
    unsigned bits;
    unsigned block_align;
    uint64_t total_bytes;
    avi_segment *segments;
    unsigned segment_count;
    unsigned segment_capacity;
} avi_audio_track;

typedef struct {
    FILE *fp;
    unsigned width;
    unsigned height;
    unsigned padded_width;
    unsigned padded_height;
    unsigned bpp;
    unsigned stride;
    int bottom_up;
    int tight_rows;
    avi_frame *frames;
    unsigned frame_count;
    unsigned frame_capacity;
    avi_audio_track tracks[AVI_MAX_TRACKS];
    unsigned track_count;
    uint8_t *scratch;
    size_t scratch_size;
} avi_state;

typedef struct {
    char type[5];
    char handler[5];
    unsigned scale;
    unsigned rate;
    unsigned width;
    unsigned height;
    unsigned bpp;
    unsigned compression;
    unsigned format_tag;
    unsigned channels;
    unsigned audio_rate;
    unsigned block_align;
    unsigned bits;
    int have_format;
} avi_stream_info;

typedef struct {
    avi_stream_info streams[AVI_MAX_STREAMS];
    unsigned stream_count;
    unsigned avih_usf;
    unsigned avih_frames;
    unsigned avih_streams;
    unsigned avih_width;
    unsigned avih_height;
    int have_avih;
    int video_stream;
    int stream_to_track[100];
} avi_header;

static smk_enc_status fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return SMK_ENC_ERR_INPUT;
}

static int read_exact(FILE *fp, void *dst, size_t size)
{
    return size == 0 || fread(dst, 1, size, fp) == size ? 0 : -1;
}

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t load_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

static int stream_index_from_id(const char *id)
{
    if (!is_digit_char(id[0]) || !is_digit_char(id[1]))
        return -1;
    return (id[0] - '0') * 10 + (id[1] - '0');
}

static int read_chunk_header(FILE *fp, char id[5], uint32_t *size)
{
    uint8_t raw[8];

    if (read_exact(fp, raw, sizeof(raw)) != 0)
        return -1;
    memcpy(id, raw, 4);
    id[4] = '\0';
    *size = load_u32(raw + 4);
    return 0;
}

static int seek_to(FILE *fp, long position)
{
    return fseek(fp, position, SEEK_SET) == 0 ? 0 : -1;
}

static unsigned gcd_unsigned(unsigned a, unsigned b)
{
    while (b != 0) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void set_rate(unsigned num, unsigned den, smk_enc_source *source)
{
    unsigned divisor;

    if (num == 0 || den == 0) {
        source->fps_num = 15;
        source->fps_den = 1;
        return;
    }
    divisor = gcd_unsigned(num, den);
    source->fps_num = num / divisor;
    source->fps_den = den / divisor;
}

static smk_enc_status parse_strl_chunks(FILE *fp, long end,
                                        avi_header *header, char *err,
                                        size_t errlen)
{
    avi_stream_info info;

    memset(&info, 0, sizeof(info));
    while (ftell(fp) < end) {
        char id[5];
        uint32_t size;
        long data_start;

        if (read_chunk_header(fp, id, &size) != 0)
            return fail(err, errlen, "truncated AVI stream header");
        data_start = ftell(fp);
        if (data_start < 0)
            return fail(err, errlen, "truncated AVI stream header");

        if (strcmp(id, "strh") == 0) {
            uint8_t raw[64];
            size_t want = size < sizeof(raw) ? size : sizeof(raw);

            if (size < 20 || want < 20)
                return fail(err, errlen, "AVI stream header is too short");
            if (read_exact(fp, raw, want) != 0)
                return fail(err, errlen, "truncated AVI stream header");
            memcpy(info.type, raw, 4);
            info.type[4] = '\0';
            memcpy(info.handler, raw + 4, 4);
            info.handler[4] = '\0';
            info.scale = load_u32(raw + 20);
            info.rate = load_u32(raw + 24);
        } else if (strcmp(id, "strf") == 0) {
            uint8_t raw[64];
            size_t want = size < sizeof(raw) ? size : sizeof(raw);

            if (read_exact(fp, raw, want) != 0)
                return fail(err, errlen, "truncated AVI stream format");
            info.have_format = 1;
            if (strcmp(info.type, "vids") == 0) {
                if (want < 40)
                    return fail(err, errlen, "AVI video format is too short");
                info.width = load_u32(raw + 4);
                info.height = load_u32(raw + 8);
                info.bpp = load_u16(raw + 14);
                info.compression = load_u32(raw + 16);
            } else if (strcmp(info.type, "auds") == 0) {
                if (want < 16)
                    return fail(err, errlen, "AVI audio format is too short");
                info.format_tag = load_u16(raw);
                info.channels = load_u16(raw + 2);
                info.audio_rate = load_u32(raw + 4);
                info.block_align = load_u16(raw + 12);
                info.bits = load_u16(raw + 14);
            }
        }

        if (seek_to(fp, data_start + (long)size + (long)(size & 1u)) != 0)
            return fail(err, errlen, "truncated AVI stream chunk");
    }

    if (info.type[0] != '\0' && header->stream_count < AVI_MAX_STREAMS)
        header->streams[header->stream_count++] = info;
    return SMK_ENC_OK;
}

static smk_enc_status parse_hdrl(FILE *fp, long end, avi_header *header,
                                 char *err, size_t errlen)
{
    while (ftell(fp) < end) {
        char id[5];
        uint32_t size;
        long data_start;

        if (read_chunk_header(fp, id, &size) != 0)
            return fail(err, errlen, "truncated AVI header list");
        data_start = ftell(fp);
        if (data_start < 0)
            return fail(err, errlen, "truncated AVI header list");

        if (strcmp(id, "avih") == 0) {
            uint8_t raw[56];
            size_t want = size < sizeof(raw) ? size : sizeof(raw);

            if (size < 40 || want < 40)
                return fail(err, errlen, "AVI main header is too short");
            if (read_exact(fp, raw, want) != 0)
                return fail(err, errlen, "truncated AVI main header");
            header->avih_usf = load_u32(raw);
            header->avih_frames = load_u32(raw + 16);
            header->avih_streams = load_u32(raw + 24);
            header->avih_width = load_u32(raw + 32);
            header->avih_height = load_u32(raw + 36);
            header->have_avih = 1;
        } else if (strcmp(id, "LIST") == 0) {
            uint8_t type_raw[4];

            if (size < 4 || read_exact(fp, type_raw, 4) != 0)
                return fail(err, errlen, "truncated AVI header list");
            if (memcmp(type_raw, "strl", 4) == 0) {
                smk_enc_status status = parse_strl_chunks(
                    fp, data_start + (long)size, header, err, errlen);
                if (status != SMK_ENC_OK)
                    return status;
            }
        }

        if (seek_to(fp, data_start + (long)size + (long)(size & 1u)) != 0)
            return fail(err, errlen, "truncated AVI header list");
    }
    return SMK_ENC_OK;
}

static smk_enc_status append_frame(avi_state *state, long offset,
                                   uint32_t size, char *err, size_t errlen)
{
    if (state->frame_count == state->frame_capacity) {
        unsigned next = state->frame_capacity == 0 ? 64
                                                   : state->frame_capacity * 2;
        avi_frame *grown = (avi_frame *)realloc(
            state->frames, (size_t)next * sizeof(avi_frame));
        if (grown == NULL)
            return fail(err, errlen, "out of memory reading AVI frames");
        state->frames = grown;
        state->frame_capacity = next;
    }
    state->frames[state->frame_count].offset = offset;
    state->frames[state->frame_count].size = size;
    state->frame_count++;
    return SMK_ENC_OK;
}

static smk_enc_status append_segment(avi_audio_track *track, long offset,
                                     uint32_t size, char *err, size_t errlen)
{
    if (track->segment_count == track->segment_capacity) {
        unsigned next = track->segment_capacity == 0
            ? 16 : track->segment_capacity * 2;
        avi_segment *grown = (avi_segment *)realloc(
            track->segments, (size_t)next * sizeof(avi_segment));
        if (grown == NULL)
            return fail(err, errlen, "out of memory reading AVI audio");
        track->segments = grown;
        track->segment_capacity = next;
    }
    track->segments[track->segment_count].offset = offset;
    track->segments[track->segment_count].size = size;
    track->segments[track->segment_count].start_byte = track->total_bytes;
    track->total_bytes += size;
    track->segment_count++;
    return SMK_ENC_OK;
}

static smk_enc_status parse_movi(avi_state *state, avi_header *header,
                                 FILE *fp, long end, char *err,
                                 size_t errlen)
{
    while (ftell(fp) < end) {
        char id[5];
        uint32_t size;
        long data_start;

        if (read_chunk_header(fp, id, &size) != 0)
            return fail(err, errlen, "truncated AVI movie data");
        data_start = ftell(fp);
        if (data_start < 0)
            return fail(err, errlen, "truncated AVI movie data");

        if (strcmp(id, "LIST") == 0) {
            uint8_t type_raw[4];

            if (size < 4 || read_exact(fp, type_raw, 4) != 0)
                return fail(err, errlen, "truncated AVI movie list");
            if (memcmp(type_raw, "rec ", 4) == 0) {
                smk_enc_status status = parse_movi(
                    state, header, fp, data_start + (long)size, err,
                    errlen);
                if (status != SMK_ENC_OK)
                    return status;
            }
        } else {
            int stream = stream_index_from_id(id);

            if (stream >= 0 && id[2] == 'd' &&
                (id[3] == 'b' || id[3] == 'c')) {
                if (stream != header->video_stream)
                    return fail(err, errlen,
                                "AVI movie has an unknown video stream");
                if (id[3] == 'c')
                    return fail(err, errlen,
                                "compressed AVI video is not supported");
                if (append_frame(state, data_start, size, err, errlen) !=
                    SMK_ENC_OK)
                    return SMK_ENC_ERR_INPUT;
            } else if (stream >= 0 && id[2] == 'w' && id[3] == 'b') {
                if (stream < 100 && header->stream_to_track[stream] >= 0) {
                    avi_audio_track *track =
                        &state->tracks[header->stream_to_track[stream]];
                    if (append_segment(track, data_start, size, err,
                                       errlen) != SMK_ENC_OK)
                        return SMK_ENC_ERR_INPUT;
                }
            }
        }

        if (seek_to(fp, data_start + (long)size + (long)(size & 1u)) != 0)
            return fail(err, errlen, "truncated AVI movie data");
    }
    return SMK_ENC_OK;
}

static smk_enc_status finish_header(avi_header *header,
                                     const smk_enc_options *options,
                                     avi_state *state, smk_enc_source *source,
                                     char *err, size_t errlen)
{
    unsigned i;
    int video = -1;

    if (!header->have_avih)
        return fail(err, errlen, "AVI has no main header");

    for (i = 0; i < header->stream_count; i++) {
        if (strcmp(header->streams[i].type, "vids") == 0) {
            if (video >= 0)
                return fail(err, errlen, "AVI has more than one video stream");
            video = (int)i;
        }
    }
    if (video < 0)
        return fail(err, errlen, "AVI has no video stream");
    header->video_stream = video;

    {
        const avi_stream_info *v = &header->streams[video];

        if (!v->have_format)
            return fail(err, errlen, "AVI video stream has no format");
        if (strcmp(v->handler, "DIB ") != 0 || v->compression != 0)
            return fail(err, errlen, "unsupported AVI video codec");
        if (v->bpp != 24 && v->bpp != 32)
            return fail(err, errlen, "unsupported AVI video depth");
        if (v->width == 0 || v->height == 0 ||
            v->width > 65535 || v->height > 65535)
            return fail(err, errlen, "AVI video has invalid dimensions");
        if ((size_t)v->width * v->height >
            ((size_t)1 << 28))
            return fail(err, errlen, "AVI video is too large");

        state->width = v->width;
        state->height = v->height;
        state->bpp = v->bpp;
        state->bottom_up = (int)v->height > 0;
        state->stride = ((v->width * (v->bpp / 8) + 3) / 4) * 4;
        if (v->scale != 0 && v->rate != 0)
            set_rate(v->rate, v->scale, source);
        else if (header->avih_usf != 0)
            set_rate(1000000u, header->avih_usf, source);
        else
            set_rate(15, 1, source);
    }

    if (options->strict &&
        (state->width % 4 != 0 || state->height % 4 != 0))
        return fail(err, errlen,
                    "AVI dimensions must be multiples of 4");
    state->padded_width = (state->width + 3) / 4 * 4;
    state->padded_height = (state->height + 3) / 4 * 4;
    if (state->padded_width != state->width ||
        state->padded_height != state->height)
        fprintf(stderr, "warning: padded %ux%u to %ux%u\n", state->width,
                state->height, state->padded_width, state->padded_height);

    for (i = 0; i < 100; i++)
        header->stream_to_track[i] = -1;

    for (i = 0; i < header->stream_count; i++) {
        const avi_stream_info *stream = &header->streams[i];
        avi_audio_track *track;

        if (strcmp(stream->type, "auds") != 0)
            continue;
        if (state->track_count >= AVI_MAX_TRACKS)
            return fail(err, errlen, "AVI has more than seven audio tracks");
        if (!stream->have_format || stream->format_tag != 1)
            return fail(err, errlen,
                        "compressed AVI audio is not supported");
        if (stream->channels != 1 && stream->channels != 2)
            return fail(err, errlen, "AVI audio must be mono or stereo");
        if (stream->bits != 8 && stream->bits != 16)
            return fail(err, errlen, "unsupported AVI audio depth");
        if (stream->audio_rate == 0)
            return fail(err, errlen, "AVI audio rate is zero");

        track = &state->tracks[state->track_count];
        track->stream_id = i;
        track->channels = stream->channels;
        track->rate = stream->audio_rate;
        track->bits = stream->bits;
        track->block_align = stream->channels * (stream->bits / 8);
        if (stream->block_align != 0 &&
            stream->block_align != track->block_align)
            track->block_align = stream->block_align;
        header->stream_to_track[i] = (int)state->track_count;
        state->track_count++;

        source->audio[state->track_count - 1].rate = track->rate;
        source->audio[state->track_count - 1].channels = track->channels;
        source->audio[state->track_count - 1].bits = track->bits;
    }
    source->audio_track_count = state->track_count;
    source->width = state->padded_width;
    source->height = state->padded_height;
    source->audio_track_count = state->track_count;
    return SMK_ENC_OK;
}

static int avi_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    avi_state *state = (avi_state *)user;
    uint32_t size;
    size_t row_bytes;
    unsigned x;
    unsigned y;
    unsigned bytes;

    if (state == NULL || rgb == NULL || index >= state->frame_count)
        return -1;
    size = state->frames[index].size;
    if (size > AVI_MAX_FRAME_BYTES)
        return -1;
    if ((size_t)size > state->scratch_size) {
        uint8_t *grown = (uint8_t *)realloc(state->scratch, size);
        if (grown == NULL)
            return -1;
        state->scratch = grown;
        state->scratch_size = size;
    }
    if (seek_to(state->fp, state->frames[index].offset) != 0 ||
        read_exact(state->fp, state->scratch, size) != 0)
        return -1;

    bytes = state->bpp / 8;
    row_bytes = state->tight_rows
        ? (size_t)state->width * bytes
        : state->stride;

    for (y = 0; y < state->padded_height; y++) {
        unsigned source_y = y < state->height ? y : state->height - 1;
        unsigned data_y = state->bottom_up
            ? state->height - 1 - source_y : source_y;
        const uint8_t *row = state->scratch + (size_t)data_y * row_bytes;

        for (x = 0; x < state->padded_width; x++) {
            unsigned source_x = x < state->width ? x : state->width - 1;
            const uint8_t *src = row + (size_t)source_x * bytes;
            uint8_t *out = rgb + ((size_t)y * state->padded_width + x) * 3;

            out[0] = src[2];
            out[1] = src[1];
            out[2] = src[0];
        }
    }
    return 0;
}

static size_t avi_read_audio(void *user, unsigned track_index,
                             size_t start_sample, size_t sample_count,
                             void *dst)
{
    avi_state *state = (avi_state *)user;
    avi_audio_track *track;
    uint64_t start_byte;
    uint64_t end_byte;
    size_t needed;
    uint8_t *out = (uint8_t *)dst;
    unsigned i;

    if (state == NULL || dst == NULL || track_index >= state->track_count)
        return 0;
    track = &state->tracks[track_index];
    needed = sample_count * track->block_align;
    start_byte = (uint64_t)start_sample * track->block_align;
    end_byte = start_byte + needed;

    memset(out, track->bits == 8 ? 0x80 : 0, needed);

    for (i = 0; i < track->segment_count; i++) {
        avi_segment *segment = &track->segments[i];
        uint64_t segment_start = segment->start_byte;
        uint64_t segment_end = segment_start + segment->size;
        uint64_t from = start_byte > segment_start ? start_byte : segment_start;
        uint64_t to = end_byte < segment_end ? end_byte : segment_end;

        if (from >= to)
            continue;
        if (seek_to(state->fp, segment->offset + (long)(from - segment_start))
            != 0)
            return 0;
        if (read_exact(state->fp, out + (size_t)(from - start_byte),
                       (size_t)(to - from)) != 0)
            return 0;
    }
    return needed;
}

static void free_state(avi_state *state)
{
    unsigned i;

    if (state == NULL)
        return;
    if (state->fp != NULL)
        fclose(state->fp);
    free(state->frames);
    for (i = 0; i < state->track_count; i++)
        free(state->tracks[i].segments);
    free(state->scratch);
    free(state);
}

smk_enc_status smk_avi_open(const char *path,
                            const smk_enc_options *options,
                            smk_enc_source *source,
                            char *err, size_t errlen)
{
    avi_state *state;
    avi_header header;
    smk_enc_status status = SMK_ENC_ERR_INPUT;
    uint8_t riff[12];
    long end;
    int have_movi = 0;

    if (path == NULL || options == NULL || source == NULL)
        return fail(err, errlen, "missing AVI argument");

    memset(source, 0, sizeof(*source));
    memset(&header, 0, sizeof(header));
    header.video_stream = -1;
    state = (avi_state *)calloc(1, sizeof(*state));
    if (state == NULL)
        return fail(err, errlen, "out of memory opening AVI");

    state->fp = fopen(path, "rb");
    if (state->fp == NULL) {
        free_state(state);
        if (err != NULL && errlen > 0)
            snprintf(err, errlen, "cannot open AVI file: %s", path);
        return SMK_ENC_ERR_INPUT;
    }

    if (read_exact(state->fp, riff, sizeof(riff)) != 0 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "AVI ", 4) != 0) {
        status = fail(err, errlen, "not a RIFF AVI file");
        goto done;
    }
    if (fseek(state->fp, 0, SEEK_END) != 0) {
        status = fail(err, errlen, "cannot seek AVI file");
        goto done;
    }
    end = ftell(state->fp);
    if (end < 12 || fseek(state->fp, 12, SEEK_SET) != 0) {
        status = fail(err, errlen, "truncated AVI file");
        goto done;
    }

    while (ftell(state->fp) < end) {
        char id[5];
        uint32_t size;
        long data_start;

        if (read_chunk_header(state->fp, id, &size) != 0)
            break;
        data_start = ftell(state->fp);
        if (data_start < 0 ||
            (long)size > end - data_start) {
            status = fail(err, errlen, "truncated AVI chunk");
            goto done;
        }

        if (strcmp(id, "LIST") == 0) {
            uint8_t type_raw[4];

            if (size < 4 || read_exact(state->fp, type_raw, 4) != 0) {
                status = fail(err, errlen, "truncated AVI list");
                goto done;
            }
            if (memcmp(type_raw, "hdrl", 4) == 0) {
                if (header.stream_count != 0 || header.have_avih) {
                    status = fail(err, errlen, "AVI has two header lists");
                    goto done;
                }
                status = parse_hdrl(state->fp,
                                    data_start + (long)size, &header, err,
                                    errlen);
                if (status != SMK_ENC_OK)
                    goto done;
                status = finish_header(&header, options, state, source, err,
                                       errlen);
                if (status != SMK_ENC_OK)
                    goto done;
            } else if (memcmp(type_raw, "movi", 4) == 0) {
                if (header.stream_count == 0) {
                    status = fail(err, errlen,
                                  "AVI movie data precedes its header");
                    goto done;
                }
                status = parse_movi(state, &header, state->fp,
                                    data_start + (long)size, err, errlen);
                if (status != SMK_ENC_OK)
                    goto done;
                have_movi = 1;
            }
        }

        if (seek_to(state->fp, data_start + (long)size +
                    (long)(size & 1u)) != 0) {
            status = fail(err, errlen, "truncated AVI chunk");
            goto done;
        }
    }

    if (header.stream_count == 0) {
        status = fail(err, errlen, "AVI has no header list");
        goto done;
    }
    if (!have_movi) {
        status = fail(err, errlen, "AVI has no movie data");
        goto done;
    }
    if (state->frame_count == 0) {
        status = fail(err, errlen, "AVI has no video frames");
        goto done;
    }

    {
        size_t tight_size = (size_t)state->width * state->height *
                            (state->bpp / 8);
        size_t padded_size = (size_t)state->stride * state->height;
        unsigned i;
        int saw_tight = 0;
        int saw_padded = 0;

        for (i = 0; i < state->frame_count; i++) {
            uint32_t size = state->frames[i].size;

            if (size == padded_size && !saw_tight)
                saw_padded = 1;
            else if (size == tight_size && !saw_padded)
                saw_tight = 1;
            else {
                status = fail(err, errlen, "AVI frame has an invalid size");
                goto done;
            }
        }
        state->tight_rows = saw_tight;
    }

    source->user = state;
    source->frame_count = state->frame_count;
    source->read_frame = avi_read_frame;
    source->read_audio = state->track_count > 0 ? avi_read_audio : NULL;
    source->audio_track_count = state->track_count;
    return SMK_ENC_OK;

done:
    free_state(state);
    memset(source, 0, sizeof(*source));
    return status;
}

void smk_avi_close(smk_enc_source *source)
{
    avi_state *state;

    if (source == NULL)
        return;
    state = (avi_state *)source->user;
    if (state == NULL)
        return;
    free_state(state);
    source->user = NULL;
    source->read_frame = NULL;
    source->read_audio = NULL;
    source->audio_track_count = 0;
}
