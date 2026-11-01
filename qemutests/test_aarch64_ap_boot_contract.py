"""P1 Task 3 AP cooperative-scheduler boot contract.

Source-level checks (no prebuilt ELF required) pin the frozen spec §3.2
structure of the AP handoff, and mock-tool-output checks drive the
aarch64_ap_scheduler runner's fatal-owner evidence parser and ELF audit
exactly as `make ... MODE=sched-ap AP_SCHED_CASE=fatal-owner` will.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

AP_SCHED_C = 'kernel/arch/aarch64/cpu/ap_sched.c'
AP_SCHED_H = 'kernel/include/arch/aarch64/ap_sched.h'
SMP_C = 'kernel/arch/aarch64/smp/smp.c'
SMP_H = 'kernel/include/arch/aarch64/smp.h'
TASK_ARCH_C = 'kernel/arch/aarch64/cpu/task_arch.c'
THREAD_ENTRY_S = 'kernel/arch/aarch64/cpu/thread_entry.S'
IDLE_C = 'kernel/arch/aarch64/cpu/idle.c'
BOOT_MAIN_C = 'kernel/arch/aarch64/boot/main.c'
KTHREAD_C = 'kernel/sched/kthread.c'
RUNNER = 'qemutests/aarch64_ap_scheduler.py'


def src(rel):
    return (ROOT / rel).read_text()


class ApHandoffSourceContract(unittest.TestCase):
    """The frozen §3.2 interface exists and is wired in the right order."""

    def test_ap_sched_module_exists_with_frozen_interface(self):
        header = src(AP_SCHED_H)
        for decl in (
            'void aarch64_ap_sched_prepare(void);',
            'void aarch64_ap_sched_release(void);',
            'bool aarch64_ap_sched_released(uint32_t cpu);',
            'void aarch64_ap_sched_enter(uint32_t cpu)',
            'void aarch64_ap_idle_handoff(uint32_t cpu)',
            'void aarch64_ap_stack_enter(uint64_t sp, thread_t *ctx, '
            'uint32_t cpu)',
        ):
            self.assertIn(decl, header, decl)

    def test_release_gate_checks_frozen_admission_bit(self):
        body = src(AP_SCHED_C)
        self.assertIn('aarch64_smp_accepted_mask', body)
        # a released(cpu) that ignores admission would admit a late or
        # failed AP — both must read the frozen mask.
        self.assertIn('aarch64_ap_sched_released', body)
        released = body.split('bool aarch64_ap_sched_released', 1)[1]
        released = released.split('}', 1)[0]
        self.assertIn('aarch64_smp_accepted_mask', released)

    def test_prepare_constructs_permanent_idles_without_current(self):
        body = src(AP_SCHED_C)
        self.assertIn('aarch64_ap_sched_prepare', body)
        # prepare initialises independent list/rb nodes and inserts the
        # idle once under the global task-list lock.
        self.assertIn('list_init', body)
        self.assertIn('task_list_lock', body)
        self.assertIn('idle_resume', body)
        prepare = body.split('void aarch64_ap_sched_prepare', 1)[1]
        prepare = prepare.split('\n}', 1)[0]
        # The boot stack has no current and the constructor owns PID
        # allocation — neither may appear in prepare.
        for banned in ('arch_current_task', 'get_current_task',
                       'alloc_pid', 'current->'):
            self.assertNotIn(banned, prepare, banned)

    def test_secondary_idle_polls_the_release_gate(self):
        body = src(SMP_C)
        loop = body.split('void secondary_idle', 1)[1]
        loop = loop.split('\n}', 1)[0]
        self.assertIn('aarch64_ap_sched_released', loop)
        self.assertIn('aarch64_ap_sched_enter', loop)

    def test_bsp_releases_after_publishing_own_readiness(self):
        body = src(TASK_ARCH_C)
        handoff = body.split('void arch_aarch64_bsp_handoff', 1)[1]
        handoff = handoff.split('\n}', 1)[0]
        start = handoff.index('sched_boot_start')
        release = handoff.index('aarch64_ap_sched_release')
        self.assertLess(start, release,
                        'AP gate must open only after BSP scheduler_ok')

    def test_bsp_prepare_runs_before_the_stack_switch(self):
        body = src(BOOT_MAIN_C)
        prepare = body.index('aarch64_ap_sched_prepare')
        bsp_prep = body.index('arch_aarch64_prepare_bsp_handoff')
        self.assertLess(prepare, bsp_prep,
                        'AP idle construction happens on the boot stack')

    def test_bsp_waits_for_accepted_ap_readiness(self):
        body = src(AP_SCHED_C)
        release = body.split('void aarch64_ap_sched_release', 1)[1]
        release = release.split('\n}', 1)[0]
        self.assertIn('scheduler_ok', release)
        self.assertIn('arch_cycle_counter', release)
        self.assertIn('arch_cycle_freq', body)

    def test_handoff_installs_idle_and_publishes_scheduler_ok(self):
        body = src(AP_SCHED_C)
        handoff = body.split('void aarch64_ap_idle_handoff', 1)[1]
        handoff = handoff.split('\n}', 1)[0]
        self.assertIn('rq->idle', handoff.replace(' ', '') or handoff)
        self.assertIn('scheduler_ok', handoff)

    def test_ap_trampoline_validates_stack_geometry(self):
        body = src(THREAD_ENTRY_S)
        self.assertIn('aarch64_ap_stack_enter', body)
        tramp = body.split('aarch64_ap_stack_enter:', 1)[1]
        tramp = tramp.split('.size', 1)[0]
        self.assertIn('aarch64_ap_idle_handoff', tramp)
        self.assertIn('b.ne', tramp)

    def test_arm_idle_polls_ap_work_and_pause_without_wfi(self):
        body = src(IDLE_C)
        self.assertIn('ap_work_run_one', body)
        self.assertNotIn('wfi', body)

    def test_kthread_prepare_accepts_verified_ap_targets(self):
        body = src(KTHREAD_C)
        # The aarch64 arch hook relaxes the CPU0-only rejection in T3;
        # the COMMON constructor still validates online + scheduler_ok.
        self.assertIn('__atomic_load_n(&percpu_data[target_cpu].scheduler_ok',
                      body)


class AcceptedMaskContract(unittest.TestCase):
    def test_smp_header_declares_accepted_mask(self):
        self.assertIn('uint64_t aarch64_smp_accepted_mask(void);', src(SMP_H))

    def test_frozen_from_boot_result_in_all_paths(self):
        body = src(SMP_C)
        fn = body.split('uint64_t aarch64_smp_accepted_mask', 1)[1]
        fn = fn.split('\n}', 1)[0]
        self.assertIn('online_mask', fn)
        # UP / PSCI-unavailable: a static result that was never written
        # must never be returned.
        self.assertIn('boot_result.online', fn)


class FatalOwnerEvidenceContract(unittest.TestCase):
    """Mock-tool-output unittests for the fatal-owner runner branch."""

    @classmethod
    def setUpClass(cls):
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            'aarch64_ap_scheduler', ROOT / RUNNER)
        cls.runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.runner)

    def test_fatal_line_parser_accepts_the_exact_evidence(self):
        parse = self.runner.parse_ap_fatal_line
        self.assertEqual(parse('[ap-fatal] owner=0 arrivals_mask=3 '
                               'entrants=1 losers=1'), (0, 3, 1, 1))
        self.assertEqual(parse('[ap-fatal] owner=1 arrivals_mask=3 '
                               'entrants=1 losers=1'), (1, 3, 1, 1))

    def test_fatal_line_parser_rejects_wrong_values(self):
        parse = self.runner.parse_ap_fatal_line
        for line in (
            '[ap-fatal] owner=2 arrivals_mask=3 entrants=1 losers=1',
            '[ap-fatal] owner=0 arrivals_mask=1 entrants=1 losers=1',
            '[ap-fatal] owner=0 arrivals_mask=3 entrants=2 losers=1',
            '[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=0',
            '[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=2',
            '[ap-fatal] owner=0 arrivals_mask=3 entrants=1',
            'owner=0 arrivals_mask=3 entrants=1 losers=1',
        ):
            self.assertIsNone(parse(line), line)

    def test_fatal_evidence_gate_requires_exactly_one_owner_line(self):
        ok, errors = self.runner.check_fatal_evidence([
            '[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1'])
        self.assertEqual((ok, errors), (0, []))
        ok, errors = self.runner.check_fatal_evidence([])
        self.assertNotEqual(ok, 0)
        ok, errors = self.runner.check_fatal_evidence([
            '[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1',
            '[ap-fatal] owner=1 arrivals_mask=3 entrants=1 losers=1'])
        self.assertNotEqual(ok, 0)

    def test_fatal_boot_must_not_report_success_end(self):
        ok, errors = self.runner.check_fatal_evidence([
            '[TEST] END failed=0',
            '[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1'])
        self.assertNotEqual(ok, 0)

    def test_normal_elf_audit_rejects_injection_symbols(self):
        class FakeNM:
            def __init__(self, symbols):
                self.symbols = symbols
                self.calls = []

            def __call__(self, _nm, elf):
                self.calls.append(elf)
                return 0, ''.join(f'00000000 T {s}\n'
                                  for s in self.symbols), ''

        # Audit convention: ok==1 means the ELF is clean; any injection
        # symbol is a failure (ok==0) naming the symbol.
        nm = FakeNM(['aarch64_ap_fatal_owner_enter',
                     'aarch64_context_fatal_asm'])
        ok, err, _sha = self.runner.audit_no_injection_symbols(
            nm, 'kernel.elf', 'llvm-nm')
        self.assertEqual(ok, 0)
        self.assertIn('aarch64_ap_fatal_owner_enter', err)

        nm = FakeNM(['aarch64_context_fatal_asm', 'idle_resume'])
        ok, err, _sha = self.runner.audit_no_injection_symbols(
            nm, 'kernel.elf', 'llvm-nm')
        self.assertEqual((ok, err), (1, ''))

    def test_missing_elf_fails_the_audit(self):
        ok, err, _sha = self.runner.audit_no_injection_symbols(
            None, '/nonexistent/kernel.elf', 'llvm-nm')
        self.assertEqual(ok, 0)
        self.assertIn('ELF not found', err)


if __name__ == '__main__':
    unittest.main()
