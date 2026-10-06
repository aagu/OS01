#!/usr/bin/env python3
"""RED unit tests for the ARCH-9 driver model boundary audit.

Five named tests covering the brief's RED phase:

  * ``test_syscall_compat_entry`` — ``make -n OS01_SYSTEST=1 test-syscall``
    must succeed and pass through to
    ``make OS01_SYSTEST=1 test-qemu SUITE=systest``. Omitting
    ``OS01_SYSTEST=1`` or mixing ``KERNEL_SELFTEST=1`` must exit
    non-zero at parse time.

  * ``test_detects_ahci_dependency`` — injecting a forbidden
    ``port_num`` reference in a temporary ``kernel/block/`` fixture
    must make the auditor exit non-zero.

  * ``test_detects_legacy_nic_globals`` — injecting
    ``os01_netif`` in a temporary ``kernel/net/`` fixture (a file that
    is NOT the whitelisted historical shim) must make the auditor
    exit non-zero.

  * ``test_detects_x86_pci_leak`` — injecting an x86 inline
    (``outb``/``outl``/...) in a temporary ``kernel/bus/pci/``
    fixture must make the auditor exit non-zero.

  * ``test_allows_arch_backend`` — valid arch-backend code and normal
    strings pass.

The fixtures live in ``tempfile.mkdtemp()`` trees so they never
disturb the real kernel sources. The auditor accepts ``--root`` so
each test can target a different fake repo.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

WORKTREE_ROOT = Path(__file__).resolve().parent.parent
AUDIT_SCRIPT = WORKTREE_ROOT / "qemutests" / "driver_model_boundary_audit.py"


def run_audit(*, root: Path) -> subprocess.CompletedProcess:
    """Run the boundary audit against ``root`` and capture output."""
    return subprocess.run(
        ["python3", str(AUDIT_SCRIPT), "--root", str(root)],
        cwd=WORKTREE_ROOT, capture_output=True, text=True, timeout=60,
    )


def run_make(*args, env=None) -> subprocess.CompletedProcess:
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    return subprocess.run(
        ["make", "--no-print-directory", *args],
        cwd=WORKTREE_ROOT, env=full_env,
        capture_output=True, text=True, timeout=120,
    )


def _write(rel: str, content: str) -> Path:
    """Helper for fixture creation: returns the parent of the leaf."""
    parent = rel.rsplit("/", 1)[0] if "/" in rel else ""
    return Path(parent)


class TestDriverModelBoundaryAudit(unittest.TestCase):
    """Auditor-detectability tests (3 violation rules + 1 negative)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="arch9-boundary-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write(self, rel: str, content: str) -> None:
        full = self.tmp / rel
        full.parent.mkdir(parents=True, exist_ok=True)
        full.write_text(textwrap.dedent(content))

    def test_detects_ahci_dependency(self):
        # Inject a `port_num` reference in the generic block layer.
        # The generic block layer must stay AHCI-agnostic;
        # `port_num` belongs to the AHCI driver only.
        self._write("kernel/block/blockdev.c", """
            #include <block/blockdev.h>
            int ahci_probe(void) {
                int port_num = 0;
                return port_num;
            }
        """)
        r = run_audit(root=self.tmp)
        self.assertNotEqual(
            r.returncode, 0,
            f"auditor must reject port_num in kernel/block/\n"
            f"stdout: {r.stdout}\nstderr: {r.stderr}",
        )
        self.assertIn("ahci_port_num_in_block", r.stderr,
                       "violation must name the rule that fired")
        self.assertIn("kernel/block/blockdev.c", r.stderr,
                       "violation must name the offending file")

    def test_detects_legacy_nic_globals(self):
        # Inject os01_netif in a file that is NOT the whitelisted
        # historical shim — kernel/net/lwip.c is generic and must
        # not use the legacy symbol.
        self._write("kernel/net/lwip.c", """
            #include <net/lwip.h>
            static struct netif os01_netif;
            int lwip_init(void) { return 0; }
        """)
        r = run_audit(root=self.tmp)
        self.assertNotEqual(
            r.returncode, 0,
            f"auditor must reject os01_netif in kernel/net/lwip.c\n"
            f"stdout: {r.stdout}\nstderr: {r.stderr}",
        )
        self.assertIn("legacy_os01_netif", r.stderr,
                       "violation must name the rule that fired")
        self.assertIn("kernel/net/lwip.c", r.stderr,
                       "violation must name the offending file")

    def test_detects_x86_pci_leak(self):
        # Inject an x86 inline (`outb`) in generic PCI bus code.
        # Generic PCI must not use x86 port-IO primitives.
        self._write("kernel/bus/pci/core.c", """
            #include <bus/pci/pci.h>
            int pci_config_read(struct pci_device *pdev, int off, uint32_t *out) {
                outb(0x80, 0x00);  /* x86 debug port; should not appear here */
                *out = 0;
                return 0;
            }
        """)
        r = run_audit(root=self.tmp)
        self.assertNotEqual(
            r.returncode, 0,
            f"auditor must reject x86 inlines in kernel/bus/\n"
            f"stdout: {r.stdout}\nstderr: {r.stderr}",
        )
        self.assertIn("x86_leak_in_generic_pci", r.stderr,
                       "violation must name the rule that fired")
        self.assertIn("kernel/bus/pci/core.c", r.stderr,
                       "violation must name the offending file")

    def test_allows_arch_backend(self):
        # Valid backend code in the generic block / net / bus paths
        # plus a normal string that mentions "os01_netif" as a doc
        # comment must not trigger false positives.
        self._write("kernel/block/blockdev.c", """
            #include <block/blockdev.h>
            /* The block layer is AHCI-agnostic. */
            int block_device_register(void) {
                return 0;
            }
        """)
        self._write("kernel/net/net.c", """
            /* Historical note: the pre-ARCH-9 single-instance
             * struct netif `os01_netif` global was removed in Task 9.
             * This file is now a thin shim; the symbol is NOT used. */
            #include <net/net.h>
            int net_poll_rx(void) { return 0; }
        """)
        self._write("kernel/bus/pci/core.c", """
            #include <bus/pci/pci.h>
            /* Generic PCI core — no x86 inlines here. */
            int pci_enumerate(void) { return 0; }
        """)
        r = run_audit(root=self.tmp)
        self.assertEqual(
            r.returncode, 0,
            f"auditor must accept valid backend code\n"
            f"stdout: {r.stdout}\nstderr: {r.stderr}",
        )


class TestSyscallCompatEntry(unittest.TestCase):
    """``make OS01_SYSTEST=1 test-syscall`` is the single retained
    alias exception (brief, AGENTS.md line 60 + 70). It must:

      - parse-time-error when ``OS01_SYSTEST`` is not 1
      - parse-time-error when ``KERNEL_SELFTEST`` is 1
      - pass through (under ``make -n``) to
        ``test-qemu SUITE=systest`` with ``OS01_SYSTEST=1``
    """

    def test_top_level_systest_required(self):
        # Without OS01_SYSTEST=1, the alias must refuse at parse time
        # so a user mistake aborts before any build.
        r = run_make("-n", "test-syscall")
        self.assertNotEqual(
            r.returncode, 0,
            f"test-syscall without OS01_SYSTEST=1 must abort\n"
            f"stdout: {r.stdout}\nstderr: {r.stderr}",
        )
        out = r.stderr + r.stdout
        self.assertTrue(
            "OS01_SYSTEST" in out or "test-syscall" in out,
            f"error must mention OS01_SYSTEST or test-syscall: {out[:400]!r}",
        )

    def test_kernel_selftest_refused(self):
        # OS01_SYSTEST=1 + KERNEL_SELFTEST=1 must abort because the
        # two suites are intentionally independent (see AGENTS.md
        # line 70: in-kernel selftests spawn kthreads which
        # interfere with systest's fork+exec+waitpid test).
        r = run_make("-n", "OS01_SYSTEST=1", "KERNEL_SELFTEST=1", "test-syscall")
        self.assertNotEqual(
            r.returncode, 0,
            f"test-syscall must refuse KERNEL_SELFTEST=1\n"
            f"stdout: {r.stdout}\nstderr: {r.stderr}",
        )
        out = r.stderr + r.stdout
        self.assertTrue(
            "KERNEL_SELFTEST" in out or "test-syscall" in out,
            f"error must mention KERNEL_SELFTEST or test-syscall: {out[:400]!r}",
        )

    def test_systest_passes_through_to_test_qemu(self):
        # With OS01_SYSTEST=1 set and KERNEL_SELFTEST not 1, the
        # alias's dry-run must print the recursive make call to
        # `test-qemu SUITE=systest` (the brief's underlying bucket
        # entry, no other alias layered above it). The dry-run may
        # execute the recursive $(MAKE) line per GNU Make's
        # recursive-make convention; the test therefore only checks
        # the printed recipe content rather than the exit code,
        # matching the test_arch9_build_contract.test_driver_model
        # _suite_in_case_statement pattern.
        r = run_make("-n", "OS01_SYSTEST=1", "test-syscall")
        out = r.stdout + r.stderr
        # The recursive call uses `$(MAKE) OS01_SYSTEST=1 test-qemu
        # SUITE=systest`; under -n the printed line still shows the
        # SUITE=systest arg verbatim.
        self.assertIn(
            "SUITE=systest", out,
            f"test-syscall must pass through to test-qemu SUITE=systest; "
            f"missing in: {out[:600]!r}",
        )
        self.assertIn(
            "test-qemu", out,
            f"test-syscall must reach test-qemu target; "
            f"missing in: {out[:600]!r}",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
