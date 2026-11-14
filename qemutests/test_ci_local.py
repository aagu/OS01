#!/usr/bin/env python3
"""Contract tests for tools/tests/ci_local.sh."""

from __future__ import annotations

import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "tests" / "ci_local.sh"

TEST_MANIFEST = {
    "id": "test_ci_local",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


def _write_fake_runtime(bindir: Path, name: str, inspect_rc: int, log_file: Path) -> None:
    script = bindir / name
    script.write_text(
        f"""#!/bin/sh
if [ "$1" = "image" ] && [ "$2" = "inspect" ]; then
    exit {inspect_rc}
fi
printf '%s\\n' "$@" > {str(log_file)!r}
exit 0
""",
        encoding="utf-8",
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class CiLocalTests(unittest.TestCase):
    def test_no_container_runtime_exits_two_with_install_hint(self):
        with tempfile.TemporaryDirectory() as empty_bin:
            env = {"PATH": empty_bin, "HOME": os.environ.get("HOME", "/tmp")}
            proc = subprocess.run(
                ["/bin/bash", str(SCRIPT), "x86_64-clang", "test-qemu", "SUITE=phase-0"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(proc.returncode, 2)
        combined = (proc.stderr + proc.stdout).lower()
        self.assertIn("docker", combined)
        self.assertIn("podman", combined)

    def test_missing_image_exits_two_with_pull_guidance(self):
        with tempfile.TemporaryDirectory() as tmp:
            bindir = Path(tmp)
            log_file = bindir / "argv.log"
            _write_fake_runtime(bindir, "docker", inspect_rc=1, log_file=log_file)
            env = {"PATH": f"{bindir}:/bin:/usr/bin", "HOME": os.environ.get("HOME", "/tmp")}
            proc = subprocess.run(
                ["/bin/bash", str(SCRIPT), "x86_64-clang", "test-qemu", "SUITE=phase-0"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(proc.returncode, 2)
        self.assertIn("docker pull ghcr.io/aagu/os01-ci:latest", proc.stderr)
        self.assertFalse(log_file.exists())

    def test_fake_docker_passes_mount_workdir_image_and_make_args(self):
        with tempfile.TemporaryDirectory() as tmp:
            bindir = Path(tmp)
            log_file = bindir / "argv.log"
            _write_fake_runtime(bindir, "docker", inspect_rc=0, log_file=log_file)
            env = {"PATH": f"{bindir}:/bin:/usr/bin", "HOME": os.environ.get("HOME", "/tmp")}
            proc = subprocess.run(
                ["/bin/bash", str(SCRIPT), "x86_64-clang", "test-qemu", "SUITE=phase-0"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stderr)
            argv = log_file.read_text(encoding="utf-8").splitlines()
        repo = str(ROOT.resolve())
        self.assertEqual(argv[0], "run")
        self.assertIn(f"{repo}:{repo}", argv)
        self.assertIn("-v", argv)
        self.assertIn("-w", argv)
        self.assertIn(repo, argv)
        self.assertIn("ghcr.io/aagu/os01-ci:latest", argv)
        idx = argv.index("make")
        self.assertEqual(
            argv[idx:],
            ["make", "PROFILE=x86_64-clang", "test-qemu", "SUITE=phase-0"],
        )
        self.assertIn("ghcr.io/aagu/os01-ci:latest", proc.stdout)

    def test_podman_fallback_when_docker_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            bindir = Path(tmp)
            log_file = bindir / "argv.log"
            _write_fake_runtime(bindir, "podman", inspect_rc=0, log_file=log_file)
            # Use only bindir so host /usr/bin/docker is not picked up if present
            env = {"PATH": str(bindir), "HOME": os.environ.get("HOME", "/tmp")}
            proc = subprocess.run(
                ["/bin/bash", str(SCRIPT), "aarch64-clang", "test-harness"],
                cwd=str(ROOT),
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stderr)
            argv = log_file.read_text(encoding="utf-8").splitlines()
        idx = argv.index("make")
        self.assertEqual(argv[idx:], ["make", "PROFILE=aarch64-clang", "test-harness"])


if __name__ == "__main__":
    unittest.main()
