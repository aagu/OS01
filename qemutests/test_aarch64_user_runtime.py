#!/usr/bin/env python3
"""Unit tests for the aarch64 user-runtime acceptance runner."""

import sys
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "qemutests.test_aarch64_user_runtime",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import aarch64_user_runtime as runner  # noqa: E402


def _good_irq_trace():
    return (
        "[TEST] START v=1 suite=kernel-selftest expected=1\n"
        "[TEST] SELECT aarch64_user_runtime_irq required=1\n"
        "[TEST] BEGIN aarch64_user_runtime_irq\n"
        "[TEST] PASS aarch64_user_runtime_irq\n"
        "[TEST] END suite=kernel-selftest total=1 passed=1 failed=0 skipped=0\n"
        "[INFO] [user-runtime] irq-samples eoi_a=3 eoi_b=4 "
        "switch_resume_a=2 switch_resume_b=2\n"
        "[INFO] [user-runtime] PASS case=irq cpu_mask=1\n"
        "[selftest] done (failed=0)\n"
    )


def _good_asid_trace():
    return (
        "[TEST] START v=1 suite=kernel-selftest expected=1\n"
        "[TEST] SELECT aarch64_user_runtime_asid required=1\n"
        "[TEST] BEGIN aarch64_user_runtime_asid\n"
        "[TEST] PASS aarch64_user_runtime_asid\n"
        "[TEST] END suite=kernel-selftest total=1 passed=1 failed=0 skipped=0\n"
        "[INFO] [user-runtime] asid-samples asid_a=1 asid_b=2 "
        "clean_switches=4 full_flushes=0 reused=1\n"
        "[INFO] [user-runtime] PASS case=asid cpu_mask=1\n"
        "[selftest] done (failed=0)\n"
    )


def _good_smp_trace():
    return (
        "[TEST] START v=1 suite=kernel-selftest expected=1\n"
        "[TEST] SELECT aarch64_user_runtime_smp required=1\n"
        "[TEST] BEGIN aarch64_user_runtime_smp\n"
        "[TEST] PASS aarch64_user_runtime_smp\n"
        "[TEST] END suite=kernel-selftest total=1 passed=1 failed=0 skipped=0\n"
        "[INFO] [user-runtime] explicit_pin pid=10 cpu=0 mask=1\n"
        "[INFO] [user-runtime] explicit_pin pid=11 cpu=1 mask=2\n"
        "[INFO] [user-runtime] smp-samples child_cpu=1 "
        "cpu0_eoi=3 cpu1_eoi=3 cpu0_sw=2 cpu1_sw=2 "
        "cow_last_free=1 waitpid_blocked=1 waitpid_exit=42 "
        "leaked_roots=0 leaked_pages=0\n"
        "[INFO] [user-runtime] PASS case=smp cpu_mask=3\n"
        "[selftest] done (failed=0)\n"
    )


def _good_migration_trace():
    lines = [
        "[TEST] START v=1 suite=kernel-selftest expected=1",
        "[TEST] SELECT aarch64_user_runtime_migration required=1",
        "[TEST] BEGIN aarch64_user_runtime_migration",
        "[TEST] PASS aarch64_user_runtime_migration",
        "[TEST] END suite=kernel-selftest total=1 passed=1 failed=0 skipped=0",
    ]
    seq = 1

    def ev(kind, **kw):
        nonlocal seq
        fields = " ".join(f"{k}={v}" for k, v in kw.items())
        lines.append(
            f"[INFO] [user-runtime] migration-event seq={seq} kind={kind} {fields}"
        )
        seq += 1

    ev("baseline", roots=0, pages=100, asids=0)
    ev("spawn", pid=20, cpu=1, mask=3)
    ev("fork", pid=21, cpu=0, mask=3)
    ev("fork", pid=22, cpu=1, mask=3)
    ev("cow_check", pid=21, va="0x40001000", cow_val="0x2222")
    ev("old_load", pid=21, cpu=0, root="0x50010000", asid=2,
       va="0x40000000", pa="0x50001000", data="0x1111")

    # Move 1: 0 -> 1 (idle AP 8-tick window)
    ev("irq_save", move_id=1, pid=21, cpu=0, root="0x50010000", asid=2, frame="0xffff00001000")
    ev("hw_detach", move_id=1, pid=21, cpu=0, root="0x50010000", asid=2)
    ev("root_unpublish", move_id=1, pid=21, cpu=0, root="0x50010000", asid=2)
    ev("tail", move_id=1, pid=21, cpu=0, root="0x50010000", asid=2, on_cpu=0)
    ev("idle_zero", cpu=1, rq_nr=0, pending=0, cadence=0)
    ev("wfi_enter", cpu=1)
    for i in range(1, 8):
        ev("idle_tick", cpu=1, intid=30, idx=i)
        ev("idle_check", cpu=1, idx=i, wfi=1, sgi=0, other_irq=0, ap_work=0, direct_sched=0)
    ev("idle_tick", cpu=1, intid=30, idx=8)
    ev("idle_take", cpu=1, sgi=0, other_irq=0, ap_work=0, direct_sched=0)
    ev("commit", move_id=1, pid=21, cpu=1, root="0x50010000", asid=2, src=0, dst=1)
    ev("pick", move_id=1, pid=21, cpu=1, root="0x50010000", asid=2, on_cpu=1)
    ev("hw_install", move_id=1, pid=21, cpu=1, root="0x50010000", asid=2)
    ev("irq_resume", move_id=1, pid=21, cpu=1, root="0x50010000", asid=2, frame="0xffff00001000", match=1)
    ev("el0_after", move_id=1, pid=21, cpu=1, root="0x50010000", asid=2, x13="0xcafe0001", tpidr="0xbeef1234")

    # Remap on CPU 1: munmap, mmap, demand fault, mprotect
    ev("edit_begin", pid=21, cpu=1, root="0x50010000", asid=2, tx=1, op="munmap")
    ev("tlbi_done", pid=21, cpu=1, root="0x50010000", asid=2, tx=1, op="munmap")
    ev("edit_leaf", pid=21, cpu=1, root="0x50010000", asid=2, tx=1, op="munmap", old_pte="0x50001743", new_pte="0x0")
    ev("edit_begin", pid=21, cpu=1, root="0x50010000", asid=2, tx=2, op="mmap")
    ev("tlbi_done", pid=21, cpu=1, root="0x50010000", asid=2, tx=2, op="mmap")
    ev("edit_vma", pid=21, cpu=1, root="0x50010000", asid=2, tx=2, op="mmap", leaf_absent=1)
    ev("edit_begin", pid=21, cpu=1, root="0x50010000", asid=2, tx=3, op="fault")
    ev("tlbi_done", pid=21, cpu=1, root="0x50010000", asid=2, tx=3, op="fault")
    ev("edit_leaf", pid=21, cpu=1, root="0x50010000", asid=2, tx=3, op="fault", old_pte="0x0", new_pte="0x50020743")
    ev("edit_begin", pid=21, cpu=1, root="0x50010000", asid=2, tx=4, op="mprotect")
    ev("tlbi_done", pid=21, cpu=1, root="0x50010000", asid=2, tx=4, op="mprotect")
    ev("edit_leaf", pid=21, cpu=1, root="0x50010000", asid=2, tx=4, op="mprotect", old_pte="0x50020743", new_pte="0x500207c3")
    ev("remap", pid=21, cpu=1, root="0x50010000", asid=2, va="0x40000000", pa="0x50020000", data="0x3333")

    # Move 2: 1 -> 0 (BSP tick schedule)
    ev("irq_save", move_id=2, pid=21, cpu=1, root="0x50010000", asid=2, frame="0xffff00001000")
    ev("hw_detach", move_id=2, pid=21, cpu=1, root="0x50010000", asid=2)
    ev("root_unpublish", move_id=2, pid=21, cpu=1, root="0x50010000", asid=2)
    ev("tail", move_id=2, pid=21, cpu=1, root="0x50010000", asid=2, on_cpu=0)
    ev("bsp_tick_schedule", move_id=2, cpu=0, bsp_req=1, sgi=0, ap_work=0, direct_sched=0)
    ev("commit", move_id=2, pid=21, cpu=0, root="0x50010000", asid=2, src=1, dst=0)
    ev("pick", move_id=2, pid=21, cpu=0, root="0x50010000", asid=2, on_cpu=1)
    ev("hw_install", move_id=2, pid=21, cpu=0, root="0x50010000", asid=2)
    ev("irq_resume", move_id=2, pid=21, cpu=0, root="0x50010000", asid=2, frame="0xffff00001000", match=1)
    ev("el0_after", move_id=2, pid=21, cpu=0, root="0x50010000", asid=2, x13="0xcafe0002", tpidr="0xbeef1234")

    ev("new_load", pid=21, cpu=0, root="0x50010000", asid=2, va="0x40000000", pa="0x50020000", data="0x3333")
    ev("permission_abort", pid=21, cpu=0, root="0x50010000", asid=2, va="0x40000000", pa="0x50020000", data="0x3333", ec="0x24", fsc="0xf", far="0x40000000", wnr=1)
    ev("wait_status", child_pid=21, returned_pid=21, status=-14)
    ev("parent_check", pid=20, va="0x40000000", pa="0x50001000",
       data="0x1111", cow_val="0x1111")

    # Move 3: child 22 1 -> 0, copy_ok/copy_denied, exec
    ev("irq_save", move_id=3, pid=22, cpu=1, root="0x50030000", asid=3, frame="0xffff00002000")
    ev("hw_detach", move_id=3, pid=22, cpu=1, root="0x50030000", asid=3)
    ev("root_unpublish", move_id=3, pid=22, cpu=1, root="0x50030000", asid=3)
    ev("tail", move_id=3, pid=22, cpu=1, root="0x50030000", asid=3, on_cpu=0)
    ev("bsp_tick_schedule", move_id=3, cpu=0, bsp_req=1, sgi=0, ap_work=0, direct_sched=0)
    ev("commit", move_id=3, pid=22, cpu=0, root="0x50030000", asid=3, src=1, dst=0)
    ev("pick", move_id=3, pid=22, cpu=0, root="0x50030000", asid=3, on_cpu=1)
    ev("hw_install", move_id=3, pid=22, cpu=0, root="0x50030000", asid=3)
    ev("irq_resume", move_id=3, pid=22, cpu=0, root="0x50030000", asid=3, frame="0xffff00002000", match=1)
    ev("el0_after", move_id=3, pid=22, cpu=0, root="0x50030000", asid=3, x13="0xcafe0003", tpidr="0xbeef5678")

    ev("copy_ok", pid=22, cpu=0, ret=16)
    ev("copy_denied", pid=22, cpu=0, ret=-14)
    ev("exec_commit", pid=22, cpu=0, old_root="0x50030000", new_root="0x50040000", old_mask=3, new_mask=3)
    ev("exec_el0", pid=22, cpu=0, root="0x50040000", mask=3)
    ev("wait_status", child_pid=22, returned_pid=22, status=10752)
    ev("final", roots=0, pages=100, asids=0)
    lines.append("[INFO] [user-runtime] PASS case=migration cpu_mask=3")
    lines.append("[selftest] done (failed=0)")
    return "\n".join(lines) + "\n"


def _good_product_trace():
    # Busybox ash shape (2026-11-21): banner once per interactive process
    # with a build timestamp, "# " root prompt (FEATURE_EDITING_FANCY_PROMPT
    # off + geteuid()==0), prompt+echoed-input merged into one line.
    return (
        "[INFO] [boot-5] init-created pid=1\n"
        "[INFO] init: inittab loaded\n"
        "BusyBox v1.36.1 (2026-11-21 00:00:00 CST) built-in shell (ash)\n"
        "# echo hi\n"
        "hi\n"
        "# exit\n"
        "BusyBox v1.36.1 (2026-11-21 00:00:00 CST) built-in shell (ash)\n"
        "# "
    )


def _good_fp_trace():
    # USER_RUNTIME_CASE=fp shape: same product boot, but the shell runs
    # /bin/test_fp and its standalone PASS line is the required evidence.
    return (
        "[INFO] [boot-5] init-created pid=1\n"
        "[INFO] init: inittab loaded\n"
        "BusyBox v1.36.1 (2026-11-21 00:00:00 CST) built-in shell (ash)\n"
        "# /bin/test_fp\n"
        "test_fp: PASS\n"
        "# exit\n"
        "BusyBox v1.36.1 (2026-11-21 00:00:00 CST) built-in shell (ash)\n"
        "# "
    )


def _good_map_shared_trace():
    # USER_RUNTIME_CASE=map-shared shape: 2-core product boot, the shell
    # runs /bin/test_map_shared, its standalone PASS line is the sole
    # success evidence, then the shell respawns.
    return (
        "[INFO] [boot-5] init-created pid=1\n"
        "[INFO] init: inittab loaded\n"
        "BusyBox v1.36.1 (2026-11-22 00:00:00 CST) built-in shell (ash)\n"
        "# /bin/test_map_shared\n"
        "test_map_shared: PASS\n"
        "# exit\n"
        "BusyBox v1.36.1 (2026-11-22 00:00:00 CST) built-in shell (ash)\n"
        "# "
    )


class MapSharedRuntimeTests(unittest.TestCase):
    def test_valid_map_shared_trace_passes(self):
        ok, errors = runner.parse_map_shared_evidence(_good_map_shared_trace())
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_map_shared_missing_pass_fails(self):
        text = _good_map_shared_trace().replace("test_map_shared: PASS\n", "")
        ok, errors = runner.parse_map_shared_evidence(text)
        self.assertFalse(ok)
        self.assertTrue(any("test_map_shared: PASS" in e for e in errors), errors)

    def test_map_shared_duplicate_pass_fails(self):
        text = _good_map_shared_trace().replace(
            "# exit\n",
            "test_map_shared: PASS\n# exit\n",
        )
        ok, errors = runner.parse_map_shared_evidence(text)
        self.assertFalse(ok)
        self.assertTrue(any("duplicate" in e for e in errors), errors)

    def test_map_shared_fail_line_fails(self):
        text = _good_map_shared_trace().replace(
            "test_map_shared: PASS\n",
            "test_map_shared: FAIL p1-uaccess-visible\n",
        )
        ok, errors = runner.parse_map_shared_evidence(text)
        self.assertFalse(ok)
        self.assertTrue(
            any("FAIL line observed" in e for e in errors), errors)

    def test_map_shared_command_echo_only_fails(self):
        # The shell echo of the typed command is NOT evidence: a trace
        # with only the echoed command (no probe output) must fail.
        text = _good_map_shared_trace().replace("test_map_shared: PASS\n", "")
        ok, errors = runner.parse_map_shared_evidence(text)
        self.assertFalse(ok)

    def test_map_shared_echoed_commands_do_not_count_as_prompts(self):
        # A trace whose only "# " occurrences are the shell ECHOING the
        # typed commands (no bare prompt after respawn) must fail: the
        # prompt count is anchored on bare prompt lines, not substrings.
        text = (
            "[INFO] [boot-5] init-created pid=1\n"
            "[INFO] init: inittab loaded\n"
            "BusyBox v1.36.1 (2026-11-22 00:00:00 CST) built-in shell (ash)\n"
            "# /bin/test_map_shared\n"
            "test_map_shared: PASS\n"
            "# exit\n"
            "BusyBox v1.36.1 (2026-11-22 00:00:00 CST) built-in shell (ash)\n"
            "# /bin/echo never-typed\n"
        )
        ok, errors = runner.parse_map_shared_evidence(text)
        self.assertFalse(ok)
        self.assertTrue(any("prompt" in e for e in errors), errors)

    def test_map_shared_respawn_required(self):
        text = _good_map_shared_trace().replace(
            "BusyBox v1.36.1 (2026-11-22 00:00:00 CST) built-in shell (ash)\n# ",
            "",
        )
        ok, errors = runner.parse_map_shared_evidence(text)
        self.assertFalse(ok)
        self.assertTrue(any("respawn" in e for e in errors), errors)

    def test_map_shared_boot_evidence_required(self):
        text = _good_map_shared_trace().replace("init-created pid=1\n", "")
        ok, errors = runner.parse_map_shared_evidence(text)
        self.assertFalse(ok)
        self.assertTrue(any("pid 1" in e or "init-created" in e
                            for e in errors), errors)

    def test_map_shared_runs_on_two_cpus(self):
        self.assertEqual(runner.boot_cpus_for("map-shared"), 2)
        self.assertEqual(runner.boot_cpus_for("irq"), 1)
        self.assertEqual(runner.boot_cpus_for("smp"), 2)
        self.assertEqual(runner.boot_cpus_for("product"), 1)

    def test_map_shared_uses_product_image_paths(self):
        self.assertIn("map-shared", runner.PRODUCT_IMAGE_CASES)
        self.assertIn("product", runner.PRODUCT_IMAGE_CASES)
        self.assertIn("fp", runner.PRODUCT_IMAGE_CASES)

    def test_writable_flag_defaults_off_and_pflash_stays_ro(self):
        base = [
            "--image", "/tmp/orig.img",
            "--firmware", "/tmp/fw.code",
        ]
        cmd = runner.qemu_command("qemu-system-aarch64", 2,
                                  "/tmp/run-copy.img", "/tmp/fw.code",
                                  "/tmp/dev.dtb")
        disk_args = [a for a in cmd if a.startswith("if=none,")]
        self.assertEqual(len(disk_args), 1)
        self.assertIn("readonly=on", disk_args[0])
        self.assertIn("file=/tmp/run-copy.img", disk_args[0])
        pflash = [a for a in cmd if a.startswith("if=pflash,")]
        self.assertTrue(all("readonly=on" in a for a in pflash))

        cmd_w = runner.qemu_command("qemu-system-aarch64", 2,
                                    "/tmp/run-copy.img", "/tmp/fw.code",
                                    "/tmp/dev.dtb", writable=True)
        disk_w = [a for a in cmd_w if a.startswith("if=none,")]
        self.assertEqual(len(disk_w), 1)
        self.assertIn("readonly=off", disk_w[0])
        self.assertIn("file=/tmp/run-copy.img", disk_w[0])
        # The ORIGINAL image path never appears as a writable drive.
        self.assertNotIn("file=/tmp/orig.img", " ".join(cmd_w))
        pflash_w = [a for a in cmd_w if a.startswith("if=pflash,")]
        self.assertTrue(all("readonly=on" in a for a in pflash_w))

    def test_other_cases_drive_stays_readonly(self):
        # The product/fp cases keep their readonly single drive: the
        # writable parameter exists only for the map-shared copy.
        for cpus in (1, 2, 8):
            cmd = runner.qemu_command("qemu-system-aarch64", cpus,
                                      "/tmp/orig.img", "/tmp/fw.code",
                                      "/tmp/dev.dtb")
            disk_args = [a for a in cmd if a.startswith("if=none,")]
            self.assertEqual(len(disk_args), 1)
            self.assertIn("readonly=on", disk_args[0])

    def test_map_shared_audit_elf_uses_product_branch(self):
        # The product ELF must carry no test symbols: map-shared audits
        # the same forbidden prefixes as product/fp (not the selftest
        # required-symbol branch).
        self.assertIn("map-shared", runner.PRODUCT_IMAGE_CASES)
        errors = runner.audit_elf("/nonexistent.elf", "llvm-nm",
                                  case="map-shared")
        self.assertTrue(errors)
        self.assertFalse(any("required symbol" in e for e in errors))


class UserRuntimeEvidenceTests(unittest.TestCase):
    def test_valid_irq_trace_passes(self):
        ok, errors = runner.parse_user_runtime_evidence(_good_irq_trace(), "irq")
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_valid_asid_trace_passes(self):
        ok, errors = runner.parse_user_runtime_evidence(_good_asid_trace(), "asid")
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_missing_asid_samples_fails(self):
        text = _good_asid_trace().replace(
            "[INFO] [user-runtime] asid-samples asid_a=1 asid_b=2 "
            "clean_switches=4 full_flushes=0 reused=1\n",
            "",
        )
        ok, errors = runner.parse_user_runtime_evidence(text, "asid")
        self.assertFalse(ok)
        self.assertTrue(any("asid-samples" in e for e in errors), errors)

    def test_duplicate_required_record_fails(self):
        dup = _good_irq_trace() + "[INFO] [user-runtime] PASS case=irq cpu_mask=1\n"
        ok, errors = runner.parse_user_runtime_evidence(dup, "irq")
        self.assertFalse(ok)
        self.assertTrue(any("duplicate" in e for e in errors), errors)

    def test_missing_samples_fails(self):
        text = _good_irq_trace().replace(
            "[INFO] [user-runtime] irq-samples eoi_a=3 eoi_b=4 "
            "switch_resume_a=2 switch_resume_b=2\n",
            "",
        )
        ok, errors = runner.parse_user_runtime_evidence(text, "irq")
        self.assertFalse(ok)
        self.assertTrue(any("irq-samples" in e for e in errors), errors)

    def test_fatal_line_fails(self):
        text = _good_irq_trace() + "[aarch64-sync] FATAL mpidr=0x0\n"
        ok, errors = runner.parse_user_runtime_evidence(text, "irq")
        self.assertFalse(ok)
        self.assertTrue(any("fatal" in e.lower() for e in errors), errors)

    def test_valid_smp_trace_passes(self):
        ok, errors = runner.parse_user_runtime_evidence(_good_smp_trace(), "smp")
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_smp_wrong_child_cpu_fails(self):
        text = _good_smp_trace().replace("child_cpu=1", "child_cpu=0")
        ok, errors = runner.parse_user_runtime_evidence(text, "smp")
        self.assertFalse(ok)
        self.assertTrue(any("child_cpu=1" in e for e in errors), errors)

    def test_smp_cow_last_free_mismatch_fails(self):
        text = _good_smp_trace().replace("cow_last_free=1", "cow_last_free=2")
        ok, errors = runner.parse_user_runtime_evidence(text, "smp")
        self.assertFalse(ok)
        self.assertTrue(any("cow_last_free=1" in e for e in errors), errors)

    def test_valid_product_trace_passes(self):
        ok, errors = runner.parse_product_boot_evidence(
            _good_product_trace(), check_shell_io=True
        )
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_product_missing_hi_fails(self):
        text = _good_product_trace().replace("hi\n", "")
        ok, errors = runner.parse_product_boot_evidence(text, check_shell_io=True)
        self.assertFalse(ok)
        self.assertTrue(any("hi" in e for e in errors), errors)

    def test_smp_missing_explicit_pin_fails(self):
        text = "\n".join(
            l for l in _good_smp_trace().splitlines() if "explicit_pin" not in l
        ) + "\n"
        ok, errors = runner.parse_user_runtime_evidence(text, "smp")
        self.assertFalse(ok)
        self.assertTrue(any("explicit_pin" in e for e in errors), errors)

    def test_valid_migration_trace_passes(self):
        ok, errors = runner.parse_user_runtime_evidence(_good_migration_trace(), "migration")
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_migration_deleted_line_fails(self):
        text = "\n".join(
            l for l in _good_migration_trace().splitlines() if "kind=el0_after move_id=1" not in l
        ) + "\n"
        ok, errors = runner.parse_user_runtime_evidence(text, "migration")
        self.assertFalse(ok)

    def test_migration_duplicate_seq_fails(self):
        lines = _good_migration_trace().splitlines()
        dup_line = next(l for l in lines if "kind=old_load" in l)
        text = "\n".join(lines[:-2] + [dup_line] + lines[-2:]) + "\n"
        ok, errors = runner.parse_user_runtime_evidence(text, "migration")
        self.assertFalse(ok)

    def test_migration_wrong_direction_fails(self):
        text = _good_migration_trace().replace("src=0 dst=1", "src=1 dst=0")
        ok, errors = runner.parse_user_runtime_evidence(text, "migration")
        self.assertFalse(ok)

    def test_migration_pa_or_tag_mismatch_fails(self):
        text = _good_migration_trace().replace(
            'kind=new_load pid=21 cpu=0 root=0x50010000 asid=2 va=0x40000000 pa=0x50020000',
            'kind=new_load pid=21 cpu=0 root=0x50010000 asid=2 va=0x40000000 pa=0x50001000',
        )
        ok, errors = runner.parse_user_runtime_evidence(text, "migration")
        self.assertFalse(ok)

        text_tag0 = _good_migration_trace().replace("asid=2", "asid=0")
        ok2, _ = runner.parse_user_runtime_evidence(text_tag0, "migration")
        self.assertFalse(ok2)

    def test_migration_sgi_in_idle_window_fails(self):
        text = _good_migration_trace().replace(
            "kind=idle_take cpu=1 sgi=0",
            "kind=idle_take cpu=1 sgi=1",
        )
        ok, errors = runner.parse_user_runtime_evidence(text, "migration")
        self.assertFalse(ok)

    def test_migration_delayed_tlbi_or_full_flush_fails(self):
        lines = _good_migration_trace().splitlines()
        i_tlbi = next(i for i, l in enumerate(lines) if "kind=tlbi_done" in l and "tx=1" in l)
        i_leaf = next(i for i, l in enumerate(lines) if "kind=edit_leaf" in l and "tx=1" in l)
        lines[i_tlbi], lines[i_leaf] = lines[i_leaf], lines[i_tlbi]
        ok, _ = runner.parse_user_runtime_evidence("\n".join(lines) + "\n", "migration")
        self.assertFalse(ok)

        lines2 = _good_migration_trace().splitlines()
        lines2.insert(i_leaf + 1, "[INFO] [user-runtime] migration-event seq=999 kind=full_flush pid=21 cpu=0 root=0x50010000 asid=2")
        ok2, _ = runner.parse_user_runtime_evidence("\n".join(lines2) + "\n", "migration")
        self.assertFalse(ok2)

    def test_migration_software_uaccess_instead_of_hw_fault_fails(self):
        text = _good_migration_trace().replace("ec=0x24 fsc=0xf", "ec=0x15 fsc=0x0")
        ok, _ = runner.parse_user_runtime_evidence(text, "migration")
        self.assertFalse(ok)

    def test_migration_pass_before_final_fails(self):
        lines = _good_migration_trace().splitlines()
        i_fin = next(i for i, l in enumerate(lines) if "kind=final" in l)
        i_pass = next(i for i, l in enumerate(lines) if "PASS case=migration" in l)
        lines[i_fin], lines[i_pass] = lines[i_pass], lines[i_fin]
        ok, _ = runner.parse_user_runtime_evidence("\n".join(lines) + "\n", "migration")
        self.assertFalse(ok)

    def test_migration_data_mismatch_fails(self):
        text = _good_migration_trace().replace("data=0x3333", "data=0x1111", 1)
        ok, _ = runner.parse_user_runtime_evidence(text, "migration")
        self.assertFalse(ok)

    def test_product_audit_forbids_mig_prefixes(self):
        self.assertIn("aarch64_user_mig_", runner.FORBIDDEN_PRODUCT_PREFIXES)
        self.assertIn("aarch64_sched_tick_mig_", runner.FORBIDDEN_PRODUCT_PREFIXES)
        self.assertIn("aarch64_asid_test_", runner.FORBIDDEN_PRODUCT_PREFIXES)

    def test_valid_fp_trace_passes(self):
        ok, errors = runner.parse_product_boot_evidence(
            _good_fp_trace(), check_shell_io=True, test_fp_pass=True
        )
        self.assertTrue(ok, errors)
        self.assertEqual(errors, [])

    def test_fp_missing_pass_line_fails(self):
        text = _good_fp_trace().replace("test_fp: PASS\n", "")
        ok, errors = runner.parse_product_boot_evidence(
            text, check_shell_io=True, test_fp_pass=True
        )
        self.assertFalse(ok)
        self.assertTrue(any("test_fp: PASS" in e for e in errors), errors)

    def test_fp_trace_without_pass_fails_plain_parse(self):
        # The fp trace has no "hi" line: the plain product parse must
        # not accept it (the two shells are distinguished by the flag).
        ok, errors = runner.parse_product_boot_evidence(
            _good_fp_trace(), check_shell_io=True
        )
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()
