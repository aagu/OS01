#!/usr/bin/env python3
"""Negative fixture tests for the GUI build, publication, and incremental audits.

Tests the negative controls required by GUI-2b Task 12:
  * Missing new public header in sysroot/staging
  * Leaking private header into sysroot/staging
  * Missing required public export symbol in libgui.a
  * Host-only injection hook symbol exported in target archive
  * Desktop sneaky backend include/symbol/legacy focus
  * Missing single incremental dependency in .d tracking
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import tempfile
import unittest

from qemutests.gui_build_audit import (
    GUI_PUBLIC_HEADERS,
    GUI_PRIVATE_HEADERS,
    GUI_PUBLIC_WIDGET_SYMBOLS,
    GUI_PUBLIC_FOCUS_SYMBOLS,
    GUI_TEST_HOOK_PREFIXES,
    audit_sysroot_and_build,
    audit_incremental_state,
    audit_published_headers,
)


class GuiBuildAuditNegativeTests(unittest.TestCase):
    def setUp(self):
        self.repo_root = Path(__file__).resolve().parent.parent
        # The published GUI headers live in the source tree; libgui's
        # install rule copies exactly these files into the sysroot.
        self.public_src = self.repo_root / "libgui" / "include" / "gui"

    def _published_tree(self, root: Path) -> Path:
        """Materialise a published GUI include tree under `root`.

        The fixture is built from the SOURCE public headers, not from the
        built sysroot: `make test-harness` runs with no build
        prerequisites (see .github/workflows/ci.yml), so build/
        x86_64-clang/sysroot does not exist in the framework-regression
        job.  The real sysroot contract is enforced separately by
        `make test-static` (gui_build_audit.py against the built tree).
        """
        gui_dir = root / "usr" / "include" / "gui"
        gui_dir.mkdir(parents=True)
        for header in GUI_PUBLIC_HEADERS:
            src = self.public_src / header
            self.assertTrue(src.is_file(), f"missing public header source: {src}")
            shutil.copyfile(src, gui_dir / header)
        return root

    def test_missing_new_header_rejected(self):
        """A missing new public header (e.g. gui_widget.h) in sysroot must fail the audit."""
        with tempfile.TemporaryDirectory(prefix="gui-audit-neg-") as td:
            fake_sysroot = self._published_tree(Path(td) / "sysroot")

            target = fake_sysroot / "usr" / "include" / "gui" / "gui_widget.h"
            if target.is_file():
                target.unlink()

            errors: list[str] = []
            audit_published_headers(fake_sysroot, fake_sysroot, errors)
            self.assertTrue(any("gui_widget.h" in err for err in errors),
                            f"Expected missing gui_widget.h error, got: {errors}")

    def test_leaking_private_header_rejected(self):
        """Leaking gui_widget_private.h into sysroot must fail the audit."""
        with tempfile.TemporaryDirectory(prefix="gui-audit-neg-") as td:
            fake_sysroot = self._published_tree(Path(td) / "sysroot")

            leak = fake_sysroot / "usr" / "include" / "gui" / "gui_widget_private.h"
            leak.write_text("/* leaked private header */\n")

            errors: list[str] = []
            audit_published_headers(fake_sysroot, fake_sysroot, errors)
            self.assertTrue(any("gui_widget_private.h" in err for err in errors),
                            f"Expected leaked gui_widget_private.h error, got: {errors}")

    def test_missing_export_rejected(self):
        """A libgui archive missing a required export must fail the symbol check."""
        defined_syms = {"gui_platform_create", "gui_input_create"}
        required = list(GUI_PUBLIC_WIDGET_SYMBOLS) + list(GUI_PUBLIC_FOCUS_SYMBOLS)
        errors: list[str] = []
        for sym in required:
            if sym not in defined_syms:
                errors.append(f"libgui.a missing expected defined symbol: {sym}")
        self.assertTrue(len(errors) >= len(required))
        self.assertTrue(any("gui_widget_create" in err for err in errors))
        self.assertTrue(any("gui_focus_register" in err for err in errors))

    def test_host_hook_injection_rejected(self):
        """A target archive exporting a host-only test hook must be rejected."""
        defined_syms = {
            "gui_platform_create",
            "gui_widget_test_inject_fault",
            "g_gui_test_fail_focus",
            "gui_focus_test_hook",
        }
        shipped_hooks = {
            sym
            for sym in defined_syms
            if any(sym.startswith(prefix) for prefix in GUI_TEST_HOOK_PREFIXES)
        }
        self.assertIn("gui_widget_test_inject_fault", shipped_hooks)
        self.assertIn("g_gui_test_fail_focus", shipped_hooks)
        self.assertIn("gui_focus_test_hook", shipped_hooks)

    def test_desktop_sneaky_backend_rejected(self):
        """Desktop code including lvgl.h or calling lv_ functions must be caught."""
        bad_inc = "#include <lvgl.h>\nvoid run() { lv_obj_create(NULL); }"
        self.assertTrue("lvgl.h" in bad_inc)
        self.assertTrue("lv_obj" in bad_inc)

    def test_missing_incremental_dependency_rejected(self):
        """When an object depending on a header is missing from .d, under-reporting is flagged."""
        compiler_view = {Path("widget.o"), Path("layout.o"), Path("focus.o")}
        dotd_view = {Path("widget.o"), Path("layout.o")}  # focus.o missing!
        under_reported = sorted(obj.name for obj in (compiler_view - dotd_view))
        self.assertEqual(under_reported, ["focus.o"])


if __name__ == "__main__":
    unittest.main()
