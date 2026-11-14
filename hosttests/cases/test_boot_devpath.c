/* @os01-test
 * id: test_boot_devpath
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: nightly
 * link: boot-devpath
 * scenarios: [boot.uefi.devpath]
 */
#include "test_framework.h"
#include <core/bootinfo.h>
#include "../../boot/uefi/boot_device.h"
#include <stdint.h>
#include <string.h>

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void write_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static void fill_hd_node(uint8_t *p, uint32_t part_num, uint64_t start,
                         uint64_t size, const uint8_t guid[16],
                         uint8_t mbr_type, uint8_t sig_type)
{
    p[0] = 0x04;
    p[1] = 0x01;
    write_le16(p + 2, 42);
    write_le32(p + 4, part_num);
    write_le64(p + 8, start);
    write_le64(p + 16, size);
    memcpy(p + 24, guid, 16);
    p[40] = mbr_type;
    p[41] = sig_type;
}

static void fill_end_entire(uint8_t *p)
{
    p[0] = 0x7f;
    p[1] = 0xff;
    p[2] = 0x04;
    p[3] = 0x00;
}

static const uint8_t k_test_guid[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01
};

static void assert_rejects_untouched(const uint8_t *dp, size_t len)
{
    struct boot_device_info out;
    struct boot_device_info expected;

    memset(&out, 0xa5, sizeof(out));
    memset(&expected, 0xa5, sizeof(expected));

    assert_true(os01_bootdev_from_devpath(dp, len, &out) != 0);
    assert_true(memcmp(&out, &expected, sizeof(out)) == 0);
}

static void test_boot_devpath_wire_unaligned(void)
{
    uint8_t raw[1 + 6 + 42 + 4];
    uint8_t *dp = raw + 1; /* intentionally unaligned */
    struct boot_device_info out;

    /* Preceding dummy Messaging node (type 03, sub 05, len 6) */
    dp[0] = 0x03;
    dp[1] = 0x05;
    dp[2] = 0x06;
    dp[3] = 0x00;
    dp[4] = 0x01;
    dp[5] = 0x00;

    fill_hd_node(dp + 6, 3u, UINT64_C(0x123456789), UINT64_C(0x23456789),
                 k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 6 + 42);

    memset(&out, 0, sizeof(out));
    assert_eq(0, os01_bootdev_from_devpath(dp, 6 + 42 + 4, &out));
    assert_true(memcmp(out.partition_guid, k_test_guid, 16) == 0);
    assert_eq(3u, out.partition_number);
    assert_eq(UINT64_C(0x123456789), out.partition_start_lba);
    assert_eq(UINT64_C(0x23456789), out.partition_size_lba);
    assert_eq(2u, out.signature_type);
}

static void test_boot_devpath_bare_hd(void)
{
    uint8_t dp[42 + 4];
    struct boot_device_info out;

    fill_hd_node(dp, 1u, 2048u, 131072u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 42);

    memset(&out, 0, sizeof(out));
    assert_eq(0, os01_bootdev_from_devpath(dp, sizeof(dp), &out));
    assert_true(memcmp(out.partition_guid, k_test_guid, 16) == 0);
    assert_eq(1u, out.partition_number);
    assert_eq(2048u, out.partition_start_lba);
    assert_eq(131072u, out.partition_size_lba);
    assert_eq(2u, out.signature_type);
}

static void test_boot_devpath_rejections(void)
{
    uint8_t dp[128];
    uint8_t zero_guid[16];
    struct boot_device_info out;

    memset(zero_guid, 0, sizeof(zero_guid));

    /* NULL / zero length */
    memset(&out, 0xa5, sizeof(out));
    assert_true(os01_bootdev_from_devpath(NULL, 46, &out) != 0);
    assert_true(os01_bootdev_from_devpath(dp, 46, NULL) != 0);
    assert_rejects_untouched(dp, 0);
    assert_rejects_untouched(dp, 3);

    /* Type 03 masquerading as HD */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    dp[0] = 0x03;
    fill_end_entire(dp + 42);
    assert_rejects_untouched(dp, 46);

    /* MBR type != 0x02 */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x01, 0x02);
    fill_end_entire(dp + 42);
    assert_rejects_untouched(dp, 46);

    /* Signature type != 0x02 */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x01);
    fill_end_entire(dp + 42);
    assert_rejects_untouched(dp, 46);

    /* Zero GUID */
    fill_hd_node(dp, 1u, 2048u, 4096u, zero_guid, 0x02, 0x02);
    fill_end_entire(dp + 42);
    assert_rejects_untouched(dp, 46);

    /* Zero partition number */
    fill_hd_node(dp, 0u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 42);
    assert_rejects_untouched(dp, 46);

    /* Zero partition size */
    fill_hd_node(dp, 1u, 2048u, 0u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 42);
    assert_rejects_untouched(dp, 46);

    /* Undersized HD node (len = 41) */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    write_le16(dp + 2, 41);
    fill_end_entire(dp + 41);
    assert_rejects_untouched(dp, 45);

    /* Oversized HD node (len = 43) */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    write_le16(dp + 2, 43);
    dp[42] = 0;
    fill_end_entire(dp + 43);
    assert_rejects_untouched(dp, 47);

    /* Node length < 4 */
    dp[0] = 0x04;
    dp[1] = 0x01;
    write_le16(dp + 2, 3);
    assert_rejects_untouched(dp, 46);

    /* Truncated node (max_len too short) */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 42);
    assert_rejects_untouched(dp, 45);
    assert_rejects_untouched(dp, 42); /* missing End Entire */
    assert_rejects_untouched(dp, 20); /* truncated inside HD node */

    /* Trailing bytes after End Entire */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 42);
    dp[46] = 0x00;
    assert_rejects_untouched(dp, 47);

    /* Bad End Entire length */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 42);
    write_le16(dp + 42 + 2, 5);
    dp[46] = 0x00;
    assert_rejects_untouched(dp, 47);

    /* End Instance instead of End Entire */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    dp[42] = 0x7f;
    dp[43] = 0x01;
    dp[44] = 0x04;
    dp[45] = 0x00;
    fill_hd_node(dp + 46, 2u, 8192u, 4096u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 88);
    assert_rejects_untouched(dp, 92);

    /* Duplicate HD node */
    fill_hd_node(dp, 1u, 2048u, 4096u, k_test_guid, 0x02, 0x02);
    fill_hd_node(dp + 42, 2u, 8192u, 4096u, k_test_guid, 0x02, 0x02);
    fill_end_entire(dp + 84);
    assert_rejects_untouched(dp, 88);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_boot_devpath_wire_unaligned),
    TEST_ENTRY(test_boot_devpath_bare_hd),
    TEST_ENTRY(test_boot_devpath_rejections),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("Boot Device Path Parser");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
