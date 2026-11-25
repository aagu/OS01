/* @os01-test
 * id: test_uefi_boot_config
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: nightly
 * link: uefi-boot-config
 * scenarios: [boot.uefi.config]
 */
#include "test_framework.h"
#include "../../boot/uefi/boot_config.h"
#include <string.h>

static void test_missing_and_empty_defaults(void)
{
    struct uefi_boot_config cfg;
    unsigned err_line = 99u;

    assert_eq(0, uefi_boot_config_parse(NULL, 0, true, &cfg, &err_line));
    assert_eq(UEFI_BOOT_PLATFORM_AUTO, cfg.platform);
    assert_eq(UEFI_BOOT_CONSOLE_DTB, cfg.console);
    assert_eq(1440u, cfg.width);
    assert_eq(900u, cfg.height);
    assert_eq(0u, err_line);

    assert_eq(0, uefi_boot_config_parse("", 0, false, &cfg, NULL));
    assert_eq(UEFI_BOOT_PLATFORM_AUTO, cfg.platform);
    assert_eq(UEFI_BOOT_CONSOLE_DTB, cfg.console);
    assert_eq(1440u, cfg.width);
    assert_eq(900u, cfg.height);
}

static void test_comments_crlf_tab_and_no_trailing_nl(void)
{
    static const char buf[] =
        "# RPi5 config\r\n"
        "  \t # indented comment\r\n"
        "\r\n"
        "  platform\trpi5  \r\n"
        "console   rp1-header\t";
    struct uefi_boot_config cfg;
    unsigned err_line = 99u;

    assert_eq(0, uefi_boot_config_parse(buf, sizeof(buf) - 1u, true, &cfg,
                                        &err_line));
    assert_eq(UEFI_BOOT_PLATFORM_RPI5, cfg.platform);
    assert_eq(UEFI_BOOT_CONSOLE_RP1_HEADER, cfg.console);
    assert_eq(1440u, cfg.width);
    assert_eq(900u, cfg.height);
    assert_eq(0u, err_line);
}

static void test_rp1_header_requires_explicit_rpi5(void)
{
    static const char only_console[] = "console rp1-header\n";
    static const char auto_platform[] =
        "platform auto\nconsole rp1-header\n";
    static const char qemu_platform[] =
        "platform qemu-virt\nconsole rp1-header\n";
    struct uefi_boot_config cfg;
    unsigned err_line = 0u;

    assert_true(uefi_boot_config_parse(only_console, sizeof(only_console) - 1u,
                                       true, &cfg, &err_line) < 0);
    assert_true(uefi_boot_config_parse(auto_platform, sizeof(auto_platform) - 1u,
                                       true, &cfg, &err_line) < 0);
    assert_true(uefi_boot_config_parse(qemu_platform, sizeof(qemu_platform) - 1u,
                                       true, &cfg, &err_line) < 0);
}

static void test_duplicate_console_and_platform_fail_on_aarch64(void)
{
    static const char dup_console[] =
        "platform rpi5\n"
        "console dtb\n"
        "console rp1-header\n";
    static const char dup_platform[] =
        "platform qemu-virt\n"
        "platform qemu-virt\n";
    struct uefi_boot_config cfg;
    unsigned err_line = 0u;

    assert_true(uefi_boot_config_parse(dup_console, sizeof(dup_console) - 1u,
                                       true, &cfg, &err_line) < 0);
    assert_eq(3u, err_line);

    err_line = 0u;
    assert_true(uefi_boot_config_parse(dup_platform, sizeof(dup_platform) - 1u,
                                       true, &cfg, &err_line) < 0);
    assert_eq(2u, err_line);
}

static void test_bare_cr_embedded_nul_and_oversize(void)
{
    static const char bare_cr[] = "platform qemu-virt\rconsole dtb\n";
    static const char embedded_nul[] = "platform qemu-virt\n\0console dtb\n";
    char big[UEFI_BOOT_CONFIG_MAX_BYTES + 1u];
    struct uefi_boot_config cfg;
    unsigned err_line = 0u;

    assert_true(uefi_boot_config_parse(bare_cr, sizeof(bare_cr) - 1u,
                                       true, &cfg, &err_line) < 0);
    assert_eq(1u, err_line);

    err_line = 0u;
    assert_true(uefi_boot_config_parse(embedded_nul, sizeof(embedded_nul) - 1u,
                                       true, &cfg, &err_line) < 0);
    assert_eq(2u, err_line);

    memset(big, '#', UEFI_BOOT_CONFIG_MAX_BYTES);
    big[UEFI_BOOT_CONFIG_MAX_BYTES] = '\n';
    assert_true(uefi_boot_config_parse(big, sizeof(big), true, &cfg,
                                       &err_line) < 0);

    assert_eq(0, uefi_boot_config_parse(big, UEFI_BOOT_CONFIG_MAX_BYTES, true,
                                        &cfg, &err_line));
}

static void test_unknown_keys_and_x86_resolution_rules(void)
{
    static const char aarch64_buf[] =
        "kernel.arg console=ttyAMA0\n"
        "console dtb\n"
        "init.arg --quiet\n";
    static const char x86_buf[] =
        "platform rpi5\n"
        "console rp1-header\n"
        "resolution 800x600\n"
        "resolution 1024x768\n"
        "resolution 0x768\n"
        "resolution 1024x0\n"
        "resolution 999999999999x768\n"
        "resolution 1280xabc\n"
        "kernel.arg foo=bar\n";
    struct uefi_boot_config cfg;

    assert_eq(0, uefi_boot_config_parse(aarch64_buf, sizeof(aarch64_buf) - 1u,
                                        true, &cfg, NULL));
    assert_eq(UEFI_BOOT_PLATFORM_AUTO, cfg.platform);
    assert_eq(UEFI_BOOT_CONSOLE_DTB, cfg.console);

    assert_eq(0, uefi_boot_config_parse(x86_buf, sizeof(x86_buf) - 1u,
                                        false, &cfg, NULL));
    assert_eq(1024u, cfg.width);
    assert_eq(768u, cfg.height);
}

int main(void)
{
    TEST_SUITE("uefi_boot_config");
    test_missing_and_empty_defaults();
    test_comments_crlf_tab_and_no_trailing_nl();
    test_rp1_header_requires_explicit_rpi5();
    test_duplicate_console_and_platform_fail_on_aarch64();
    test_bare_cr_embedded_nul_and_oversize();
    test_unknown_keys_and_x86_resolution_rules();
    TEST_RESULTS();
    return __test_stats.failed ? 1 : 0;
}
