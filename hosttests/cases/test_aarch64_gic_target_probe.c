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

#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#define M_GICD_SGIR      0xF00
#define M_ITARGETSR0_OFF 0x800

static uint32_t gicd_mock[0x400];
static uint32_t gicc_mock[0x20];
static struct gic_dev dev;
static uint32_t mock_cpu_count = 4;

struct gic_dev *gic_dev_current(void) { return &dev; }
uint32_t dtb_cpu_count(void) { return mock_cpu_count; }
uint64_t dtb_gicd_base(void) { return 0; } /* Behavior assertion: ipi.c must use gic_dev_current()->gicd */

static void set_itargetsr_bytes(const uint8_t *bytes, size_t n)
{
    for (unsigned w = 0; w < 8; w++) {
        gicd_mock[M_ITARGETSR0_OFF / 4 + w] = 0;
    }
    volatile uint8_t *p = (volatile uint8_t *)&gicd_mock[M_ITARGETSR0_OFF / 4];
    for (size_t i = 0; i < n && i < 32; i++) {
        p[i] = bytes[i];
    }
}

static void mock_reset_typer(uint32_t typer)
{
    for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
    for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
    gicd_mock[0x004 / 4] = typer;
    gicd_mock[0x008 / 4] = 0x0200143b; /* IIDR nonzero */
    assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
}

static void mock_reset(void)
{
    mock_reset_typer(0x007fu); /* 4 CPU interfaces, 1020 intids */
}

extern int host_fork(void) __asm__("fork");
extern int host_waitpid(int pid, int *status, int options) __asm__("waitpid");

static void run_isolated(void (*fn)(void), const char *name)
{
    int pid = host_fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        __test_stats.total = 0;
        __test_stats.passed = 0;
        __test_stats.failed = 0;
        fn();
        exit(__test_stats.failed ? 1 : 0);
    }
    int status = 0;
    assert_eq(pid, host_waitpid(pid, &status, 0));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("  FAIL isolated fixture: %s (status=%d)\n", name, status);
        assert_true(0);
    } else {
        assert_true(1);
    }
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
    TEST_SUITE("single-core RAZ/WI fallback only when TYPER single-interface");
    /* DT single-core + TYPER single-interface (CPUNumber=0) + all-zero -> fallback 1 */
    mock_reset_typer(0x001fu);
    mock_cpu_count = 1;
    assert_eq((uint8_t)0x01, gic_cpu_target_mask(&dev, true));
    gic_target_bit_init(0);
    assert_eq((uint8_t)0x01, gic_target_bit_get(0));
}

static void suite_single_core_multi_iface_ok(void)
{
    TEST_SUITE("DT single-core + multi-interface TYPER: accepts 0x04");
    mock_reset_typer(0x007fu); /* 4 CPU interfaces */
    mock_cpu_count = 1;

    const uint8_t ok_bytes[] = { 0, 0x04, 0x04, 0 };
    set_itargetsr_bytes(ok_bytes, sizeof(ok_bytes));
    assert_eq((uint8_t)0x04, gic_cpu_target_mask(&dev, true));
    gic_target_bit_init(0);
    assert_eq((uint8_t)0x04, gic_target_bit_get(0));
}

static void suite_single_core_multi_iface_zero_fatal(void)
{
    TEST_SUITE("DT single-core + multi-interface TYPER: rejects all-zero");
    mock_reset_typer(0x007fu); /* 4 CPU interfaces */
    mock_cpu_count = 1;

    const uint8_t zero_bytes[] = { 0, 0, 0, 0 };
    set_itargetsr_bytes(zero_bytes, sizeof(zero_bytes));
    assert_eq((uint8_t)0x00, gic_cpu_target_mask(&dev, true));
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(0);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(0));
    }
}

static void suite_multi_core_non_identity(void)
{
    TEST_SUITE("multi-core non-identity {0x04, 0x02} -> SGIR mask 0x06");
    mock_reset();
    mock_cpu_count = 2;

    const uint8_t cpu0_bytes[] = { 0, 0x04, 0x04, 0 };
    set_itargetsr_bytes(cpu0_bytes, sizeof(cpu0_bytes));
    assert_eq((uint8_t)0x04, gic_cpu_target_mask(&dev, false));
    gic_target_bit_init(0);
    assert_eq((uint8_t)0x04, gic_target_bit_get(0));

    const uint8_t cpu1_bytes[] = { 0x02, 0, 0x02, 0 };
    set_itargetsr_bytes(cpu1_bytes, sizeof(cpu1_bytes));
    assert_eq((uint8_t)0x02, gic_cpu_target_mask(&dev, false));
    gic_target_bit_init(1);
    assert_eq((uint8_t)0x02, gic_target_bit_get(1));

    assert_eq((uint8_t)0x00, gic_target_bit_get(2));

    /* logical mask 3 (cpu0 | cpu1) -> SGIR targets 0x04 | 0x02 = 0x06 */
    ipi_broadcast(IPI_VECTOR_TLB, 3u);
    assert_eq((uint32_t)(3u | (0x06u << 16)), gicd_mock[M_GICD_SGIR / 4]);
}

static void suite_duplicate_claim_fatal(void)
{
    TEST_SUITE("duplicate physical target bit claim is FATAL and keeps earlier slot");
    mock_reset();
    mock_cpu_count = 2;

    const uint8_t b04[] = { 0, 0x04, 0x04, 0 };
    set_itargetsr_bytes(b04, sizeof(b04));
    gic_target_bit_init(0);
    assert_eq((uint8_t)0x04, gic_target_bit_get(0));

    /* Second logical CPU gets same physical bit 0x04 -> FATAL, slot 0 stays 0x04, slot 1 stays 0 */
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(1);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x04, gic_target_bit_get(0));
        assert_eq((uint8_t)0x00, gic_target_bit_get(1));
    }
}

static void suite_fatal_cases(void)
{
    TEST_SUITE("zero / conflicting / multi-bit / out-of-range ITARGETSR are FATAL");
    mock_cpu_count = 4;

    /* [0,0,0,0] -> 0 and FATAL */
    mock_reset();
    assert_eq((uint8_t)0x00, gic_cpu_target_mask(&dev, false));
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(0);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(0));
    }

    /* [0x04, 0x02] conflicting non-zero bytes -> 0 and FATAL */
    mock_reset();
    const uint8_t conflict[] = { 0x04, 0x02 };
    set_itargetsr_bytes(conflict, sizeof(conflict));
    assert_eq((uint8_t)0x00, gic_cpu_target_mask(&dev, false));
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(0);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(0));
    }

    /* [0x05] multi-bit -> 0 and FATAL */
    mock_reset();
    const uint8_t multibit[] = { 0x05 };
    set_itargetsr_bytes(multibit, sizeof(multibit));
    assert_eq((uint8_t)0x00, gic_cpu_target_mask(&dev, false));
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(0);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(0));
    }

    /* cpu_id out of range: single-core and multi-core both FATAL */
    mock_reset_typer(0x001fu);
    mock_cpu_count = 1;
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(AARCH64_BOOT_MAX_CPUS);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(AARCH64_BOOT_MAX_CPUS));
    }

    mock_reset();
    mock_cpu_count = 4;
    const uint8_t b02[] = { 0x02 };
    set_itargetsr_bytes(b02, sizeof(b02));
    if (setjmp(fatal_jb) == 0) {
        fatal_armed = 1;
        gic_target_bit_init(AARCH64_BOOT_MAX_CPUS + 1);
        fatal_armed = 0;
        assert_true(0);
    } else {
        assert_true(1);
        assert_eq((uint8_t)0x00, gic_target_bit_get(AARCH64_BOOT_MAX_CPUS + 1));
    }
}

static void suite_mask_out_of_range_guard(void)
{
    TEST_SUITE("M7.3: out-of-range mask bit triggers violation hook");
    mock_reset();
    mock_cpu_count = 4;
    const uint8_t b01[] = { 0x01 };
    set_itargetsr_bytes(b01, sizeof(b01));
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
}

int main(void)
{
    run_isolated(suite_single_core_razwi, "single_core_razwi");
    run_isolated(suite_single_core_multi_iface_ok, "single_core_multi_iface_ok");
    run_isolated(suite_single_core_multi_iface_zero_fatal, "single_core_multi_iface_zero_fatal");
    run_isolated(suite_multi_core_non_identity, "multi_core_non_identity");
    run_isolated(suite_duplicate_claim_fatal, "duplicate_claim_fatal");
    run_isolated(suite_fatal_cases, "fatal_cases");
    run_isolated(suite_mask_out_of_range_guard, "mask_out_of_range_guard");
    suite_source_scan_no_fixed_base();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_aarch64_gic_target_probe",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
