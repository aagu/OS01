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
            self.assertEqual(ap_sched.parse_ap_start(line), (cpus,
                                                             (1 << cpus) - 1))

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
