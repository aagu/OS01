#!/usr/bin/env python3
"""Unit tests for the aarch64 per-CPU tick evidence runner (Task 3).

The suite is deliberately negative-heavy: the runner is a GATE, so the
interesting property is not "a good trace passes" but "every broken
trace fails".  The covered failure modes are the ones the plan freezes:

  * a dropped AP row, a duplicated CPU row and a mixed-round row;
  * an AP that claims a global tick;
  * ``busy`` and ``runtime`` that disagree;
  * a switch inside the deadline IRQ window;
  * a missing END, an unknown case ID and a duplicated case row;
  * the wrong SMP count and a wrong admission mask;
  * a FATAL / panic line and a failed late case;
  * a boot that exceeds its absolute deadline — a fake QEMU that keeps
    printing ticks can never extend the budget (the deadline is
    shortened here instead of really waiting 120 s);
  * a QEMU that exits before the runner completed its drain;
  * artifacts that are not an AArch64 ELF, that lack the dedicated
    selftest symbols, or that come from another variant / profile.
"""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import aarch64_sched_tick as runner  # noqa: E402


def _elf_bytes(machine=runner.EM_AARCH64):
    """A minimal ELF64 little-endian header with the given e_machine."""
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2          # ELFCLASS64
    header[5] = 1          # ELFDATA2LSB
    header[6] = 1          # EV_CURRENT
    header[16:18] = (2).to_bytes(2, "little")   # e_type = ET_EXEC
    header[18:20] = machine.to_bytes(2, "little")
    return bytes(header)


class EvidenceParserTests(unittest.TestCase):
    """The strict evidence gate."""

    def _ok(self, text, cpus):
        return runner.parse_sched_tick_evidence(text, cpus)

    def _good(self, cpus=2):
        return runner._fixture(cpus)

    def _bad(self, text, cpus=2):
        ok, errors = self._ok(text, cpus)
        self.assertFalse(ok, errors)
        self.assertTrue(errors)

    def test_complete_traces_pass(self):
        for cpus in (1, 2, 4, 8):
            ok, errors = self._ok(self._good(cpus), cpus)
            self.assertTrue(ok, errors)
            self.assertEqual(errors, [], f"cpus={cpus}")

    def test_missing_ap_row_rejected(self):
        self._bad(self._good().replace(
            "[sched-tick] case=tick-busy cpu=1 samples=7 busy=7 idle=0 "
            "runtime=7 vruntime=7\n", ""))

    def test_duplicate_cpu_row_rejected(self):
        self._bad(self._good().replace(
            "[sched-tick] case=tick-busy cpu=1 samples=7 busy=7 idle=0 "
            "runtime=7 vruntime=7\n",
            "[sched-tick] case=tick-busy cpu=1 samples=7 busy=7 idle=0 "
            "runtime=7 vruntime=7\n"
            "[sched-tick] case=tick-busy cpu=1 samples=9 busy=9 idle=0 "
            "runtime=9 vruntime=9\n"))

    def test_mixed_round_rows_rejected(self):
        """A row of one case carrying another case's fields is rejected
        by the frozen per-case schema."""
        self._bad(self._good().replace(
            "[sched-tick] case=tick-busy cpu=0 samples=7 busy=7 idle=0 "
            "runtime=7 vruntime=7",
            "[sched-tick] case=tick-busy cpu=0 started=1 valid=1 hz=100"))

    def test_ap_global_tick_rejected(self):
        self._bad(self._good().replace(
            "case=tick-global cpu=1 local_samples=10 busy=7 idle=3 "
            "global_ticks=0",
            "case=tick-global cpu=1 local_samples=10 busy=7 idle=3 "
            "global_ticks=4"))

    def test_empty_global_window_rejected(self):
        """An empty window (0 == 0 + 0) proves nothing and is rejected."""
        self._bad(self._good().replace(
            "case=tick-global cpu=1 local_samples=10 busy=7 idle=3 "
            "global_ticks=0",
            "case=tick-global cpu=1 local_samples=0 busy=0 idle=0 "
            "global_ticks=0"))

    def test_bsp_local_samples_must_match_global_ticks(self):
        """Every BSP tick IS a global tick: the local samples of the
        window and the global tick count must be equal."""
        self._bad(self._good().replace(
            "case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
            "global_ticks=10 jiffies_delta=10",
            "case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
            "global_ticks=4 jiffies_delta=4"))

    def test_bsp_jiffies_delta_must_match_global_ticks(self):
        self._bad(self._good().replace(
            "case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
            "global_ticks=10 jiffies_delta=10",
            "case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
            "global_ticks=10 jiffies_delta=9"))

    def test_busy_runtime_mismatch_rejected(self):
        self._bad(self._good().replace(
            "case=tick-busy cpu=0 samples=7 busy=7 idle=0 runtime=7 vruntime=7",
            "case=tick-busy cpu=0 samples=7 busy=7 idle=0 runtime=8 vruntime=7"))

    def test_irq_switches_rejected(self):
        self._bad(self._good().replace(
            "case=tick-deadline cpu=0 need=1 irq_switches=0",
            "case=tick-deadline cpu=0 need=1 irq_switches=1"))

    def test_missing_end_rejected(self):
        self._bad(self._good().replace("[sched-tick] END failed=0\n", ""))

    def test_failed_end_rejected(self):
        self._bad(self._good().replace("[sched-tick] END failed=0",
                                       "[sched-tick] END failed=1"))

    def test_unknown_case_id_rejected(self):
        self._bad(self._good().replace("[sched-tick] case=tick-ready cpu=0",
                                       "[sched-tick] case=tick-ghost cpu=0"))

    def test_duplicate_case_line_rejected(self):
        self._bad(self._good() +
                  "[sched-tick] case=tick-ready cpu=0 started=1 valid=1 hz=100\n")

    def test_wrong_smp_count_rejected(self):
        self._bad(self._good().replace("[sched-tick] START cpus=2",
                                       "[sched-tick] START cpus=4"))

    def test_wrong_admission_mask_rejected(self):
        for mask in ("1", "7"):
            with self.subTest(mask=mask):
                self._bad(self._good().replace("accepted_mask=3",
                                               f"accepted_mask={mask}"))

    def test_missing_field_rejected(self):
        self._bad(self._good().replace(" started=1 valid=1 hz=100",
                                       " valid=1 hz=100"))

    def test_unknown_field_rejected(self):
        self._bad(self._good().replace(" started=1 valid=1 hz=100",
                                       " started=1 valid=1 hz=100 invented=1"))

    def test_negative_value_rejected(self):
        self._bad(self._good().replace(" started=1 valid=1 hz=100",
                                       " started=1 valid=1 hz=-100"))

    def test_out_of_range_cpu_rejected(self):
        self._bad(self._good().replace("case=tick-busy cpu=1",
                                       "case=tick-busy cpu=9"))

    def test_fatal_and_panic_rejected(self):
        for marker in ("[aarch64] context fatal code=1 sp=0x1 base=0x2 top=0x3\n",
                       "kernel panic - not syncing: boom\n"):
            with self.subTest(marker=marker):
                self._bad(self._good() + marker)

    def test_failed_late_case_rejected(self):
        text = self._good().replace("[TEST] PASS tick-busy",
                                    "[TEST] FAIL tick-busy reason=boom")
        text = text.replace(f"passed={len(runner.EXPECTED_IDS)} failed=0",
                            f"passed={len(runner.EXPECTED_IDS) - 1} failed=1")
        self._bad(text)

    def test_missing_late_case_rejected(self):
        text = self._good().replace("[TEST] BEGIN tick-yield\n", "")
        text = text.replace("[TEST] PASS tick-yield\n", "")
        text = text.replace(f"expected={len(runner.EXPECTED_IDS)}",
                            f"expected={len(runner.EXPECTED_IDS) - 1}")
        text = text.replace(f"total={len(runner.EXPECTED_IDS)} "
                            f"passed={len(runner.EXPECTED_IDS)}",
                            f"total={len(runner.EXPECTED_IDS) - 1} "
                            f"passed={len(runner.EXPECTED_IDS) - 1}")
        self._bad(text)

    def test_evidence_after_record_late_rejected(self):
        text = self._good().replace(
            "[sched-tick] case=tick-ready cpu=0 started=1 valid=1 hz=100\n", "")
        text = text.replace(
            "[TEST] PASS tick-ready\n",
            "[TEST] PASS tick-ready\n"
            "[sched-tick] case=tick-ready cpu=0 started=1 valid=1 hz=100\n")
        self._bad(text)

    def test_end_before_selftest_end_rejected(self):
        text = self._good().replace(
            "[TEST] END suite=kernel-selftest",
            "[sched-tick] END failed=0\n[TEST] END suite=kernel-selftest")
        text = text.replace("[sched-tick] END failed=0\n[sched-tick] END "
                             "failed=0\n", "[sched-tick] END failed=0\n")
        self._bad(text)

    def test_empty_trace_rejected(self):
        self._bad("")

    def test_single_cpu_remote_is_na(self):
        ok, errors = self._ok(self._good(1), 1)
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])
        # A single-CPU boot may not claim a real remote observation.
        self._bad(self._good(1).replace(
            "case=tick-remote cpu=0 sgi_ok=0 tlb_ok=0 wfi_enter=0 "
            "timer_live=0 na=1",
            "case=tick-remote cpu=0 sgi_ok=1 tlb_ok=1 wfi_enter=1 "
            "timer_live=1 na=0"))


class ArtifactAuditTests(unittest.TestCase):
    """The artifact audit: content and provenance, never the file name."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.build = self.root / "build" / "aarch64-clang"
        (self.build / "kernel" / "sched-tick").mkdir(parents=True)
        (self.build / "image" / "sched-tick").mkdir(parents=True)
        self.elf = self.build / "kernel" / "sched-tick" / "kernel.elf"
        self.elf.write_bytes(_elf_bytes())
        self.image = self.build / "image" / "sched-tick" / "aarch64-uefi.img"
        self.image.write_bytes(b"image")
        self.firmware = self.build / "image" / "sched-tick" / "QEMU_EFI.fd"
        self.firmware.write_bytes(b"firmware")
        self.nm = self.root / "fake-nm"
        self.nm.write_text(
            "#!/bin/sh\n"
            "for s in " + " ".join(runner.TICK_SYMBOLS) + "; do\n"
            "  echo \"0000000000000000 T $s\"\n"
            "done\n")
        self.nm.chmod(self.nm.stat().st_mode | stat.S_IEXEC)

    def tearDown(self):
        self.tmp.cleanup()

    def _audit(self, **kwargs):
        params = dict(llvm_nm=str(self.nm), build_dir=str(self.build),
                      profile="aarch64-clang", image=str(self.image),
                      firmware=str(self.firmware), elf=str(self.elf))
        params.update(kwargs)
        return runner.audit_artifacts(**params)

    def test_valid_artifacts_pass(self):
        ok, errors, records = self._audit()
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [], records)

    def test_not_an_aarch64_elf_rejected(self):
        self.elf.write_bytes(_elf_bytes(machine=62))     # EM_X86_64
        ok, errors, _ = self._audit()
        self.assertFalse(ok)

    def test_non_elf_rejected(self):
        self.elf.write_bytes(b"not an elf at all")
        ok, errors, _ = self._audit()
        self.assertFalse(ok)

    def test_missing_dedicated_symbol_rejected(self):
        self.nm.write_text("#!/bin/sh\necho '0000000000000000 T other'\n")
        self.nm.chmod(self.nm.stat().st_mode | stat.S_IEXEC)
        ok, errors, _ = self._audit()
        self.assertFalse(ok)

    def test_wrong_variant_directory_rejected(self):
        other = self.build / "image" / "sched"
        other.mkdir(parents=True)
        shutil_image = self.image
        image = other / "aarch64-uefi.img"
        image.write_bytes(b"image")
        ok, errors, _ = self._audit(image=str(image))
        self.assertFalse(ok)

    def test_wrong_profile_rejected(self):
        ok, errors, _ = self._audit(profile="x86_64-clang")
        self.assertFalse(ok)

    def test_missing_file_rejected(self):
        ok, errors, _ = self._audit(elf=str(self.build / "kernel" /
                                            "sched-tick" / "absent.elf"))
        self.assertFalse(ok)

    def test_symbol_leak_into_ordinary_product_rejected(self):
        ordinary = self.build / "kernel" / "kernel.elf"
        ordinary.write_bytes(_elf_bytes())
        ok, errors, _ = self._audit()
        self.assertFalse(ok)


class BootBehaviourTests(unittest.TestCase):
    """The live-boot behaviour: the absolute deadline and the drain."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.log_dir = self.root / "logs"
        self.log_dir.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def _fake_qemu(self, body):
        script = self.root / "fake-qemu"
        script.write_text("#!/bin/sh\n" + body)
        script.chmod(script.stat().st_mode | stat.S_IEXEC)
        return str(script)

    def _args(self, qemu):
        image = self.root / "image.img"
        image.write_bytes(b"image")
        firmware = self.root / "QEMU_EFI.fd"
        firmware.write_bytes(b"firmware")
        return Namespace(qemu=qemu, image=str(image), firmware=str(firmware),
                         elf=str(image), build_dir=None,
                         profile="aarch64-clang", log_dir=str(self.log_dir))

    def test_continuous_ticks_cannot_extend_the_deadline(self):
        """A fake QEMU that never stops printing ticks must still hit the
        absolute budget (shortened here — the unit test must not really
        wait 120 s)."""
        qemu = self._fake_qemu(
            'i=0\nwhile :; do echo "[tick] $i"; i=$((i+1)); sleep 0.05; done\n')
        args = self._args(qemu)
        code = runner.run_boot(args, cpus=2, repetition=1, deadline_s=2.0,
                               dtb_factory=lambda q, d, c: str(self.root /
                                                               "fake.dtb"))
        self.assertEqual(code, 1)
        metadata = (self.log_dir / "cpus-2-run-1" / "metadata.json").read_text()
        self.assertIn('"timeout": true', metadata)

    def test_qemu_exit_before_the_drain_is_a_failure(self):
        qemu = self._fake_qemu('echo "[sched-tick] END failed=0"\n')
        args = self._args(qemu)
        code = runner.run_boot(args, cpus=2, repetition=1, deadline_s=5.0,
                               dtb_factory=lambda q, d, c: str(self.root /
                                                               "fake.dtb"))
        self.assertEqual(code, 1)

    def test_a_complete_trace_passes(self):
        """A boot that publishes the whole evidence batch and stays alive
        (like the real ``-no-shutdown`` QEMU) is accepted."""
        qemu = self._fake_qemu(
            'cat "' + str(self.root / "trace.txt") + '"\nsleep 30\n')
        (self.root / "trace.txt").write_text(runner._fixture(2))
        args = self._args(qemu)
        code = runner.run_boot(args, cpus=2, repetition=1, deadline_s=10.0,
                               dtb_factory=lambda q, d, c: str(self.root /
                                                               "fake.dtb"))
        self.assertEqual(code, 0)


class SelfTestEntryPointTests(unittest.TestCase):
    def test_module_self_test_passes(self):
        runner.self_test()


class SelectorMatrixTests(unittest.TestCase):
    """The build-selector contract of the tick variant (Task 4 Step 1).

    ``AARCH64_SCHED_TICK_TEST=1`` is a compile-affecting variant like
    every other aarch64 test variant: it needs ``KERNEL_SELFTEST=1``,
    it refuses every other variant, only the value ``1`` is accepted,
    and the x86_64 profile rejects it outright.  ``make -n`` evaluates
    the parse-time gates without building anything.
    """

    AARCH64_TARGET = "aarch64-uefi-kernel"

    def _make(self, *args, profile="aarch64-clang", target=None):
        return subprocess.run(
            ["make", "-n", f"PROFILE={profile}", *args,
             target or self.AARCH64_TARGET],
            cwd=_ROOT, capture_output=True, text=True, timeout=180)

    def test_tick_selector_alone_is_accepted(self):
        proc = self._make("KERNEL_SELFTEST=1", "AARCH64_SCHED_TICK_TEST=1")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("AARCH64_SCHED_TICK_TEST=1", proc.stdout)
        self.assertIn("-DAARCH64_SCHED_TICK_TEST=1", proc.stdout)
        # The variant owns an ISOLATED kernel tree.
        self.assertIn("kernel/sched-tick", proc.stdout)

    def test_tick_selector_requires_kernel_selftest(self):
        proc = self._make("AARCH64_SCHED_TICK_TEST=1")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("AARCH64_SCHED_TICK_TEST", proc.stderr)

    def test_invalid_tick_selector_value_rejected(self):
        proc = self._make("KERNEL_SELFTEST=1", "AARCH64_SCHED_TICK_TEST=2")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("AARCH64_SCHED_TICK_TEST", proc.stderr)

    def test_tick_selector_conflicts_with_every_other_variant(self):
        conflicts = (
            "AARCH64_SCHED_TEST=positive",
            "AARCH64_AP_SCHED_TEST=ap",
            "AARCH64_M1_TEST=sparse",
            "AARCH64_SMP_TEST_NO_ACK_CPU=1",
            "AARCH64_SYNC_FAULT_TEST=1",
            "KERNEL_TEST_FORCE_NO_RNDRRS=1",
            "OS01_SYSTEST=1",
        )
        for conflict in conflicts:
            with self.subTest(conflict=conflict):
                proc = self._make("KERNEL_SELFTEST=1",
                                  "AARCH64_SCHED_TICK_TEST=1", conflict)
                self.assertNotEqual(proc.returncode, 0, proc.stdout)
                self.assertIn("AARCH64_SCHED_TICK_TEST", proc.stderr)

    def test_x86_profile_rejects_the_aarch64_tick_selector(self):
        proc = self._make("KERNEL_SELFTEST=1", "AARCH64_SCHED_TICK_TEST=1",
                          profile="x86_64-clang", target="kernel.bin")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("AARCH64_SCHED_TICK_TEST", proc.stderr)

    def test_mode_sched_tick_sets_its_own_selector(self):
        proc = subprocess.run(
            ["make", "-n", "PROFILE=aarch64-clang", "test-aarch64",
             "MODE=sched-tick"],
            cwd=_ROOT, capture_output=True, text=True, timeout=300)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("AARCH64_SCHED_TICK_TEST=1", proc.stdout)
        self.assertIn("qemutests/aarch64_sched_tick.py", proc.stdout)

    def test_mode_sched_tick_rejects_foreign_overrides(self):
        for override in ("KERNEL_SELFTEST=1",
                         "AARCH64_SCHED_TEST=positive",
                         "AARCH64_AP_SCHED_TEST=ap"):
            with self.subTest(override=override):
                proc = subprocess.run(
                    ["make", "-n", "PROFILE=aarch64-clang", "test-aarch64",
                     "MODE=sched-tick", "KERNEL_SELFTEST=1", override],
                    cwd=_ROOT, capture_output=True, text=True, timeout=300)
                self.assertNotEqual(proc.returncode, 0, proc.stdout)
                self.assertIn("sched-tick", proc.stderr)


if __name__ == '__main__':
    unittest.main()
