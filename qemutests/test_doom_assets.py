#!/usr/bin/env python3
"""D05 — Doom IWAD asset validation and provenance packaging tests.

Covers:
  - T1: CLI and env-mode validation (partial inputs, hash mismatch, directories,
        unsupported path characters, multiline source text, mixed input modes,
        source literal preservation without execution, atomic publishing).
  - T2: (extended in T2) rootfs integration, stamp stability, and provenance tracking.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "prepare_doom_assets.py"


def sha256_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class DoomAssetsValidationTest(unittest.TestCase):
    """T1: Validation and asset packaging unit tests."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="doom-assets-test-")
        self.tmp_dir = Path(self._tmp.name)
        self.fake_iwad = self.tmp_dir / "doom1.wad"
        self.fake_iwad_data = b"IWAD\x0c\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" + b"\x42" * 1024
        self.fake_iwad.write_bytes(self.fake_iwad_data)
        self.fake_sha256 = sha256_of(self.fake_iwad_data)

        self.fake_license = self.tmp_dir / "LICENSE.txt"
        self.fake_license_data = b"Doom Shareware License text\nAll rights reserved.\n"
        self.fake_license.write_bytes(self.fake_license_data)

        self.fake_source = "id Software / shareware release v1.9"
        self.output_dir = self.tmp_dir / "out"

    def tearDown(self):
        self._tmp.cleanup()

    def run_script(self, args: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        cmd = ["python3", str(SCRIPT)] + args
        merged_env = os.environ.copy()
        if env is not None:
            # Clear any OS01_DOOM_ASSET_* variables in current environment first
            for k in list(merged_env.keys()):
                if k.startswith("OS01_DOOM_ASSET_"):
                    del merged_env[k]
            merged_env.update(env)
        return subprocess.run(cmd, capture_output=True, text=True, env=merged_env)

    def test_partial_inputs_fail(self):
        """Missing any required input fails with a non-zero exit code."""
        valid_args = [
            "--iwad", str(self.fake_iwad),
            "--sha256", self.fake_sha256,
            "--license", str(self.fake_license),
            "--source", self.fake_source,
            "--output-dir", str(self.output_dir),
        ]

        # Missing one argument at a time
        for opt in ("--iwad", "--sha256", "--license", "--source", "--output-dir"):
            idx = valid_args.index(opt)
            corrupt_args = valid_args[:idx] + valid_args[idx + 2:]
            r = self.run_script(corrupt_args)
            self.assertNotEqual(r.returncode, 0, f"missing {opt} must fail")

        # In env mode: missing one env var at a time
        base_env = {
            "OS01_DOOM_ASSET_IWAD": str(self.fake_iwad),
            "OS01_DOOM_ASSET_SHA256": self.fake_sha256,
            "OS01_DOOM_ASSET_LICENSE": str(self.fake_license),
            "OS01_DOOM_ASSET_SOURCE": self.fake_source,
        }
        for k in base_env:
            corrupt_env = base_env.copy()
            del corrupt_env[k]
            r = self.run_script(["--from-env", "--output-dir", str(self.output_dir)], env=corrupt_env)
            self.assertNotEqual(r.returncode, 0, f"missing env var {k} must fail")

        # Empty string env var must fail
        for k in base_env:
            corrupt_env = base_env.copy()
            corrupt_env[k] = ""
            r = self.run_script(["--from-env", "--output-dir", str(self.output_dir)], env=corrupt_env)
            self.assertNotEqual(r.returncode, 0, f"empty env var {k} must fail")

    def test_hash_mismatch(self):
        """A SHA256 mismatch is rejected with a non-zero exit code."""
        wrong_sha256 = "0" * 64
        r = self.run_script([
            "--iwad", str(self.fake_iwad),
            "--sha256", wrong_sha256,
            "--license", str(self.fake_license),
            "--source", self.fake_source,
            "--output-dir", str(self.output_dir),
        ])
        self.assertNotEqual(r.returncode, 0, "hash mismatch must fail")
        self.assertFalse(self.output_dir.exists() and any(self.output_dir.iterdir()),
                         "no partial output directory on hash mismatch")

    def test_directory_rejected(self):
        """Directories given as IWAD or license must be rejected."""
        some_dir = self.tmp_dir / "a_dir"
        some_dir.mkdir()

        # IWAD is directory
        r = self.run_script([
            "--iwad", str(some_dir),
            "--sha256", self.fake_sha256,
            "--license", str(self.fake_license),
            "--source", self.fake_source,
            "--output-dir", str(self.output_dir),
        ])
        self.assertNotEqual(r.returncode, 0, "directory as IWAD must fail")

        # License is directory
        r = self.run_script([
            "--iwad", str(self.fake_iwad),
            "--sha256", self.fake_sha256,
            "--license", str(some_dir),
            "--source", self.fake_source,
            "--output-dir", str(self.output_dir),
        ])
        self.assertNotEqual(r.returncode, 0, "directory as license must fail")

    def test_unsupported_path_rejected(self):
        """Paths with whitespace, colon, newline, or NUL are rejected."""
        bad_chars = ["path with space", "path:with:colon", "path\nnewline"]
        for bad in bad_chars:
            bad_path = self.tmp_dir / bad
            r = self.run_script([
                "--iwad", str(bad_path),
                "--sha256", self.fake_sha256,
                "--license", str(self.fake_license),
                "--source", self.fake_source,
                "--output-dir", str(self.output_dir),
            ])
            self.assertNotEqual(r.returncode, 0, f"path with '{bad}' must fail")

    def test_source_literal_no_execution(self):
        """Source strings containing quotes, dollar signs, backticks and semicolons are preserved verbatim."""
        sentinel = self.tmp_dir / "sentinel.txt"
        dangerous_source = f'source with "double", \'single\', $UNDEFINED_VAR, `touch {sentinel}`, ; exit 1;'

        r = self.run_script([
            "--iwad", str(self.fake_iwad),
            "--sha256", self.fake_sha256,
            "--license", str(self.fake_license),
            "--source", dangerous_source,
            "--output-dir", str(self.output_dir),
        ])
        self.assertEqual(r.returncode, 0, f"script failed: {r.stderr}")
        self.assertFalse(sentinel.exists(), "sentinel file must not have been created by shell execution")

        source_file = self.output_dir / "IWAD-SOURCE.txt"
        self.assertTrue(source_file.is_file())
        self.assertEqual(source_file.read_text().rstrip("\r\n"), dangerous_source)

    def test_source_multiline_rejected(self):
        """Source text with embedded newlines is rejected."""
        multiline_source = "First line\nSecond line"
        r = self.run_script([
            "--iwad", str(self.fake_iwad),
            "--sha256", self.fake_sha256,
            "--license", str(self.fake_license),
            "--source", multiline_source,
            "--output-dir", str(self.output_dir),
        ])
        self.assertNotEqual(r.returncode, 0, "multiline source must fail")

    def test_mixed_input_modes_fail(self):
        """Mixing --from-env with explicit asset arguments fails."""
        r = self.run_script([
            "--from-env",
            "--iwad", str(self.fake_iwad),
            "--output-dir", str(self.output_dir),
        ])
        self.assertNotEqual(r.returncode, 0, "mixing --from-env and --iwad must fail")

    def test_env_mode_matches_cli(self):
        """--from-env mode produces identical files to CLI arguments."""
        out_cli = self.tmp_dir / "out_cli"
        out_env = self.tmp_dir / "out_env"

        r1 = self.run_script([
            "--iwad", str(self.fake_iwad),
            "--sha256", self.fake_sha256,
            "--license", str(self.fake_license),
            "--source", self.fake_source,
            "--output-dir", str(out_cli),
        ])
        self.assertEqual(r1.returncode, 0, r1.stderr)

        env = {
            "OS01_DOOM_ASSET_IWAD": str(self.fake_iwad),
            "OS01_DOOM_ASSET_SHA256": self.fake_sha256,
            "OS01_DOOM_ASSET_LICENSE": str(self.fake_license),
            "OS01_DOOM_ASSET_SOURCE": self.fake_source,
        }
        r2 = self.run_script(["--from-env", "--output-dir", str(out_env)], env=env)
        self.assertEqual(r2.returncode, 0, r2.stderr)

        for fname in ("iwad.wad", "IWAD-LICENSE.txt", "IWAD-SOURCE.txt", ".doom_assets.stamp"):
            f_cli = out_cli / fname
            f_env = out_env / fname
            self.assertTrue(f_cli.is_file(), f"{fname} missing in CLI output")
            self.assertTrue(f_env.is_file(), f"{fname} missing in env output")
            self.assertEqual(f_cli.read_bytes(), f_env.read_bytes(), f"{fname} content differs between modes")

    def test_success_output_byte_match(self):
        """Successful run produces exact byte-for-byte outputs and a stamp."""
        r = self.run_script([
            "--iwad", str(self.fake_iwad),
            "--sha256", self.fake_sha256,
            "--license", str(self.fake_license),
            "--source", self.fake_source,
            "--output-dir", str(self.output_dir),
        ])
        self.assertEqual(r.returncode, 0, r.stderr)

        out_iwad = self.output_dir / "iwad.wad"
        out_license = self.output_dir / "IWAD-LICENSE.txt"
        out_source = self.output_dir / "IWAD-SOURCE.txt"
        out_stamp = self.output_dir / ".doom_assets.stamp"

        self.assertTrue(out_iwad.is_file())
        self.assertEqual(out_iwad.read_bytes(), self.fake_iwad_data)

        self.assertTrue(out_license.is_file())
        self.assertEqual(out_license.read_bytes(), self.fake_license_data)

        self.assertTrue(out_source.is_file())
        self.assertEqual(out_source.read_text(), self.fake_source + "\n")

        self.assertTrue(out_stamp.is_file())
        self.assertGreater(len(out_stamp.read_text().strip()), 0)


if __name__ == "__main__":
    unittest.main()
