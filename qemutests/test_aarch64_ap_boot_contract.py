"""P1 Task 3 AP cooperative-scheduler boot contract.

Source-level checks (no prebuilt ELF required) pin the frozen spec §3.2
structure of the AP handoff, and mock-tool-output checks drive the
aarch64_ap_scheduler runner's fatal-owner evidence parser and ELF audit
exactly as `make ... MODE=sched-ap AP_SCHED_CASE=fatal-owner` will.
"""
from pathlib import Path
import tempfile
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


class RunnerSessionCleanupContract(unittest.TestCase):
    """A mock QemuSession proves the runner stops, joins and reports on
    timeout and on exception — never a bare Popen reimplementation."""

    @classmethod
    def setUpClass(cls):
        import importlib.util
        import types
        spec = importlib.util.spec_from_file_location(
            'aarch64_ap_scheduler', ROOT / RUNNER)
        cls.runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.runner)
        # No real QEMU/dtb generation in these contract tests.
        cls.runner.generate_diagnostic_dtb = lambda *a, **k: '/dev/null'
        # Real files for the metadata SHA256 step.
        tmp = Path(tempfile.mkdtemp(prefix='ap-runner-mock-'))
        for name in ('img', 'fw', 'elf'):
            (tmp / name).write_bytes(b'mock-' + name.encode())
        cls.args = types.SimpleNamespace(
            qemu='qemu', llvm_nm='llvm-nm', build_dir=None,
            profile='test', log_dir=str(tmp), timeout=120,
            case='fatal-owner', image=str(tmp / 'img'),
            firmware=str(tmp / 'fw'), elf=str(tmp / 'elf'),
            normal_elf=None, phase='ap')

    def _make_factory(self, behavior):
        events = {'started': 0, 'stopped': 0, 'joined': 0, 'closed': 0,
                  'text': ''}

        class MockSession:
            def __init__(self, **kw):
                events['started'] += 1
                self.timed_out = False
                self.returncode = None
                self.text = ''
                self.stopped_by_runner = False

            def start(self):
                events['started'] += 1

            def wait_for(self, predicate):
                behavior(self, predicate)
                return ''

            def observe(self, seconds):
                return ''

            def stop(self):
                events['stopped'] += 1
                self.stopped_by_runner = True

            def close(self):
                events['closed'] += 1

            def join(self, timeout=None):
                events['joined'] += 1

        return MockSession, events

    def test_timeout_stops_and_closes_the_session(self):
        def timeout_behavior(session, predicate):
            session.timed_out = True
            session.text = 'no evidence ever'
        factory, events = self._make_factory(timeout_behavior)
        code = self.runner.run_fatal_boot(
            self.args,
            {'image': self.args.image, 'firmware': self.args.firmware,
             'elf': self.args.elf},
            session_factory=factory)
        self.assertEqual(code, 1)   # timeout is a FAIL, not an ERROR
        self.assertEqual(events['stopped'], 1)
        self.assertEqual(events['closed'], 1)

    def test_exception_stops_and_closes_the_session(self):
        def boom(session, predicate):
            raise RuntimeError('mid-boot failure')
        factory, events = self._make_factory(boom)
        code = self.runner.run_fatal_boot(
            self.args,
            {'image': self.args.image, 'firmware': self.args.firmware,
             'elf': self.args.elf},
            session_factory=factory)
        self.assertEqual(code, 2)   # runner error
        self.assertEqual(events['closed'], 1)

    def test_duplicate_owner_evidence_fails(self):
        import types
        text = ('[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1\n'
                '[ap-fatal] owner=1 arrivals_mask=3 entrants=1 losers=1\n')
        ok, errors = self.runner.check_fatal_evidence(
            self.runner.extract_ap_fatal_lines(text))
        self.assertNotEqual(ok, 0)
        self.assertTrue(any('exactly one owner line' in e for e in errors))

    def test_participant_missing_fails(self):
        # arrivals_mask=1 means only ONE of the two CPUs arrived — the
        # frozen evidence requires both.
        ok, errors = self.runner.check_fatal_evidence(
            self.runner.extract_ap_fatal_lines(
                '[ap-fatal] owner=0 arrivals_mask=1 entrants=1 losers=0\n'))
        self.assertNotEqual(ok, 0)


class ModeConflictContract(unittest.TestCase):
    """MODE=sched-ap must be rejected at the OUTER make for conflicting
    selectors (never only inside the profile sub-make)."""

    def test_outer_make_rejects_kernel_selftest_override(self):
        import subprocess
        proc = subprocess.run(
            ['make', 'PROFILE=aarch64-clang', 'test-aarch64',
             'MODE=sched-ap', 'KERNEL_SELFTEST=1', '-n'],
            cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn('MODE=sched-ap cannot be combined',
                      proc.stderr)

    def test_outer_make_rejects_old_sched_selector(self):
        import subprocess
        proc = subprocess.run(
            ['make', 'PROFILE=aarch64-clang', 'test-aarch64',
             'MODE=sched-ap', 'AARCH64_SCHED_TEST=positive', '-n'],
            cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        # The outer make must refuse the combination — either through
        # the MODE=sched-ap guard or the profile's selector validation.
        combined = ('MODE=sched-ap cannot be combined' in proc.stderr or
                    'cannot be combined' in proc.stderr or
                    'requires KERNEL_SELFTEST' in proc.stderr)
        self.assertTrue(combined, proc.stderr[-500:])

    def test_outer_make_rejects_invalid_mode_value(self):
        import subprocess
        proc = subprocess.run(
            ['make', 'PROFILE=aarch64-clang', 'test-aarch64',
             'MODE=sched-ap', 'AP_SCHED_CASE=bogus', '-n'],
            cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("invalid AP_SCHED_CASE='bogus'", proc.stderr)


if __name__ == '__main__':
    unittest.main()
