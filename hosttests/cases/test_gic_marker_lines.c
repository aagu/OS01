/* hosttests/cases/test_gic_marker_lines.c — GICv2 marker 源码存在性断言
 * (Task 2.1 RED)
 *
 * 目的：QEMU aarch64-uefi 镜像当前受 thirdpart/{posix-uefi,mbedtls,lwip}
 * submodule 空目录阻断（Task 1.1/1.2 报告记录），无法在 host 端运行真实
 * kernel 来观察 marker 行。本 hosttest 退而求其次：源码级 RED——
 *
 *   1. 把 kernel/arch/aarch64/intr/gic.c / main.c 整文件读进内存
 *   2. 断言每个 kernel 端会发出的 marker 字符串（gic.c kputs 模板 + 拼接
 *      后的子串）字面量都能在源文件中被找到
 *
 * 当前 RED 状态（Task 2.1 之前）：
 *   - "[gic] GICv2 driver: intids="           — 在 gic.c:54（Task 1.2 已发）
 *   - "[gic] dispatch ready"                  — 暂无（Task 2.2 才加，main.c）
 *   - "[gic-probe] save-restore OK"           — 暂无（Task 2.2 才加，irq_probe.c）
 *   - "[gic-probe] unexpected intid=40 survived" — 暂无（Task 2.2 才加）
 *
 * 因此 hosttest 会在 3 个 missing-assert 上 FAIL —— 这是占位 RED，
 * Task 2.2 把对应 marker 加入 gic.c（dispatch ready 由 main.c 经 OS01_SELFTEST
 * 门控发出，但 dispatch ready 文本需在 main.c 里；本 test 同时扫 main.c
 * 以便覆盖 main.c 路径）/irq_probe.c 后自然转 GREEN。
 *
 * 注：host test 路径只读 gic.c / main.c 源码字符串，不编译 kernel，
 * 不碰 Device MMIO；QEMU 跑不起来 = 镜像缺失本身即是 Task 2.1 的 RED 证据，
 * hosttest 仅作为 harness 正确性的并行校验。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "test_framework.h"

/* ── 源文件路径（由 Makefile 通过 -DOS01_KERNEL_SRC=... 注入，绝对路径）── */
#ifndef OS01_KERNEL_SRC
#error "OS01_KERNEL_SRC must be defined to the kernel source root"
#endif

/* ── 测试用例：每个 marker 字符串对（kernel 文件路径, 字面量）── */
typedef struct {
    const char *label;       /* 描述，打印用 */
    const char *path;        /* kernel 源码相对路径（拼到 OS01_KERNEL_SRC）*/
    const char *needle;      /* 必须出现在文件中的字面量 */
} marker_check_t;

/* 4 个 spec §7.2 marker —— Task 2.1 RED 期望：intids= 在（Task 1.2），
 * 其余 3 个暂不在源里（Task 2.2 才补）。dispatch ready 在 main.c 里
 * （OS01_SELFTEST 门控区）；其余在 irq_probe.c（Task 2.2 才创建）。
 * 本 RED 只检查 gic.c / main.c / irq_probe.c 已存在的三个文件即可证
 * 3-marker missing → RED fail。 */
static const marker_check_t kChecks[] = {
    {
        "marker: [gic] GICv2 driver: intids=",
        "kernel/arch/aarch64/intr/gic.c",
        "[gic] GICv2 driver: intids=",
    },
    {
        "marker: [gic] dispatch ready  (Task 2.2 will emit from main.c)",
        "kernel/arch/aarch64/boot/main.c",
        "[gic] dispatch ready",
    },
    {
        "marker: [gic-probe] save-restore OK  (Task 2.2 will emit from irq_probe.c — file may not exist yet)",
        "kernel/arch/aarch64/intr/irq_probe.c",
        "[gic-probe] save-restore OK",
    },
    {
        "marker: [gic-probe] unexpected intid=40 survived  (Task 2.2 will emit from irq_probe.c)",
        "kernel/arch/aarch64/intr/irq_probe.c",
        "[gic-probe] unexpected intid=40 survived",
    },
};
static const int kChecksCount = (int)(sizeof(kChecks) / sizeof(kChecks[0]));

/* ── 把 path 整个读到 buffer（malloc）。返回 0 ok / -1 fail ── */
static int slurp_file(const char *path, char **out_buf, size_t *out_len)
{
    /* Avoid fseek/ftell — libc stdio.h shim (force-included via
     * test_platform.h) doesn't expose them. fread-grow loop works
     * against any stdio.h that defines fread(). */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("  [INFO] cannot open %s (file may not exist yet, RED-allowed)\n",
               path);
        *out_buf = NULL;
        *out_len = 0;
        return -1;
    }
    size_t cap = 4096;
    char *buf = (char *)malloc(cap);
    if (!buf) { fclose(fp); return -1; }
    size_t n = 0;
    for (;;) {
        if (n + 1024 > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) { free(buf); fclose(fp); return -1; }
            buf = nb; cap = new_cap;
        }
        size_t got = fread(buf + n, 1, 1024, fp);
        n += got;
        if (got < 1024) break;            /* EOF or error */
    }
    fclose(fp);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return 0;
}

/* 简单的子串包含（不需要 stdlib strstr 边界外的依赖；strstr 已 include） */
static bool contains(const char *hay, size_t hay_len, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    if (nl > hay_len) return false;
    for (size_t i = 0; i + nl <= hay_len; ++i) {
        if (memcmp(hay + i, needle, nl) == 0) return true;
    }
    return false;
}

static void test_marker_lines_in_source(void)
{
    TEST_SUITE("gic marker strings present in kernel source");

    for (int i = 0; i < kChecksCount; ++i) {
        const marker_check_t *c = &kChecks[i];
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 OS01_KERNEL_SRC, c->path);
        char *buf = NULL;
        size_t len = 0;
        int rc = slurp_file(full_path, &buf, &len);
        if (rc != 0) {
            /* 文件不存在 —— RED 阶段允许（Task 2.2 才创建 irq_probe.c）；
             * 仍 fail 而不是 skip，让 RED 证据显式。 */
            printf("  [FAIL] %s: source file missing: %s\n",
                   c->label, full_path);
            __test_stats.failed++;
            __test_stats.total++;
            continue;
        }
        bool hit = contains(buf, len, c->needle);
        if (hit) {
            printf("  [PASS] %s: found in %s\n", c->label, c->path);
            __test_stats.passed++;
        } else {
            printf("  [FAIL] %s: NOT FOUND in %s (Task 2.2 GREEN will add)\n",
                   c->label, c->path);
            __test_stats.failed++;
        }
        __test_stats.total++;
        free(buf);
    }
}

int main(void)
{
    /* Call suite directly — RUN_ALL_TESTS() resets __test_stats via
     * TEST_RESULTS() before we can read it, which masks the exit code.
     * Match test_gic_driver's pattern: call suites directly, then read
     * stats before returning. */
    test_marker_lines_in_source();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_gic_marker_lines",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}