#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

char *test_tempdir(void)
{
#if defined(_WIN32)
    char base[MAX_PATH];
    char *path;
    DWORD n = GetTempPathA((DWORD)sizeof(base), base);
    if (n == 0 || n >= sizeof(base))
        return NULL;
    path = (char *)malloc(strlen(base) + 16);
    if (path == NULL)
        return NULL;
    sprintf(path, "%ssmkXXXXXX", base);
    if (_mktemp_s(path, strlen(path) + 1) != 0) {
        free(path);
        return NULL;
    }
    if (_mkdir(path) != 0) {
        free(path);
        return NULL;
    }
    return path;
#else
    char template[] = "/tmp/smk-bsmk-XXXXXX";
    char *path = mkdtemp(template);
    return path == NULL ? NULL : strdup(path);
#endif
}

void test_tempdir_free(char *path)
{
    free(path);
}

int test_write_file(const char *path, const void *data, size_t size)
{
    FILE *fp = fopen(path, "wb");
    size_t written;

    if (fp == NULL)
        return -1;
    written = size == 0 ? 0 : fwrite(data, 1, size, fp);
    if (fclose(fp) != 0)
        return -1;
    return written == size ? 0 : -1;
}

int test_read_file(const char *path, void **data, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    long length;
    void *buffer;

    if (fp == NULL)
        return -1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    length = ftell(fp);
    if (length < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(fp);
        return -1;
    }
    if (length > 0 && fread(buffer, 1, (size_t)length, fp) != (size_t)length) {
        free(buffer);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *data = buffer;
    *size = (size_t)length;
    return 0;
}
