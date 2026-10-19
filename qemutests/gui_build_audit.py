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
        help="Sysroot path for sysroot audit (reserved for Task 5)",
    )
    parser.add_argument(
        "--build-dir",
        type=str,
        default=None,
        help="Build directory for sysroot audit (reserved for Task 5)",
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

    if args.assert_config:
        success = audit_assert_config(repo_root, host_cc)
        return 0 if success else 1

    if args.sysroot or args.build_dir:
        sys.stderr.write("Sysroot audit mode will be implemented in Task 5.\n")
        return 2

    parser.print_help(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
