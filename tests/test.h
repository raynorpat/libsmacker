#ifndef SMK_TEST_H
#define SMK_TEST_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef void (*smk_test_fn)(void);

typedef struct {
    const char *name;
    smk_test_fn fn;
} smk_test_case;

extern int smk_test_failures;
extern int smk_test_checks;

#define SMK_ASSERT(cond)                                                      \
    do {                                                                      \
        smk_test_checks++;                                                    \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  %s:%d: assertion failed: %s\n",               \
                    __FILE__, __LINE__, #cond);                               \
            smk_test_failures++;                                              \
        }                                                                     \
    } while (0)

#define SMK_ASSERT_EQ_INT(actual, expected)                                   \
    do {                                                                      \
        long long a_ = (long long)(actual);                                   \
        long long e_ = (long long)(expected);                                 \
        smk_test_checks++;                                                    \
        if (a_ != e_) {                                                       \
            fprintf(stderr,                                                   \
                    "  %s:%d: expected %lld, got %lld\n",                     \
                    __FILE__, __LINE__, e_, a_);                              \
            smk_test_failures++;                                              \
        }                                                                     \
    } while (0)

#define SMK_ASSERT_STR_EQ(actual, expected)                                   \
    do {                                                                      \
        const char *a_ = (actual);                                            \
        const char *e_ = (expected);                                          \
        smk_test_checks++;                                                    \
        if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0) {                \
            fprintf(stderr, "  %s:%d: expected \"%s\", got \"%s\"\n",         \
                    __FILE__, __LINE__, e_ ? e_ : "(null)",                   \
                    a_ ? a_ : "(null)");                                      \
            smk_test_failures++;                                              \
        }                                                                     \
    } while (0)

#define SMK_ASSERT_MEM_EQ(actual, expected, size)                             \
    do {                                                                      \
        const void *a_ = (actual);                                            \
        const void *e_ = (expected);                                          \
        size_t n_ = (size_t)(size);                                           \
        smk_test_checks++;                                                    \
        if (a_ == NULL || e_ == NULL || memcmp(a_, e_, n_) != 0) {            \
            fprintf(stderr, "  %s:%d: memory differs (%lu bytes)\n",          \
                    __FILE__, __LINE__, (unsigned long)n_);                   \
            smk_test_failures++;                                              \
        }                                                                     \
    } while (0)

#endif
