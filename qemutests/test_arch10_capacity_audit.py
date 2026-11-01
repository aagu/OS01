#!/usr/bin/env python3
"""Unit tests for ARCH-10 capacity, depfile, and engineering hygiene audit."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from qemutests import arch10_capacity_audit

ROOT = Path(__file__).resolve().parents[1]


class Arch10CapacityAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="arch10-audit-")
        self.addCleanup(self.tmp.cleanup)
        self.test_root = Path(self.tmp.name)

    def test_config_limits_detection(self):
        """Audit must require kernel/include/config/limits.h with NR_CPUS and OS01_NOFILE_CAPACITY."""
        # 1. Missing limits.h
        errs = arch10_capacity_audit.check_config_limits(self.test_root)
        self.assertTrue(any("Missing required capacity config header" in e for e in errs))

        # 2. Incomplete / invalid limits.h
        limits_h = self.test_root / "kernel" / "include" / "config" / "limits.h"
        limits_h.parent.mkdir(parents=True, exist_ok=True)
        limits_h.write_text("#define NR_CPUS 4\n")
        errs = arch10_capacity_audit.check_config_limits(self.test_root)
        self.assertTrue(any("must define '#define NR_CPUS 8'" in e for e in errs))
        self.assertTrue(any("must define '#define OS01_NOFILE_CAPACITY 16'" in e for e in errs))

        # 3. Non-integer C constructs
        limits_h.write_text("#define NR_CPUS 8\n#define OS01_NOFILE_CAPACITY 16\ntypedef int my_int;\n")
        errs = arch10_capacity_audit.check_config_limits(self.test_root)
        self.assertTrue(any("must be a pure integer config header" in e for e in errs))

        # 4. Valid pure integer limits.h
        limits_h.write_text(
            "#ifndef _OS01_CONFIG_LIMITS_H\n"
            "#define _OS01_CONFIG_LIMITS_H\n"
            "#ifndef NR_CPUS\n"
            "#define NR_CPUS 8\n"
            "#endif\n"
            "#ifndef OS01_NOFILE_CAPACITY\n"
            "#define OS01_NOFILE_CAPACITY 16\n"
            "#endif\n"
            "#endif\n"
        )
        errs = arch10_capacity_audit.check_config_limits(self.test_root)
        self.assertEqual(errs, [])

    def test_duplicate_nr_cpus_detected(self):
        """Audit must reject multiple or redundant #define NR_CPUS in production and unapproved mocks."""
        # Production duplicate
        prod_h = self.test_root / "kernel" / "include" / "arch" / "cpu.h"
        prod_h.parent.mkdir(parents=True, exist_ok=True)
        prod_h.write_text("#define NR_CPUS 8\n")

        # Approved mock
        mock_ok = self.test_root / "hosttests" / "mock" / "pmm_boot_include" / "percpu" / "percpu.h"
        mock_ok.parent.mkdir(parents=True, exist_ok=True)
        mock_ok.write_text("#define NR_CPUS 1\n")

        # Unapproved mock
        mock_bad = self.test_root / "hosttests" / "mock" / "slab_include" / "percpu" / "percpu.h"
        mock_bad.parent.mkdir(parents=True, exist_ok=True)
        mock_bad.write_text("#define NR_CPUS 8\n")

        errs = arch10_capacity_audit.check_nr_cpus_definitions(self.test_root)
        self.assertTrue(any("kernel/include/arch/cpu.h" in e and "duplicate/redundant" in e for e in errs))
        self.assertTrue(any("hosttests/mock/slab_include/percpu/percpu.h" in e and "unapproved" in e for e in errs))
        self.assertFalse(any("pmm_boot_include" in e for e in errs))

    def test_aarch64_aliases(self):
        """Audit must enforce AARCH64_BOOT_MAX_CPUS and AARCH64_BOOT_CAPACITY as aliases to NR_CPUS."""
        dtb_h = self.test_root / "kernel" / "include" / "arch" / "aarch64" / "dtb.h"
        dtb_h.parent.mkdir(parents=True, exist_ok=True)
        boot_offsets_h = self.test_root / "kernel" / "include" / "arch" / "aarch64" / "boot_offsets.h"
        boot_offsets_h.parent.mkdir(parents=True, exist_ok=True)

        dtb_h.write_text("#define AARCH64_BOOT_MAX_CPUS 8\n")
        boot_offsets_h.write_text("#define AARCH64_BOOT_CAPACITY 8\n")

        errs = arch10_capacity_audit.check_aarch64_aliases(self.test_root)
        self.assertTrue(any("AARCH64_BOOT_MAX_CPUS must alias to NR_CPUS" in e for e in errs))
        self.assertTrue(any("AARCH64_BOOT_CAPACITY must alias to NR_CPUS" in e for e in errs))

        dtb_h.write_text("#define AARCH64_BOOT_MAX_CPUS NR_CPUS\n")
        boot_offsets_h.write_text("#define AARCH64_BOOT_CAPACITY NR_CPUS\n")

        errs = arch10_capacity_audit.check_aarch64_aliases(self.test_root)
        self.assertEqual(errs, [])

    def test_gitignore_and_tracking(self):
        """Audit must reject blanket *.S in .gitignore and verify normal .S files are not ignored."""
        gitignore = self.test_root / ".gitignore"
        gitignore.write_text("*.S\n!kernel/arch/x86_64/thread_entry.S\n/build/\n")

        # Initialize git repo in test_root to test --no-index
        subprocess.run(["git", "init"], cwd=self.test_root, capture_output=True, check=True)
        head_s = self.test_root / "kernel" / "arch" / "x86_64" / "head.S"
        head_s.parent.mkdir(parents=True, exist_ok=True)
        head_s.write_text("// test head.S\n")

        errs = arch10_capacity_audit.check_gitignore_and_tracking(self.test_root)
        self.assertTrue(any("blanket '*.S' ignore pattern is forbidden" in e for e in errs))
        self.assertTrue(any("obsolete exception" in e for e in errs))
        self.assertTrue(any("Normal assembly source is unexpectedly ignored" in e for e in errs))

        # Fix .gitignore
        gitignore.write_text("/build/\n*.o\n*.bin\n")
        errs = arch10_capacity_audit.check_gitignore_and_tracking(self.test_root)
        self.assertEqual(errs, [])

    def test_source_tree_hygiene(self):
        """Audit must detect stray .o, .bin, .elf, .d in source directories."""
        stray_o = self.test_root / "kernel" / "fs" / "select.o"
        stray_o.parent.mkdir(parents=True, exist_ok=True)
        stray_o.write_bytes(b"\x7fELF")

        stray_bin = self.test_root / "kernel" / "kernel.bin"
        stray_bin.write_bytes(b"\x00" * 16)

        errs = arch10_capacity_audit.check_source_tree_hygiene(self.test_root)
        self.assertTrue(any("kernel/fs/select.o" in e for e in errs))
        self.assertTrue(any("kernel/kernel.bin" in e for e in errs))

        stray_o.unlink()
        stray_bin.unlink()
        errs = arch10_capacity_audit.check_source_tree_hygiene(self.test_root)
        self.assertEqual(errs, [])

    def test_kernel_makefile_deps(self):
        """Audit must verify ALL_DEPS includes ASM_OBJECTS and trampoline depfile."""
        makefile = self.test_root / "kernel" / "Makefile"
        makefile.parent.mkdir(parents=True, exist_ok=True)

        # Old makefile ALL_DEPS
        makefile.write_text("ALL_DEPS := $(patsubst %.o,%.d,$(C_OBJECTS))\n-include $(ALL_DEPS)\n")
        errs = arch10_capacity_audit.check_kernel_makefile_deps(self.test_root)
        self.assertTrue(any("ALL_DEPS must consume $(ASM_OBJECTS)" in e for e in errs))
        self.assertTrue(any("ALL_DEPS must consume trampoline depfile" in e for e in errs))

        # Updated makefile ALL_DEPS
        makefile.write_text(
            "ALL_DEPS := $(patsubst %.o,%.d,$(C_OBJECTS) $(ASM_OBJECTS))\n"
            "ifeq ($(ARCH),x86_64)\n"
            "ALL_DEPS += $(BUILD_DIR)/$(ARCHDIR)/trampoline.d\n"
            "endif\n"
            "-include $(ALL_DEPS)\n"
        )
        errs = arch10_capacity_audit.check_kernel_makefile_deps(self.test_root)
        self.assertEqual(errs, [])

    def test_current_repo_audit_integration(self):
        """Run audit CLI directly against current workspace (must exit 0 and pass once Task 1 is implemented)."""
        res = subprocess.run(
            ["python3", str(ROOT / "qemutests" / "arch10_capacity_audit.py")],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(res.returncode, 0, f"Audit unexpectedly failed on repository:\n{res.stderr}")
        self.assertIn("ARCH-10 capacity audit: PASS", res.stdout)


if __name__ == "__main__":
    unittest.main()
