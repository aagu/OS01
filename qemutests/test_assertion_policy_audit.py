#!/usr/bin/env python3
"""Tests for assertion_policy_audit.py (assertion layering lint + exemption registry)."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
AUDIT_SCRIPT = ROOT / "qemutests" / "assertion_policy_audit.py"

TEST_MANIFEST = {
    "id": "test_assertion_policy_audit",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}


def _write_fake_repo(
    root: Path,
    py_rel: str,
    py_code: str,
    exemptions: list[dict] | None = None,
) -> None:
    target = root / py_rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(py_code, encoding="utf-8")

    cfg = root / "config"
    cfg.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "exemptions": exemptions if exemptions is not None else [],
    }
    (cfg / "assertion-exemptions.json").write_text(
        json.dumps(payload, indent=2),
        encoding="utf-8",
    )


def _run_audit(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(AUDIT_SCRIPT),
            "--repo-root",
            str(root),
            "--exemptions",
            str(root / "config" / "assertion-exemptions.json"),
        ],
        capture_output=True,
        text=True,
    )


class AssertionPolicyAuditTests(unittest.TestCase):
    def test_detects_unexempted_read_text_and_regex_on_prod_source(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_fake_repo(
                root,
                "qemutests/bad_audit.py",
                (
                    "import re\n"
                    "from pathlib import Path\n"
                    "def check(root: Path) -> None:\n"
                    "    src = (root / 'kernel/mm/pmm.c').read_text()\n"
                    "    assert re.search(r'pmm_alloc', src)\n"
                ),
            )
            proc = _run_audit(root)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            combined = proc.stdout + proc.stderr
            self.assertIn("qemutests/bad_audit.py:4", combined)

    def test_detects_unexempted_open_and_in_on_prod_source(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_fake_repo(
                root,
                "qemutests/bad_open_audit.py",
                (
                    "def check() -> None:\n"
                    "    with open('user/init.c', 'r', encoding='utf-8') as f:\n"
                    "        text = f.read()\n"
                    "    assert 'parse_inittab' in text\n"
                ),
            )
            proc = _run_audit(root)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            combined = proc.stdout + proc.stderr
            self.assertIn("qemutests/bad_open_audit.py:2", combined)

    def test_passes_with_structural_invariant_and_policy_ref(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_fake_repo(
                root,
                "qemutests/struct_audit.py",
                (
                    "import re\n"
                    "from pathlib import Path\n"
                    "def check(root: Path) -> None:\n"
                    "    src = (root / 'kernel/core/main.c').read_text()\n"
                    "    assert re.search(r'kernel_main', src)\n"
                ),
                exemptions=[
                    {
                        "path": "qemutests/struct_audit.py",
                        "reason_class": "structural-invariant",
                        "policy_ref": "AGENTS.md#Directory organization",
                    }
                ],
            )
            proc = _run_audit(root)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_fails_when_structural_invariant_missing_policy_ref(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_fake_repo(
                root,
                "qemutests/struct_audit.py",
                (
                    "import re\n"
                    "from pathlib import Path\n"
                    "def check(root: Path) -> None:\n"
                    "    src = (root / 'kernel/core/main.c').read_text()\n"
                    "    assert re.search(r'kernel_main', src)\n"
                ),
                exemptions=[
                    {
                        "path": "qemutests/struct_audit.py",
                        "reason_class": "structural-invariant",
                    }
                ],
            )
            proc = _run_audit(root)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn("policy_ref", proc.stdout + proc.stderr)

    def test_passes_with_pending_exemption(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_fake_repo(
                root,
                "qemutests/pending_audit.py",
                (
                    "from pathlib import Path\n"
                    "def check(root: Path) -> None:\n"
                    "    src = (root / 'libc/stdio.c').read_text()\n"
                    "    assert 'printf' in src\n"
                ),
                exemptions=[
                    {
                        "path": "qemutests/pending_audit.py",
                        "reason_class": "pending",
                    }
                ],
            )
            proc = _run_audit(root)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_ignores_non_production_file_reads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_fake_repo(
                root,
                "qemutests/good_audit.py",
                (
                    "import re\n"
                    "from pathlib import Path\n"
                    "def check(root: Path) -> None:\n"
                    "    cfg = (root / 'config/test-catalog.json').read_text()\n"
                    "    assert 'schema_version' in cfg\n"
                    "    assert re.search(r'\"tests\"', cfg)\n"
                ),
            )
            proc = _run_audit(root)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
