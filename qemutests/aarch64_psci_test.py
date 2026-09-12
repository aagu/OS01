#!/usr/bin/env python3
"""Exercise the production PSCI policy through host-side conduit stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

RUNNER = r'''
#include <arch/aarch64/psci.h>
#include <stdint.h>

static uint64_t smc_result;
static uint64_t hvc_result;
static uint64_t last_fid, last_a1, last_a2, last_a3;
static unsigned calls;
static unsigned smc_calls;
static unsigned hvc_calls;

uint64_t psci_call_smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
    calls++; smc_calls++;
    last_fid = fid; last_a1 = a1; last_a2 = a2; last_a3 = a3;
    return smc_result;
}

uint64_t psci_call_hvc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
    calls++; hvc_calls++;
    last_fid = fid; last_a1 = a1; last_a2 = a2; last_a3 = a3;
    return hvc_result;
}

static int check(int ok) { return ok ? 0 : 1; }

int main(void)
{
    const uint64_t mpidr = UINT64_C(0x123456789abcdef0);
    const uint64_t entry = UINT64_C(0xffff000012345678);
    const uint64_t context = UINT64_C(0xfeedfacecafebeef);
    unsigned before;

    smc_result = UINT64_C(0x00000002);  /* PSCI 0.2 */
    if (check(psci_init(PSCI_CONDUIT_SMC) == 0)) return 10;
    if (check(calls == 1 && smc_calls == 1 && hvc_calls == 0)) return 11;
    if (check(last_fid == UINT64_C(0x84000000) && !last_a1 && !last_a2 && !last_a3)) return 12;
    smc_result = UINT64_C(0xfffffffffffffffc);  /* PSCI_ALREADY_ON */
    if (check(psci_cpu_on(mpidr, entry, context) == -4)) return 13;
    if (check(last_fid == UINT64_C(0xc4000003) && last_a1 == mpidr &&
              last_a2 == entry && last_a3 == context)) return 14;

    hvc_result = UINT64_C(0x00010000);  /* PSCI 1.0 */
    if (check(psci_init(PSCI_CONDUIT_HVC) == 0)) return 20;
    if (check(hvc_calls == 1 && last_fid == UINT64_C(0x84000000))) return 21;
    hvc_result = UINT64_C(0xfffffffffffffffd);
    if (check(psci_cpu_on(mpidr, entry, context) == -3 && hvc_calls == 2)) return 22;

    before = calls;
    if (check(psci_init(PSCI_CONDUIT_NONE) == -1)) return 30;
    if (check(calls == before && psci_cpu_on(mpidr, entry, context) == -1 && calls == before)) return 31;

    smc_result = UINT64_C(0x00000001);  /* PSCI 0.1 */
    if (check(psci_init(PSCI_CONDUIT_SMC) == -1)) return 40;
    before = calls;
    if (check(psci_cpu_on(mpidr, entry, context) == -1 && calls == before)) return 41;

    smc_result = UINT64_C(0xffffffffffffffff);  /* PSCI_NOT_SUPPORTED */
    if (check(psci_init(PSCI_CONDUIT_SMC) == -1)) return 50;
    before = calls;
    if (check(psci_cpu_on(mpidr, entry, context) == -1 && calls == before)) return 51;
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='aarch64-psci-') as tmp:
        tmp = Path(tmp)
        runner_c = tmp / 'runner.c'
        runner_c.write_text(RUNNER)
        runner = tmp / 'runner'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Ikernel/include', 'kernel/arch/aarch64/psci.c', str(runner_c),
            '-o', str(runner),
        ], cwd=ROOT, check=True)
        subprocess.run([str(runner)], check=True)
    print('aarch64_psci: policy and 64-bit conduit ABI passed')


if __name__ == '__main__':
    main()
