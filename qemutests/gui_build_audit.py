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


# ── Publication contract ────────────────────────────────────────────────
# The GUI publication tree (usr/include/gui) is the ONLY place where the
# public GUI API may live.  libgui/Makefile's install rule copies exactly
# these three headers; everything else in libgui/internal is a private
# bridge header and must never reach the published generation.

GUI_PUBLIC_HEADERS = (
    "gui.h",
    "gui_platform.h",
    "gui_input.h",
)

# Private / bridge headers of libgui.  They carry the internal state layout,
# the LVGL bridge and the host-only injection hooks.  None of them may be
# published: a user program that could include them would depend on the
# library internals (and on LVGL).
GUI_PRIVATE_HEADERS = (
    "gui_input_private.h",
    "gui_platform_private.h",
    "gui_input_ps2.h",
    "gui_lvgl_private.h",
    "gui_tick.h",
)

# The public input API (libgui/include/gui/gui_input.h).  These are the
# symbols an application actually calls; the archive must export them.
GUI_PUBLIC_INPUT_SYMBOLS = (
    "gui_input_create",
    "gui_input_destroy",
    "gui_input_get_pollfds",
    "gui_input_has_mouse",
    "gui_input_dispatch",
    "gui_input_set_key_handler",
    "gui_input_set_pointer_handler",
)

# Host-only injection hooks.  They exist only under #ifdef OS01_HOST_TEST
# and must be ABSENT from the shipped archive and from the published
# headers.
GUI_TEST_HOOK_SYMBOLS = (
    "gui_input_test_feed_mouse",
    "gui_input_test_feed_keybytes",
)

# Production objects that must be archive members.  The GUI-2 input units
# (input.o / input_lvgl.o / input_focus.o / input_ps2.o) are part of the
# shipped library, not of the host fixtures.
GUI_REQUIRED_ARCHIVE_MEMBERS = (
    "platform.o",
    "tick.o",
    "gfx_client_policy.o",
    "input.o",
    "input_lvgl.o",
    "input_focus.o",
    "input_ps2.o",
)

# The probe consumer references a symbol that no archive can define; the
# negative control proves the link probe really links.
GUI_PROBE_NEGATIVE_SYMBOL = "gui_audit_symbol_that_must_not_exist"


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


# ── Profile / user build introspection ──────────────────────────────────


def _profile_vars(
    repo_root: Path,
    profile: str,
    gen_dir: Path,
    work_dir: Path,
) -> tuple[dict[str, str], str | None]:
    """Read the REAL target build variables out of the selected profile.

    The probe must use the same compiler, linker, flags and library order as
    the user build, so the values are taken from ``user/Makefile`` (which
    itself includes the profile) instead of being hard-coded.  A tiny
    makefile in the audit's temporary directory includes user/Makefile the
    same way the repository's own ``os01_submake`` helper does and prints
    the evaluated variables.

    Returns the variable map and an error message (None on success).
    """
    profile_file = repo_root / "mk" / "profiles" / f"{profile}.mk"
    if not profile_file.is_file():
        return {}, f"profile file not found: {profile_file}"

    probe_mk = work_dir / "user_build_probe.mk"
    probe_mk.write_text(
        f"OS01_PROFILE_FILE := {profile_file}\n"
        f"PROFILE := {profile}\n"
        f"SYSROOT_GENERATION_DIR := {gen_dir}\n"
        f"include {repo_root / 'user' / 'Makefile'}\n"
        "gui-audit-profile-vars:\n"
        "\t@printf '%s\\n' \"$(EFFECTIVE_CC)\" \"$(TARGET_LD)\" \"$(USER_RAW_LDFLAGS)\" "
        "\"$(CFLAGS)\" \"$(TARGET_INCDIR)\" \"$(TARGET_LIBDIR_LOCAL)\" "
        "\"$(PROFILE_CAPABILITIES)\" \"$(TARGET_TRIPLE)\"\n"
    )

    res = run_cmd(
        ["make", "-f", str(probe_mk), "--no-print-directory", "gui-audit-profile-vars"],
        cwd=repo_root,
    )
    if res.returncode != 0:
        return {}, f"failed to read the target build variables from user/Makefile:\n{res.stderr.strip()}"

    lines = [ln for ln in res.stdout.splitlines() if ln != ""]
    keys = [
        "cc",
        "ld",
        "raw_ldflags",
        "cflags",
        "incdir",
        "libdir",
        "capabilities",
        "triple",
    ]
    if len(lines) < len(keys):
        return {}, f"unexpected profile variable output: {res.stdout!r}"
    return dict(zip(keys, lines[: len(keys)])), None


def _include_paths(flags: list[str], base: Path) -> list[tuple[str, Path]]:
    """Extract the (flag, resolved path) pairs of every -I/-isystem flag."""
    out: list[tuple[str, Path]] = []
    idx = 0
    while idx < len(flags):
        tok = flags[idx]
        if tok.startswith("-isystem") and len(tok) > len("-isystem"):
            out.append((tok, Path(tok[len("-isystem"):])))
        elif tok == "-isystem" and idx + 1 < len(flags):
            out.append((tok, Path(flags[idx + 1])))
            idx += 1
        elif tok.startswith("-I") and len(tok) > len("-I"):
            out.append((tok, Path(tok[len("-I"):])))
        elif tok == "-I" and idx + 1 < len(flags):
            out.append((tok, Path(flags[idx + 1])))
            idx += 1
        idx += 1
    resolved: list[tuple[str, Path]] = []
    for tok, path in out:
        if not path.is_absolute():
            path = base / path
        resolved.append((tok, path))
    return resolved


def audit_user_flag_hygiene(
    repo_root: Path,
    profile_vars: dict[str, str],
    errors: list[str],
) -> None:
    """Ordinary user programs must not compile against LVGL.

    LVGL is an internal implementation detail of libgui.a: a user program
    that needs an LVGL include path means the private bridge leaked into the
    published contract.  The global user flags (the ones EVERY ordinary user
    program gets) must therefore be free of LVGL (and of the libgui private
    bridge) include paths.
    """
    user_dir = repo_root / "user"
    for label, raw in (
        ("user CFLAGS", profile_vars.get("cflags", "")),
        ("user LDFLAGS", profile_vars.get("raw_ldflags", "")),
    ):
        flags = raw.split()
        for tok, path in _include_paths(flags, user_dir):
            resolved = str(path)
            if "lvgl" in resolved.lower():
                errors.append(
                    f"{label} contain an LVGL include path: {tok} -> {resolved}"
                )
            if "lv_conf.h" in resolved:
                errors.append(
                    f"{label} contain the LVGL configuration include path: {tok} -> {resolved}"
                )
            for private_dir in ("libgui/internal", "libgui/src"):
                if private_dir in resolved:
                    errors.append(
                        f"{label} contain the private libgui include path: {tok} -> {resolved}"
                    )

    # The published sysroot must not expose LVGL either (checked by the
    # caller); the global user include path is the sysroot include path, so
    # verify that LVGL is unreachable through it.
    incdir = Path(profile_vars.get("incdir", ""))
    if incdir and (incdir / "lvgl").exists():
        errors.append(f"published include path exposes LVGL headers: {incdir / 'lvgl'}")


def _strip_c_comments(content: str) -> str:
    """Remove C comments so a documentation mention is not a dependency."""
    content = re.sub(r"/\*.*?\*/", " ", content, flags=re.S)
    content = re.sub(r"//[^\n]*", " ", content)
    return content


def audit_published_headers(
    gen_dir: Path,
    sysroot: Path,
    errors: list[str],
) -> None:
    """The published GUI tree must be exactly the public headers."""
    roots: list[Path] = []
    for candidate in (gen_dir, sysroot):
        if not candidate or not candidate.exists():
            continue
        resolved = candidate.resolve()
        if resolved not in [r.resolve() for r in roots]:
            roots.append(candidate)
    for root in roots:
        gui_dir = root / "usr" / "include" / "gui"
        if not gui_dir.is_dir():
            errors.append(f"published GUI include directory missing: {gui_dir}")
            continue
        found = sorted(
            str(p.relative_to(gui_dir))
            for p in gui_dir.rglob("*")
            if p.is_file()
        )
        expected = sorted(GUI_PUBLIC_HEADERS)
        if found != expected:
            errors.append(
                f"published GUI include tree {gui_dir} contains {found}, expected exactly {expected}"
            )
        else:
            # A published header may not include a gui header that is not
            # published: the consumer would fail to compile.
            for hdr in sorted(gui_dir.rglob("*.h")):
                for target in re.findall(r"#\s*include\s*<([^>]+)>", hdr.read_text()):
                    if not target.startswith("gui/"):
                        continue
                    if not (gui_dir / Path(target).name).is_file():
                        errors.append(
                            f"published header {hdr.relative_to(root)} includes the "
                            f"unpublished header {target}"
                        )

    # No private libgui header may appear ANYWHERE in the published
    # generation (a private header copied under another directory is still a
    # leak).
    for root in roots:
        for p in root.rglob("*"):
            if p.is_file() and p.name in GUI_PRIVATE_HEADERS:
                errors.append(
                    f"private libgui header published in the sysroot generation: "
                    f"{p.relative_to(root)}"
                )


# ── The real target link probe ──────────────────────────────────────────

_LINK_PROBE_CONSUMER = """\
/* Generated by qemutests/gui_build_audit.py — the GUI publication link probe.
 *
 * The consumer includes ONLY the published public header.  It must compile
 * with no private include path at all: if it needed one, the publication
 * would be wrong.  main() references the public create/dispatch entry
 * points so the link REALLY resolves them out of the shipped archive.
 */
#include <gui/gui.h>

int main(void)
{
    gui_config_t cfg;
    gui_platform_t *platform;
    gui_input_t *input;

    cfg.mode = GUI_MODE_OFFSCREEN;
    cfg.width = 64;
    cfg.height = 64;
    cfg.buffer_lines = 8;

    /* create */
    platform = gui_platform_create(&cfg);
    if (platform == 0) {
        return 1;
    }
    input = gui_input_create(platform, 0);
    if (input == 0) {
        gui_platform_destroy(platform);
        return 2;
    }

    /* dispatch */
    if (gui_input_dispatch(input, 0, 0) != 0) {
        gui_input_destroy(input);
        gui_platform_destroy(platform);
        return 3;
    }

    gui_input_destroy(input);
    gui_platform_destroy(platform);
    return 0;
}
"""

_LINK_PROBE_NEGATIVE = """\
/* Generated by qemutests/gui_build_audit.py — the NEGATIVE control of the
 * link probe.  It references a symbol that no archive can define, so the
 * link MUST fail.  A passing negative control would mean the probe is not
 * really linking.
 */
#include <gui/gui.h>

extern int PROBE_NEGATIVE_SYMBOL(void);

int main(void)
{
    gui_config_t cfg = { GUI_MODE_OFFSCREEN, 0, 0, 0 };
    (void)cfg;
    return PROBE_NEGATIVE_SYMBOL();
}
"""


def audit_link_probe(
    repo_root: Path,
    gen_dir: Path,
    profile_vars: dict[str, str],
    work_dir: Path,
    errors: list[str],
) -> None:
    """Link a real target ELF against the published sysroot.

    This is a LINK validation, not a syntax check: the consumer is compiled
    with the target toolchain and linked with the target linker, the real
    crt0/sigreturn startup objects and the real linker script, in the exact
    library order user/Makefile uses::

        crt0.o sigreturn_trampoline.o consumer.o -lgui -lgfx -lc
    """
    user_dir = repo_root / "user"
    cc = profile_vars.get("cc", "").split()
    ld = profile_vars.get("ld", "").split()
    raw_ldflags = profile_vars.get("raw_ldflags", "").split()
    cflags = profile_vars.get("cflags", "").split()

    if not cc or not ld or not raw_ldflags:
        errors.append("link probe: the target toolchain variables are empty")
        return

    # The consumer must compile against the published sysroot ONLY.  The
    # user build adds "-Icommon" (the user program's own shared directory);
    # the probe drops it and proves that nothing else is needed.
    common_dir = (user_dir / "common").resolve()

    def _is_common(path: Path) -> bool:
        try:
            return path.resolve() == common_dir
        except OSError:
            return False

    consumer_flags: list[str] = []
    idx = 0
    while idx < len(cflags):
        tok = cflags[idx]
        if tok in ("-I", "-isystem"):
            # A separated flag/value pair: drop it only when it points at
            # the user program's own common directory.
            if idx + 1 < len(cflags) and _is_common(user_dir / cflags[idx + 1]):
                idx += 2
                continue
            consumer_flags.extend(cflags[idx: idx + 2])
            idx += 2
            continue
        if tok.startswith("-I"):
            if _is_common(user_dir / tok[len("-I"):]):
                idx += 1
                continue
        elif tok.startswith("-isystem"):
            if _is_common(user_dir / tok[len("-isystem"):]):
                idx += 1
                continue
        consumer_flags.append(tok)
        idx += 1

    consumer_includes = [p for _, p in _include_paths(consumer_flags, repo_root)]
    for path in consumer_includes:
        if gen_dir not in path.parents and path != gen_dir:
            errors.append(
                f"link probe: the consumer include path {path} is outside the published sysroot"
            )

    obj_dir = work_dir / "linkprobe"
    obj_dir.mkdir(parents=True, exist_ok=True)

    # 1. The startup objects, built with the target toolchain and the same
    #    flags user/Makefile uses (cwd = user/, like the user sub-make).
    startup_srcs = {
        "crt0.o": user_dir / "crt0.S",
        "sigreturn_trampoline.o": user_dir / "sigreturn_trampoline.S",
    }
    for obj_name, src in startup_srcs.items():
        if not src.is_file():
            errors.append(f"link probe: startup source missing: {src}")
            return
        res = run_cmd(
            [*cc, *cflags, "-c", str(src), "-o", str(obj_dir / obj_name)],
            cwd=user_dir,
        )
        if res.returncode != 0:
            errors.append(
                f"link probe: building {obj_name} from {src.name} failed:\n{res.stderr.strip()}"
            )
            return

    # 2. The consumer: only <gui/gui.h>, no private include path.
    consumer_c = work_dir / "gui_link_probe_consumer.c"
    consumer_c.write_text(_LINK_PROBE_CONSUMER)
    consumer_o = obj_dir / "consumer.o"
    res = run_cmd(
        [*cc, *consumer_flags, "-c", str(consumer_c), "-o", str(consumer_o)],
        cwd=repo_root,
    )
    if res.returncode != 0:
        errors.append(
            "link probe: the public-only consumer failed to compile "
            "(the published headers are not self-sufficient):\n"
            f"{res.stderr.strip()}"
        )
        return

    # 3. The link, in the exact order user/Makefile uses.
    elf = obj_dir / "consumer.elf"
    link_cmd = [
        *ld,
        *raw_ldflags,
        "-o",
        str(elf),
        str(obj_dir / "crt0.o"),
        str(obj_dir / "sigreturn_trampoline.o"),
        str(consumer_o),
        "-lgui",
        "-lgfx",
        "-lc",
    ]
    res = run_cmd(link_cmd, cwd=user_dir)
    if res.returncode != 0:
        errors.append(
            "link probe: linking the public-only consumer against the published "
            "sysroot failed:\n"
            f"command: {' '.join(link_cmd)}\n"
            f"{res.stderr.strip()}"
        )
        return
    if not elf.is_file():
        errors.append("link probe: the linker produced no ELF")
        return

    # 4. The link must really have resolved the public symbols out of the
    #    shipped archive (a syntax-only compile would never get here).
    llvm_nm = os.environ.get("LLVM_NM", "llvm-nm")
    nm_res = run_cmd([llvm_nm, "--defined-only", str(elf)], cwd=repo_root)
    if nm_res.returncode != 0:
        errors.append(f"link probe: cannot inspect the linked ELF with {llvm_nm}")
        return
    linked = set()
    for line in nm_res.stdout.splitlines():
        parts = line.split()
        if parts:
            linked.add(parts[-1])
    for sym in ("gui_platform_create", "gui_input_create", "gui_input_dispatch"):
        if sym not in linked:
            errors.append(f"link probe: the linked ELF does not define {sym}")

    # 5. Negative control: the SAME link must FAIL when the consumer
    #    references a symbol that is not in the archive.  A probe that cannot
    #    fail proves nothing.
    neg_c = work_dir / "gui_link_probe_negative.c"
    neg_c.write_text(_LINK_PROBE_NEGATIVE.replace("PROBE_NEGATIVE_SYMBOL", GUI_PROBE_NEGATIVE_SYMBOL))
    neg_o = obj_dir / "negative.o"
    res = run_cmd(
        [*cc, *consumer_flags, "-c", str(neg_c), "-o", str(neg_o)],
        cwd=repo_root,
    )
    if res.returncode != 0:
        errors.append(f"link probe negative control failed to compile:\n{res.stderr.strip()}")
        return
    neg_link = [
        *ld,
        *raw_ldflags,
        "-o",
        str(obj_dir / "negative.elf"),
        str(obj_dir / "crt0.o"),
        str(obj_dir / "sigreturn_trampoline.o"),
        str(neg_o),
        "-lgui",
        "-lgfx",
        "-lc",
    ]
    res = run_cmd(neg_link, cwd=user_dir)
    if res.returncode == 0:
        errors.append(
            "link probe negative control FAILED: the link succeeded although the "
            f"consumer references the undefined symbol {GUI_PROBE_NEGATIVE_SYMBOL}"
        )
    else:
        print(
            "gui link probe: PASS "
            f"(negative control rejected the undefined symbol {GUI_PROBE_NEGATIVE_SYMBOL})"
        )


# ── Host fixture build contract ─────────────────────────────────────────


def _make_rule_block(text: str, target: str) -> str:
    """Return the recipe block of the make rule whose target matches.

    Only a real rule line matches (the target followed by the ``:``
    separator); a target merely mentioned in a variable list is not a rule.
    """
    target_re = re.compile(r"^\s*" + re.escape(target) + r"\s*:")
    lines = text.splitlines()
    out: list[str] = []
    collecting = False
    for line in lines:
        if not collecting:
            if target_re.match(line):
                collecting = True
                out.append(line)
            continue
        if line.strip() == "":
            out.append(line)
            continue
        if line[0] in " \t":
            out.append(line)
            continue
        break
    return "\n".join(out)


def audit_host_fixture_contract(
    repo_root: Path,
    build_dir: Path,
    errors: list[str],
) -> None:
    """The real native LVGL fixture must reuse the SINGLE host archive.

    The GUI-2 host fixtures (input-platform's recording mock and
    input-lvgl's real LVGL) must not grow a second, private copy of LVGL:
    there is exactly ONE host LVGL archive, its version comes from
    thirdpart/lvgl.manifest, and the two fixtures use separate object
    directories so their objects can never be mixed up.
    """
    host_mk = repo_root / "hosttests" / "Makefile"
    if not host_mk.is_file():
        errors.append(f"host fixture contract: {host_mk} not found")
        return
    text = host_mk.read_text()

    # (a) exactly one host LVGL archive definition
    archive_defs = re.findall(r"^\s*GUI_LVGL_ARCHIVE\s*:?=\s*(\S+)", text, re.M)
    if len(archive_defs) != 1:
        errors.append(
            f"host fixture contract: expected exactly one GUI_LVGL_ARCHIVE definition, "
            f"found {len(archive_defs)}: {archive_defs}"
        )
    else:
        if "liblvgl-host.a" not in archive_defs[0]:
            errors.append(
                f"host fixture contract: GUI_LVGL_ARCHIVE is '{archive_defs[0]}', "
                "expected the single host archive liblvgl-host.a"
            )

    # (b) no second LVGL archive may appear anywhere in the host build file
    lvgl_archive_mentions = re.findall(r"liblvgl-host\.a", text)
    if len(lvgl_archive_mentions) != 1:
        errors.append(
            "host fixture contract: the host LVGL archive must be declared exactly "
            f"once in hosttests/Makefile, found {len(lvgl_archive_mentions)} mentions"
        )
    other_archives = sorted(
        {
            m
            for m in re.findall(r"[A-Za-z0-9_./-]*\.a\b", text)
            if "liblvgl-host.a" not in m
        }
    )
    if other_archives:
        errors.append(
            "host fixture contract: hosttests/Makefile references additional archives: "
            f"{other_archives}"
        )

    # (c) the real native LVGL fixture links the single host archive
    lvgl_rule = _make_rule_block(text, "$(TEST_BLD)/test_gui_input_lvgl.elf")
    if not lvgl_rule:
        errors.append(
            "host fixture contract: no rule found for $(TEST_BLD)/test_gui_input_lvgl.elf"
        )
    elif "$(GUI_LVGL_ARCHIVE)" not in lvgl_rule:
        errors.append(
            "host fixture contract: the real native LVGL fixture does not link "
            "$(GUI_LVGL_ARCHIVE)"
        )

    # (d) the mock fixture and the real LVGL fixture use separate object dirs
    mock_dir = re.search(r"^\s*GUI_INPUT_DIR\s*:?=\s*(\S+)", text, re.M)
    real_dir = re.search(r"^\s*GUI_INPUT_LVGL_DIR\s*:?=\s*(\S+)", text, re.M)
    if not mock_dir or not real_dir:
        errors.append(
            "host fixture contract: GUI_INPUT_DIR / GUI_INPUT_LVGL_DIR are not both declared"
        )
    elif mock_dir.group(1) == real_dir.group(1):
        errors.append(
            "host fixture contract: the input-platform mock fixture and the "
            "input-lvgl real LVGL fixture share the object directory "
            f"{mock_dir.group(1)}"
        )

    # (e) the LVGL version comes from thirdpart/lvgl.manifest
    stamp_rule = _make_rule_block(text, "$(GUI_HOST_DIR)/lvgl-version.stamp")
    if not stamp_rule:
        errors.append("host fixture contract: no rule found for $(GUI_HOST_DIR)/lvgl-version.stamp")
    else:
        if "thirdpart/lvgl.manifest" not in stamp_rule:
            errors.append(
                "host fixture contract: the LVGL version stamp does not depend on "
                "thirdpart/lvgl.manifest"
            )
        if "LVGL_REVISION" not in stamp_rule:
            errors.append(
                "host fixture contract: the LVGL version stamp does not compare against "
                "the manifest LVGL_REVISION"
            )

    # (f) the pinned revision really is the checked-out submodule revision
    manifest = repo_root / "thirdpart" / "lvgl.manifest"
    if not manifest.is_file():
        errors.append(f"host fixture contract: {manifest} not found")
    else:
        rev_match = re.search(r"LVGL_REVISION\s*:?=\s*(\S+)", manifest.read_text())
        if not rev_match:
            errors.append("host fixture contract: thirdpart/lvgl.manifest has no LVGL_REVISION")
        else:
            pinned = rev_match.group(1)
            git_res = run_cmd(
                ["git", "-C", str(repo_root / "thirdpart" / "lvgl"), "rev-parse", "--verify", "HEAD"],
                cwd=repo_root,
            )
            if git_res.returncode != 0:
                errors.append(
                    "host fixture contract: cannot read the LVGL submodule revision:\n"
                    f"{git_res.stderr.strip()}"
                )
            elif git_res.stdout.strip() != pinned:
                errors.append(
                    f"host fixture contract: the LVGL submodule is at "
                    f"{git_res.stdout.strip()} but thirdpart/lvgl.manifest pins {pinned}"
                )

    # (g) at runtime there must be exactly ONE host LVGL archive on disk
    host_build = build_dir / "host-test"
    if host_build.is_dir():
        found = sorted(str(p) for p in host_build.rglob("liblvgl*.a"))
        if len(found) != 1:
            errors.append(
                f"host fixture contract: expected exactly one built host LVGL archive, "
                f"found {len(found)}: {found}"
            )


# ── The main sysroot / build audit ──────────────────────────────────────


def audit_sysroot_and_build(
    sysroot: Path,
    build_dir: Path,
    repo_root: Path,
    host_cc: str = "clang",
    profile: str = "x86_64-clang",
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

    # 2. Required files in sysroot.  gui_input.h is part of the published
    #    public API (the GUI-2 native input API), so the audit must prove it
    #    is published.  The same list is the expected staging manifest.
    required_files = [
        "usr/lib/libgui.a",
        "usr/include/gui/gui.h",
        "usr/include/gui/gui_platform.h",
        "usr/include/gui/gui_input.h",
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
        "usr/include/gui/gui_input_private.h",
        "usr/include/gui/gui_platform_private.h",
        "usr/include/gui/gui_input_ps2.h",
        "usr/include/gui/gui_tick.h",
        # The kernel's PS/2 Set 1 framer and the physical input route are
        # kernel-private interfaces: the userspace input ABI is
        # uapi/gfx.h + uapi/mouse.h and libgui decodes the raw Set 1 bytes
        # itself.  They must not be reachable from the user include path.
        "usr/include/driver/input_ps2.h",
        "usr/include/driver/input_route.h",
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

    # 3b. The published GUI tree is EXACTLY the public headers (no private
    #     bridge header, no PS/2 decoder header, no frame header).
    if gen_dir:
        audit_published_headers(gen_dir, sysroot, errors)

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
            if not has_lvgl:
                errors.append("libgui.a missing LVGL core member objects")
            for member in GUI_REQUIRED_ARCHIVE_MEMBERS:
                if member not in members:
                    errors.append(f"libgui.a missing member object: {member}")

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
            ] + list(GUI_PUBLIC_INPUT_SYMBOLS)
            for sym in required_symbols:
                if sym not in defined_syms:
                    errors.append(f"libgui.a missing expected defined symbol: {sym}")

            # The host-only injection hooks must NOT be shipped.
            for sym in GUI_TEST_HOOK_SYMBOLS:
                if sym in defined_syms:
                    errors.append(
                        f"libgui.a exports the host-only test injection hook: {sym}"
                    )
            for sym in sorted(s for s in defined_syms if s.startswith("gui_input_test_")):
                errors.append(
                    f"libgui.a exports a host-only test injection hook: {sym}"
                )

    # 6. Public header hygiene and consumer compilation
    gui_h = sysroot / "usr" / "include" / "gui" / "gui.h"
    gui_platform_h = sysroot / "usr" / "include" / "gui" / "gui_platform.h"
    gui_input_h = sysroot / "usr" / "include" / "gui" / "gui_input.h"
    if gui_h.is_file() and gui_platform_h.is_file():
        for hdr in [gui_h, gui_platform_h, gui_input_h]:
            if not hdr.is_file():
                continue
            content = hdr.read_text()
            # A prose mention of LVGL in a comment is documentation, not a
            # dependency; the code itself must not mention LVGL at all.
            code = _strip_c_comments(content)
            if "lvgl" in code.lower():
                errors.append(f"Public header {hdr.name} contains reference to 'lvgl'")
            if re.search(r"\blv_\w+", content):
                errors.append(f"Public header {hdr.name} contains reference to 'lv_' symbols")
            if "OS01_HOST_TEST" in content:
                errors.append(
                    f"Public header {hdr.name} contains the host-test switch OS01_HOST_TEST"
                )
            for hook in GUI_TEST_HOOK_SYMBOLS:
                if hook in content:
                    errors.append(
                        f"Public header {hdr.name} declares the host-only test hook {hook}"
                    )

        with tempfile.TemporaryDirectory(prefix="gui-consumer-") as td:
            temp_dir = Path(td)
            # Minimal C consumer — a SYNTAX check only.  The real link
            # validation is the target link probe below.
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
                errors.append(f"Minimal C consumer syntax check failed:\n{c_res.stderr.strip()}")
            else:
                print("gui C consumer syntax check: PASS (syntax only, not a link check)")

            # Minimal C++ consumer — a SYNTAX check only.
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
                errors.append(f"Minimal C++ consumer syntax check failed:\n{cpp_res.stderr.strip()}")
            else:
                print("gui C++ consumer syntax check: PASS (syntax only, not a link check)")

    # 7. The real target link probe (LINK validation, not a syntax check).
    if gen_dir:
        with tempfile.TemporaryDirectory(prefix="gui-link-probe-") as td:
            work_dir = Path(td)
            profile_vars, perr = _profile_vars(repo_root, profile, gen_dir, work_dir)
            if perr:
                errors.append(perr)
            else:
                caps = profile_vars.get("capabilities", "").split()
                if "userland" not in caps:
                    print(
                        f"gui link probe: SKIPPED (profile '{profile}' has no userland capability)"
                    )
                else:
                    audit_user_flag_hygiene(repo_root, profile_vars, errors)
                    audit_link_probe(repo_root, gen_dir, profile_vars, work_dir, errors)

    # 8. The host fixture build contract.
    audit_host_fixture_contract(repo_root, build_dir, errors)

    if errors:
        sys.stderr.write("GUI build / sysroot audit failed with the following errors:\n")
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        print("gui sysroot audit: FAIL")
        return False

    print("gui sysroot audit: PASS")
    return True


# ── Incremental build verification ──────────────────────────────────────


def _snapshot(root: Path) -> dict[str, int]:
    out: dict[str, int] = {}
    if root.exists():
        for p in sorted(root.rglob("*")):
            if p.is_file():
                out[str(p)] = p.stat().st_mtime_ns
    return out


def audit_incremental_build(
    repo_root: Path,
    build_dir: Path,
    profile: str,
    errors: list[str],
) -> bool:
    """Verify the incremental / republication contract of the GUI library.

    Checks:
      * a repeated build with no source change does not recompile;
      * touching gui_input.h rebuilds the library and the consumers that
        depend on it;
      * install clears orphaned files under the staging tree;
      * a new generation contains no private headers;
      * the preserved historical generations are not broken.
    """
    profile_file = repo_root / "mk" / "profiles" / f"{profile}.mk"
    libgui_dir = repo_root / "libgui"
    libgui_build = build_dir / "libgui"
    obj_root = libgui_build / "obj"
    archive = libgui_build / "lib" / "libgui.a"
    public_header = repo_root / "libgui" / "include" / "gui" / "gui_input.h"

    def libgui_make(*args: str) -> subprocess.CompletedProcess[str]:
        return run_cmd(
            [
                "make",
                "-C",
                str(libgui_dir),
                f"OS01_PROFILE_FILE={profile_file}",
                f"PROFILE={profile}",
                *args,
            ],
            cwd=repo_root,
        )

    # 1. Build, then build again: the second build must not recompile.
    res = libgui_make("all")
    if res.returncode != 0:
        errors.append(f"incremental: libgui build failed:\n{res.stderr.strip()}")
        return False
    before = _snapshot(obj_root)
    archive_before = archive.stat().st_mtime_ns if archive.is_file() else None

    res = libgui_make("all")
    if res.returncode != 0:
        errors.append(f"incremental: repeated libgui build failed:\n{res.stderr.strip()}")
        return False
    after = _snapshot(obj_root)
    if before != after:
        changed = sorted(
            k for k in set(before) | set(after) if before.get(k) != after.get(k)
        )
        errors.append(
            f"incremental: a repeated build recompiled {len(changed)} object(s): {changed[:8]}"
        )
    archive_after = archive.stat().st_mtime_ns if archive.is_file() else None
    if archive_before != archive_after:
        errors.append("incremental: a repeated build relinked libgui.a")

    # 2. Which objects depend on the public input header?
    header_dep_objs: list[Path] = []
    for dep_file in sorted(obj_root.rglob("*.d")):
        try:
            content = dep_file.read_text()
        except OSError:
            continue
        if "gui_input.h" in content:
            header_dep_objs.append(dep_file.with_suffix(".o"))
    if not header_dep_objs:
        errors.append(
            "incremental: no libgui object depends on gui_input.h — the dependency "
            "tracking is broken"
        )
        return False

    # 3. Touching the public header must rebuild the dependents and relink.
    stat_before = public_header.stat()
    try:
        os.utime(public_header, None)
        res = libgui_make("all")
        if res.returncode != 0:
            errors.append(f"incremental: libgui rebuild after touch failed:\n{res.stderr.strip()}")
            return False
        rebuilt = [
            str(obj) for obj in header_dep_objs if obj.is_file() and obj.stat().st_mtime_ns > stat_before.st_mtime_ns
        ]
        if not rebuilt:
            errors.append(
                "incremental: touching gui_input.h did not rebuild any dependent object"
            )
        if archive.is_file() and archive.stat().st_mtime_ns <= stat_before.st_mtime_ns:
            errors.append("incremental: touching gui_input.h did not relink libgui.a")
    finally:
        os.utime(public_header, ns=(stat_before.st_atime_ns, stat_before.st_mtime_ns))

    # 4. install must clear orphaned files under the staging tree.
    with tempfile.TemporaryDirectory(prefix="gui-staging-") as td:
        sim_staging = Path(td) / "staging" / "gui"
        orphan = sim_staging / "usr" / "include" / "gui" / "gui_input_private.h"
        orphan.parent.mkdir(parents=True, exist_ok=True)
        orphan.write_text("/* orphaned file */\n")
        res = libgui_make(f"INSTALL_ROOT={sim_staging}", "install")
        if res.returncode != 0:
            errors.append(f"incremental: libgui install failed:\n{res.stderr.strip()}")
            return False
        if orphan.exists():
            errors.append("incremental: install did not clear the orphaned staging file")
        sim_manifest = sim_staging / "manifest"
        if not sim_manifest.is_file():
            errors.append("incremental: install did not produce a staging manifest")
        else:
            staged = sorted(ln.strip() for ln in sim_manifest.read_text().splitlines() if ln.strip())
            expected = sorted(
                [
                    "usr/include/gui/gui.h",
                    "usr/include/gui/gui_platform.h",
                    "usr/include/gui/gui_input.h",
                    "usr/lib/libgui.a",
                    "usr/share/licenses/libgui/LVGL-LICENCE.txt",
                ]
            )
            if staged != expected:
                errors.append(
                    f"incremental: the staged manifest is {staged}, expected {expected}"
                )
        for leaked in sorted(sim_staging.rglob("*.h")):
            if leaked.name in GUI_PRIVATE_HEADERS:
                errors.append(
                    f"incremental: a private header was staged: {leaked.relative_to(sim_staging)}"
                )

    # 5. The published generations: the NEW one must be clean and the
    #    preserved historical ones must not be broken.
    generations_dir = build_dir / "sysroot-generations"
    if not generations_dir.is_dir():
        errors.append(f"incremental: no sysroot generations directory: {generations_dir}")
        return False

    before_gens = {
        p.name: _snapshot(p) for p in sorted(generations_dir.iterdir()) if p.is_dir()
    }

    # Publish a fresh generation through the repository root entry point.
    # The publisher is content-gated (an unchanged staging tree does not
    # re-publish), so the publish stamp is removed to force the real
    # republication path to run.  The stamp is recreated by the same
    # invocation.
    publish_stamp = build_dir / "stamps" / "sysroot.stamp"
    stamp_existed = publish_stamp.is_file()
    if stamp_existed:
        publish_stamp.unlink()
    res = run_cmd(["make", f"PROFILE={profile}", "sysroot"], cwd=repo_root)
    if res.returncode != 0:
        errors.append(f"incremental: publishing a new sysroot generation failed:\n{res.stderr.strip()}")
        return False

    after_gens = {
        p.name: _snapshot(p) for p in sorted(generations_dir.iterdir()) if p.is_dir()
    }
    new_gens = sorted(set(after_gens) - set(before_gens))
    if not new_gens:
        errors.append("incremental: publishing did not create a new sysroot generation")

    # The historical generations must be preserved untouched.
    for name, snap in before_gens.items():
        if name not in after_gens:
            errors.append(f"incremental: the preserved generation {name} was removed")
            continue
        if after_gens[name] != snap:
            errors.append(f"incremental: the preserved generation {name} was modified")

    # Every published generation (old and new) must expose the public GUI
    # headers only.
    for name in sorted(after_gens):
        gen = generations_dir / name
        gui_dir = gen / "usr" / "include" / "gui"
        if not gui_dir.is_dir():
            continue
        found = sorted(str(p.relative_to(gui_dir)) for p in gui_dir.rglob("*") if p.is_file())
        leaked = [f for f in found if f not in GUI_PUBLIC_HEADERS]
        if leaked:
            errors.append(
                f"incremental: the published generation {name} exposes private headers: {leaked}"
            )
        for private in GUI_PRIVATE_HEADERS:
            if (gen / "usr" / "include" / "gui" / private).exists():
                errors.append(
                    f"incremental: the published generation {name} contains the private header {private}"
                )

    # 6. The CONSUMERS: a host fixture that consumes the public input header
    #    must be rebuilt when that header changes.  The focused host case is
    #    driven through the repository ROOT entry point — never through a
    #    direct `make -C hosttests`.
    fixture = build_dir / "host-test" / "test_gui_input_decode.elf"
    host_case = "input-decode"

    def build_host_case() -> subprocess.CompletedProcess[str]:
        return run_cmd(
            ["make", f"PROFILE={profile}", "test-gui-host", f"CASE={host_case}"],
            cwd=repo_root,
        )

    res = build_host_case()
    if res.returncode != 0:
        errors.append(
            f"incremental: the focused host case '{host_case}' failed:\n{res.stdout[-2000:]}"
        )
        return False
    if not fixture.is_file():
        errors.append(f"incremental: the host fixture was not built: {fixture}")
        return False
    fixture_before = fixture.stat().st_mtime_ns

    stat_before = public_header.stat()
    try:
        os.utime(public_header, None)
        res = build_host_case()
        if res.returncode != 0:
            errors.append(
                f"incremental: the focused host case '{host_case}' failed after the "
                f"header touch:\n{res.stdout[-2000:]}"
            )
            return False
    finally:
        os.utime(public_header, ns=(stat_before.st_atime_ns, stat_before.st_mtime_ns))

    if fixture.stat().st_mtime_ns <= fixture_before:
        errors.append(
            "incremental: touching gui_input.h did not rebuild the consumer "
            f"({fixture.name})"
        )

    if errors:
        return False
    print("gui incremental build audit: PASS")
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
    parser.add_argument(
        "--profile",
        type=str,
        default=None,
        help="Profile name (defaults to PROFILE or the build directory name)",
    )
    parser.add_argument(
        "--incremental",
        action="store_true",
        help="Also verify the incremental build / republication contract",
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

        profile = args.profile or os.environ.get("PROFILE")
        if not profile:
            # build/<profile> — the build directory name IS the profile name.
            profile = build_dir_path.parent.name if build_dir_path.parent.name else "x86_64-clang"

        if not audit_sysroot_and_build(sysroot_path, build_dir_path, repo_root, host_cc, profile):
            success = False

        if args.incremental:
            ran_any = True
            incremental_errors: list[str] = []
            if not audit_incremental_build(repo_root, build_dir_path, profile, incremental_errors):
                sys.stderr.write(
                    "GUI incremental build audit failed with the following errors:\n"
                )
                for err in incremental_errors:
                    sys.stderr.write(f"  - {err}\n")
                print("gui incremental build audit: FAIL")
                success = False

    if not ran_any:
        parser.print_help(sys.stderr)
        return 2

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
