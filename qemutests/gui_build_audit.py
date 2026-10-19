#!/usr/bin/env python3
"""Audit LVGL build and assertion configurations for OS01 GUI."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run_cmd(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        capture_output=True,
        text=True,
    )


def audit_assert_config(repo_root: Path, host_cc: str) -> bool:
    """Audit LVGL assertion configuration and real macro expansion."""
    lvgl_inc = repo_root / "thirdpart" / "lvgl"
    config_inc = repo_root / "config"
    include_flags = [
        f"-I{lvgl_inc}",
        f"-I{config_inc}",
        "-DLV_CONF_INCLUDE_SIMPLE",
    ]

    errors: list[str] = []

    with tempfile.TemporaryDirectory(prefix="gui-assert-audit-") as td:
        temp_dir = Path(td)

        # 1. Macro preprocessor audit: inspect defined macros from lv_conf.h via lv_assert.h
        probe_c = temp_dir / "probe.c"
        probe_c.write_text("#include <src/misc/lv_assert.h>\n")

        dm_res = run_cmd(
            [host_cc, "-E", "-dM", *include_flags, "-x", "c", str(probe_c)],
            cwd=repo_root,
        )
        if dm_res.returncode != 0:
            errors.append(f"Preprocessor probe (-dM) failed: {dm_res.stderr.strip()}")
            macro_defs = {}
        else:
            macro_defs = {}
            for line in dm_res.stdout.splitlines():
                if line.startswith("#define "):
                    parts = line[len("#define "):].split(None, 1)
                    if len(parts) == 2:
                        macro_defs[parts[0]] = parts[1].strip()
                    elif len(parts) == 1:
                        macro_defs[parts[0]] = ""

            inc_val = macro_defs.get("LV_ASSERT_HANDLER_INCLUDE", "")
            if "unistd.h" not in inc_val:
                errors.append(
                    f"LV_ASSERT_HANDLER_INCLUDE is '{inc_val}', expected to include <unistd.h>"
                )

            handler_val = macro_defs.get("LV_ASSERT_HANDLER", "")
            if "_exit(1)" not in handler_val:
                errors.append(
                    f"LV_ASSERT_HANDLER is '{handler_val}', expected call to _exit(1)"
                )
            if "while(1)" in handler_val.replace(" ", ""):
                errors.append(
                    f"LV_ASSERT_HANDLER is '{handler_val}', still halts in an infinite loop"
                )
            if not handler_val.rstrip().endswith(";"):
                errors.append(
                    f"LV_ASSERT_HANDLER is '{handler_val}', must include trailing semicolon"
                )

        # Also inspect preprocessor expansion (-E) of LV_ASSERT(expr)
        expand_c = temp_dir / "expand.c"
        expand_c.write_text(
            "#include <src/misc/lv_assert.h>\n"
            "void probe_fn(void *p) {\n"
            "    LV_ASSERT(p != (void *)0);\n"
            "}\n"
        )
        e_res = run_cmd(
            [host_cc, "-E", *include_flags, "-x", "c", str(expand_c)],
            cwd=repo_root,
        )
        if e_res.returncode != 0:
            errors.append(f"Preprocessor expansion probe (-E) failed: {e_res.stderr.strip()}")
        else:
            expanded_text = e_res.stdout
            if "_exit(1)" not in expanded_text:
                errors.append("Expanded LV_ASSERT does not call _exit(1)")
            if "while(1)" in expanded_text.replace(" ", ""):
                errors.append("Expanded LV_ASSERT still contains while(1) loop")

        # 2. Positive compilation test: real compilation expanding all assertion macros
        pos_c = temp_dir / "pos_assert.c"
        pos_o = temp_dir / "pos_assert.o"
        pos_c.write_text(
            "#include <src/misc/lv_assert.h>\n"
            "\n"
            "void audit_positive_macro_expansion(void *p) {\n"
            "    LV_ASSERT(p != (void *)0);\n"
            "    LV_ASSERT_MSG(p != (void *)0, \"test assert message\");\n"
            "    LV_ASSERT_FORMAT_MSG(p != (void *)0, \"formatted message: %d\", 123);\n"
            "    LV_ASSERT_MALLOC(p);\n"
            "}\n"
        )
        pos_res = run_cmd(
            [host_cc, "-Wall", "-Wextra", "-Werror", "-c", *include_flags, "-x", "c", str(pos_c), "-o", str(pos_o)],
            cwd=repo_root,
        )
        if pos_res.returncode != 0:
            errors.append(f"Positive macro expansion compilation failed:\n{pos_res.stderr.strip()}")

        # 3. Negative compilation test: deliberately omit trailing semicolon
        # The real LV_ASSERT definition does not append a semicolon after LV_ASSERT_HANDLER,
        # so omitting the semicolon in LV_ASSERT_HANDLER must be rejected by the compiler.
        neg_c = temp_dir / "neg_missing_semicolon.c"
        neg_o = temp_dir / "neg_missing_semicolon.o"
        neg_c.write_text(
            "#include <unistd.h>\n"
            "#include <src/misc/lv_assert.h>\n"
            "\n"
            "#undef LV_ASSERT_HANDLER\n"
            "#define LV_ASSERT_HANDLER do { _exit(1); } while (0)\n"
            "\n"
            "void audit_negative_missing_semicolon(void *p) {\n"
            "    LV_ASSERT(p != (void *)0);\n"
            "}\n"
        )
        neg_res = run_cmd(
            [host_cc, "-c", *include_flags, "-x", "c", str(neg_c), "-o", str(neg_o)],
            cwd=repo_root,
        )
        if neg_res.returncode == 0:
            errors.append(
                "Negative audit failed: compiler unexpectedly accepted LV_ASSERT_HANDLER without trailing semicolon"
            )
        else:
            # Confirm that the compiler error was due to missing semicolon
            err_lower = neg_res.stderr.lower()
            if ";" not in err_lower and "expected" not in err_lower and "error" not in err_lower:
                errors.append(
                    f"Negative audit: expected syntax error for missing semicolon, got:\n{neg_res.stderr.strip()}"
                )

    if errors:
        sys.stderr.write("Assertion configuration audit failed with the following errors:\n")
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        return False

    print("gui assert config: PASS")
    return True


def audit_sysroot_and_build(
    sysroot: Path,
    build_dir: Path,
    repo_root: Path,
    host_cc: str = "clang",
) -> bool:
    """Audit sysroot and build directory for libgui publication contract."""
    errors: list[str] = []

    # 1. Sysroot presence and generation symlink resolution
    if not sysroot.exists():
        errors.append(f"Sysroot directory does not exist: {sysroot}")
        gen_dir = None
    else:
        gen_dir = sysroot.resolve()
        if not gen_dir.exists() or not gen_dir.is_dir():
            errors.append(f"Resolved sysroot generation directory does not exist: {gen_dir}")

    # 2. Required files in sysroot
    required_files = [
        "usr/lib/libgui.a",
        "usr/include/gui/gui.h",
        "usr/include/gui/gui_platform.h",
        "usr/share/licenses/libgui/LVGL-LICENCE.txt",
    ]
    for rel_path in required_files:
        p = sysroot / rel_path
        if not p.is_file():
            errors.append(f"Required sysroot file missing: {rel_path}")
        elif p.stat().st_size == 0:
            errors.append(f"Required sysroot file is empty: {rel_path}")

    # 3. Forbidden files in sysroot / generation
    forbidden_files = [
        "usr/lib/liblvgl.a",
        "usr/include/lvgl",
        "usr/include/gui/gui_lvgl_private.h",
    ]
    for rel_path in forbidden_files:
        p = sysroot / rel_path
        if p.exists():
            errors.append(f"Forbidden path exists in sysroot: {rel_path}")
        if gen_dir and gen_dir != sysroot:
            gp = gen_dir / rel_path
            if gp.exists():
                errors.append(f"Forbidden path exists in sysroot generation: {rel_path}")

    # Check for any lv_conf.h in sysroot
    if sysroot.exists():
        for found in sysroot.glob("**/lv_conf.h"):
            errors.append(f"Forbidden configuration header found in sysroot: {found.relative_to(sysroot)}")

    # 4. Staging validation: staging/gui must exist and match manifest
    staging_gui = build_dir / "staging" / "gui"
    manifest_file = staging_gui / "manifest"
    if not staging_gui.is_dir():
        errors.append(f"staging/gui directory missing: {staging_gui}")
    elif not manifest_file.is_file():
        errors.append(f"staging/gui/manifest missing: {manifest_file}")
    else:
        manifest_lines = [
            line.strip()
            for line in manifest_file.read_text().splitlines()
            if line.strip()
        ]
        sorted_lines = sorted(manifest_lines)
        if manifest_lines != sorted_lines:
            errors.append("staging/gui/manifest is not lexicographically sorted")

        expected_manifest = sorted(required_files)
        if manifest_lines != expected_manifest:
            errors.append(
                f"staging/gui/manifest entries {manifest_lines} do not match expected {expected_manifest}"
            )

        for rel in manifest_lines:
            staged_p = staging_gui / rel
            if not staged_p.is_file():
                errors.append(f"File listed in staging manifest does not exist: {rel}")

        # Check for unexpected files in staging/gui
        for f in staging_gui.glob("**/*"):
            if f.is_file() and f != manifest_file:
                rel = str(f.relative_to(staging_gui))
                if rel not in manifest_lines:
                    errors.append(f"Unexpected file in staging/gui not in manifest: {rel}")

    # Check that if staging/lvgl exists, none of its files leaked into sysroot
    staging_lvgl = build_dir / "staging" / "lvgl"
    if staging_lvgl.is_dir():
        lvgl_mf = staging_lvgl / "manifest"
        if lvgl_mf.is_file():
            for line in lvgl_mf.read_text().splitlines():
                rel = line.strip()
                if rel and (sysroot / rel).exists():
                    errors.append(
                        f"Legacy staging/lvgl file leaked into sysroot: {rel}"
                    )

    # 5. Build dir & archive validation
    libgui_build = build_dir / "libgui"
    archive = libgui_build / "lib" / "libgui.a"
    llvm_ar = os.environ.get("LLVM_AR", "llvm-ar")
    llvm_nm = os.environ.get("LLVM_NM", "llvm-nm")

    if not archive.is_file():
        errors.append(f"libgui archive missing: {archive}")
    else:
        # Check archive member objects
        ar_res = run_cmd([llvm_ar, "t", str(archive)], cwd=repo_root)
        if ar_res.returncode != 0:
            errors.append(f"Failed to inspect archive members with {llvm_ar}: {ar_res.stderr.strip()}")
        else:
            members = ar_res.stdout.splitlines()
            has_lvgl = any("lv_" in m for m in members)
            has_platform = any("platform.o" in m for m in members)
            has_tick = any("tick.o" in m for m in members)
            has_policy = any("gfx_client_policy.o" in m for m in members)
            if not has_lvgl:
                errors.append("libgui.a missing LVGL core member objects")
            if not has_platform:
                errors.append("libgui.a missing platform.o member object")
            if not has_tick:
                errors.append("libgui.a missing tick.o member object")
            if not has_policy:
                errors.append("libgui.a missing gfx_client_policy.o member object")

        # Check defined symbols
        nm_res = run_cmd([llvm_nm, "--defined-only", str(archive)], cwd=repo_root)
        if nm_res.returncode != 0:
            errors.append(f"Failed to inspect symbols with {llvm_nm}: {nm_res.stderr.strip()}")
        else:
            defined_syms = set()
            for line in nm_res.stdout.splitlines():
                parts = line.strip().split()
                if len(parts) >= 3:
                    defined_syms.add(parts[2])
                elif len(parts) == 2:
                    defined_syms.add(parts[1])

            required_symbols = [
                "gui_platform_create",
                "gui_platform_destroy",
                "gui_platform_step",
                "gui_platform_get_width",
                "gui_platform_get_height",
                "gui_platform_get_mode",
                "gui_platform_get_pixel",
                "gui_platform_get_lv_display",
                "lv_init",
                "lv_timer_handler",
                "gfx_client_present_policy",
            ]
            for sym in required_symbols:
                if sym not in defined_syms:
                    errors.append(f"libgui.a missing expected defined symbol: {sym}")

    # 6. Public header hygiene and consumer compilation
    gui_h = sysroot / "usr" / "include" / "gui" / "gui.h"
    gui_platform_h = sysroot / "usr" / "include" / "gui" / "gui_platform.h"
    if gui_h.is_file() and gui_platform_h.is_file():
        for hdr in [gui_h, gui_platform_h]:
            content = hdr.read_text()
            if "lvgl" in content.lower():
                errors.append(f"Public header {hdr.name} contains reference to 'lvgl'")
            if re.search(r"\blv_\w+", content):
                errors.append(f"Public header {hdr.name} contains reference to 'lv_' symbols")

        with tempfile.TemporaryDirectory(prefix="gui-consumer-") as td:
            temp_dir = Path(td)
            # Minimal C consumer
            c_src = temp_dir / "consumer.c"
            c_src.write_text(
                "#include <gui/gui.h>\n"
                "#include <gui/gui_platform.h>\n"
                "int test_consumer(void) {\n"
                "    gui_config_t cfg = {0};\n"
                "    (void)cfg;\n"
                "    return 0;\n"
                "}\n"
            )
            c_res = run_cmd(
                [
                    host_cc,
                    "-fsyntax-only",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"--sysroot={sysroot}",
                    f"-isystem{sysroot}/usr/include",
                    "-x",
                    "c",
                    str(c_src),
                ],
                cwd=repo_root,
            )
            if c_res.returncode != 0:
                errors.append(f"Minimal C consumer compile failed:\n{c_res.stderr.strip()}")

            # Minimal C++ consumer
            cpp_src = temp_dir / "consumer.cpp"
            cpp_src.write_text(
                "#include <gui/gui.h>\n"
                "#include <gui/gui_platform.h>\n"
                "int test_consumer() {\n"
                "    gui_config_t cfg = {};\n"
                "    (void)cfg;\n"
                "    return 0;\n"
                "}\n"
            )
            cpp_res = run_cmd(
                [
                    host_cc,
                    "-fsyntax-only",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"--sysroot={sysroot}",
                    f"-isystem{sysroot}/usr/include",
                    "-x",
                    "c++",
                    str(cpp_src),
                ],
                cwd=repo_root,
            )
            if cpp_res.returncode != 0:
                errors.append(f"Minimal C++ consumer compile failed:\n{cpp_res.stderr.strip()}")

    if errors:
        sys.stderr.write("GUI build / sysroot audit failed with the following errors:\n")
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        print("gui sysroot audit: FAIL")
        return False

    print("gui sysroot audit: PASS")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="GUI build and assertion configuration audit")
    parser.add_argument(
        "--assert-config",
        action="store_true",
        help="Audit LVGL fatal assertion macro configuration and expansion",
    )
    parser.add_argument(
        "--sysroot",
        type=str,
        default=None,
        help="Sysroot path for sysroot audit",
    )
    parser.add_argument(
        "--build-dir",
        type=str,
        default=None,
        help="Build directory for sysroot audit",
    )
    parser.add_argument(
        "--repo-root",
        type=str,
        default=None,
        help="Path to repository root (defaults to parent of qemutests)",
    )

    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve() if args.repo_root else Path(__file__).resolve().parents[1]
    host_cc = os.environ.get("HOST_CC", "clang")

    success = True
    ran_any = False

    if args.assert_config:
        ran_any = True
        if not audit_assert_config(repo_root, host_cc):
            success = False

    if args.sysroot or args.build_dir:
        ran_any = True
        if not args.sysroot or not args.build_dir:
            sys.stderr.write("Both --sysroot and --build-dir are required for sysroot audit.\n")
            return 2
        sysroot_path = Path(args.sysroot).resolve()
        build_dir_path = Path(args.build_dir).resolve()
        if not audit_sysroot_and_build(sysroot_path, build_dir_path, repo_root, host_cc):
            success = False

    if not ran_any:
        parser.print_help(sys.stderr)
        return 2

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())

