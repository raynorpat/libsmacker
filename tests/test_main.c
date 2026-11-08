#include "test.h"

int smk_test_failures = 0;
int smk_test_checks = 0;

void test_cli_defaults(void);
void test_encode_null_source(void);
void test_encode_null_output(void);
void test_encode_missing_input(void);

static const smk_test_case tests[] = {
    { "cli_defaults", test_cli_defaults },
    { "encode_null_source", test_encode_null_source },
    { "encode_null_output", test_encode_null_output },
    { "encode_missing_input", test_encode_missing_input },
    { NULL, NULL }
};

int main(void)
{
    int i;
    int failed_tests = 0;

    for (i = 0; tests[i].name != NULL; i++) {
        smk_test_failures = 0;
        smk_test_checks = 0;
        tests[i].fn();
        if (smk_test_failures == 0) {
            printf("ok %d - %s (%d checks)\n", i + 1, tests[i].name,
                   smk_test_checks);
        } else {
            printf("not ok %d - %s (%d failures)\n", i + 1, tests[i].name,
                   smk_test_failures);
            failed_tests++;
        }
    }

    printf("%d tests, %d failed\n", i, failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
