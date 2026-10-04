/* hosttests/cases/test_aarch64_gic_target_probe.c — gic_target_bit_init
 * (M3 Task 8) 单元测试：从 GICD_ITARGETSR0 实测构建 per-CPU target byte。
 *
 * 编译【真实生产文件】kernel/arch/aarch64/intr/ipi.c（host clang，无修改）。
 * dtb_cpu_count() / dtb_gicd_base() 由本测试 mock（dtb.c 太重，host 不编）；
 * GICD 寄存器是一个 mock 数组，dtb_gicd_base() 返回它的地址 —— 因此
 * ITARGETSR0（offset 0x800）就落在 gicd_mock[0x800/4]，byte 0 是小端最低字节。
 *
 * 覆盖：
 *   1. dtb_cpu_count()==1 → 单核 RAZ/WI 例外：gic_target_bit[0] = 1，
 *      不读 ITARGETSR0（mock 保持 0 也能过）；
 *   2. dtb_cpu_count()==4 + ITARGETSR0 byte0=0x02 → init(1) → bit[1]=0x02；
 *      byte0=0x01 → init(0) → bit[0]=0x01（恒等拓扑）；
 *   3. FATAL（weak hook，非 WARN）：byte0=0x00；byte0=0x05（多 bit）；
 *      byte0=0x02 但 cpu_id=0（非恒等拓扑，v1 review item 11）；
 *   4. M7.3 guard：logical_to_gic_targets 丢弃 >= AARCH64_BOOT_MAX_CPUS
 *      的 mask bit 时触发 weak 违规 hook；
 *   5. 源码扫描：ipi.c 禁用固定 GIC_DIST_BASE 字面赋值，基址必须来自
 *      dtb_gicd_base()。
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <intr/ipi.h>
#include <arch/aarch64/ipi.h>
#include <arch/aarch64/gic.h>
#include <arch/aarch64/dtb.h>

#ifndef OS01_KERNEL_SRC
#error "OS01_KERNEL_SRC must be defined to the kernel source root"
#endif

#define M_GICD_SGIR      0xF00
#define M_ITARGETSR0_OFF 0x800

static uint32_t gicd_mock[0x400];
static uint32_t gicc_mock[0x20];
static struct gic_dev dev;
static uint32_t mock_cpu_count = 4;

struct gic_dev *gic_dev_current(void) { return &dev; }
uint32_t dtb_cpu_count(void) { return mock_cpu_count; }
uint64_t dtb_gicd_base(void) { return (uint64_t)(uintptr_t)gicd_mock; }

/* ITARGETSR0 byte 0（SGI 0..3 的 target byte，小端最低字节） */
static void set_itargetsr0_byte0(uint8_t b)
{
    gicd_mock[M_ITARGETSR0_OFF / 4] = b; /* 其他 byte 不关心，写 0 */
}

static void mock_reset(void)
{
    for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
    for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
    gicd_mock[0x004 / 4] = 0x1f;       /* TYPER: ITLines=31 */
    gicd_mock[0x008 / 4] = 0x0200143b; /* IIDR nonzero */
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
}

/* FATAL hook（weak 覆盖）：armed 时 longjmp 回测试点 */
static jmp_buf fatal_jb;
static int fatal_armed;
void ipi_fatal_itargets(uint32_t cpu_id, uint8_t byte)
{
    (void)cpu_id; (void)byte;
    if (fatal_armed) {
        fatal_armed = 0;
        longjmp(fatal_jb, 1);
    }
    fprintf(stderr, "ipi_fatal_itargets outside armed test\n");
    exit(2);
}

/* M7.3 违规 hook（weak 覆盖）：mask bit 超出表容量 */
static jmp_buf oor_jb;
static int oor_armed;
void ipi_warn_mask_bit_out_of_range(uint64_t mask)
{
    (void)mask;
    if (oor_armed) {
        oor_armed = 0;
        longjmp(oor_jb, 1);
    }
}

/* 生产代码不会走到 unsupported-vector 路径；覆盖 weak 默认的死循环，
 * 避免意外时 hang 住整个测试进程。 */
void ipi_panic_unsupported_vector(uint32_t vector)
{
    (void)vector;
    fprintf(stderr, "ipi_panic_unsupported_vector reached\n");
    exit(2);
}

static void suite_single_core_razwi(void)
{
    TEST_SUITE("single-core RAZ/WI exception: bit[0]=1 without ITARGETSR0 read");
    mock_reset();
    mock_cpu_count = 1;
    set_itargetsr0_byte0(0x00);   /* RAZ/WI: 读回 0 也不校验 */
    gic_target_bit_init(0);
    assert_eq((uint8_t)0x01, gic_target_bit_get(0));
}

static void suite_multi_core_identity(void)
{
    TEST_SUITE("multi-core: ITARGETSR0 byte0 per-CPU identity probe");
    mock_reset();
    mock_cpu_count = 4;

    set_itargetsr0_byte0(0x01);
    gic_target_bit_init(0);
    assert_eq((uint8_t)0x01, gic_target_bit_get(0));

    set_itargetsr0_byte0(0x02);
    gic_target_bit_init(1);
    assert_eq((uint8_t)0x02, gic_target_bit_get(1));

    /* init 不得污染其他条目 */
    assert_eq((uint8_t)0x00, gic_target_bit_get(2));
}

static void suite_fatal_cases(void)
{
    TEST_SUITE("non-identity / zero / multi-bit ITARGETSR0 are FATAL");
    mock_cpu_count = 4;
    gic_target_bit_inject(0, 0); /* 清掉上一 suite 写入的 bit[0] */

    /* byte0 = 0：无 target（不应发生）→ FATAL */
    mock_reset();
    set_itargetsr0_byte0(0x00);
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(0);
        fatal_armed = 0;
        assert_true(0);           /* 未 FATAL = 失败 */
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(0)); /* 未写入 cache */
    }

    /* byte0 = 0x05：多 bit，违反 QEMU virt 恒等拓扑 → FATAL（item 11） */
    mock_reset();
    set_itargetsr0_byte0(0x05);
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(0);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(0)); /* 未写入 cache */
    }

    /* byte0 = 0x02 但 cpu_id = 0：非恒等拓扑 → FATAL */
    mock_reset();
    set_itargetsr0_byte0(0x02);
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(0);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(0));
    }

    /* cpu_id 越界（fix round 1）：单核路径不读硬件也必须拒绝 → FATAL */
    mock_reset();
    mock_cpu_count = 1;
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(AARCH64_BOOT_MAX_CPUS);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00,
                  gic_target_bit_get(AARCH64_BOOT_MAX_CPUS));
    }

    /* cpu_id 越界：多核路径同样拒绝（不再靠 1u<<cpu 不匹配的巧合） */
    mock_reset();
    mock_cpu_count = 4;
    set_itargetsr0_byte0(0x02);
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(AARCH64_BOOT_MAX_CPUS + 1);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00,
                  gic_target_bit_get(AARCH64_BOOT_MAX_CPUS + 1));
    }
}

static void suite_mask_out_of_range_guard(void)
{
    TEST_SUITE("M7.3: out-of-range mask bit triggers violation hook");
    mock_reset();
    mock_cpu_count = 4;
    set_itargetsr0_byte0(0x01);
    gic_target_bit_init(0);

    /* in-range mask 正常发送，不触发 hook */
    ipi_broadcast(IPI_VECTOR_TLB, 0x1);
    assert_eq((uint32_t)(3u | (0x01u << 16)), gicd_mock[M_GICD_SGIR / 4]);

    /* bit >= AARCH64_BOOT_MAX_CPUS：hook 触发，SGIR 不写 */
    mock_reset();
    if (setjmp(oor_jb) == 0) {
        oor_armed = 1;
        ipi_broadcast(IPI_VECTOR_TLB, UINT64_C(1) << 63);
        oor_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint32_t)0, gicd_mock[M_GICD_SGIR / 4]);
    }
}

/* libc stdio.h shim 的 fread 在 host 上不可靠 — 直接用 POSIX read */
#include <fcntl.h>
#include <unistd.h>
static int slurp_file(const char *path, char **out_buf)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { *out_buf = NULL; return -1; }
    size_t cap = 4096, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); *out_buf = NULL; return -1; }
    for (;;) {
        if (n + 1024 > cap) {
            char *nb = (char *)realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); *out_buf = NULL; return -1; }
            buf = nb; cap *= 2;
        }
        ssize_t got = read(fd, buf + n, 1024);
        if (got < 0) { free(buf); close(fd); *out_buf = NULL; return -1; }
        n += (size_t)got;
        if (got == 0) break;
    }
    close(fd);
    buf[n] = '\0';
    *out_buf = buf;
    return 0;
}

static void suite_source_scan_no_fixed_base(void)
{
    TEST_SUITE("source scan: ipi.c has no fixed GIC_DIST_BASE assignment");
    char *buf = NULL;
    assert_eq(0, slurp_file(OS01_KERNEL_SRC "/kernel/arch/aarch64/intr/ipi.c",
                            &buf));
    assert_true(buf != NULL);

    bool uses_dtb_base = (strstr(buf, "dtb_gicd_base()") != NULL);
    bool fixed_base_assign = false;
    /* 逐行等价 grep -E "GIC_DIST_BASE\s*="：固定基址字面赋值 = 禁止 */
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *hit = strstr(line, "GIC_DIST_BASE");
        if (!hit) continue;
        const char *p = hit + strlen("GIC_DIST_BASE");
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '=') fixed_base_assign = true;
    }
    free(buf);
    assert_true(!fixed_base_assign);
    assert_true(uses_dtb_base);   /* 正向断言：基址必须来自 dtb_gicd_base() */
}

int main(void)
{
    suite_single_core_razwi();
    suite_multi_core_identity();
    suite_fatal_cases();
    suite_mask_out_of_range_guard();
    suite_source_scan_no_fixed_base();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_aarch64_gic_target_probe",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
