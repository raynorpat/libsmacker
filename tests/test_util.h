#ifndef SMK_TEST_UTIL_H
#define SMK_TEST_UTIL_H

#include <stddef.h>

/* Create a fresh temporary directory. Returns a malloc'd path or NULL. */
char *test_tempdir(void);

/* Free a path returned by test_tempdir() without deleting it. */
void test_tempdir_free(char *path);

/* Write size bytes to path. Returns 0 on success. */
int test_write_file(const char *path, const void *data, size_t size);

/* Read a whole file into a malloc'd buffer. Returns 0 on success. */
int test_read_file(const char *path, void **data, size_t *size);

#endif
