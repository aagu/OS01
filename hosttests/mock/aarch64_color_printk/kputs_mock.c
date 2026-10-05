/* test/mock/aarch64_color_printk/kputs_mock.c — kputs capture mock for
 * test_aarch64_color_printk_abi. Records the last string handed to
 * kputs so the test can prove the production aarch64 printk_stub.c
 * forwards the literal format string (and never dereferences the
 * FRcolor/BKcolor arguments as pointers). */
#include <stddef.h>

#include <arch/aarch64/boot_log.h>

static const char *last_string;

void kputs(const char *message)
{
    last_string = message;
}

void mock_kputs_clear(void)
{
    last_string = NULL;
}

const char *mock_kputs_last(void)
{
    return last_string;
}
