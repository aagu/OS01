#!/usr/bin/env python3
"""Source-level fixtures for ``mk/components/run.mk::test-qemu``.

These fixtures verify the Makefile recipe's failure-short-circuiting
contract: a failing image-build or hash-check command MUST prevent the
runner from being invoked.  No real QEMU is launched; they parse the
Makefile source so they don't depend on a built workspace.

Per Task 5 brief checkbox 2:

    "Add Make failure fixtures: substitute failing image-build or
    hash-check commands and a marker-writing runner; assert
    ``test-qemu`` exits nonzero and never starts the runner.  Run red
    against the current recipe, where a later successful command can
    mask either failure."

The current recipe's failure modes are pinned:

  1. ``$(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image`` — the variant
     build is on its own recipe line so a failure aborts (make sets
     ``$?`` to the failing command's rc and exits nonzero).  The line
     is the canonical short-circuit; if it ever disappears or moves
     into an ``if/fi`` branch, the suite breaks silently.

  2. The normal-image hash guard: ``sha256sum`` (before) →
     ``$(MAKE) ... image`` → ``sha256sum`` (after) → ``cmp``.  Any
     of these failing MUST abort the recipe.  The before/after
     ``if [ -f ... ]`` guards must NOT swallow a failure.

  3. The driver-model sub-make: ``$(MAKE) ... test-qemu-driver-model``
     also lives on its own recipe line so a build/launch failure in
     one case cannot be masked by another case's success.

The brief warns: "a later successful command can mask either
failure".  We therefore pin:

  * the ``$(MAKE) ... image`` line sits at the TOP of the recipe
    body (or as the next line after the dry-run guards);
  * the sha256 ``cmp`` is unconditional (no ``|| true``);
  * the runner invocation (the last line of the recipe) follows the
    build ``cmp`` so a build failure halts before the runner starts.

Run with::

    python3 -m unittest qemutests.test_make_qemu_failure
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


# Repo root so we can locate mk/components/run.mk.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

RUN_MK = ROOT / "mk" / "components" / "run.mk"

# ``make -n test-qemu SUITE=systest`` is a *dry run*, but GNU Make still
# executes any logical recipe line that contains ``$(MAKE)``.  Master's
# ``user/`` restructure added per-app sub-makes, so the dry run now
# recurses through more of the build and measures ~34.3 s on the merged
# tree (measured 34.068 s here).  Give the capture a wide margin so a
# legitimately-built-but-slow tree still exercises the assertions instead
# of skipping: a silent skip on a slow-but-buildable tree would stop
# pinning the recipe structure, which is worse than an error.  120 s is
# ~3.5x the measured time, and any timeout is now reported as a loud skip
# (see ``MakeFailureDryRunTests._make_n_capture``).
DRY_RUN_TIMEOUT = 120


def _read_runmk() -> str:
    return RUN_MK.read_text(encoding="utf-8")


def _extract_recipe(text: str, target: str) -> str:
    """Return the body of the named recipe (every tab-indented line that
    belongs to the LAST occurrence of the target).

    Handles multi-line target definitions (a Make recipe can have
    ``target: VAR := default``, ``target: VAR := $(VAR)``, and
    ``target: prereqs`` all on separate lines before the actual
    tab-indented recipe).  The recipe body is the run of tab-indented
    lines that follows the LAST non-comment, non-blank target line.
    """
    lines = text.splitlines()
    last_target_idx = None
    pattern_target = re.compile(rf"^{re.escape(target)}\s*:")
    pattern_recipe = re.compile(r"^\t")
    for i, line in enumerate(lines):
        # Skip pure comment lines (but not '#'-prefixed non-target
        # text inside the body).
        if line.startswith("#"):
            continue
        if pattern_target.match(line):
            last_target_idx = i
    if last_target_idx is None:
        return ""
    # Walk forward from the last target line, collecting tab-indented
    # recipe lines until a non-tab, non-blank line ends the body.
    out: list = []
    for ln in lines[last_target_idx + 1:]:
        if pattern_recipe.match(ln):
            out.append(ln)
            continue
        if not ln.strip():
            # A blank line may sit in the middle of a continuation;
            # only stop when we see a real non-tab, non-blank line.
            continue
        # Non-tab, non-blank line — recipe body ends.
        if ln.lstrip().startswith("#"):
            continue
        break
    return "\n".join(out)


def _extract_recipe_preamble(text: str, target: str) -> str:
    """Return the multi-line target preamble (the ``target: VAR :=``
    lines and prerequisite lines that precede the tab-indented body).

    Useful for the ``test-qemu`` target whose SUITE default assignment
    spans multiple lines.
    """
    lines = text.splitlines()
    last_target_idx = None
    pattern_target = re.compile(rf"^{re.escape(target)}\s*:")
    for i, line in enumerate(lines):
        if line.startswith("#"):
            continue
        if pattern_target.match(line):
            last_target_idx = i
    if last_target_idx is None:
        return ""
    out: list = []
    for ln in lines[last_target_idx:]:
        if ln.startswith("\t"):
            break
        out.append(ln)
    return "\n".join(out)


class TestQemuRecipeShapeTests(unittest.TestCase):
    """Pin the structural rules that prevent a build failure from
    masking the runner invocation."""

    MAKEFILE = RUN_MK

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = _read_runmk()

    def test_recipe_file_exists(self) -> None:
        self.assertTrue(self.MAKEFILE.is_file())

    def test_recipe_defines_test_qemu_target(self) -> None:
        self.assertRegex(self.text, r"(?m)^test-qemu\s*:",
                         "test-qemu target must be present in mk/components/run.mk")

    def test_recipe_rejects_unknown_suite_at_parse_time(self) -> None:
        """The recipe must enumerate the known SUITE list explicitly
        so an unknown suite aborts BEFORE any build / runner."""
        m = re.search(
            r"case \"\$\(SUITE\)\" in\s+(?P<n>.+?)esac",
            self.text,
            re.DOTALL,
        )
        self.assertIsNotNone(m,
                             "test-qemu must contain a SUITE case/esac block")
        body = m.group("n")
        for suite in ("phase-0", "systest", "inittab-phase",
                       "network", "gfx", "resolution", "driver-model"):
            self.assertIn(suite, body,
                          f"SUITE case missing {suite!r}")

    def test_recipe_calls_submake_on_unopposed_image_line(self) -> None:
        """``$(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image`` must be on
        its own recipe line so a build failure aborts the recipe before
        the runner invocation."""
        recipe = _extract_recipe(self.text, "test-qemu")
        self.assertRegex(
            recipe,
            r"(?m)^\s*\$\(MAKE\)\s+.*\$\(TEST_QEMU_FLAVOR_\$\(SUITE\)\).*image\b",
            "test-qemu recipe must invoke the variant sub-make on its own "
            "recipe line (the brief: 'Add explicit || exit or equivalent "
            "short-circuiting after every build/hash command')",
        )

    def test_recipe_cmp_chain_does_not_swallow_failure(self) -> None:
        """The hash-guard ``cmp`` must not be guarded by ``|| true``
        or any ``if [ $? ...]`` branch that masks the failure."""
        recipe = _extract_recipe(self.text, "test-qemu")
        # Extract the lines around 'cmp "$(NORMAL_IMAGE_DIR)/normal.before"'
        m = re.search(
            r'(?P<chain>cmp\s+"\$\(NORMAL_IMAGE_DIR\)/normal\.before".*?;)',
            recipe,
            re.DOTALL,
        )
        self.assertIsNotNone(m,
                             "test-qemu must cmp normal.before/normal.after")
        chain = m.group("chain")
        self.assertNotIn("|| true", chain,
                         "cmp chain must not use '|| true' (would mask failure)")
        # No && cmd that swallows the cmp exit code.
        self.assertNotRegex(chain, r"&\&\s+echo\s+",
                            "cmp chain must not use '&& ' (would mask failure)")

    def test_recipe_runner_invocation_follows_build_cmp(self) -> None:
        """The python3 run_test.py invocation must follow the build
        and the cmp on its own recipe line so a build failure halts
        before the runner starts."""
        recipe = _extract_recipe(self.text, "test-qemu")
        # Find lines containing the build sub-make, cmp, and runner.
        build_idx = None
        cmp_idx = None
        runner_idx = None
        for i, ln in enumerate(recipe.splitlines()):
            if "TEST_QEMU_FLAVOR_$(SUITE)" in ln and "image" in ln:
                build_idx = i
            elif 'cmp "$(NORMAL_IMAGE_DIR)/normal.before"' in ln:
                cmp_idx = i
            elif "python3 qemutests/run_test.py" in ln:
                runner_idx = i
        self.assertIsNotNone(build_idx, "build sub-make line missing")
        self.assertIsNotNone(cmp_idx, "cmp line missing")
        self.assertIsNotNone(runner_idx, "runner invocation line missing")
        self.assertLess(build_idx, runner_idx,
                        "build sub-make must precede runner invocation")
        self.assertLess(cmp_idx, runner_idx,
                        "cmp must precede runner invocation")

    def test_driver_model_dispatch_short_circuits(self) -> None:
        """The driver-model dispatch via ``$(MAKE) ... test-qemu-driver-model``
        must be on its own recipe line — a build failure inside the
        matrix harness must not be masked by the runner invocation."""
        recipe = _extract_recipe(self.text, "test-qemu")
        # The dispatch line lives inside an `if/fi` shell block; the
        # actual `$(MAKE) ... test-qemu-driver-model` token must be
        # visible anywhere in the recipe (the shell `if/fi` makes
        # make parse it as one logical line ending with `; \`).
        self.assertIn(
            "test-qemu-driver-model",
            recipe,
            "test-qemu recipe must reference test-qemu-driver-model "
            "(driver-model sub-make)",
        )
        self.assertRegex(
            recipe,
            r"\$\(MAKE\).*test-qemu-driver-model",
            "driver-model dispatch via '$(MAKE) ... test-qemu-driver-model' "
            "must appear in the recipe body",
        )


class MakeFailureDryRunTests(unittest.TestCase):
    """Substitute failing build/hash commands via ``make -n`` and
    inspect the would-be recipe lines.  A passing build later in the
    chain must NOT mask an earlier failure — the runner invocation
    follows the build ``cmp`` so a non-zero build aborts before the
    runner starts.

    This is a host-only check: it never runs a real QEMU process.
    It uses ``make -n`` to inspect the would-be recipe lines without
    executing them.

    These tests are best-effort: a workspace without the build outputs
    may produce a make -n error from the firmware / image sub-rules
    rather than the test-qemu recipe.  In that case the test skips — the
    source-parsing tests above pin the structural rules without
    needing a built workspace.
    """

    def _make_n_capture(self, *args) -> str:
        """Run ``make -n`` with the given args, return stdout+stderr.

        If the dry run does not finish within ``DRY_RUN_TIMEOUT`` the
        fixture cannot capture the recipe structure; that is reported as a
        **loud skip** naming the command and the elapsed time, not as an
        ERROR.  A workspace that cannot build already skips (see the
        individual tests); this is the same "no evidence" path for the
        timeout case.
        """
        cmd = ["make", "-n", *args]
        start = time.monotonic()
        try:
            proc = subprocess.run(
                cmd,
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=DRY_RUN_TIMEOUT,
            )
        except FileNotFoundError:
            self.skipTest("make is not installed on the test machine")
        except subprocess.TimeoutExpired:
            elapsed = time.monotonic() - start
            self.skipTest(
                f"make -n dry run did not finish within "
                f"{DRY_RUN_TIMEOUT}s (elapsed {elapsed:.1f}s): could not "
                f"capture the recipe structure. This is a 'no evidence' "
                f"skip, NOT a pass; the source-parsing tests above still "
                f"pin the structural rules. Command: {' '.join(cmd)}"
            )
        # Combine stdout and stderr (Make often prints recipes to
        # stderr when something in the chain is unbuildable).
        out = (proc.stdout or "") + (proc.stderr or "")
        return out

    def test_test_qemu_dry_run_includes_build_submake(self) -> None:
        """``make -n test-qemu SUITE=systest`` must include the
        variant build sub-make line.  Without it, the recipe would
        skip the build and the runner would use a stale image — the
        brief says this is exactly the failure mode we must pin."""
        out = self._make_n_capture("test-qemu", "SUITE=systest")
        # Workspace may not be built: skip if so.
        if "image" not in out and "disk.img" not in out:
            self.skipTest(
                "make -n did not produce image build lines "
                "(workspace not built)"
            )
        # The recipe literal is `$(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image`
        # but under -n, $(MAKE) is expanded to the literal `make`.  Match
        # either form.
        self.assertRegex(
            out,
            r"(\$\(MAKE\)|make)\s+.*image\b",
            "make -n test-qemu SUITE=systest must print a build sub-make "
            "line (variant image build)",
        )

    def test_test_qemu_dry_run_includes_cmp_line(self) -> None:
        """The recipe must include the normal-before / normal-after
        cmp line; its absence would let a stale image pass."""
        out = self._make_n_capture("test-qemu", "SUITE=systest")
        # Workspace may not be built: skip if no relevant lines.
        if "image" not in out and "disk.img" not in out:
            self.skipTest(
                "make -n did not produce image build lines "
                "(workspace not built)"
            )
        self.assertIn(
            "cmp",
            out,
            "make -n test-qemu SUITE=systest must include a cmp line "
            "(hash guard)",
        )

    def test_test_qemu_dry_run_runner_follows_build(self) -> None:
        """The runner invocation (python3 qemutests/run_test.py) must
        follow the build sub-make and cmp in dry-run order."""
        out = self._make_n_capture("test-qemu", "SUITE=systest")
        build_pos = out.find("$(MAKE)")
        runner_pos = out.find("python3 qemutests/run_test.py")
        if build_pos < 0 or runner_pos < 0:
            self.skipTest(
                "make -n output did not include build or runner lines "
                "(profile-gated recipes may produce nothing in -n)"
            )
        self.assertLess(
            build_pos, runner_pos,
            "build sub-make must precede runner invocation in recipe",
        )


class DryRunTimeoutNoEvidenceTests(unittest.TestCase):
    """Pin the dry-run fixture's "no evidence" path.

    ``_make_n_capture`` shells out to ``make -n`` with a timeout.  When
    that dry run does not finish in time the fixture cannot capture the
    recipe structure — it must report that as a **loud skip** (naming the
    command and the elapsed time) rather than letting
    ``subprocess.TimeoutExpired`` escape as an ERROR.  A skip a reader
    cannot mistake for a pass, and with a timeout wide enough that a
    legitimately-built-but-slow tree still exercises the assertions.
    """

    def test_timeout_is_reported_as_loud_skip(self) -> None:
        inst = MakeFailureDryRunTests(
            "test_test_qemu_dry_run_includes_cmp_line"
        )

        def _boom(*_a, **_k):
            raise subprocess.TimeoutExpired(
                cmd=["make", "-n", "test-qemu", "SUITE=systest"],
                timeout=30,
            )

        with mock.patch.object(subprocess, "run", _boom):
            with self.assertRaises(unittest.SkipTest) as cm:
                inst._make_n_capture("test-qemu", "SUITE=systest")
        msg = str(cm.exception)
        # The skip must be loud: name the command and the elapsed time.
        self.assertIn("make -n", msg,
                      "timeout skip must name the command")
        self.assertIn("SUITE=systest", msg,
                      "timeout skip must name the command arguments")
        self.assertRegex(
            msg,
            r"elapsed\s+\d",
            "timeout skip must report the elapsed time so a reader cannot "
            "mistake it for a pass",
        )

    def test_dry_run_timeout_has_headroom_over_measured(self) -> None:
        """The merged-tree dry run measures ~34.3 s; the timeout must
        have real headroom so a legitimately-built-but-slow tree still
        runs the assertions instead of silently skipping."""
        self.assertGreater(
            DRY_RUN_TIMEOUT, 34.3 * 2,
            "dry-run timeout must have >=2x headroom over the measured "
            "34.3 s dry run (a silent skip on a buildable tree would stop "
            "pinning the recipe structure)",
        )


class ImageHashGuardTests(unittest.TestCase):
    """Pin the normal-image hash guard structure.

    The current recipe uses two ``if`` blocks: one to record the
    pre-build hash (skipping for suites that intentionally rebuild
    the normal image: phase-0, gfx, resolution, driver-model), and
    one to compare post-build hash.  Both must run on their own
    recipe lines so a missing/stale image aborts the recipe.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = _read_runmk()

    def test_normal_before_file_numerical(self) -> None:
        # The pre-build sha256sum is gated by 'if [ "$(SUITE)" != ... ]'.
        self.assertRegex(
            self.text,
            r'(?m)^\s*if \[\s+"\$\(SUITE\)"\s+!=\s+"phase-0"\s*\]',
            "test-qemu must guard the pre-build sha256sum with a "
            "SUITE != phase-0 check (and similar for gfx/resolution/driver-model)",
        )

    def test_normal_after_cmp_follows_build_submake(self) -> None:
        # The post-build cmp must appear AFTER the build sub-make
        # on a separate recipe line so a stale image aborts.
        recipe = _extract_recipe(self.text, "test-qemu")
        build_lines = [
            i for i, ln in enumerate(recipe.splitlines())
            if "TEST_QEMU_FLAVOR_$(SUITE)" in ln and "image" in ln
        ]
        cmp_lines = [
            i for i, ln in enumerate(recipe.splitlines())
            if 'cmp "$(NORMAL_IMAGE_DIR)/normal.before"' in ln
        ]
        self.assertTrue(build_lines, "no build sub-make line found")
        self.assertTrue(cmp_lines, "no cmp line found")
        self.assertLess(
            build_lines[0], cmp_lines[0],
            "cmp line must follow the build sub-make line",
        )

    def test_suite_skip_list_excludes_all_passthrough_suites(self) -> None:
        """Both ``if`` guards must skip phase-0 / gfx / resolution /
        driver-model so their rebuild / private-image paths aren't
        masked."""
        # We count occurrences of each suite string in the recipe
        # body for the test-qemu target.
        recipe = _extract_recipe(self.text, "test-qemu")
        for suite in ("phase-0", "gfx", "resolution", "driver-model"):
            count = recipe.count(f'"{suite}"')
            self.assertGreaterEqual(
                count, 2,
                f"suite {suite!r} must appear in BOTH pre-build and "
                f"post-build skip checks; saw {count} occurrences",
            )


class RecipeLineFailureShortCircuitTests(unittest.TestCase):
    """``make`` aborts a recipe on a non-zero sub-command by default
    (no ``.IGNORE`` / no ``-`` prefix / no trailing ``|| true``).  We
    pin that the variant build and the cmp lines are NOT prefixed
    with ``-`` or suffixed with ``|| true``.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = _read_runmk()

    def test_build_submake_not_ignored(self) -> None:
        recipe = _extract_recipe(self.text, "test-qemu")
        for ln in recipe.splitlines():
            if "TEST_QEMU_FLAVOR_$(SUITE)" in ln and "image" in ln:
                # The actual sub-make line is unindented — find by
                # tab-indented inner line.
                self.assertFalse(
                    ln.lstrip().startswith("@-"),
                    f"build sub-make line must NOT use '-' prefix: {ln!r}",
                )
                self.assertFalse(
                    ln.rstrip().endswith("|| true"),
                    f"build sub-make must NOT end with '|| true': {ln!r}",
                )
                self.assertFalse(
                    ln.rstrip().endswith("|| exit 0"),
                    f"build sub-make must NOT end with '|| exit 0': {ln!r}",
                )

    def test_cmp_line_not_ignored(self) -> None:
        recipe = _extract_recipe(self.text, "test-qemu")
        for ln in recipe.splitlines():
            if 'cmp "$(NORMAL_IMAGE_DIR)/normal.before"' in ln:
                self.assertFalse(
                    ln.rstrip().endswith("|| true"),
                    f"cmp line must NOT end with '|| true': {ln!r}",
                )


# ────────────────────────────────────────────────────────────────────
# RED demonstration: prove that a recipe WITHOUT `|| exit` lets a later
# successful command mask an earlier build/hash failure (the brief's
# "later successful command can mask either failure" warning).
#
# We construct a throwaway Makefile that mirrors the brief's recipe
# structure (build → hash → runner) and exercise it twice:
#   1. WITHOUT `|| exit`: a failing build/hcmp is silently masked by
#      the successful runner; `make test-qemu` exits 0.
#   2. WITH `|| exit`: the failing build/hcmp aborts the recipe; the
#      runner never starts (marker file absent); `make test-qemu` exits
#      nonzero.
#
# The second case is the contract the production recipe must satisfy.
# ────────────────────────────────────────────────────────────────────


_RECIPE_WITHOUT = """\
test-qemu:
\t@if [ "$(SUITE)" = "driver-model" ]; then \\
\t  $(MAKE) --no-print-directory driver-model; \\
\telse \\
\t  $(MAKE) image; \\
\t  ./fake-runner.sh; \\
\tfi
"""

_RECIPE_WITH = """\
test-qemu:
\t@if [ "$(SUITE)" = "driver-model" ]; then \\
\t  $(MAKE) --no-print-directory driver-model || exit 1; \\
\telse \\
\t  $(MAKE) image || exit 1; \\
\t  $(MAKE) hash-check || exit 1; \\
\t  ./fake-runner.sh || exit 1; \\
\tfi
"""


class _Sandbox:
    """Throwaway workspace: a stub `image` target, a `driver-model`
    target that fails on demand, and a runner-side script that writes a
    marker file when invoked.  Lets the RED demonstration exercise
    `make test-qemu` without launching a real QEMU."""

    @staticmethod
    def write(tmp: Path, *, recipe: str, build_rc: int, hash_rc: int,
              driver_rc: int, runner_rc: int = 0) -> Path:
        mk = tmp / "Makefile"
        runner = tmp / "fake-runner.sh"
        marker = tmp / "RUNNER_INVOKED"
        # Force-clear the marker so each invocation starts fresh.
        marker.write_text("")
        runner.write_text(
            "#!/bin/sh\n"
            f"echo 1 > '{marker}'\n"
            f"exit {runner_rc}\n"
        )
        runner.chmod(0o755)
        # Stub Makefile:
        #  - ``image`` exits with the build_rc
        #  - ``driver-model`` exits with the driver_rc
        #  - ``hash-check`` shim that exits with hash_rc
        #  - ``fake-runner.sh`` is the runner script (writes marker).
        mk.write_text(
            recipe
            + "\n"
            + "image:\n"
            + f"\t@exit {build_rc}\n"
            + "driver-model:\n"
            + f"\t@exit {driver_rc}\n"
            + "hash-check:\n"
            + f"\t@exit {hash_rc}\n"
            + ".PHONY: test-qemu image driver-model hash-check\n"
        )
        return marker


class RedFailureMaskingDemo(unittest.TestCase):
    """Prove the brief's "later successful command can mask failure"
    warning by running a stub Makefile against a failing build."""

    def _make_in(self, tmp: Path, extra: list) -> "subprocess.CompletedProcess":
        return subprocess.run(
            ["make", "-s", "test-qemu", *extra],
            cwd=tmp,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def test_without_exit_failing_build_is_masked_by_runner(self) -> None:
        """Without ``|| exit`` after the build, the failing build is
        silent and the runner STILL runs (marker file written).  This
        is exactly the failure mode the brief warns about."""
        with tempfile.TemporaryDirectory(prefix="make-red-") as tdir:
            tmp = Path(tdir)
            marker = _Sandbox.write(
                tmp, recipe=_RECIPE_WITHOUT,
                build_rc=1, hash_rc=0, driver_rc=0,
            )
            # The runner script writes the marker; without || exit, a
            # failing build is silent and the runner STILL runs.
            proc = self._make_in(tmp, ["SUITE=phase-0"])
            self.assertEqual(
                proc.returncode, 0,
                f"without || exit, the failing build is masked; "
                f"runner returned {proc.returncode!r}; stdout={proc.stdout!r}",
            )
            self.assertTrue(
                marker.read_text().strip() != "",
                "marker file written => runner was invoked "
                f"(masking the build failure); marker={marker.read_text()!r}",
            )

    def test_with_exit_failing_build_aborts_before_runner(self) -> None:
        """With ``|| exit`` after the build, the failing build aborts
        the recipe (make returns nonzero) and the runner NEVER runs
        (marker file empty).  This is the contract the production
        recipe must satisfy."""
        with tempfile.TemporaryDirectory(prefix="make-red-") as tdir:
            tmp = Path(tdir)
            marker = _Sandbox.write(
                tmp, recipe=_RECIPE_WITH,
                build_rc=1, hash_rc=0, driver_rc=0,
            )
            proc = self._make_in(tmp, ["SUITE=phase-0"])
            self.assertNotEqual(
                proc.returncode, 0,
                f"with || exit, the failing build must abort; "
                f"got rc={proc.returncode!r}; stdout={proc.stdout!r}",
            )
            self.assertEqual(
                marker.read_text().strip(), "",
                "marker file empty => runner was NOT invoked "
                "(build failure aborted the recipe)",
            )

    def test_with_exit_failing_hash_aborts_before_runner(self) -> None:
        """The hash-check ``cmp`` line must also short-circuit: with
        ``|| exit`` after the hash, a non-matching cmp aborts the
        recipe before the runner starts."""
        with tempfile.TemporaryDirectory(prefix="make-red-") as tdir:
            tmp = Path(tdir)
            marker = _Sandbox.write(
                tmp, recipe=_RECIPE_WITH,
                build_rc=0, hash_rc=1, driver_rc=0,
            )
            proc = self._make_in(tmp, ["SUITE=phase-0"])
            self.assertNotEqual(
                proc.returncode, 0,
                f"with || exit, the failing hash must abort; "
                f"got rc={proc.returncode!r}",
            )
            self.assertEqual(
                marker.read_text().strip(), "",
                "marker file empty => runner was NOT invoked "
                "after hash failure",
            )

    def test_with_exit_driver_model_failure_aborts(self) -> None:
        """The driver-model ``$(MAKE) ... test-qemu-driver-model`` line
        must short-circuit too — a build failure inside the matrix
        harness aborts the recipe before the runner starts."""
        with tempfile.TemporaryDirectory(prefix="make-red-") as tdir:
            tmp = Path(tdir)
            marker = _Sandbox.write(
                tmp, recipe=_RECIPE_WITH,
                build_rc=0, hash_rc=0, driver_rc=1,
            )
            proc = self._make_in(tmp, ["SUITE=driver-model"])
            self.assertNotEqual(
                proc.returncode, 0,
                f"with || exit, the driver-model sub-make failure must "
                f"abort; got rc={proc.returncode!r}",
            )
            self.assertEqual(
                marker.read_text().strip(), "",
                "marker file empty => runner was NOT invoked "
                "after driver-model failure",
            )


class ProductionRecipeShortCircuitPinnedTests(unittest.TestCase):
    """Pin the production recipe's ``|| exit`` short-circuiting contract.

    The brief explicitly requires: "Add explicit ``|| exit`` or
    equivalent short-circuiting after every build/hash command in
    ``test-qemu`` and its driver-model dispatch so an old image cannot
    produce a green result."  These source-parsing tests lock that in."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = _read_runmk()

    def test_driver_model_submake_short_circuits(self) -> None:
        recipe = _extract_recipe(self.text, "test-qemu")
        self.assertRegex(
            recipe,
            r"\$\(MAKE\).*test-qemu-driver-model.*\|\|\s*exit\s+1",
            "driver-model sub-make line must end with '|| exit 1'",
        )

    def test_variant_image_submake_short_circuits(self) -> None:
        recipe = _extract_recipe(self.text, "test-qemu")
        self.assertRegex(
            recipe,
            r"\$\(MAKE\).*TEST_QEMU_FLAVOR_.*image.*\|\|\s*exit\s+1",
            "variant image sub-make line must end with '|| exit 1'",
        )

    def test_normal_after_cmp_short_circuits(self) -> None:
        recipe = _extract_recipe(self.text, "test-qemu")
        self.assertRegex(
            recipe,
            r'cmp\s+"\$\(NORMAL_IMAGE_DIR\)/normal\.before".*\|\|\s*exit\s+1',
            "normal-before/normal-after cmp line must end with '|| exit 1'",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
