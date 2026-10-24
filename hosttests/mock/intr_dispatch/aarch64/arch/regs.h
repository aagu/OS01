#ifndef _HOSTTEST_MOCK_ARCH_REGS_H
#define _HOSTTEST_MOCK_ARCH_REGS_H

// ─────────────────────────────────────────────────────────
//  hosttests/mock/intr_dispatch/aarch64/arch/regs.h
//
//  Shadows the arch-neutral facade kernel/include/arch/regs.h for the
//  aarch64 interrupt-decode host test ONLY.
//
//  On an x86_64 HOST the facade selects arch/x86_64/regs.h through
//  __x86_64__, so the aarch64 test would silently compile against the
//  x86 pt_regs_t layout.  This mock directory is placed on the include
//  path AHEAD of kernel/include for that one binary, so <arch/regs.h>
//  resolves here; the shim then re-includes the PRODUCTION
//  arch/aarch64/regs.h.
//
//  The production header is found because this mock tree has no
//  arch/aarch64/ directory: the include search continues into
//  kernel/include, where arch/aarch64/regs.h lives.
//
//  Deliberately contains NO copy of the register struct and NO ISA
//  macro override — the guard name below differs from the production
//  guard (_KERNEL_ARCH_AARCH64_REGS_H) so the shim is never skipped.
// ─────────────────────────────────────────────────────────

#include <arch/aarch64/regs.h>

#endif /* _HOSTTEST_MOCK_ARCH_REGS_H */
