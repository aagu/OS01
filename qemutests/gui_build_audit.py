#!/usr/bin/env python3
"""Audit LVGL build and assertion configurations for OS01 GUI."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
from pathlib import Path, PurePosixPath
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
    # GUI-3: the Material design token table.  It is pure data (no LVGL
    # types), so it can be published; the step that turns the token table
    # into an LVGL theme stays inside libgui (libgui/src/theme_apply.c).
    "gui_theme.h",
    # GUI-2b: the public widget handle types (a value handle, never a
    # pointer) and the public event types.  Both are backend-free: the
    # handle carries a uint64_t identity, the event carries the handle.
    "gui_widget.h",
    "gui_event.h",
    # GUI-2b Task 4: the typed geometry / layout / text-mode description
    # layer.  It is backend-free: the caller describes what it wants, the
    # library turns that into backend geometry.
    "gui_layout.h",
    # GUI-2b Task 7: the single focus engine's public API.  It is
    # backend-free: the caller names a widget by its value handle and a
    # policy by an enum.
    "gui_focus.h",
    # D02: the pure userspace PS/2 Set 1 decoder.  It is backend-free (no
    # LVGL types, no device IO), so an independent application can use it
    # without the GUI platform layer.
    "gui_input_ps2.h",
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
    "gui_cursor_icon.h",
    # GUI-2b: the native widget registry bridge (fixed table, slot states,
    # the identity counter).  It names the backend object, so it must never
    # be published.
    "gui_widget_private.h",
)

def _published_relpath(path: Path, root: Path) -> tuple[str, ...] | None:
    """The normalised path of `path` relative to `root`, or None.

    The path is normalised textually (so "usr/include/gui/../include/gui/x.h"
    is NOT the same location as "usr/include/gui/x.h") but symlinks are NOT
    resolved: a link inside the published tree must never be mistaken for a
    published file.
    """
    try:
        rel = PurePosixPath(os.path.normpath(str(path))).relative_to(
            PurePosixPath(os.path.normpath(str(root)))
        )
    except ValueError:
        return None
    return rel.parts


def _is_public_gui_header(path: Path, root: Path) -> bool:
    """True only for the PUBLISHED public GUI header <root>/usr/include/gui/<name>.

    The public headers live at exactly <root>/usr/include/gui/<name>.  The
    private list is matched by BASENAME, so a public header whose name
    collides with a private one (gui_input_ps2.h is both: the published
    decoder API and the internal forwarder) must be recognised by its
    POSITION — the full canonical position relative to the published root,
    not just a basename plus an "include/gui" suffix.  A copy under another
    directory (usr/include/other/include/gui/<name>) is NOT the published
    header.

    The path is NORMALISED (os.path.normpath) before it is compared, so a
    path that denotes another directory — e.g. usr/include/gui/../include/
    gui/<name>, which normalises to usr/include/include/gui/<name> — is
    rejected, while a path that denotes the very same file through ".."
    (usr/include/gui/../gui/<name>) is accepted: after normalisation it IS
    the canonical position.  Textual paths of that kind are not produced by
    the tree walk; the rule is stated here so the policy is exact.

    A symbolic link is never the published file: it may point outside the
    published tree, so neither the file itself nor any directory inside
    the published root may be a link.
    """
    if path.name not in GUI_PUBLIC_HEADERS:
        return False
    rel = _published_relpath(path, root)
    if rel != ("usr", "include", "gui", path.name):
        return False
    current = root
    for part in rel[:-1]:
        current = current / part
        if current.is_symlink():
            return False
    return not root.joinpath(*rel).is_symlink()


def _private_header_leaks(root: Path) -> list[Path]:
    """Every private bridge header published anywhere under `root`.

    Symlinks are inspected too: a link named like a private header is a
    leak even when it does not resolve to a regular file.
    """
    leaks: list[Path] = []
    for p in sorted(root.rglob("*")):
        if not (p.is_file() or p.is_symlink()):
            continue
        if p.name not in GUI_PRIVATE_HEADERS:
            continue
        if _is_public_gui_header(p, root):
            continue        # the published decoder API, not the bridge
        leaks.append(p)
    return leaks


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

# The public widget API (libgui/include/gui/gui_widget.h).  These are the
# symbols an application actually calls.  The list grows phase by phase:
# only the symbols the phase really implements may be required, so the
# archive check never demands an export that does not exist yet.
GUI_PUBLIC_WIDGET_SYMBOLS = (
    "gui_widget_is_valid",
    "gui_widget_get_kind",
    "gui_widget_create",
    "gui_widget_destroy",
    "gui_widget_clear",
    "gui_widget_set_text",
    "gui_widget_set_visible",
    "gui_widget_set_enabled",
    "gui_widget_get_bounds",
    # GUI-2b Task 4: the typed geometry / layout / text-mode setters.
    "gui_widget_set_geometry",
    "gui_widget_set_alignment",
    "gui_widget_set_layout",
    "gui_widget_set_scroll",
    # GUI-2b Task 5: the Material appearance setter.
    "gui_widget_set_appearance",
    # GUI-2b Task 6: the event bridge setter and the platform statistics.
    "gui_widget_set_event_handler",
    "gui_platform_get_stats",
)

# The public focus API (libgui/include/gui/gui_focus.h).  These are the
# symbols an application actually calls.
GUI_PUBLIC_FOCUS_SYMBOLS = (
    "gui_focus_register",
    "gui_focus_unregister",
    "gui_focus_set",
    "gui_focus_get",
    "gui_focus_move",
    "gui_focus_set_initial_policy",
)

# Host-only injection hooks.  They exist only under #ifdef OS01_HOST_TEST
# and must be ABSENT from the shipped archive and from the published
# headers.
GUI_TEST_HOOK_SYMBOLS = (
    "gui_input_test_feed_mouse",
    "gui_input_test_feed_keybytes",
)

# The host-only hook FAMILIES.  The checks below match on the prefix, not on
# a fixed name list: a new gui_input_test_* / gui_widget_test_* symbol must
# not be able to escape the rejection.
GUI_TEST_HOOK_PREFIXES = (
    "gui_input_test_",
    "gui_widget_test_",
    "gui_focus_test_",
    "g_gui_test_",
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
    # GUI-2b: the native widget registry ships with the library.
    "widget.o",
    # GUI-2b Task 4: the typed geometry / layout / text-mode unit.
    "layout.o",
    # GUI-2b Task 5: the Material appearance unit.
    "appearance.o",
    # GUI-2b Task 6: the native event bridge unit.
    "event.o",
    # GUI-2b Task 7: the public focus API unit.
    "focus.o",
)

# The probe consumer references a symbol that no archive can define; the
# negative control proves the link probe really links.
GUI_PROBE_NEGATIVE_SYMBOL = "gui_audit_symbol_that_must_not_exist"

# ── GUI-2 guest input probe ─────────────────────────────────────────────
# The guest input probe (user/test_gui_input) is the evidence producer of
# the exclusive physical input lease: it runs inside OS01, owns the real
# /dev/keyboard + /dev/mouse lease and reports a machine-checkable protocol.
#
# Two rules are pinned here:
#
#   1. The probe keeps its *per-app* private-include exception.  The global
#      user flags must stay free of LVGL / private-bridge include paths
#      (audit_user_flag_hygiene), so the exception may only live in the
#      probe's own app.mk.
#   2. The probe must be INSTALLED in the published image: an image without
#      /bin/test_gui_input cannot exercise the protocol at all.

GUI_INPUT_PROBE_APP_MK = "user/test_gui_input/app.mk"
GUI_INPUT_PROBE_SOURCE = "user/test_gui_input/test_gui_input.c"
GUI_INPUT_PROBE_NONCE_HEADER = "user/test_gui_input/gui_input_nonce.h"
GUI_INPUT_PROBE_BINARY = "/bin/test_gui_input"
GUI_INPUT_PROBE_APP_MK_REQUIRED = (
    "APP_SOURCES := test_gui_input.c",
    "-I$(OS01_ROOT)/libgui/internal",
    "-I$(OS01_ROOT)/thirdpart",
    "-I$(OS01_ROOT)/thirdpart/lvgl",
    "-I$(OS01_ROOT)/config",
    "-DLV_CONF_INCLUDE_SIMPLE",
    "APP_LDLIBS := -lgui -lgfx",
)


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
            # The LVGL configuration header lives in config/lv_conf.h: the
            # include DIRECTORY must be resolved to the file, not matched
            # as a substring of the path.
            if (path / "lv_conf.h").is_file():
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


def _include_targets(content: str) -> list[tuple[str, str]]:
    """Every #include target of a C header as (form, target) pairs.

    Both include forms are matched: ``<angle>`` and ``"quoted"``.
    """
    out: list[tuple[str, str]] = []
    for match in re.finditer(r'#\s*include\s*([<"])([^>"]+)[>"]', content):
        out.append((match.group(1), match.group(2)))
    return out


def _read_published_header(path: Path, root: Path, errors: list[str], reported: set[Path]) -> str | None:
    """Read a published header, reporting irregular entries as errors.

    A header that is a symbolic link to a directory (or any non-regular
    file) must be a STRUCTURED audit error: reading it would raise
    IsADirectoryError and abort the whole audit.
    """
    if path in reported:
        return None
    if path.is_dir() or not path.is_file():
        reported.add(path)
        errors.append(
            f"published header {path.relative_to(root)} is not a regular file"
        )
        return None
    try:
        return path.read_text(errors="replace")
    except OSError as exc:
        reported.add(path)
        errors.append(
            f"published header {path.relative_to(root)} cannot be read: {exc}"
        )
        return None


def audit_published_headers(
    gen_dir: Path,
    sysroot: Path,
    errors: list[str],
) -> None:
    """The published GUI tree must be exactly the public headers, and the
    published sysroot must not expose any LVGL type."""
    roots: list[Path] = []
    for candidate in (gen_dir, sysroot):
        if not candidate or not candidate.exists():
            continue
        resolved = candidate.resolve()
        if resolved not in [r.resolve() for r in roots]:
            roots.append(candidate)
    for root in roots:
        gui_dir = root / "usr" / "include" / "gui"
        inc_root = root / "usr" / "include"
        # The published include tree must consist of REAL directories.  A
        # symbolic link for usr, usr/include or usr/include/gui can
        # redirect the whole tree: rglob does NOT descend into a linked
        # directory, so the per-file checks below would never see its
        # content — the generation would publish headers from outside it.
        # The ROOT itself may be a link on purpose (the sysroot points at
        # its generation), so it is deliberately not restricted here.
        redirected = False
        for rel_dir in ("usr", "usr/include", "usr/include/gui"):
            d = root / rel_dir
            if d.is_symlink():
                redirected = True
                errors.append(
                    f"symbolic link in the published include tree: "
                    f"{d.relative_to(root)} -> {os.readlink(d)}"
                )
        if redirected:
            # Do not read through the link: that tree is not part of the
            # published generation.
            continue
        reported: set[Path] = set()
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
            # A published header may not include a header that is not
            # published: the consumer would fail to compile.  Both include
            # forms are matched and the target is resolved by its FULL
            # relative path (never by basename), so <gui/sub/x.h> is not
            # accepted because a gui/x.h happens to exist.
            #
            # Resolution: an include RESOLVES when ANY of its candidate
            # paths exists.  A quoted include is first looked up next to
            # the including header, then in the include root; an angle
            # include is looked up in the include root.
            #
            # Scope: the GUI publication tree.  A tree-relative include is
            # one that names a directory ("<gui/...>", "<driver/...>"); a
            # bare "<stdio.h>" is a compiler/system header and is not the
            # publication's responsibility.
            for hdr in sorted(gui_dir.rglob("*.h")):
                content = _read_published_header(hdr, root, errors, reported)
                if content is None:
                    continue
                for form, target in _include_targets(content):
                    if form == '"':
                        candidates = [hdr.parent / target, inc_root / target]
                    elif "/" in target:
                        candidates = [inc_root / target]
                    else:
                        # A bare system/compiler header — not ours to publish.
                        continue
                    inside_candidates = []
                    for candidate in candidates:
                        try:
                            if inc_root in candidate.parents:
                                inside_candidates.append(candidate)
                        except (OSError, ValueError):
                            continue
                    if not inside_candidates:
                        continue
                    if not any(candidate.is_file() for candidate in inside_candidates):
                        errors.append(
                            f"published header {hdr.relative_to(root)} includes the "
                            f"unpublished header {target}"
                        )

        # The binding constraint is about the SYSROOT, not only the GUI
        # include tree: no published header may expose an LVGL type.
        if inc_root.is_dir():
            for hdr in sorted(inc_root.rglob("*.h")):
                code = _read_published_header(hdr, root, errors, reported)
                if code is None:
                    continue
                match = re.search(r"\blv_\w+", _strip_c_comments(code))
                if match:
                    errors.append(
                        f"published header {hdr.relative_to(root)} exposes the LVGL "
                        f"symbol {match.group(0)}"
                    )

    # No private libgui header may appear ANYWHERE in the published
    # generation (a private header copied under another directory is still a
    # leak), and the published include tree must not contain symbolic links
    # (a link may resolve outside the published tree).
    for root in roots:
        for leak in _private_header_leaks(root):
            errors.append(
                f"private libgui header published in the sysroot generation: "
                f"{leak.relative_to(root)}"
            )
        inc_root = root / "usr" / "include"
        if inc_root.is_dir():
            for link in sorted(inc_root.rglob("*")):
                if link.is_symlink():
                    errors.append(
                        f"symbolic link in the published include tree: "
                        f"{link.relative_to(root)} -> {os.readlink(link)}"
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

    /* widget handles & layout & appearance & events (GUI-2b) */
    {
        gui_widget_t widget = GUI_WIDGET_NONE;
        gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;
        gui_rect_t rect;
        gui_geometry_t geom = {0, 0, GUI_SIZE_PX(10), GUI_SIZE_FILL};
        gui_alignment_t align = {GUI_ALIGN_MID, 0, 0};
        gui_layout_t layout = {0};
        gui_scroll_t scroll = {0};
        gui_appearance_t app = GUI_APPEARANCE_DEFAULT;
        gui_platform_stats_t stats;

        (void)gui_widget_is_valid(platform, widget);
        (void)gui_widget_get_kind(platform, widget, &kind);
        (void)gui_widget_create(platform, GUI_WIDGET_NONE, GUI_WIDGET_CONTAINER, &widget);
        (void)gui_widget_set_text(platform, widget, "x");
        (void)gui_widget_set_visible(platform, widget, 1);
        (void)gui_widget_set_enabled(platform, widget, 1);
        (void)gui_widget_get_bounds(platform, widget, &rect);
        (void)gui_widget_set_geometry(platform, widget, &geom);
        (void)gui_widget_set_alignment(platform, widget, &align);
        (void)gui_widget_set_layout(platform, widget, &layout);
        (void)gui_widget_set_scroll(platform, widget, &scroll);
        (void)gui_widget_set_appearance(platform, widget, &app);
        (void)gui_widget_set_event_handler(platform, widget, (gui_event_handler_t)0, (void *)0);
        (void)gui_platform_get_stats(platform, &stats);

        /* focus engine (GUI-2b) */
        (void)gui_focus_set_initial_policy(input, GUI_FOCUS_INITIAL_MANUAL);
        (void)gui_focus_register(input, widget, GUI_FOCUS_TAB);
        (void)gui_focus_set(input, widget);
        (void)gui_focus_get(input, &widget);
        (void)gui_focus_move(input, GUI_FOCUS_FORWARD);
        (void)gui_focus_unregister(input, widget);

        (void)gui_widget_clear(platform, widget);
        (void)gui_widget_destroy(platform, widget);
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


def _consumer_flags(repo_root: Path, cflags: list[str]) -> list[str]:
    """The user build's compile flags WITHOUT the user program's own
    ``-Icommon`` directory.

    The link probes compile a consumer that must see the PUBLISHED headers
    only: the user's shared directory is the one include path that belongs
    to the program under test, so it is dropped.  Everything else (the
    sysroot include paths, the toolchain flags) is kept as it is.
    """
    user_dir = repo_root / "user"
    common_dir = (user_dir / "common").resolve()

    def _is_common(path: Path) -> bool:
        try:
            return path.resolve() == common_dir
        except OSError:
            return False

    out: list[str] = []
    idx = 0
    while idx < len(cflags):
        tok = cflags[idx]
        if tok in ("-I", "-isystem"):
            # A separated flag/value pair: drop it only when it points at
            # the user program's own common directory.
            if idx + 1 < len(cflags) and _is_common(user_dir / cflags[idx + 1]):
                idx += 2
                continue
            out.extend(cflags[idx: idx + 2])
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
        out.append(tok)
        idx += 1
    return out


def _build_startup_objects(
    repo_root: Path,
    profile_vars: dict[str, str],
    obj_dir: Path,
    errors: list[str],
    label: str,
) -> dict[str, Path] | None:
    """Build crt0.o and sigreturn_trampoline.o the way user/Makefile does.

    The startup objects are part of the TARGET link, so a consumer probe
    must link against the same ones — they are built here with the target
    toolchain and the same flags, from the same sources.
    """
    user_dir = repo_root / "user"
    cc = profile_vars.get("cc", "").split()
    cflags = profile_vars.get("cflags", "").split()
    startup_srcs = {
        "crt0.o": user_dir / "crt0.S",
        "sigreturn_trampoline.o": user_dir / "sigreturn_trampoline.S",
    }
    built: dict[str, Path] = {}
    for obj_name, src in startup_srcs.items():
        if not src.is_file():
            errors.append(f"{label}: startup source missing: {src}")
            return None
        res = run_cmd(
            [*cc, *cflags, "-c", str(src), "-o", str(obj_dir / obj_name)],
            cwd=user_dir,
        )
        if res.returncode != 0:
            errors.append(
                f"{label}: building {obj_name} from {src.name} failed:\n{res.stderr.strip()}"
            )
            return None
        built[obj_name] = obj_dir / obj_name
    return built


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
    consumer_flags = _consumer_flags(repo_root, cflags)

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
    startup = _build_startup_objects(repo_root, profile_vars, obj_dir, errors, "link probe")
    if startup is None:
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
    for sym in (
        "gui_platform_create",
        "gui_input_create",
        "gui_input_dispatch",
        *GUI_PUBLIC_WIDGET_SYMBOLS,
        *GUI_PUBLIC_FOCUS_SYMBOLS,
    ):
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


# ── D02: the published PS/2 decoder header and its consumer ──────────────
#
# The pure PS/2 decoder is published as <gui/gui_input_ps2.h>.  Two
# contracts follow from that and are checked here:
#
#   published_ps2_header   the header is really published, byte-identical
#                          to its source, self-sufficient on the published
#                          include path alone, and free of private/LVGL
#                          includes;
#   decoder_only_link_map  an independent consumer that calls ONLY
#                          gui_ps2_* links the decoder and nothing else:
#                          no platform object, no input adapter, no LVGL
#                          object, no gfx object.

PS2_PUBLIC_HEADER = "gui_input_ps2.h"
PS2_PROBE_SOURCE = "hosttests/cases/probe_gui_ps2_public.c"

# The archive members libgui may contribute to a decoder-only link.
PS2_ALLOWED_LIBGUI_MEMBERS = frozenset({"input_ps2.o"})

# libgui members that must never appear in a decoder-only link: the
# platform layer, the full input adapter layer, the widget/layout/theme
# layer and the shared gfx client policy.
PS2_FORBIDDEN_LIBGUI_MEMBERS = frozenset({
    "platform.o", "tick.o", "input.o", "input_lvgl.o", "input_focus.o",
    "widget.o", "layout.o", "appearance.o", "event.o", "focus.o",
    "theme.o", "theme_apply.o", "cursor_icon.o", "gfx_client_policy.o",
})

# Every LVGL object is named lv_*.o.
PS2_FORBIDDEN_MEMBER_PREFIXES = ("lv_",)

# The only gui_* symbols a decoder-only consumer may end up with.
PS2_ALLOWED_GUI_SYMBOLS = frozenset({
    "gui_ps2_init", "gui_ps2_feed", "gui_ps2_expire",
})

# The includes the published header may name.  Anything else (LVGL, the
# private bridge headers, the gfx client) would make the published header
# unusable by an independent consumer.
PS2_ALLOWED_HEADER_INCLUDES = frozenset({"stdint.h", "gui/gui_input.h"})


def _map_members(map_file: Path) -> list[tuple[str, str]]:
    """The (archive, member) pairs a linker map records.

    The map lists every archive member the linker pulled in, which is the
    only place the "what did this link really drag in" question can be
    answered exactly.  The target linker (lld) writes the member as
    ``<path>/<archive>.a(<member>.o):(<section>)``, so the archive name and
    the member name are read out of that shape.
    """
    try:
        text = map_file.read_text(errors="replace")
    except OSError:
        return []
    out: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for archive, member in re.findall(r"([A-Za-z0-9_.+-]+\.a)\(([^)]+)\)", text):
        key = (archive, member)
        if key in seen:
            continue
        seen.add(key)
        out.append(key)
    return out


def audit_public_header_position_guard(
    repo_root: Path,
    gen_dir: Path,
    work_dir: Path,
    errors: list[str],
) -> bool:
    """The public-header exemption is decided by the EXACT published position.

    The published decoder API shares its basename with the internal bridge
    header, so the exemption must not be reachable from another directory,
    from a ".." path or through a symbolic link.  Every counterexample below
    must be REJECTED: the bridge header counts as a leak.
    """
    ok = True

    def expect(condition: bool, message: str) -> None:
        nonlocal ok
        if not condition:
            errors.append(f"public_header_position_guard: {message}")
            ok = False

    source_public = repo_root / "libgui" / "include" / "gui" / PS2_PUBLIC_HEADER
    source_bridge = repo_root / "libgui" / "internal" / "gui_input_ps2.h"
    public_bytes = source_public.read_bytes()
    bridge_bytes = source_bridge.read_bytes()

    # 1. the canonical published position is the only exempt one
    root = work_dir / "position-guard-root"
    if root.exists():
        shutil.rmtree(root)
    canonical = root / "usr" / "include" / "gui" / PS2_PUBLIC_HEADER
    canonical.parent.mkdir(parents=True)
    canonical.write_bytes(public_bytes)
    expect(
        _is_public_gui_header(canonical, root),
        "the canonical published header was not recognised",
    )

    # 2. the same basename in ANOTHER include/gui directory is a leak
    foreign = root / "usr" / "include" / "other" / "include" / "gui" / PS2_PUBLIC_HEADER
    foreign.parent.mkdir(parents=True)
    foreign.write_bytes(bridge_bytes)
    expect(
        not _is_public_gui_header(foreign, root),
        "a bridge header under another include/gui directory was exempted",
    )
    leaks = {p.relative_to(root).as_posix() for p in _private_header_leaks(root)}
    expect(
        "usr/include/other/include/gui/gui_input_ps2.h" in leaks,
        f"the foreign bridge header was not reported as a leak (leaks: {sorted(leaks)})",
    )

    # 3. the canonical position reached through ".." is not the published one
    dotted = root / "usr" / "include" / "gui" / ".." / "include" / "gui" / PS2_PUBLIC_HEADER
    expect(
        not _is_public_gui_header(dotted, root),
        "a '..' path to the published position was exempted",
    )

    # 4. a FILE symlink at the published position is not the published file
    link_root = work_dir / "position-guard-filelink-root"
    if link_root.exists():
        shutil.rmtree(link_root)
    link_target = link_root / "usr" / "include" / "gui" / PS2_PUBLIC_HEADER
    link_target.parent.mkdir(parents=True)
    os.symlink(source_public, link_target)
    expect(
        not _is_public_gui_header(link_target, link_root),
        "a symbolic link at the published position was exempted",
    )
    link_leaks = {p.relative_to(link_root).as_posix() for p in _private_header_leaks(link_root)}
    expect(
        f"usr/include/gui/{PS2_PUBLIC_HEADER}" in link_leaks,
        f"a symbolic link named like a private header was not reported as a leak "
        f"(leaks: {sorted(link_leaks)})",
    )

    # 5. a DIRECTORY symlink for the include/gui directory is not the
    #    published tree either
    dir_root = work_dir / "position-guard-dirlink-root"
    if dir_root.exists():
        shutil.rmtree(dir_root)
    real_dir = dir_root / "elsewhere" / "gui"
    real_dir.mkdir(parents=True)
    (real_dir / PS2_PUBLIC_HEADER).write_bytes(public_bytes)
    (dir_root / "usr" / "include").mkdir(parents=True)
    os.symlink(real_dir, dir_root / "usr" / "include" / "gui")
    expect(
        not _is_public_gui_header(dir_root / "usr" / "include" / "gui" / PS2_PUBLIC_HEADER, dir_root),
        "a symlinked include/gui directory was exempted",
    )

    # 6. FULL-FUNCTION level: an ancestor directory link (usr, usr/include)
    #    must be reported by audit_published_headers itself.  The helper
    #    already refuses those paths, but rglob does not descend into a
    #    linked directory, so only the complete audit can catch them.
    for link_name, payload in (
        ("usr", ("include", "gui")),
        ("usr/include", ("gui",)),
    ):
        outside = work_dir / f"outside-{link_name.replace('/', '-')}"
        outside.mkdir(parents=True, exist_ok=True)
        for public in sorted(GUI_PUBLIC_HEADERS):
            dest = outside.joinpath(*payload, public)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(public_bytes)
        tree = work_dir / f"ancestor-{link_name.replace('/', '-')}-root"
        if tree.exists():
            shutil.rmtree(tree)
        link_path = tree / link_name
        link_path.parent.mkdir(parents=True, exist_ok=True)
        os.symlink(outside, link_path)
        full_errors: list[str] = []
        audit_published_headers(tree, tree, full_errors)
        expect(
            any("symbolic link in the published include tree" in e for e in full_errors),
            f"audit_published_headers did not report the {link_name} link "
            f"(errors: {full_errors})",
        )

    # 7. FULL-FUNCTION level: a published header that is a link to a
    #    directory must be a structured error, not an exception that
    #    aborts the audit.
    irregular = work_dir / "irregular-header-root"
    if irregular.exists():
        shutil.rmtree(irregular)
    empty_dir = irregular / "empty"
    empty_dir.mkdir(parents=True)
    odd_gui = irregular / "usr" / "include" / "gui"
    odd_gui.mkdir(parents=True)
    for public in sorted(GUI_PUBLIC_HEADERS):
        if public == PS2_PUBLIC_HEADER:
            continue
        (odd_gui / public).write_bytes(public_bytes)
    os.symlink(empty_dir, odd_gui / PS2_PUBLIC_HEADER)
    irregular_errors: list[str] = []
    try:
        audit_published_headers(irregular, irregular, irregular_errors)
        raised = False
    except Exception as exc:  # pragma: no cover - the audit must not raise
        irregular_errors.append(f"raised: {exc}")
        raised = True
    expect(not raised, f"audit_published_headers raised on an irregular header: {irregular_errors}")
    expect(
        any("not a regular file" in e for e in irregular_errors),
        f"the irregular published header was not reported: {irregular_errors}",
    )

    if ok:
        print("gui public header position guard: PASS")
    return ok


def audit_published_ps2_header(
    repo_root: Path,
    gen_dir: Path,
    profile_vars: dict[str, str],
    work_dir: Path,
    errors: list[str],
) -> bool:
    """The published <gui/gui_input_ps2.h> is really published and usable."""
    ok = True
    published = gen_dir / "usr" / "include" / "gui" / PS2_PUBLIC_HEADER
    source = repo_root / "libgui" / "include" / "gui" / PS2_PUBLIC_HEADER

    if not published.is_file():
        errors.append(
            f"published_ps2_header: the published header is missing: {published}"
        )
        return False
    if published.stat().st_size == 0:
        errors.append("published_ps2_header: the published header is empty")
        ok = False

    # The published copy must be the source, byte for byte: the install
    # rule copies the file, so any difference is a broken publication.
    if source.is_file():
        if published.read_bytes() != source.read_bytes():
            errors.append(
                "published_ps2_header: the published header differs from its "
                f"source ({source})"
            )
            ok = False
    else:
        errors.append(f"published_ps2_header: the source header is missing: {source}")
        ok = False

    # No private or LVGL include may appear in the published header.
    text = published.read_text(errors="replace")
    for inc in re.findall(r'#\s*include\s+[<"]([^>"]+)[>"]', text):
        if inc not in PS2_ALLOWED_HEADER_INCLUDES:
            errors.append(
                f"published_ps2_header: the published header includes {inc}, "
                "which an independent consumer cannot use"
            )
            ok = False

    # Self-sufficiency: the published include path ALONE must compile a
    # translation unit that includes the header and nothing else.
    cc = profile_vars.get("cc", "").split()
    if not cc:
        errors.append("published_ps2_header: the target compiler is empty")
        return False
    tu = work_dir / "ps2_selfcontained.c"
    tu.write_text(
        "#include <gui/gui_input_ps2.h>\n"
        "int os01_ps2_selfcontained(void) { return 0; }\n"
    )
    res = run_cmd(
        [
            *cc,
            "-ffreestanding",
            "-fno-builtin",
            "-std=c11",
            "-fsyntax-only",
            "-I",
            str(gen_dir / "usr" / "include"),
            str(tu),
        ],
        cwd=work_dir,
    )
    if res.returncode != 0:
        errors.append(
            "published_ps2_header: the published include path alone cannot "
            f"compile the header:\n{res.stderr.strip()}"
        )
        ok = False

    if ok:
        print(
            "published_ps2_header: PASS "
            f"({published} is published, byte-identical to its source and "
            "self-sufficient on the published include path)"
        )
    return ok


def audit_ps2_decoder_link_map(
    repo_root: Path,
    gen_dir: Path,
    profile_vars: dict[str, str],
    work_dir: Path,
    errors: list[str],
) -> bool:
    """Link an independent gui_ps2_* consumer and audit the link map.

    The consumer is the repository's probe TU (hosttests/cases/
    probe_gui_ps2_public.c).  It is compiled with the target toolchain
    against the PUBLISHED generation only, and linked in the exact order
    user/Makefile uses::

        crt0.o sigreturn_trampoline.o probe.o -lgui -lgfx -lc

    The link map then answers the question the contract asks: what did
    this consumer really drag in?  Only the decoder and libc may answer.
    """
    ok = True
    cc = profile_vars.get("cc", "").split()
    ld = profile_vars.get("ld", "").split()
    raw_ldflags = profile_vars.get("raw_ldflags", "").split()
    cflags = profile_vars.get("cflags", "").split()
    if not cc or not ld or not raw_ldflags:
        errors.append("decoder_only_link_map: the target toolchain variables are empty")
        return False

    consumer_flags = _consumer_flags(repo_root, cflags)

    # Every include path the consumer sees must be inside the published
    # generation: the probe must compile against the PUBLICATION, not
    # against the repository sources.
    include_paths = [p for _, p in _include_paths(consumer_flags, repo_root)]
    for path in include_paths:
        if gen_dir not in path.parents and path != gen_dir:
            errors.append(
                f"decoder_only_link_map: the consumer include path {path} is "
                "outside the published sysroot"
            )
            ok = False

    obj_dir = work_dir / "ps2probe"
    obj_dir.mkdir(parents=True, exist_ok=True)

    startup = _build_startup_objects(repo_root, profile_vars, obj_dir, errors, "decoder_only_link_map")
    if startup is None:
        return False

    probe_src = repo_root / PS2_PROBE_SOURCE
    if not probe_src.is_file():
        errors.append(f"decoder_only_link_map: the probe source is missing: {probe_src}")
        return False
    probe_o = obj_dir / "probe_gui_ps2_public.o"

    # -H prints every header the preprocessor opened: it is the proof that
    # the PUBLISHED decoder header (not a repository copy) was used.
    res = run_cmd(
        [*cc, *consumer_flags, "-H", "-c", str(probe_src), "-o", str(probe_o)],
        cwd=repo_root,
    )
    if res.returncode != 0:
        errors.append(
            "decoder_only_link_map: the independent consumer failed to compile "
            f"against the published headers:\n{res.stderr.strip()}"
        )
        return False
    trace = res.stderr
    published_trace = [
        ln for ln in trace.splitlines()
        if PS2_PUBLIC_HEADER in ln
    ]
    if not published_trace:
        errors.append(
            "decoder_only_link_map: the consumer did not include "
            f"<gui/{PS2_PUBLIC_HEADER}> at all"
        )
        ok = False
    for ln in published_trace:
        if str(gen_dir) not in ln:
            errors.append(
                "decoder_only_link_map: the consumer picked up the decoder "
                f"header OUTSIDE the published generation: {ln.strip()}"
            )
            ok = False

    # The link, in the exact order user/Makefile uses, with a map.
    elf = obj_dir / "ps2_probe.elf"
    map_file = obj_dir / "ps2_probe.map"
    link_cmd = [
        *ld,
        *raw_ldflags,
        f"-Map={map_file}",
        "-o",
        str(elf),
        str(startup["crt0.o"]),
        str(startup["sigreturn_trampoline.o"]),
        str(probe_o),
        "-lgui",
        "-lgfx",
        "-lc",
    ]
    res = run_cmd(link_cmd, cwd=repo_root / "user")
    if res.returncode != 0:
        errors.append(
            "decoder_only_link_map: linking the independent consumer failed:\n"
            f"command: {' '.join(link_cmd)}\n{res.stderr.strip()}"
        )
        return False
    if not elf.is_file():
        errors.append("decoder_only_link_map: the linker produced no ELF")
        return False

    members = _map_members(map_file)
    if not members:
        errors.append("decoder_only_link_map: the link map records no archive member")
        ok = False

    libgui_members = sorted({m for a, m in members if a.startswith("libgui.a")})
    libgfx_members = sorted({m for a, m in members if a.startswith("libgfx.a")})
    lvgl_members = sorted(
        {m for _a, m in members if m.startswith(PS2_FORBIDDEN_MEMBER_PREFIXES)}
    )
    forbidden = sorted(set(libgui_members) & PS2_FORBIDDEN_LIBGUI_MEMBERS)

    if not libgui_members:
        errors.append(
            "decoder_only_link_map: the link pulled NO member out of libgui.a — "
            "the decoder was not linked at all"
        )
        ok = False
    extra = sorted(set(libgui_members) - PS2_ALLOWED_LIBGUI_MEMBERS)
    if extra:
        errors.append(
            "decoder_only_link_map: libgui contributed members a decoder-only "
            f"consumer must not pull in: {extra}"
        )
        ok = False
    if forbidden:
        errors.append(
            "decoder_only_link_map: the platform/input/LVGL layer was linked in: "
            f"{forbidden}"
        )
        ok = False
    if lvgl_members:
        errors.append(
            f"decoder_only_link_map: LVGL objects were linked in: {lvgl_members}"
        )
        ok = False
    if libgfx_members:
        errors.append(
            "decoder_only_link_map: the gfx library was linked in: "
            f"{libgfx_members}"
        )
        ok = False

    # The linked ELF must really carry the decoder, and nothing else from
    # the GUI world.
    llvm_nm = os.environ.get("LLVM_NM", "llvm-nm")
    nm_res = run_cmd([llvm_nm, "--defined-only", str(elf)], cwd=repo_root)
    if nm_res.returncode != 0:
        errors.append(
            f"decoder_only_link_map: cannot inspect the linked ELF with {llvm_nm}"
        )
        ok = False
    else:
        defined = set()
        for line in nm_res.stdout.splitlines():
            parts = line.split()
            if parts:
                defined.add(parts[-1])
        missing = sorted(PS2_ALLOWED_GUI_SYMBOLS - defined)
        if missing:
            errors.append(
                "decoder_only_link_map: the linked ELF does not define the "
                f"decoder entry points: {missing}"
            )
            ok = False
        stray = sorted(
            s for s in defined
            if s.startswith("gui_") and s not in PS2_ALLOWED_GUI_SYMBOLS
        )
        if stray:
            errors.append(
                "decoder_only_link_map: the linked ELF carries GUI symbols a "
                f"decoder-only consumer must not have: {stray}"
            )
            ok = False
        stray_lvgl = sorted(s for s in defined if s.startswith("lv_"))
        if stray_lvgl:
            errors.append(
                f"decoder_only_link_map: the linked ELF carries LVGL symbols: {stray_lvgl}"
            )
            ok = False

    if ok:
        print(
            "decoder_only_link_map: PASS "
            f"(libgui members: {libgui_members}, "
            f"libc/libgfx members: {sorted({m for a, m in members if not a.startswith('libgui.a')})})"
        )
    return ok


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
    gui_mk = repo_root / "hosttests" / "mk" / "gui.mk"
    if gui_mk.is_file():
        text = gui_mk.read_text()
    else:
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


def audit_gui_input_probe(repo_root: Path, build_dir: Path, errors: list[str]) -> None:
    """Audit the GUI-2 guest input probe: its per-app private exception and
    its installation in the published image."""
    app_mk = repo_root / GUI_INPUT_PROBE_APP_MK
    source = repo_root / GUI_INPUT_PROBE_SOURCE

    if not app_mk.is_file():
        errors.append(f"The guest input probe app.mk is missing: {GUI_INPUT_PROBE_APP_MK}")
    else:
        content = app_mk.read_text()
        for required in GUI_INPUT_PROBE_APP_MK_REQUIRED:
            if required not in content:
                errors.append(
                    f"The guest input probe app.mk does not declare {required!r}"
                )

    if not source.is_file():
        errors.append(f"The guest input probe source is missing: {GUI_INPUT_PROBE_SOURCE}")

    # The nonce rule lives in its own header because the host test
    # (hosttests/cases/test_gui_input_nonce.c) compiles the REAL rule.
    nonce_header = repo_root / GUI_INPUT_PROBE_NONCE_HEADER
    if not nonce_header.is_file():
        errors.append(
            f"The guest input probe nonce header is missing: {GUI_INPUT_PROBE_NONCE_HEADER}"
        )
    else:
        content = nonce_header.read_text()
        # The nonce must be validated as hex: it is interpolated into the
        # control file's path, so a path escape must be impossible.
        for required in (
            "gui_input_nonce_is_valid",
            "gui_input_nonce_ctl_path",
            "GUI_INPUT_NONCE_MAX_CHARS",
        ):
            if required not in content:
                errors.append(
                    f"The guest input probe nonce header does not declare {required!r}"
                )

    # The probe must be installed in the published image.  The image may not
    # have been built yet, so a missing manifest is not an error; a manifest
    # that exists without the probe IS.
    manifest = build_dir / "image" / "rootfs.manifest"
    if not manifest.is_file():
        return

    entries: dict[str, tuple[str, str]] = {}
    for line in manifest.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) >= 4 and parts[0].strip() == "file":
            entries[parts[1].strip()] = (parts[2].strip(), parts[3].strip())

    if GUI_INPUT_PROBE_BINARY not in entries:
        errors.append(
            f"The guest input probe is not installed in the published image: "
            f"{GUI_INPUT_PROBE_BINARY}"
        )
        return

    staged, mode = entries[GUI_INPUT_PROBE_BINARY]
    if mode != "0755":
        errors.append(
            f"The guest input probe must be installed executable: "
            f"{GUI_INPUT_PROBE_BINARY} has mode {mode}"
        )
    if not Path(staged).is_file():
        errors.append(f"The guest input probe staged file is missing: {staged}")


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
        "usr/include/gui/gui_theme.h",
        "usr/include/gui/gui_widget.h",
        "usr/include/gui/gui_event.h",
        "usr/include/gui/gui_layout.h",
        "usr/include/gui/gui_focus.h",
        "usr/include/gui/gui_input_ps2.h",
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
        # D02: the PS/2 decoder's PUBLIC header (gui/gui_input_ps2.h) IS
        # published — the private BRIDGE (libgui/internal/gui_input_ps2.h)
        # is what must never reach the generation, and it is published
        # under no path.
        "usr/include/gui/gui_tick.h",
        "usr/include/gui/gui_cursor_icon.h",
        "usr/include/gui/gui_widget_private.h",
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

    # 3c. The GUI-2 guest input probe: its per-app private exception and its
    #     installation in the published image.
    audit_gui_input_probe(repo_root, build_dir, errors)

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
                # GUI-3: the Material design token table is public API.
                "gui_theme_colors",
                "gui_theme_font_size",
                "gui_theme_space",
                "gui_theme_radius",
                "gui_theme_elevation_shadow_width",
                "gui_theme_elevation_shadow_offset_y",
                "gui_theme_elevation_shadow_opa",
                "gui_theme_elevation_surface",
                "lv_init",
                "lv_timer_handler",
                "gfx_client_present_policy",
            ] + list(GUI_PUBLIC_INPUT_SYMBOLS) + list(GUI_PUBLIC_WIDGET_SYMBOLS) \
              + list(GUI_PUBLIC_FOCUS_SYMBOLS)
            for sym in required_symbols:
                if sym not in defined_syms:
                    errors.append(f"libgui.a missing expected defined symbol: {sym}")

            # The host-only injection hooks must NOT be shipped.  The two
            # lists overlap on purpose (the named hooks are a subset of the
            # gui_input_test_* / gui_widget_test_* families), so the offending
            # set is collected once and each symbol is reported exactly once.
            shipped_hooks = {
                sym
                for sym in defined_syms
                if any(sym.startswith(prefix) for prefix in GUI_TEST_HOOK_PREFIXES)
            }
            shipped_hooks.update(sym for sym in GUI_TEST_HOOK_SYMBOLS if sym in defined_syms)
            for sym in sorted(shipped_hooks):
                errors.append(
                    f"libgui.a exports the host-only test injection hook: {sym}"
                )

    # 6. Public header hygiene and consumer compilation
    gui_h = sysroot / "usr" / "include" / "gui" / "gui.h"
    gui_platform_h = sysroot / "usr" / "include" / "gui" / "gui_platform.h"
    gui_input_h = sysroot / "usr" / "include" / "gui" / "gui_input.h"
    gui_widget_h = sysroot / "usr" / "include" / "gui" / "gui_widget.h"
    gui_event_h = sysroot / "usr" / "include" / "gui" / "gui_event.h"
    gui_layout_h = sysroot / "usr" / "include" / "gui" / "gui_layout.h"
    gui_focus_h = sysroot / "usr" / "include" / "gui" / "gui_focus.h"
    if gui_h.is_file() and gui_platform_h.is_file():
        for hdr in [gui_h, gui_platform_h, gui_input_h, gui_widget_h, gui_event_h, gui_layout_h,
                    gui_focus_h]:
            if not hdr.is_file():
                continue
            content = hdr.read_text()
            # A prose mention of LVGL in a comment is documentation, not a
            # dependency: the CODE of a public header must not mention LVGL
            # at all.  Both the word check and the symbol check run on the
            # comment-stripped text.
            code = _strip_c_comments(content)
            if "lvgl" in code.lower():
                errors.append(f"Public header {hdr.name} contains reference to 'lvgl'")
            if re.search(r"\blv_\w+", code):
                errors.append(f"Public header {hdr.name} contains reference to 'lv_' symbols")
            if "OS01_HOST_TEST" in content:
                errors.append(
                    f"Public header {hdr.name} contains the host-test switch OS01_HOST_TEST"
                )
            # The injection hooks are a FAMILY, not a fixed name list: the
            # archive check rejects every gui_input_test_* / gui_widget_test_*
            # symbol, so the public header check must use the same family
            # match — a header declaring a different hook symbol must not
            # escape.
            for hook in sorted(
                {
                    match
                    for prefix in GUI_TEST_HOOK_PREFIXES
                    for match in re.findall(r"\b" + prefix + r"\w+", content)
                }
            ):
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
                "#include <gui/gui_input.h>\n"
                "#include <gui/gui_theme.h>\n"
                "#include <gui/gui_widget.h>\n"
                "#include <gui/gui_event.h>\n"
                "#include <gui/gui_layout.h>\n"
                "#include <gui/gui_focus.h>\n"
                "int test_consumer(void) {\n"
                "    gui_config_t cfg = {0};\n"
                "    gui_widget_t widget = GUI_WIDGET_NONE;\n"
                "    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;\n"
                "    gui_size_t size = GUI_SIZE_CONTENT;\n"
                "    gui_geometry_t geometry = {0, 0, GUI_SIZE_PX(10), GUI_SIZE_FILL};\n"
                "    gui_appearance_t appearance = GUI_APPEARANCE_DEFAULT;\n"
                "    gui_platform_stats_t stats = {0};\n"
                "    gui_platform_t *platform = (gui_platform_t *)0;\n"
                "    gui_input_t *input = (gui_input_t *)0;\n"
                "    int rc = gui_platform_get_stats(platform, &stats);\n"
                "    rc += gui_widget_set_event_handler(platform, widget, (gui_event_handler_t)0,\n"
                "                                    (void *)0);\n"
                "    rc += gui_focus_set_initial_policy(input, GUI_FOCUS_INITIAL_MANUAL);\n"
                "    rc += gui_focus_register(input, widget, GUI_FOCUS_TAB);\n"
                "    rc += gui_focus_move(input, GUI_FOCUS_FORWARD);\n"
                "    (void)rc;\n"
                "    (void)cfg;\n"
                "    (void)widget;\n"
                "    (void)kind;\n"
                "    (void)size;\n"
                "    (void)geometry;\n"
                "    (void)appearance;\n"
                "    (void)stats;\n"
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
                "#include <gui/gui_input.h>\n"
                "#include <gui/gui_theme.h>\n"
                "#include <gui/gui_widget.h>\n"
                "#include <gui/gui_event.h>\n"
                "#include <gui/gui_layout.h>\n"
                "#include <gui/gui_focus.h>\n"
                "int test_consumer() {\n"
                "    gui_config_t cfg = {};\n"
                "    gui_widget_t widget = GUI_WIDGET_NONE;\n"
                "    gui_widget_kind_t kind = GUI_WIDGET_KIND_COUNT;\n"
                "    gui_size_t size = GUI_SIZE_CONTENT;\n"
                "    gui_geometry_t geometry = {0, 0, GUI_SIZE_PX(10), GUI_SIZE_FILL};\n"
                "    gui_appearance_t appearance = GUI_APPEARANCE_DEFAULT;\n"
                "    gui_platform_stats_t stats = {};\n"
                "    gui_platform_t *platform = (gui_platform_t *)0;\n"
                "    gui_input_t *input = (gui_input_t *)0;\n"
                "    int rc = gui_platform_get_stats(platform, &stats);\n"
                "    rc += gui_widget_set_event_handler(platform, widget, (gui_event_handler_t)0,\n"
                "                                    (void *)0);\n"
                "    rc += gui_focus_set_initial_policy(input, GUI_FOCUS_INITIAL_MANUAL);\n"
                "    rc += gui_focus_register(input, widget, GUI_FOCUS_TAB);\n"
                "    rc += gui_focus_move(input, GUI_FOCUS_FORWARD);\n"
                "    (void)rc;\n"
                "    (void)cfg;\n"
                "    (void)widget;\n"
                "    (void)kind;\n"
                "    (void)size;\n"
                "    (void)geometry;\n"
                "    (void)appearance;\n"
                "    (void)stats;\n"
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
                    if not audit_published_ps2_header(
                        repo_root, gen_dir, profile_vars, work_dir, errors
                    ):
                        errors.append("published_ps2_header: FAILED")
                    if not audit_ps2_decoder_link_map(
                        repo_root, gen_dir, profile_vars, work_dir, errors
                    ):
                        errors.append("decoder_only_link_map: FAILED")
                    if not audit_public_header_position_guard(
                        repo_root, gen_dir, work_dir, errors
                    ):
                        errors.append("public_header_position_guard: FAILED")

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


# ── Incremental build verification (observe-only) ────────────────────────
#
# The INCREMENTAL modes never spawn make.  The make recipe owns every
# build step (mk/components/run.mk::test-gui-incremental) and the audit
# only records and compares snapshots.  (The --sysroot mode does spawn
# make once, read-only, to read the profile's build variables.)
#
#   --incremental-snapshot FILE --incremental-phase PHASE   record
#   --incremental-compare  FILE --incremental-phase PHASE   compare
#
# The phases mirror the four assertions of the incremental contract:
#
#   noop    a repeated build with no source change must not recompile
#   touch   touching gui_input.h must rebuild the dependents and relink
#   install install must clear orphaned files under staging/gui
#   publish a new generation must be clean and the old ones preserved
#
# The `--incremental` mode is the build-free half: it verifies the state
# invariants that need no build step (the dependency tracking itself, the
# staging manifest, the published generations).

INCREMENTAL_PHASES = ("noop", "touch", "install", "publish", "ps2rekey")

# The files libgui's install rule publishes.  The staging tree must contain
# exactly these (plus its own manifest) after an install.
GUI_STAGED_FILES = (
    "usr/include/gui/gui.h",
    "usr/include/gui/gui_platform.h",
    "usr/include/gui/gui_input.h",
    "usr/include/gui/gui_theme.h",
    "usr/include/gui/gui_widget.h",
    "usr/include/gui/gui_event.h",
    "usr/include/gui/gui_layout.h",
    "usr/include/gui/gui_focus.h",
    "usr/include/gui/gui_input_ps2.h",
    "usr/lib/libgui.a",
    "usr/share/licenses/libgui/LVGL-LICENCE.txt",
)


def _snapshot(root: Path) -> dict[str, int]:
    """Every file under root with its mtime (ns)."""
    out: dict[str, int] = {}
    if root.exists():
        for p in sorted(root.rglob("*")):
            if p.is_file():
                out[str(p)] = p.stat().st_mtime_ns
    return out


def _parse_make_deps(dep_file: Path) -> list[str]:
    """The dependency list of a make -MMD dependency file."""
    try:
        text = dep_file.read_text(errors="replace")
    except OSError:
        return []
    text = text.replace("\\\n", " ")
    _, _, deps = text.partition(":")
    return [d for d in deps.split() if d]


def _normalise_object_path(path: Path) -> Path:
    """The canonical form of a build-object path.

    Object paths are compared as SETS (the compiler view against the
    dependency-file view), so every producer must normalise the SAME way.

    The normalisation is ``os.path.normpath``: it collapses ``.``/``..`` and
    a trailing separator, but it deliberately does NOT follow symlinks.
    ``Path.resolve()`` must not be used here: the dependency-file view
    reports the object next to its ``.d`` file (unresolved), so resolving
    only one side would make a symlinked target land in the under- AND the
    over-report set at once — a false RED.
    """
    return Path(os.path.normpath(path))


def _dependent_objects(obj_root: Path, header: Path) -> list[Path]:
    """The objects whose dependency file lists the EXACT header path.

    The match is on the resolved path, never on a substring: a decoy
    dependency such as ``gui_input.h.stub`` does NOT satisfy it.
    """
    try:
        wanted = header.resolve()
    except OSError:
        wanted = header
    out: list[Path] = []
    if not obj_root.is_dir():
        return out
    for dep_file in sorted(obj_root.rglob("*.d")):
        hit = False
        for dep in _parse_make_deps(dep_file):
            try:
                if Path(dep).resolve() == wanted:
                    hit = True
                    break
            except OSError:
                continue
        if hit:
            out.append(_normalise_object_path(dep_file.with_suffix(".o")))
    return out


def _object_for_source(obj_root: Path, src: Path) -> Path | None:
    """The build object of a libgui production source.

    The build's own layout is preferred (``obj/platform/<stem>.o``).  The
    fallback never matches on the stem alone: it looks the source up in
    the dependency files, so an object that merely shares the stem with
    another source can never be picked up.
    """
    preferred = obj_root / "platform" / f"{src.stem}.o"
    if preferred.is_file():
        return _normalise_object_path(preferred)
    try:
        wanted = src.resolve()
    except OSError:
        wanted = src
    for dep_file in sorted(obj_root.rglob(f"{src.stem}.d")):
        try:
            text = dep_file.read_text(errors="replace").replace("\\\n", " ")
        except OSError:
            continue
        target, _, deps = text.partition(":")
        for dep in deps.split():
            try:
                if Path(dep).resolve() == wanted:
                    candidate = Path(target.strip())
                    if not candidate.is_absolute():
                        # A relative .d target is relative to the directory
                        # the dependency file lives in (the object's own
                        # directory).  It must be normalised the same way
                        # every other path in the pipeline is, or the same
                        # object would appear in BOTH the under- and the
                        # over-report set.
                        candidate = dep_file.parent / candidate
                    # normpath, NOT resolve: the dependency-file view
                    # reports the object unresolved, so resolving here
                    # would split a symlinked target across the under- and
                    # the over-report set.
                    return _normalise_object_path(candidate)
            except OSError:
                continue
    return None


def _compiler_dependency_view(
    compile_cmd: str,
    repo_root: Path,
    build_dir: Path,
    header: Path,
    work_dir: Path,
) -> tuple[set[Path], list[str]]:
    """The objects that REALLY depend on the header, per the compiler.

    The dependency data comes from the TARGET compiler's own dependency
    mode (-MMD) with the compile command the build uses, so the
    preprocessor decides: an ``#if 0`` include, an include inside a
    comment and an include under a false ``#ifdef`` are all excluded —
    exactly as the build sees them.  A raw-text regex cannot do this: it
    over-counts and makes the incremental gate fail on a healthy tree.

    Returns the set of objects and the errors.
    """
    errors: list[str] = []
    if not compile_cmd:
        return set(), [
            "the libgui compile command is required for the compiler dependency "
            "view (pass --libgui-compile)"
        ]
    try:
        cmd = shlex.split(compile_cmd)
    except ValueError as exc:
        return set(), [f"cannot parse the libgui compile command {compile_cmd!r}: {exc}"]
    if not cmd:
        return set(), ["the libgui compile command is empty"]

    try:
        wanted = header.resolve()
    except OSError:
        wanted = header

    obj_root = build_dir / "libgui" / "obj"
    src_dir = repo_root / "libgui" / "src"
    out: set[Path] = set()
    for src in sorted(src_dir.glob("*.c")):
        dep_file = work_dir / f"{src.stem}.compiler.d"
        res = run_cmd(
            [
                *cmd,
                "-MMD",
                "-MF",
                str(dep_file),
                "-E",
                "-o",
                os.devnull,
                str(src),
            ],
            cwd=repo_root,
        )
        if res.returncode != 0 or not dep_file.is_file():
            errors.append(
                f"the compiler dependency run for {src.name} failed:\n"
                f"{res.stderr.strip()}"
            )
            continue
        for dep in _parse_make_deps(dep_file):
            try:
                if Path(dep).resolve() == wanted:
                    obj = _object_for_source(obj_root, src)
                    if obj is not None:
                        out.add(obj)
                    break
            except OSError:
                continue
    return out, errors


def _generation_snapshots(
    generations_dir: Path,
    with_digests: bool = False,
) -> dict[str, dict[str, object]]:
    """The published generations with a per-file fingerprint.

    With ``with_digests`` the fingerprint is the file's SHA-256, so the
    comparison proves the preserved generations are BYTE-identical, not
    merely that their mtimes did not change.
    """
    import hashlib

    if not generations_dir.is_dir():
        return {}
    out: dict[str, dict[str, object]] = {}
    for gen in sorted(p for p in generations_dir.iterdir() if p.is_dir()):
        files: dict[str, object] = {}
        for p in sorted(gen.rglob("*")):
            if not p.is_file():
                continue
            rel = str(p.relative_to(gen))
            if with_digests:
                digest = hashlib.sha256()
                with p.open("rb") as handle:
                    for chunk in iter(lambda: handle.read(1 << 20), b""):
                        digest.update(chunk)
                files[rel] = digest.hexdigest()
            else:
                files[rel] = p.stat().st_mtime_ns
        out[gen.name] = files
    return out


def _incremental_paths(
    repo_root: Path, build_dir: Path, header_name: str = "gui_input.h"
) -> dict[str, Path]:
    consumer_fixtures = {
        "gui_input.h": "test_gui_input_decode.elf",
        "gui_widget.h": "test_gui_widget.elf",
        "gui_layout.h": "test_gui_widget_layout.elf",
        "gui_event.h": "test_gui_widget_event.elf",
        "gui_focus.h": "test_gui_focus.elf",
        # D02: the published PS/2 decoder header — its consumer is the
        # input-decode fixture, which compiles against the public path.
        "gui_input_ps2.h": "test_gui_input_decode.elf",
    }
    fixture_name = consumer_fixtures.get(header_name, "test_gui_input_decode.elf")
    return {
        "obj_root": build_dir / "libgui" / "obj",
        "archive": build_dir / "libgui" / "lib" / "libgui.a",
        "fixture": build_dir / "host-test" / fixture_name,
        "staging": build_dir / "staging" / "gui",
        "generations": build_dir / "sysroot-generations",
        "sysroot": build_dir / "sysroot",
        "header": repo_root / "libgui" / "include" / "gui" / header_name,
    }


def incremental_snapshot(
    repo_root: Path,
    build_dir: Path,
    profile: str,
    phase: str,
    out_file: Path,
    header_name: str = "gui_input.h",
) -> bool:
    """Record the baseline the compare step will check against."""
    errors: list[str] = []
    paths = _incremental_paths(repo_root, build_dir, header_name=header_name)
    header = paths["header"]
    if not header.is_file():
        errors.append(f"incremental: the public header is missing: {header}")
    deps = _dependent_objects(paths["obj_root"], header)

    data = {
        "phase": phase,
        "profile": profile,
        "repo_root": str(repo_root),
        "build_dir": str(build_dir),
        "header": str(header),
        "header_name": header.name,
        "dependent_objects": {str(o): (o.stat().st_mtime_ns if o.is_file() else None) for o in deps},
        "objects": _snapshot(paths["obj_root"]),
        "archive": (
            {"path": str(paths["archive"]), "mtime_ns": paths["archive"].stat().st_mtime_ns}
            if paths["archive"].is_file()
            else None
        ),
        "fixture": (
            {"path": str(paths["fixture"]), "mtime_ns": paths["fixture"].stat().st_mtime_ns}
            if paths["fixture"].is_file()
            else None
        ),
        "staging": {
            str(p.relative_to(paths["staging"])): p.stat().st_mtime_ns
            for p in sorted(paths["staging"].rglob("*"))
            if p.is_file()
        } if paths["staging"].is_dir() else {},
        "generations": _generation_snapshots(
            paths["generations"], with_digests=(phase in ("publish", "ps2rekey"))
        ),
    }
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")

    if errors:
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        return False
    print(
        f"gui incremental snapshot ({phase}): PASS "
        f"({len(deps)} object(s) depend on {header.name})"
    )
    return True


def _compare_mtimes(
    label: str,
    before: dict[str, int],
    after: dict[str, int],
    errors: list[str],
    expect_changed: bool,
) -> None:
    """Compare two mtime maps; `expect_changed` demands a difference."""
    changed = sorted(
        k for k in set(before) | set(after) if before.get(k) != after.get(k)
    )
    if expect_changed:
        if not changed:
            errors.append(f"incremental: {label} was not rebuilt")
    else:
        if changed:
            errors.append(
                f"incremental: {label} changed although nothing should have "
                f"rebuilt it: {changed[:8]}"
            )


def incremental_compare(
    repo_root: Path,
    build_dir: Path,
    profile: str,
    phase: str,
    snapshot_file: Path,
    compile_cmd: str = "",
    header_name: str = "",
) -> bool:
    """Check the recorded baseline against the current build state."""
    errors: list[str] = []
    if not snapshot_file.is_file():
        errors.append(f"incremental: the snapshot file is missing: {snapshot_file}")
        return _report_incremental(errors)
    try:
        data = json.loads(snapshot_file.read_text())
    except (OSError, ValueError) as exc:
        errors.append(f"incremental: cannot read the snapshot {snapshot_file}: {exc}")
        return _report_incremental(errors)

    if data.get("phase") != phase:
        errors.append(
            f"incremental: the snapshot was taken for phase '{data.get('phase')}' "
            f"but the compare runs phase '{phase}'"
        )
        return _report_incremental(errors)

    if not header_name:
        header_name = data.get("header_name", "gui_input.h")
    paths = _incremental_paths(repo_root, build_dir, header_name=header_name)
    header = paths["header"]

    if phase == "noop":
        _compare_mtimes(
            "the libgui objects", data.get("objects", {}), _snapshot(paths["obj_root"]), errors, False
        )
        before_archive = (data.get("archive") or {}).get("mtime_ns")
        after_archive = paths["archive"].stat().st_mtime_ns if paths["archive"].is_file() else None
        if before_archive != after_archive:
            errors.append("incremental: a repeated build relinked libgui.a")
        before_fixture = (data.get("fixture") or {}).get("mtime_ns")
        after_fixture = paths["fixture"].stat().st_mtime_ns if paths["fixture"].is_file() else None
        if before_fixture != after_fixture:
            errors.append("incremental: a repeated build rebuilt the consumer")
        _compare_generations(data.get("generations", {}), paths["generations"], errors, False)

    elif phase == "touch":
        # Two independent views of the same contract:
        #
        #   compiler_view the objects the TARGET COMPILER records as
        #                  depending on the header (-MMD with the build's
        #                  own compile command).  This is the truth: the
        #                  preprocessor decides, so an #if 0 include, an
        #                  include in a comment and an include under a
        #                  false #ifdef are all excluded.
        #   dotd_view     the objects whose BUILD dependency file lists
        #                  the exact header.  This is what make uses.
        #
        # A rebuild is required only for the objects BOTH views agree on.
        # The two views must also AGREE: a dependency file that misses an
        # object the compiler records (under-reporting) and one that
        # records an object the compiler does not (over-reporting) are
        # both dependency-tracking defects.  They are reported as two
        # distinct classes so a reader can tell them apart.
        dotd_view = set(_dependent_objects(paths["obj_root"], header))
        with tempfile.TemporaryDirectory(prefix="gui-compiler-deps-") as td:
            compiler_view, cerr = _compiler_dependency_view(
                compile_cmd, repo_root, build_dir, header, Path(td)
            )
        errors.extend(cerr)
        if not compiler_view and not cerr:
            errors.append(
                f"incremental: no libgui source depends on {header} — the audit "
                "cannot establish the rebuild contract"
            )
        under_reported = sorted(obj.name for obj in (compiler_view - dotd_view))
        if under_reported:
            errors.append(
                f"incremental: the dependency tracking UNDER-reports — the compiler "
                f"records {under_reported} as depending on {header.name} but the "
                "build dependency files do not"
            )
        over_reported = sorted(obj.name for obj in (dotd_view - compiler_view))
        if over_reported:
            errors.append(
                f"incremental: the dependency tracking OVER-reports — the build "
                f"dependency files record {over_reported} as depending on "
                f"{header.name} but the compiler does not"
            )
        objects_before = data.get("objects", {})
        for obj in sorted(compiler_view & dotd_view):
            before = objects_before.get(str(obj))
            if before is None:
                errors.append(
                    f"incremental: the dependent object {obj.name} did not exist "
                    "in the baseline"
                )
                continue
            after = obj.stat().st_mtime_ns if obj.is_file() else None
            if before == after:
                errors.append(
                    f"incremental: touching {header.name} did not rebuild the "
                    f"dependent object {obj.name}"
                )
        before_archive = (data.get("archive") or {}).get("mtime_ns")
        after_archive = paths["archive"].stat().st_mtime_ns if paths["archive"].is_file() else None
        if before_archive == after_archive:
            errors.append(
                f"incremental: touching {header.name} did not relink libgui.a"
            )
        before_fixture = (data.get("fixture") or {}).get("mtime_ns")
        after_fixture = paths["fixture"].stat().st_mtime_ns if paths["fixture"].is_file() else None
        if before_fixture is not None and before_fixture == after_fixture:
            errors.append(
                f"incremental: touching {header.name} did not rebuild the consumer "
                f"{paths['fixture'].name}"
            )

    elif phase == "install":
        staging = paths["staging"]
        if not staging.is_dir():
            errors.append(f"incremental: the staging tree is missing: {staging}")
        else:
            staged = sorted(
                str(p.relative_to(staging))
                for p in staging.rglob("*")
                if p.is_file() and p.name != "manifest"
            )
            if staged != sorted(GUI_STAGED_FILES):
                errors.append(
                    f"incremental: install did not clear the orphaned staging file(s); "
                    f"the staging tree contains {staged}, expected {sorted(GUI_STAGED_FILES)}"
                )
            manifest = staging / "manifest"
            if not manifest.is_file():
                errors.append("incremental: install did not produce a staging manifest")
            else:
                lines = sorted(
                    ln.strip() for ln in manifest.read_text().splitlines() if ln.strip()
                )
                if lines != sorted(GUI_STAGED_FILES):
                    errors.append(
                        f"incremental: the staging manifest is {lines}, expected "
                        f"{sorted(GUI_STAGED_FILES)}"
                    )
            for leaked in sorted(staging.rglob("*.h")):
                if leaked.name not in GUI_PRIVATE_HEADERS:
                    continue
                if _is_public_gui_header(leaked, staging):
                    continue        # the published decoder API, not the bridge
                errors.append(
                    f"incremental: a private header was staged: "
                    f"{leaked.relative_to(staging)}"
                )

    elif phase == "ps2rekey":
        # D02: modifying the PUBLISHED decoder header must rekey the
        # generation and rebuild the consumers.
        #
        # The staging input digest covers every file under libgui/, so a
        # change to the public header produces a NEW generation.  The
        # generation must then carry the CHANGED header (byte-wise), the
        # objects that depend on it must have been rebuilt, the archive
        # must have been relinked and the consumer fixture must have been
        # rebuilt.
        before_gens = data.get("generations", {})
        after_gens = _generation_snapshots(paths["generations"], with_digests=True)
        new_gens = sorted(set(after_gens) - set(before_gens))
        if not new_gens:
            errors.append(
                f"incremental: modifying {header.name} did not create a new "
                "sysroot generation"
            )
        rel = f"usr/include/gui/{PS2_PUBLIC_HEADER}"
        for name in new_gens:
            files = after_gens[name]
            if rel not in files:
                errors.append(
                    f"incremental: the new generation {name} does not publish {rel}"
                )
                continue
            old_digests = {
                before_gens[old].get(rel)
                for old in before_gens
                if rel in before_gens.get(old, {})
            }
            if files[rel] in old_digests:
                errors.append(
                    f"incremental: the new generation {name} publishes an "
                    f"UNCHANGED {rel} — the header change did not reach the "
                    "generation"
                )
            # The published copy must be the CURRENT source, byte for
            # byte: this states the contract ("a changed public header
            # must reach the generation") directly, without relying on
            # the digests of the older generations.
            published = paths["generations"] / name / rel
            source_bytes = header.read_bytes() if header.is_file() else None
            if source_bytes is not None and published.is_file():
                if published.read_bytes() != source_bytes:
                    errors.append(
                        f"incremental: the new generation {name} does not "
                        f"publish the current content of {rel} — the header "
                        "change did not reach the generation"
                    )
        dotd_view = set(_dependent_objects(paths["obj_root"], header))
        if not dotd_view:
            errors.append(
                f"incremental: no libgui object depends on {header.name} — the "
                "rebuild contract cannot be established"
            )
        objects_before = data.get("objects", {})
        for obj in sorted(dotd_view):
            before = objects_before.get(str(obj))
            after = obj.stat().st_mtime_ns if obj.is_file() else None
            if before is None or before == after:
                errors.append(
                    f"incremental: modifying {header.name} did not rebuild the "
                    f"dependent object {obj.name}"
                )
        before_archive = (data.get("archive") or {}).get("mtime_ns")
        after_archive = paths["archive"].stat().st_mtime_ns if paths["archive"].is_file() else None
        if before_archive == after_archive:
            errors.append(
                f"incremental: modifying {header.name} did not relink libgui.a"
            )
        before_fixture = (data.get("fixture") or {}).get("mtime_ns")
        after_fixture = paths["fixture"].stat().st_mtime_ns if paths["fixture"].is_file() else None
        if before_fixture is not None and before_fixture == after_fixture:
            errors.append(
                f"incremental: modifying {header.name} did not rebuild the "
                f"consumer {paths['fixture'].name}"
            )

    elif phase == "publish":
        before_gens = data.get("generations", {})
        after_gens = _generation_snapshots(paths["generations"], with_digests=True)
        new_gens = sorted(set(after_gens) - set(before_gens))
        if not new_gens:
            errors.append("incremental: publishing did not create a new sysroot generation")
        _compare_generations(
            before_gens, paths["generations"], errors, False, with_digests=True
        )
        for name in new_gens:
            gen = paths["generations"] / name
            gui_dir = gen / "usr" / "include" / "gui"
            if not gui_dir.is_dir():
                errors.append(
                    f"incremental: the new generation {name} has no published GUI tree"
                )
                continue
            found = sorted(
                str(p.relative_to(gui_dir)) for p in gui_dir.rglob("*") if p.is_file()
            )
            leaked = [f for f in found if f not in GUI_PUBLIC_HEADERS]
            if leaked:
                errors.append(
                    f"incremental: the new generation {name} exposes private headers: {leaked}"
                )
        if new_gens and paths["sysroot"].is_symlink():
            current = os.readlink(paths["sysroot"])
            if Path(current).name not in new_gens:
                errors.append(
                    f"incremental: the published sysroot does not point at the new "
                    f"generation {new_gens[-1]} (it points at {current})"
                )

    else:
        errors.append(f"incremental: unknown phase '{phase}'")

    return _report_incremental(errors)


def _compare_generations(
    before: dict[str, dict[str, object]],
    generations_dir: Path,
    errors: list[str],
    expect_new: bool,
    with_digests: bool = False,
) -> None:
    """The preserved historical generations must not be broken.

    With ``with_digests`` the comparison is byte-exact (SHA-256 per file);
    otherwise it compares the mtimes.
    """
    after = _generation_snapshots(generations_dir, with_digests=with_digests)
    for name, snap in before.items():
        if name not in after:
            errors.append(f"incremental: the preserved generation {name} was removed")
            continue
        if after[name] != snap:
            changed = sorted(
                k for k in set(snap) | set(after[name]) if snap.get(k) != after[name].get(k)
            )
            errors.append(
                f"incremental: the preserved generation {name} was modified: {changed[:8]}"
            )
    if expect_new and not (set(after) - set(before)):
        errors.append("incremental: no new sysroot generation was published")


def _report_incremental(errors: list[str]) -> bool:
    if errors:
        sys.stderr.write("GUI incremental build audit failed with the following errors:\n")
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        print("gui incremental build audit: FAIL")
        return False
    print("gui incremental build audit: PASS")
    return True


def audit_incremental_state(
    repo_root: Path,
    build_dir: Path,
    errors: list[str],
    compile_cmd: str = "",
) -> bool:
    """The build-free half of the incremental contract.

    Verifies the invariants that need no build step:
      * the libgui dependency tracking names the EXACT public headers (a
        decoy dependency or a missing dependency fails here) across all
        public headers with library dependents;
      * the staging tree contains exactly the published files;
      * every published generation exposes only the public GUI headers.
    """
    paths = _incremental_paths(repo_root, build_dir)
    incremental_headers = (
        "gui_input.h",
        "gui_widget.h",
        "gui_layout.h",
        "gui_event.h",
        "gui_focus.h",
    )
    for hdr_name in incremental_headers:
        header = repo_root / "libgui" / "include" / "gui" / hdr_name
        if not header.is_file():
            errors.append(f"incremental: the public header is missing: {header}")
            continue

        deps = set(_dependent_objects(paths["obj_root"], header))
        compiler_view: set[Path] = set()
        cerr: list[str] = []
        if compile_cmd:
            with tempfile.TemporaryDirectory(prefix="gui-compiler-deps-") as td:
                compiler_view, cerr = _compiler_dependency_view(
                    compile_cmd, repo_root, build_dir, header, Path(td)
                )
            errors.extend(cerr)
        if compile_cmd and not compiler_view and not cerr:
            errors.append(
                f"incremental: no libgui source depends on {header} — the audit cannot "
                "establish the rebuild contract"
            )
        if not deps:
            errors.append(
                f"incremental: no libgui object depends on {header} — the dependency "
                "tracking is broken"
            )
        if compile_cmd:
            under_reported = sorted(obj.name for obj in (compiler_view - deps))
            if under_reported:
                errors.append(
                    f"incremental: the dependency tracking UNDER-reports — the compiler "
                    f"records {under_reported} as depending on {header.name} but the "
                    "build dependency files do not"
                )
            over_reported = sorted(obj.name for obj in (deps - compiler_view))
            if over_reported:
                errors.append(
                    f"incremental: the dependency tracking OVER-reports — the build "
                    f"dependency files record {over_reported} as depending on "
                    f"{header.name} but the compiler does not"
                )
        print(
            f"gui incremental state: {len(compiler_view) or len(deps)} libgui object(s) "
            f"depend on {header.name} "
            f"(compiler view: {len(compiler_view)}, dependency-file view: {len(deps)})"
        )

    staging = paths["staging"]
    if staging.is_dir():
        staged = sorted(
            str(p.relative_to(staging))
            for p in staging.rglob("*")
            if p.is_file() and p.name != "manifest"
        )
        if staged != sorted(GUI_STAGED_FILES):
            errors.append(
                f"incremental: the staging tree contains {staged}, expected "
                f"{sorted(GUI_STAGED_FILES)}"
            )

    generations = paths["generations"]
    if not generations.is_dir():
        errors.append(f"incremental: no sysroot generations directory: {generations}")
    else:
        for gen in sorted(p for p in generations.iterdir() if p.is_dir()):
            gui_dir = gen / "usr" / "include" / "gui"
            if not gui_dir.is_dir():
                continue
            found = sorted(
                str(p.relative_to(gui_dir)) for p in gui_dir.rglob("*") if p.is_file()
            )
            leaked = [f for f in found if f not in GUI_PUBLIC_HEADERS]
            if leaked:
                errors.append(
                    f"incremental: the published generation {gen.name} exposes "
                    f"private headers: {leaked}"
                )
            for private in GUI_PRIVATE_HEADERS:
                if (gen / "usr" / "include" / "gui" / private).exists():
                    errors.append(
                        f"incremental: the published generation {gen.name} contains "
                        f"the private header {private}"
                    )

    if errors:
        return False
    print("gui incremental state audit: PASS")
    return True


def _read_compile_command(explicit: str | None, build_dir: Path) -> str:
    """The libgui compile command the build uses.

    The audit does not spawn make for this: the recipe writes the command
    out (the build system knows it exactly) and the audit only reads it.
    """
    candidate = Path(explicit) if explicit else build_dir / "incr-libgui-compile.txt"
    if not candidate.is_file():
        return ""
    text = candidate.read_text(errors="replace").strip()
    # The file holds one command line; take the first non-empty one and
    # skip the make driver's own chatter ("make: Entering directory ...").
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("make:") or line.startswith("make["):
            continue
        return line
    return ""


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
        help="Verify the build-free half of the incremental contract "
             "(dependency tracking, staging tree, published generations)",
    )
    parser.add_argument(
        "--incremental-snapshot",
        type=str,
        default=None,
        help="Record an incremental baseline into the given file "
             "(the incremental modes never spawn make: the recipe drives "
             "the builds)",
    )
    parser.add_argument(
        "--incremental-compare",
        type=str,
        default=None,
        help="Compare the current build state against a recorded baseline",
    )
    parser.add_argument(
        "--incremental-phase",
        type=str,
        default=None,
        choices=list(INCREMENTAL_PHASES),
        help="The incremental phase a snapshot/compare pair belongs to",
    )
    parser.add_argument(
        "--header",
        type=str,
        default="gui_input.h",
        help="Header to audit in incremental touch phase (default: gui_input.h)",
    )
    parser.add_argument(
        "--libgui-compile",
        type=str,
        default=None,
        help="File holding the libgui compile command (one line).  The audit "
             "recomputes the compiler's own dependency view with it.  Defaults "
             "to <build-dir>/incr-libgui-compile.txt when that file exists.",
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

    if args.sysroot and args.build_dir:
        ran_any = True
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
        if not args.build_dir:
            sys.stderr.write("--build-dir is required for the incremental state audit.\n")
            return 2
        build_dir_path = Path(args.build_dir).resolve()
        incremental_errors: list[str] = []
        compile_cmd = _read_compile_command(args.libgui_compile, build_dir_path)
        if not audit_incremental_state(
            repo_root, build_dir_path, incremental_errors, compile_cmd
        ):
            sys.stderr.write(
                "GUI incremental build audit failed with the following errors:\n"
            )
            for err in incremental_errors:
                sys.stderr.write(f"  - {err}\n")
            print("gui incremental build audit: FAIL")
            success = False

    # The snapshot / compare pair needs the build directory only: it never
    # touches the published sysroot, it only records and compares build
    # state.  The make recipe (mk/components/run.mk::test-gui-incremental)
    # owns every build step — the incremental modes never spawn make.
    if args.incremental_snapshot or args.incremental_compare:
        ran_any = True
        if not args.build_dir:
            sys.stderr.write("--build-dir is required for the incremental snapshot/compare.\n")
            return 2
        if not args.incremental_phase:
            sys.stderr.write(
                "--incremental-snapshot/--incremental-compare require --incremental-phase.\n"
            )
            return 2
        build_dir_path = Path(args.build_dir).resolve()
        profile = args.profile or os.environ.get("PROFILE") or (
            build_dir_path.parent.name if build_dir_path.parent.name else "x86_64-clang"
        )
        if args.incremental_snapshot:
            if not incremental_snapshot(
                repo_root,
                build_dir_path,
                profile,
                args.incremental_phase,
                Path(args.incremental_snapshot),
                header_name=args.header,
            ):
                success = False
        if args.incremental_compare:
            if not incremental_compare(
                repo_root,
                build_dir_path,
                profile,
                args.incremental_phase,
                Path(args.incremental_compare),
                _read_compile_command(args.libgui_compile, build_dir_path),
                header_name=args.header,
            ):
                success = False

    if not ran_any:
        parser.print_help(sys.stderr)
        return 2

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
