#!/usr/bin/env python3
"""Unit tests for the ARCH-6 fsstress parser and SMP resolution.

Fail-closed contract (plan Task 9 Step 1): missing/duplicate worker
results, any FAIL marker, a missing final PASS, a final PASS without
worker evidence, kernel panics, a non-four-CPU boot, or an SMP /
QEMU_SMP conflict must all fail.  A complete four-CPU log passes.
These tests never launch QEMU.
"""

import unittest
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import textwrap
from types import SimpleNamespace

from qemutests.arch6_fsstress import parse_fsstress_log, resolve_smp, test_arch6_fsstress


def _worker_markers(workers=(1, 2, 3, 4),
                    fs=("ext2", "tmpfs", "vfat")) -> str:
    return "".join(
        f"[arch6-fsstress] {f} worker{w} PASS\n"
        for f in fs for w in workers)


def _full_log(smp=4):
    return (
        f"percpu: BSP  (cpu=0, apic_id=0) online\n"
        f"percpu: {smp} CPU(s) registered ({smp} in MADT)\n"
        + _worker_markers()
        + "[arch6-fsstress] PASS\n")


class ParseFsStressLogTests(unittest.TestCase):
    def test_full_four_cpu_log_passes(self):
        res = parse_fsstress_log(_full_log(), expected_smp=4)
        self.assertTrue(res.passed, res.errors)
        self.assertEqual(len(res.workers), 12)

    def test_missing_worker_result_fails(self):
        log = _full_log().replace(
            "[arch6-fsstress] tmpfs worker3 PASS\n", "")
        res = parse_fsstress_log(log, expected_smp=4)
        self.assertFalse(res.passed)

    def test_duplicate_worker_result_fails(self):
        log = _full_log() + "[arch6-fsstress] ext2 worker2 PASS\n"
        res = parse_fsstress_log(log, expected_smp=4)
        self.assertFalse(res.passed)

    def test_any_fail_marker_fails(self):
        log = (_full_log()
               + "[arch6-fsstress] vfat worker4 FAIL(checksum)\n")
        res = parse_fsstress_log(log, expected_smp=4)
        self.assertFalse(res.passed)

    def test_missing_final_pass_fails(self):
        log = _full_log().replace("[arch6-fsstress] PASS\n", "")
        res = parse_fsstress_log(log, expected_smp=4)
        self.assertFalse(res.passed)

    def test_only_final_pass_fails(self):
        res = parse_fsstress_log(
            "percpu: 4 CPU(s) registered (4 in MADT)\n"
            "[arch6-fsstress] PASS\n", expected_smp=4)
        self.assertFalse(res.passed)

    def test_kernel_panic_fails(self):
        log = _full_log() + "Kernel Panic: something\n"
        res = parse_fsstress_log(log, expected_smp=4)
        self.assertFalse(res.passed)

    def test_non_four_cpu_boot_fails(self):
        res = parse_fsstress_log(_full_log(smp=2), expected_smp=4)
        self.assertFalse(res.passed)

    def test_missing_cpu_evidence_fails(self):
        log = _worker_markers() + "[arch6-fsstress] PASS\n"
        res = parse_fsstress_log(log, expected_smp=4)
        self.assertFalse(res.passed)


class StrictResultTests(unittest.TestCase):
    def test_parent_fail_after_worker_passes_fails(self):
        self.assertFalse(parse_fsstress_log(_full_log() + "[arch6-fsstress] FAIL\n").passed)

    def test_out_of_range_worker_fails(self):
        self.assertFalse(parse_fsstress_log(_full_log().replace("worker4", "worker5")).passed)

    def test_bracketed_kernel_panic_fails(self):
        self.assertFalse(parse_fsstress_log(_full_log() + "[kernel panic] fatal\n").passed)

    def test_final_pass_before_workers_fails(self):
        log = "percpu: 4 CPU(s) registered (4 in MADT)\n[arch6-fsstress] PASS\n" + _worker_markers()
        self.assertFalse(parse_fsstress_log(log).passed)


class RunnerTests(unittest.TestCase):
    class Tester:
        _effective_smp = "4"
        timeout = 60

        def __init__(self, log, tail=""):
            self.log, self.tail = log, tail
            self.process = SimpleNamespace(observe=self.observe)

        def start_qemu(self, **kwargs):
            self.started = kwargs

        def read_until(self, pattern, timeout):
            self.pattern = pattern
            if isinstance(pattern, str):
                return self.log if pattern in self.log else None
            return pattern.search(self.log)

        def observe(self, seconds):
            self.log += self.tail
            return self.tail

        def _read_available(self):
            return self.log.encode()

    def test_runner_captures_serial_in_shared_archive(self):
        tester = self.Tester(_full_log())
        self.assertTrue(test_arch6_fsstress(tester))
        self.assertTrue(tester.started["serial_stdio"])

    def test_runner_waits_for_fail_as_well_as_pass(self):
        tester = self.Tester("[arch6-fsstress] FAIL\n")
        self.assertFalse(test_arch6_fsstress(tester))
        self.assertNotIsInstance(tester.pattern, str)
        self.assertIsNotNone(tester.pattern.search(tester.log))

    def test_observation_window_rejects_late_panic(self):
        tester = self.Tester(_full_log(), "[kernel panic] late failure\n")
        self.assertFalse(test_arch6_fsstress(tester))


class NativeWorkerProtocolTests(unittest.TestCase):
    """Run the real user program with native fork/pipes/files in a temp dir.

    Only filesystem placement and a delayed/failing syscall are substituted;
    the event protocol, barrier releases and child lifecycle are production.
    No QEMU and no kernel FS is involved in these coordination regressions.
    """
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="arch6-native-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.base = Path(cls.tmp.name)
        source = Path(__file__).resolve().parents[1] / "user/test_arch6_fsstress/test_arch6_fsstress.c"
        wrapper = cls.base / "native.c"
        wrapper.write_text(textwrap.dedent(r"""
            #define _GNU_SOURCE
            #include <stdio.h>
            #include <stdlib.h>
            #include <fcntl.h>
            #include <stdarg.h>
            #include <unistd.h>
            #include <errno.h>
            #include <string.h>
            static int native_delay, native_fail;
            static int native_rename(const char *, const char *);
            static int native_open(const char *, int, ...);
            #define rename native_rename
            #define open native_open
            #define main fsstress_main
            #include "SOURCE_FILE"
            #undef main
            #undef open
            #undef rename
            static int native_rename(const char *old, const char *new) {
                if (g_worker_id == 4 && native_delay) usleep(native_delay);
                return renameat(AT_FDCWD, old, AT_FDCWD, new);
            }
            static int native_open(const char *path, int flags, ...) {
                int mode = 0;
                if (flags & O_CREAT) {
                    va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args);
                    if (strstr(path, "/shared/"))
                        dprintf(2, "SHARED_CREATE %s\n", path);
                    if (g_worker_id == native_fail && strstr(path, "/files/")) {
                        errno = EIO; return -1;
                    }
                }
                return openat(AT_FDCWD, path, flags, mode);
            }
            int main(int argc, char **argv) {
                static char roots[3][512];
                native_delay = atoi(argv[2]); native_fail = atoi(argv[3]);
                for (int i = 0; i < 3; ++i) {
                    snprintf(roots[i], sizeof(roots[i]), "%s/%s", argv[1], FS_NAME[i]);
                    FS_ROOT[i] = roots[i];
                }
                return fsstress_main();
            }
        """).replace("SOURCE_FILE", str(source)))
        cls.binary = cls.base / "native"
        subprocess.run(["cc", "-std=gnu11", "-O0", str(wrapper), "-o", str(cls.binary)], check=True, capture_output=True)

    def run_native(self, delay=0, fail=0):
        with tempfile.TemporaryDirectory(dir=self.base) as root:
            proc = subprocess.Popen([str(self.binary), root, str(delay), str(fail)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, start_new_session=True)
            try:
                stdout, stderr = proc.communicate(timeout=8)
            finally:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
            return proc.returncode, stdout, stderr

    def test_success_records_allow_final_pass(self):
        rc, out, err = self.run_native()
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(out.count("[arch6-fsstress] PASS\n"), 1)

    def test_cross_read_waits_for_slow_peer_rename(self):
        rc, out, err = self.run_native(delay=250000)
        self.assertEqual(rc, 0, out + err)
        self.assertNotIn("FAIL", out)

    def test_shared_create_delete_runs_on_all_three_filesystems(self):
        _, _, err = self.run_native()
        for fs in ("ext2", "tmpfs", "vfat"):
            self.assertEqual(err.count(f"/{fs}/shared/"), 4, err)

    def test_worker_failure_stops_subsequent_filesystems(self):
        rc, out, err = self.run_native(fail=2)
        self.assertNotEqual(rc, 0, out + err)
        self.assertIn("FAIL", out)
        self.assertNotIn("tmpfs worker", out)
        self.assertNotIn("vfat worker", out)
        self.assertNotIn("[arch6-fsstress] PASS\n", out)


class MakeVariantTests(unittest.TestCase):
    def test_suite_catalog_declares_four_cpu_variant(self):
        from qemutests.test_catalog import load_catalog
        records, errors = load_catalog()
        self.assertEqual(errors, [])
        suites = {item.id: item for item in records}
        self.assertIn("arch6-fsstress", suites)
        entry = suites["arch6-fsstress"]
        self.assertEqual(entry.cpus, [4])
        self.assertEqual(entry.variant, "arch6-fsstress")
        self.assertEqual(entry.timeout_s, 180)

    def test_ci_runs_four_cpu_variant(self):
        from qemutests.test_ci_workflow import _job_blocks, _step_run_commands, _make_commands
        root = Path(__file__).resolve().parents[1]
        workflow = (root / ".github/workflows/ci.yml").read_text()
        commands = [line for block in _job_blocks(workflow).values()
                    for script in _step_run_commands(block)
                    for line in _make_commands(script)
                    if "SUITE=arch6-fsstress" in line]
        self.assertTrue(commands, "ARCH-6 must run in a real CI step")
        for command in commands:
            tokens = command.split()
            self.assertIn("ARCH6_FSSTRESS_TEST=1", tokens)
            self.assertIn("SMP=4", tokens)

    def test_rootfs_filters_are_variant_specific(self):
        root = Path(__file__).resolve().parents[1]
        probe = 'arch6-filter-check:;@printf "%s\\n" "$(ROOTFS_USER_PROGRAMS)"'
        for flag, wanted, absent in (
            (None, None, ("test_arch6_fsstress", "test_arch10_capacity")),
            ("ARCH6_FSSTRESS_TEST=1", "test_arch6_fsstress", ("test_arch10_capacity",)),
            ("CAPACITY_TEST=1", "test_arch10_capacity", ("test_arch6_fsstress",)),
        ):
            with self.subTest(flag=flag):
                args = ["make", "-s", "--eval", probe, "arch6-filter-check"]
                if flag: args.append(flag)
                proc = subprocess.run(args, cwd=root, text=True, capture_output=True, timeout=10, check=True)
                programs = set(proc.stdout.split())
                self.assertNotIn("sh", programs)
                if wanted: self.assertIn(wanted, programs)
                for name in absent: self.assertNotIn(name, programs)

    def test_conflicting_test_variants_rejected(self):
        root = Path(__file__).resolve().parents[1]
        for extra in ("TERM_PRESENT_TEST=1", "INITTAB_FILE=config/inittab.test",
                      "KERNEL_SELFTEST=1", "CAPACITY_TEST=1"):
            with self.subTest(extra=extra):
                proc = subprocess.run(
                    ["make", "-s", "ARCH6_FSSTRESS_TEST=1", extra, "print-run-paths"],
                    cwd=root, text=True, capture_output=True, timeout=10)
                self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)


class ResolveSmpTests(unittest.TestCase):
    def test_plain_smp_ok(self):
        self.assertEqual(resolve_smp("4", None), "4")

    def test_matching_qemu_smp_ok(self):
        self.assertEqual(resolve_smp("4", "4"), "4")

    def test_conflicting_qemu_smp_rejected(self):
        with self.assertRaises(ValueError):
            resolve_smp("4", "2")

    def test_non_four_rejected(self):
        with self.assertRaises(ValueError):
            resolve_smp("2", None)


if __name__ == "__main__":
    unittest.main()
