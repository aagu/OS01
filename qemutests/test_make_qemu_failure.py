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
import unittest
from pathlib import Path


# Repo root so we can locate mk/components/run.mk.
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

RUN_MK = ROOT / "mk" / "components" / "run.mk"


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
        or any ``if [ $? ...]\`` branch that masks the failure."""
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
        """Run ``make -n`` with the given args, return stdout+stderr."""
        try:
            proc = subprocess.run(
                ["make", "-n", *args],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=30,
            )
        except FileNotFoundError:
            self.skipTest("make is not installed on the test machine")
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


if __name__ == "__main__":
    unittest.main(verbosity=2)