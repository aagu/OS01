"""P1 Task 4 unittests for the MODE=sched-ap positive runner parser:
frozen schema, per-CPU completeness, and the negative contract (a
missing CPU, a duplicate, a wrong mask, a resource over-count or a
half-finished release can never pass)."""
from pathlib import Path
import importlib.util
import unittest

ROOT = Path(__file__).resolve().parents[1]

_spec = importlib.util.spec_from_file_location(
    'aarch64_ap_scheduler', ROOT / 'qemutests' / 'aarch64_ap_scheduler.py')
ap_sched = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ap_sched)


def case_line(case, cpu, fields):
    kv = " ".join(f"{k}={v}" for k, v in fields.items())
    return f"[ap-sched] phase=ap case={case} cpu={cpu} {kv}"


def build_trace(cpus):
    """A complete, valid positive trace at @cpus."""
    lines = [f"[ap-sched] START phase=ap cpus={cpus} "
             f"accepted_mask={(1 << cpus) - 1}"]
    for cpu in range(cpus):
        lines += [
            case_line("ready", cpu, {"scheduler_ok": 1, "idle_on_cpu": 1,
                                     "idle_on_rq": 0, "unique_stack": 1}),
            case_line("entry", cpu, {"entries": 2, "wrong_cpu": 0,
                                     "bad_mask": 0, "bad_sp": 0,
                                     "bad_regs": 0}),
            case_line("yield", cpu, {"tasks": 2, "yields": 64,
                                     "bad_canary": 0, "irq_switches": 0}),
            case_line("reap", cpu, {"tasks": 256, "stack_alloc": 256,
                                    "stack_free": 256, "ctx_alloc": 256,
                                    "ctx_free": 256, "bad_stack": 0,
                                    "duplicate": 0, "overflow": 0,
                                    "idle_freed": 0}),
            case_line("work", cpu, {"pre_handoff": 1, "post_handoff": 1,
                                    "seq_ok": 1}),
        ]
    lines.append(
        case_line("admission", 0,
                  {"invalid_rejected": 1, "zero_publish": 1,
                   "unready_rejected": 0 if cpus == 1 else 1,
                   "unready_na": 1 if cpus == 1 else 0}))
    lines.append(
        case_line("default-mask", 0,
                  {"default_cpu0": 1,
                   "ap_to_bsp": 0 if cpus == 1 else 1,
                   "bsp_to_ap": 0 if cpus == 1 else 1,
                   "ap_to_ap": 0 if cpus == 1 else 1,
                   "na": 1 if cpus == 1 else 0}))
    lines.append("[ap-sched] END phase=ap failed=0")
    lines.append("[TEST] END suite=kernel-selftest total=12 passed=12 failed=0 skipped=0")
    return "\n".join(lines) + "\n"


class StartLineTests(unittest.TestCase):
    def test_start_accepts_full_masks(self):
        for cpus in (1, 2, 4, 8):
            line = (f"[ap-sched] START phase=ap cpus={cpus} "
                    f"accepted_mask={(1 << cpus) - 1}")
            self.assertEqual(ap_sched.parse_ap_start(line),
                             (cpus, (1 << cpus) - 1, 'ap'))

    def test_start_rejects_missing_ap_downgrade(self):
        # a 4-CPU boot whose mask claims only 2 accepted APs must never
        # parse: a missing AP may not downgrade to PASS.
        self.assertIsNone(ap_sched.parse_ap_start(
            "[ap-sched] START phase=ap cpus=4 accepted_mask=3"))
        self.assertIsNone(ap_sched.parse_ap_start(
            "[ap-sched] START phase=ap cpus=2 accepted_mask=0"))
        self.assertIsNone(ap_sched.parse_ap_start(
            "[ap-sched] START phase=ap cpus=2 accepted_mask=x"))


class PositiveGateTests(unittest.TestCase):
    def _ok(self, text, cpus):
        return ap_sched.check_ap_positive_evidence(text, cpus)

    def test_complete_traces_pass(self):
        for cpus in (1, 2, 4, 8):
            ok, errors = self._ok(build_trace(cpus), cpus)
            self.assertEqual((ok, errors), (0, []), f"cpus={cpus}")

    def test_missing_cpu_rejected(self):
        text = build_trace(2).replace(
            case_line("yield", 1, {"tasks": 2, "yields": 64,
                                   "bad_canary": 0, "irq_switches": 0}) + "\n",
            "")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)
        self.assertTrue(any("yield cpu=1" in e for e in errors))

    def test_duplicate_cpu_rejected(self):
        dup = case_line("entry", 0, {"entries": 2, "wrong_cpu": 0,
                                     "bad_mask": 0, "bad_sp": 0,
                                     "bad_regs": 0})
        text = build_trace(2).replace(
            "[ap-sched] END phase=ap failed=0", dup + "\n" +
            "[ap-sched] END phase=ap failed=0")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_half_release_rejected(self):
        # a worker that returned but whose stack was never released
        text = build_trace(1).replace("stack_free=256", "stack_free=255")
        ok, errors = self._ok(text, 1)
        self.assertNotEqual(ok, 0)

    def test_resource_overcount_rejected(self):
        text = build_trace(1).replace("ctx_alloc=256", "ctx_alloc=257")
        ok, errors = self._ok(text, 1)
        self.assertNotEqual(ok, 0)

    def test_mask_error_rejected(self):
        text = build_trace(2).replace("bad_mask=0", "bad_mask=1")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_wrong_cpu_count_rejected(self):
        ok, errors = self._ok(build_trace(2), 4)
        self.assertNotEqual(ok, 0)

    def test_missing_end_rejected(self):
        text = build_trace(1).replace("[ap-sched] END phase=ap failed=0\n",
                                      "")
        ok, errors = self._ok(text, 1)
        self.assertNotEqual(ok, 0)

    def test_failed_end_rejected(self):
        text = build_trace(1).replace(
            "[ap-sched] END phase=ap failed=0",
            "[ap-sched] END phase=ap failed=1")
        ok, errors = self._ok(text, 1)
        self.assertNotEqual(ok, 0)

    def test_admission_on_nonzero_cpu_rejected(self):
        text = build_trace(2).replace(
            "[ap-sched] END phase=ap failed=0",
            case_line("admission", 1,
                      {"invalid_rejected": 1, "zero_publish": 1,
                       "unready_rejected": 1, "unready_na": 0}) + "\n" +
            "[ap-sched] END phase=ap failed=0")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_up_na_semantics_pass_and_smp_na_rejected(self):
        ok, errors = self._ok(build_trace(1), 1)
        self.assertEqual((ok, errors), (0, []))
        # an SMP boot claiming na=1 (no-AP semantics) is a downgrade
        text = build_trace(2).replace("na=0", "na=1").replace(
            "ap_to_bsp=1", "ap_to_bsp=0").replace(
            "bsp_to_ap=1", "bsp_to_ap=0").replace(
            "ap_to_ap=1", "ap_to_ap=0")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_unknown_and_malformed_lines_rejected(self):
        base = build_trace(1)
        # Non-[ap-sched] kernel output is tolerated (real serial logs
        # carry boot banners between protocol lines), but an unknown
        # CASE line and malformed values are rejected.
        ok, _ = self._ok(base.replace(
            "[ap-sched] END phase=ap failed=0",
            "[clocksource] active=true\n"
            "[ap-sched] END phase=ap failed=0"), 1)
        self.assertEqual(ok, 0)
        ok, _ = self._ok(base.replace(
            "[ap-sched] END phase=ap failed=0",
            case_line("bogus", 0, {"x": 1}) +
            "\n[ap-sched] END phase=ap failed=0"), 1)
        self.assertNotEqual(ok, 0)
        ok, _ = self._ok(base.replace("yields=64", "yields=abc"), 1)
        self.assertNotEqual(ok, 0)
        ok, _ = self._ok(base.replace("tasks=2", "tasks=2 tasks=2"), 1)
        self.assertNotEqual(ok, 0)

    def test_missing_framework_end_rejected(self):
        text = build_trace(1).replace("[TEST] END suite=kernel-selftest total=12 passed=12 failed=0 skipped=0\n", "")
        ok, errors = self._ok(text, 1)
        self.assertNotEqual(ok, 0)


if __name__ == '__main__':
    unittest.main()


class WakeGateTests(unittest.TestCase):
    """P2 Task 4: the phase=wake envelope (P1-ready + six wake cases),
    per-CPU sgi rows and the UP NA shapes."""

    def _ok(self, text, cpus):
        return ap_sched.check_ap_positive_evidence(text, cpus, "wake")

    def _wake_trace(self, cpus):
        lines = build_trace(cpus).replace('phase=ap', 'phase=wake').splitlines()
        extra = []
        for cpu in (range(1, cpus) if cpus > 1 else (0,)):
            na = int(cpus == 1)
            rows = {
                'remote-wake': dict(rounds=100, completed=100, duplicate=0,
                                    explicit=100, scan=0, na=0),
                'wake-switch': dict(rounds=100, completed=100, duplicate=0,
                                    on_cpu_wakes=100, finish_enqueues=100, na=0),
                'idle-race': dict(before_check=100, after_check=100, at_wait=100,
                                  completed=300, lost=0, wfi_enter=100,
                                  wfi_return=100, na=0),
                'work-sleep': dict(ready_overlap=1, pending_before=100,
                                   after_wait=100, completed=201, bad_seq=0, na=0),
                'scan-fallback': dict(rounds=100, completed=100, explicit=0,
                                      scan=100, na=0),
            }
            for case, fields in rows.items():
                if na and case != 'scan-fallback':
                    fields = {key: int(key == 'na') for key in fields}
                extra.append(case_line(case, cpu, fields).replace('phase=ap', 'phase=wake'))
        for cpu in range(cpus):
            extra.append(case_line('sgi', cpu, dict(handler=3, eoi=3, bad_iar=0,
                              irq_switches=0, tlb_ack_ok=1)).replace('phase=ap', 'phase=wake'))
        lines[-2:-2] = extra
        return '\n'.join(lines) + '\n'

    def test_case_phase_must_match_envelope(self):
        text = self._wake_trace(2).replace('phase=wake case=entry', 'phase=ap case=entry')
        self.assertNotEqual(self._ok(text, 2)[0], 0)

    def test_wake_inherits_every_p1_case(self):
        lines = self._wake_trace(2).splitlines()
        text = '\n'.join(line for line in lines if 'case=reap ' not in line)
        self.assertNotEqual(self._ok(text, 2)[0], 0)

    def test_each_ap_requires_wake_evidence(self):
        text = '\n'.join(line for line in self._wake_trace(4).splitlines()
                         if 'case=remote-wake cpu=2 ' not in line)
        self.assertNotEqual(self._ok(text, 4)[0], 0)

    def test_unknown_field_and_cpu_rejected(self):
        for text in (self._wake_trace(2).replace('case=ready cpu=1', 'case=ready cpu=7'),
                     self._wake_trace(2).replace('bad_seq=0', 'bad_seq=0 invented=1')):
            self.assertNotEqual(self._ok(text, 2)[0], 0)

    def test_duplicate_end_and_outside_case_rejected(self):
        trace = self._wake_trace(2)
        for suffix in ('[ap-sched] END phase=wake failed=0\n',
                       '[ap-sched] phase=wake case=sgi cpu=1 handler=3 eoi=3 bad_iar=0 irq_switches=0 tlb_ack_ok=1\n'):
            self.assertNotEqual(self._ok(trace + suffix, 2)[0], 0)

    def test_complete_wake_traces_pass(self):
        for cpus in (1, 2, 4, 8):
            ok, errors = self._ok(self._wake_trace(cpus), cpus)
            self.assertEqual((ok, errors), (0, []), f"cpus={cpus}")

    def test_wrong_phase_rejected(self):
        text = self._wake_trace(2).replace(
            "phase=wake", "phase=ap")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_explicit_backed_by_scan_rejected(self):
        text = self._wake_trace(2).replace("scan=0", "scan=50")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_completed_short_by_one_rejected(self):
        text = self._wake_trace(2).replace("completed=100",
                                           "completed=99")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_missing_ap_sgi_row_rejected(self):
        text = self._wake_trace(2).replace(
            case_line("sgi", 1, {"handler": 3, "eoi": 3, "bad_iar": 0,
                                 "irq_switches": 0,
                                 "tlb_ack_ok": 1}).replace("phase=ap", "phase=wake") + "\n", "")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_eoi_mismatch_rejected(self):
        text = self._wake_trace(2).replace("eoi=3", "eoi=2")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

    def test_na_pseudo_success_rejected(self):
        # a 2-CPU boot claiming the UP na=1 shape is a downgrade
        text = self._wake_trace(2).replace("na=0", "na=1").replace(
            "rounds=100", "rounds=0").replace(
            "explicit=100", "explicit=0").replace(
            "scan=100", "scan=0").replace(
            "on_cpu_wakes=100", "on_cpu_wakes=0").replace(
            "finish_enqueues=100", "finish_enqueues=0").replace(
            "before_check=100", "before_check=0").replace(
            "after_check=100", "after_check=0").replace(
            "at_wait=100", "at_wait=0").replace(
            "completed=300", "completed=0").replace(
            "completed=200", "completed=0").replace(
            "ready_overlap=1", "ready_overlap=0").replace(
            "pending_before=100", "pending_before=0").replace(
            "after_wait=100", "after_wait=0").replace(
            "wfi_enter=100", "wfi_enter=0").replace(
            "wfi_return=100", "wfi_return=0")
        ok, errors = self._ok(text, 2)
        self.assertNotEqual(ok, 0)

class PreparedElfAuditTests(unittest.TestCase):
    def test_prepared_elf_wait_instructions_are_audited(self):
        from unittest import mock
        import tempfile
        import types
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / 'kernel.elf'
            elf.write_bytes(b'ELF')
            with mock.patch.object(ap_sched.subprocess, 'run', return_value=types.SimpleNamespace(
                    returncode=0, stdout='0000 <aarch64_idle_wait_irq>:\n  0:\tdsb\tsy\n  4:\twfi\n  8:\tret\n', stderr='')) as run:
                self.assertEqual(ap_sched.audit_idle_wait_elf(elf, 'llvm-objdump'), (1, '', None))
                self.assertIn('--disassemble-symbols=aarch64_idle_wait_irq', run.call_args.args[0])
            self.assertEqual(ap_sched.audit_idle_wait_elf(elf.parent / 'absent', 'llvm-objdump')[0], 0)
