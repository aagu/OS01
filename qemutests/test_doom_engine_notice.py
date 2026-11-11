#!/usr/bin/env python3
"""D06 — Doom engine notice and provenance packaging tests.

The contract:
- Whenever /bin/doom is published, engine notice files are installed:
    /usr/share/licenses/doom/LICENSE
    /usr/share/doc/doom/SOURCE.txt
    /usr/share/doc/doom/PATCHES.txt
    /usr/share/doc/doom/patches/<filename>
- This does not depend on whether an IWAD is present (engine notice without WAD).
- SOURCE.txt records repository origin and pinned commit.
- PATCHES.txt records ordered patch list, reasons, and SHA256 hashes matching files.
- Modifying a patch in place re-keys and updates notice artifacts.
- Missing license or patch file fails with non-zero exit and rejects publishing.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "prepare_doom_engine_notice.py"
MANIFEST = ROOT / "thirdpart" / "doomgeneric.manifest"
LICENSE_FILE = ROOT / "thirdpart" / "doomgeneric" / "LICENSE"
PATCH_DIR = ROOT / "thirdpart" / "patches" / "doomgeneric"


class DoomEngineNoticeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out_dir = Path(self.tmp.name) / "notice"

    def run_script(self, manifest, license_path, patch_dir, out_dir, check=True):
        cmd = [
            sys.executable,
            str(SCRIPT),
            "--manifest",
            str(manifest),
            "--license",
            str(license_path),
            "--patch-dir",
            str(patch_dir),
            "--output-dir",
            str(out_dir),
        ]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if check and res.returncode != 0:
            raise RuntimeError(f"Script failed (exit {res.returncode}):\n{res.stderr}\n{res.stdout}")
        return res

    def test_notice_records_commit_and_patch_order(self):
        """Verify real repository manifest, license, and patches produce exact notice artifacts."""
        self.assertTrue(SCRIPT.is_file(), f"Missing script {SCRIPT}")
        res = self.run_script(MANIFEST, LICENSE_FILE, PATCH_DIR, self.out_dir)
        self.assertEqual(res.returncode, 0)

        license_out = self.out_dir / "LICENSE"
        source_out = self.out_dir / "SOURCE.txt"
        patches_out = self.out_dir / "PATCHES.txt"
        patches_sub = self.out_dir / "patches"

        self.assertTrue(license_out.is_file())
        self.assertTrue(source_out.is_file())
        self.assertTrue(patches_out.is_file())
        self.assertTrue(patches_sub.is_dir())

        # Check LICENSE verbatim
        self.assertEqual(license_out.read_bytes(), LICENSE_FILE.read_bytes())

        # Check SOURCE.txt records commit
        source_text = source_out.read_text()
        self.assertIn("e257a09527062bfb986b6e5e0780839b6cbd5b71", source_text)
        self.assertIn("doomgeneric", source_text)

        # Check PATCHES.txt records ordered list with sha256
        patches_text = patches_out.read_text()
        expected_patches = [
            "0001-os01-no-zenity.patch",
            "0002-os01-quit.patch",
            "0003-os01-config.patch",
            "0004-os01-input-budget.patch",
            "0005-os01-test-observe.patch",
        ]
        pos = -1
        for patch_name in expected_patches:
            patch_file = PATCH_DIR / patch_name
            self.assertTrue(patch_file.is_file())
            sha256 = hashlib.sha256(patch_file.read_bytes()).hexdigest()

            # Must appear in order
            new_pos = patches_text.find(patch_name)
            self.assertGreater(new_pos, pos, f"{patch_name} not in expected order in PATCHES.txt")
            pos = new_pos

            # Must contain sha256
            self.assertIn(sha256, patches_text)

            # Must have copied patch file
            staged_patch = patches_sub / patch_name
            self.assertTrue(staged_patch.is_file())
            self.assertEqual(staged_patch.read_bytes(), patch_file.read_bytes())

    def test_missing_engine_material_fails(self):
        """Missing LICENSE or patch file fails with non-zero exit without partial stamp."""
        self.assertTrue(SCRIPT.is_file(), f"Missing script {SCRIPT}")
        # Missing license
        bogus_license = Path(self.tmp.name) / "nonexistent-license"
        res = self.run_script(MANIFEST, bogus_license, PATCH_DIR, self.out_dir, check=False)
        self.assertNotEqual(res.returncode, 0)
        self.assertFalse((self.out_dir / ".doom_notice.stamp").exists())

        # Missing patch directory / file
        fake_patch_dir = Path(self.tmp.name) / "empty-patches"
        fake_patch_dir.mkdir()
        res = self.run_script(MANIFEST, LICENSE_FILE, fake_patch_dir, self.out_dir, check=False)
        self.assertNotEqual(res.returncode, 0)
        self.assertFalse((self.out_dir / ".doom_notice.stamp").exists())

    def test_patch_same_path_rekeys_notice(self):
        """In-place modification of a patch updates the SHA256 and copied content."""
        self.assertTrue(SCRIPT.is_file(), f"Missing script {SCRIPT}")
        # Setup fixture with copy of patches and manifest
        fixture_dir = Path(self.tmp.name) / "fixture"
        fixture_patches = fixture_dir / "patches"
        fixture_patches.mkdir(parents=True)
        shutil.copy(MANIFEST, fixture_dir / "manifest")
        shutil.copy(LICENSE_FILE, fixture_dir / "LICENSE")

        expected_patches = [
            "0001-os01-no-zenity.patch",
            "0002-os01-quit.patch",
            "0003-os01-config.patch",
            "0004-os01-input-budget.patch",
            "0005-os01-test-observe.patch",
        ]
        for p in expected_patches:
            shutil.copy(PATCH_DIR / p, fixture_patches / p)

        # Run 1
        res1 = self.run_script(fixture_dir / "manifest", fixture_dir / "LICENSE", fixture_patches, self.out_dir)
        self.assertEqual(res1.returncode, 0)
        orig_patches_text = (self.out_dir / "PATCHES.txt").read_text()

        # Modify 0005 in place
        target_patch = fixture_patches / "0005-os01-test-observe.patch"
        with open(target_patch, "a") as f:
            f.write("\n# modified test comment\n")
        new_sha = hashlib.sha256(target_patch.read_bytes()).hexdigest()

        # Run 2
        res2 = self.run_script(fixture_dir / "manifest", fixture_dir / "LICENSE", fixture_patches, self.out_dir)
        self.assertEqual(res2.returncode, 0)
        new_patches_text = (self.out_dir / "PATCHES.txt").read_text()

        self.assertNotEqual(orig_patches_text, new_patches_text)
        self.assertIn(new_sha, new_patches_text)
        self.assertEqual((self.out_dir / "patches" / "0005-os01-test-observe.patch").read_bytes(), target_patch.read_bytes())

    def test_engine_notice_without_wad(self):
        """Check rootfs integration: when doom is in ROOTFS_USER_PROGRAMS without WAD, license files are present."""
        wrapper = Path(self.tmp.name) / "rootfs_check.mk"
        wrapper.write_text(f"""
OS01_ROOT := {ROOT}
PROFILE := x86_64-clang
USER_PROGRAMS := doom hello
USER_ARTIFACT_DIR := {Path(self.tmp.name)}/artifacts
KERNEL_ARTIFACT := {Path(self.tmp.name)}/kernel.bin
INITTAB_FILE := {ROOT}/config/inittab
BUILD_DIR := {Path(self.tmp.name)}/build
DOOM_ASSET_COUNT := 0
include {ROOT}/config/rootfs.mk
.PHONY: check
check:
\t@echo "FILES=$(ROOTFS_FILES)"
""")
        res = subprocess.run(["make", "-f", str(wrapper), "check"], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(res.returncode, 0, res.stderr)
        stdout = res.stdout

        # Verify notice files are included in rootfs files
        self.assertIn("/usr/share/licenses/doom/LICENSE=", stdout)
        self.assertIn("/usr/share/doc/doom/SOURCE.txt=", stdout)
        self.assertIn("/usr/share/doc/doom/PATCHES.txt=", stdout)
        self.assertIn("/usr/share/doc/doom/patches/0001-os01-no-zenity.patch=", stdout)
        self.assertIn("/usr/share/doc/doom/patches/0005-os01-test-observe.patch=", stdout)

        # Verify IWAD is NOT included when DOOM_ASSET_COUNT is not 4
        self.assertNotIn("/usr/share/games/doom/iwad.wad=", stdout)


if __name__ == "__main__":
    unittest.main()
