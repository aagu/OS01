/* aarch64 phase 1: minimal PSCI driver (Task 4b).
 *
 * PSCI is the ARM standard for CPU power management.  QEMU's `virt`
 * machine provides EL3 firmware that handles SMC #0 calls; we issue
 * smc #0 with the function id in x0 and the rest of the calling
 * convention in x1..x3 per the PSCI 0.2 spec (DEN 0022A).
 *
 *   PSCI_VERSION         (0x84000000):  x0=id, x0<-version
 *   PSCI_CPU_ON_aarch64  (0xC4000003):  x0=id, x1=target_mpidr,
 *                                       x2=entry_addr, x3=context_id,
 *                                       x0<-int32 return value
 *
 * Return values (low 32 bits of x0):
 *   0       PSCI_SUCCESS
 *   -1      PSCI_NOT_SUPPORTED
 *   -2      PSCI_INVALID_PARAMETERS
 *   -3      PSCI_DENIED
 *   -4      PSCI_ALREADY_ON          (target already running)
 *   -5      PSCI_ON_PENDING
 *   -6      PSCI_INTERNAL_FAILURE
 *   ...
 *
 * We do not enable the GIC's SGI path; cpu_on enters the secondary at
 * its physical entry address with MMU off.  The trampoline
 * (`secondary_start` in head.S) is responsible for enabling the MMU
 * once the BSP has cleared .boot.bss and built page tables.
 *
 * Reference: PSCI 0.2 (DEN 0022A), SMC Calling Conventions (DEN 0028A).
 */

#include <stdint.h>
#include <stdbool.h>

/* Forward from pl011.c — panic path. */
void kputs(const char *s);
void kputx(uint64_t v);
void kputu(uint64_t v);

/* ── PSCI 0.2 function IDs (32-bit) ─────────────────────────────────
 * The SMC32 variants take parameters in 32-bit registers (we zero-extend
 * x1/x2 to 64-bit on entry).  The SMC64 variants of cpu_on take 64-bit
 * addresses.  PSCI 0.2 mandates both; PSCI 1.x deprecates SMC32 cpu_on.
 *
 * For cpu_on we use SMC64 (C4000003) so the entry address can be a full
 * 64-bit physical. */
#define PSCI_VERSION                0x84000000U
/* Use the SMC32 variant of CPU_ON (0x84000003).  SMC64
 * (0xC4000003) requires the SMC64 conduit, which QEMU's HVC
 * trap does not always implement in the same way; the SMC32
 * variant works for both 32-bit and 64-bit callers as long as
 * the arguments fit in 32 bits, which is fine for QEMU virt
 * (kernel image below 4 GiB, MPIDR < 32 bits). */
#define PSCI_0_2_FN_CPU_ON          0x84000003U

/* PSCI return codes (low 32 bits of x0). */
#define PSCI_SUCCESS                0
#define PSCI_NOT_SUPPORTED          ((int32_t)-1)
#define PSCI_INVALID_PARAMETERS     ((int32_t)-2)
#define PSCI_DENIED                 ((int32_t)-3)
#define PSCI_ALREADY_ON             ((int32_t)-4)
#define PSCI_ON_PENDING             ((int32_t)-5)
#define PSCI_INTERNAL_FAILURE       ((int32_t)-6)
#define PSCI_NOT_PRESENT            ((int32_t)-7)
#define PSCI_DISABLED               ((int32_t)-8)
#define PSCI_INVALID_ADDRESS        ((int32_t)-9)

/* Decode PSCI return code for logging. */
__attribute__((used))
static const char *psci_rc_name(int32_t rc)
{
    switch (rc) {
    case PSCI_SUCCESS:              return "SUCCESS";
    case PSCI_NOT_SUPPORTED:        return "NOT_SUPPORTED";
    case PSCI_INVALID_PARAMETERS:   return "INVALID_PARAMETERS";
    case PSCI_DENIED:               return "DENIED";
    case PSCI_ALREADY_ON:           return "ALREADY_ON";
    case PSCI_ON_PENDING:           return "ON_PENDING";
    case PSCI_INTERNAL_FAILURE:     return "INTERNAL_FAILURE";
    case PSCI_NOT_PRESENT:          return "NOT_PRESENT";
    case PSCI_DISABLED:             return "DISABLED";
    case PSCI_INVALID_ADDRESS:      return "INVALID_ADDRESS";
    default:                        return "?";
    }
}

/* Issue an SMC #1 with the supplied arguments and return the
 * 32-bit return value from x0.  We use SMC (not HVC) because
 * QEMU's PSCI implementation is reached via SMC traps on the
 * virt machine; HVC works on some configurations but not all.
 *
 * Implementation note: there are two subtleties with clang/LLVM
 * inline asm and the SMC instruction.
 *
 *   1. clang treats `smc #0` as a register-form operand when it appears
 *      in inline asm alongside register operands — the immediate is
 *      silently overwritten with the OUTPUT register number, producing
 *      a wrong encoding.  We work around this with `.inst <raw>`.
 *
 *   2. SMC's encoding is `1101 0100 000 (imm16<<5) 00011`.  With
 *      imm16=0, the result (0xD4000003) is UNPREDICTABLE per the
 *      ARM ARM (D17.2.119) and QEMU treats it as #UD.  We use imm16=1
 *      (0xD4000023) instead — QEMU's SMC trap handler ignores the
 *      immediate, so this is functionally equivalent to smc #0.
 *
 * SMC requires EL3 to be implemented in the guest.  Use
 * `-M virt,secure=on` so QEMU provides the EL3 firmware. */
static int32_t psci_call(uint32_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint64_t x0 = (uint64_t)fid;
    __asm__ __volatile__(
        "mov x1, %1\n\t"
        "mov x2, %2\n\t"
        "mov x3, %3\n\t"
        ".inst 0xD4000023\n\t"   /* smc #1 (smc #0 is UNPREDICTABLE) */
        : "+r"(x0)
        : "r"(a1), "r"(a2), "r"(a3)
        : "x1", "x2", "x3", "x4", "x5", "x6", "x7",
          "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
          "x16", "x17", "memory"
    );
    return (int32_t)x0;
}

/* Probe PSCI version.  Returns the integer (1.0 → 0x00010xxx, 0.2 → 0x0002xxxx),
 * or -1 if unsupported. */
int32_t psci_version(void)
{
    int32_t rc = psci_call(PSCI_VERSION, 0, 0, 0);
    return rc;
}

/* Bring `target_mpidr` up at the supplied physical `entry` (with
 * context_id in x3 — passed verbatim to the secondary).  Returns the
 * PSCI return code (PSCI_SUCCESS = 0; PSCI_ALREADY_ON = -4 if the
 * BSP mistakenly called this on itself). */
int32_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry, uint64_t context_id)
{
    return psci_call(PSCI_0_2_FN_CPU_ON, target_mpidr, entry, context_id);
}

/* Print a one-line confirmation that PSCI is alive.  Called from
 * smp_boot_aps() before iterating the DTB table. */
void psci_init(void)
{
    int32_t v = psci_version();
    kputs("[psci] version=");
    if (v < 0) {
        kputs("NOT_SUPPORTED rc=");
        kputx((uint64_t)(uint32_t)v);
        kputs("\n");
        return;
    }
    /* 1.0 = 0x0001_0000; 0.2 = 0x0002_0000.  Print major.minor. */
    uint32_t major = (uint32_t)v & 0xFFFFU;
    uint32_t minor = ((uint32_t)v >> 16) & 0xFFFFU;
    kputu(major);
    kputs(".");
    kputu(minor);
    kputs("\n");
}