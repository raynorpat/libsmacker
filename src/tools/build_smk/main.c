#include "smk_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUILD_SMK_VERSION "1.0.0"

static void usage(FILE *out, const char *program)
{
    fprintf(out,
        "usage: %s <input> [-o output.smk] [options]\n"
        "\n"
        "  <input>                 image directory or .avi file\n"
        "  -o, --output FILE       output .smk (default: <input>.smk)\n"
        "      --audio FILE.wav    audio for image-directory input\n"
        "      --fps N[/D]         frame rate (default 15; AVI uses its own)\n"
        "      --palette MODE      adaptive|global|frame (default adaptive)\n"
        "      --dither MODE       none|ordered|fs (default none)\n"
        "      --colors N          palette size 2..256 (default 256)\n"
        "      --keyframe-interval N  frames between keyframes (default 150)\n"
        "      --ring              add a seamless loop ring frame\n"
        "      --background RRGGBB alpha composite color (default 000000)\n"
        "      --pad               pad dimensions to a multiple of 4 (default)\n"
        "      --strict            error on dimensions not divisible by 4\n"
        "      --verify            decode the result and compare it\n"
        "      --verbose           print progress\n"
        "  -h, --help              show this help\n"
        "      --version           show the version\n",
        program);
}

static int parse_fps(const char *text, unsigned *num, unsigned *den)
{
    char *end;
    unsigned long n;
    unsigned long d = 1;

    n = strtoul(text, &end, 10);
    if (end == text || n == 0 || n > 1000000UL)
        return -1;
    if (*end == '/') {
        const char *start = end + 1;
        d = strtoul(start, &end, 10);
        if (end == start || d == 0 || d > 1000000UL)
            return -1;
    }
    if (*end != '\0')
        return -1;
    *num = (unsigned)n;
    *den = (unsigned)d;
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_color(const char *text, uint8_t out[3])
{
    int i;

    if (strlen(text) != 6)
        return -1;
    for (i = 0; i < 3; i++) {
        int hi = hex_value(text[i * 2]);
        int lo = hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int parse_uint(const char *text, unsigned *out)
{
    char *end;
    unsigned long value = strtoul(text, &end, 10);

    if (end == text || *end != '\0' || value > 0xffffffffUL)
        return -1;
    *out = (unsigned)value;
    return 0;
}

static void derive_output(const char *input, char *out, size_t outlen)
{
    const char *base;
    const char *dot;
    size_t length;
    size_t i;

    base = input;
    for (i = 0; input[i] != '\0'; i++) {
        if (input[i] == '/' || input[i] == '\\')
            base = input + i + 1;
    }
    if (*base == '\0')
        base = "out";

    dot = strrchr(base, '.');
    length = dot != NULL && dot != base ? (size_t)(dot - base) : strlen(base);
    if (length > outlen - 5)
        length = outlen - 5;
    memcpy(out, base, length);
    memcpy(out + length, ".smk", 5);
}

static int usage_error(const char *program, const char *message)
{
    fprintf(stderr, "build_smk: %s\n", message);
    usage(stderr, program);
    return SMK_ENC_ERR_USAGE;
}

int main(int argc, char **argv)
{
    smk_enc_options options;
    const char *input = NULL;
    const char *output = NULL;
    const char *audio = NULL;
    char output_buffer[4096];
    int i;

    smk_enc_options_default(&options);
    options.ring = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout, argv[0]);
            return SMK_ENC_OK;
        }
        if (strcmp(arg, "--version") == 0) {
            printf("build_smk %s\n", BUILD_SMK_VERSION);
            return SMK_ENC_OK;
        }
        if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (++i >= argc)
                return usage_error(argv[0], "--output needs a value");
            output = argv[i];
        } else if (strcmp(arg, "--audio") == 0) {
            if (++i >= argc)
                return usage_error(argv[0], "--audio needs a value");
            audio = argv[i];
        } else if (strcmp(arg, "--fps") == 0) {
            if (++i >= argc || parse_fps(argv[i], &options.fps_num,
                                         &options.fps_den) != 0)
                return usage_error(argv[0], "--fps expects N or N/D");
        } else if (strcmp(arg, "--palette") == 0) {
            if (++i >= argc)
                return usage_error(argv[0], "--palette needs a value");
            if (strcmp(argv[i], "adaptive") == 0)
                options.palette = SMK_ENC_PALETTE_ADAPTIVE;
            else if (strcmp(argv[i], "global") == 0)
                options.palette = SMK_ENC_PALETTE_GLOBAL;
            else if (strcmp(argv[i], "frame") == 0)
                options.palette = SMK_ENC_PALETTE_FRAME;
            else
                return usage_error(argv[0], "unknown palette mode");
        } else if (strcmp(arg, "--dither") == 0) {
            if (++i >= argc)
                return usage_error(argv[0], "--dither needs a value");
            if (strcmp(argv[i], "none") == 0)
                options.dither = SMK_ENC_DITHER_NONE;
            else if (strcmp(argv[i], "ordered") == 0)
                options.dither = SMK_ENC_DITHER_ORDERED;
            else if (strcmp(argv[i], "fs") == 0)
                options.dither = SMK_ENC_DITHER_FS;
            else
                return usage_error(argv[0], "unknown dither mode");
        } else if (strcmp(arg, "--colors") == 0) {
            if (++i >= argc || parse_uint(argv[i], &options.colors) != 0 ||
                options.colors < 2 || options.colors > 256)
                return usage_error(argv[0], "--colors expects 2..256");
        } else if (strcmp(arg, "--keyframe-interval") == 0) {
            if (++i >= argc ||
                parse_uint(argv[i], &options.keyframe_interval) != 0)
                return usage_error(argv[0],
                                   "--keyframe-interval expects a number");
        } else if (strcmp(arg, "--ring") == 0) {
            options.ring = 1;
        } else if (strcmp(arg, "--background") == 0) {
            if (++i >= argc || parse_color(argv[i], options.background) != 0)
                return usage_error(argv[0], "--background expects RRGGBB");
        } else if (strcmp(arg, "--pad") == 0) {
            options.strict = 0;
        } else if (strcmp(arg, "--strict") == 0) {
            options.strict = 1;
        } else if (strcmp(arg, "--verify") == 0) {
            options.verify = 1;
        } else if (strcmp(arg, "--verbose") == 0) {
            options.verbose = 1;
        } else if (strcmp(arg, "--quiet") == 0) {
            options.verbose = 0;
        } else if (arg[0] == '-') {
            fprintf(stderr, "build_smk: unknown option %s\n", arg);
            return SMK_ENC_ERR_USAGE;
        } else if (input == NULL) {
            input = arg;
        } else {
            return usage_error(argv[0], "only one input is accepted");
        }
    }

    (void)audio;
    if (input == NULL)
        return usage_error(argv[0], "missing input");

    if (output == NULL) {
        derive_output(input, output_buffer, sizeof(output_buffer));
        output = output_buffer;
    }

    fprintf(stderr, "build_smk: input handling is not implemented yet\n");
    return SMK_ENC_ERR_INPUT;
}
