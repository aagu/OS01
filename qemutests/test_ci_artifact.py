#!/usr/bin/env python3
"""Contract tests for CI artifact manifest creation and verification (Task C4)."""

from __future__ import annotations

import json
import os
import stat
import tempfile
import unittest
from pathlib import Path

from qemutests import ci_artifact


class CIArtifactContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="os01-ci-artifact-")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.image = self.dir / "disk.img"
        self.image.write_bytes(b"TEST_IMAGE_DATA_1234567890")
        self.manifest = self.dir / "disk.img.manifest.json"

    def test_create_and_verify_success(self) -> None:
        manifest_data = ci_artifact.create_manifest(
            image_path=self.image,
            profile="x86_64-clang",
            variant="normal",
            commit="abcdef1234567890",
            output_path=self.manifest,
        )
        self.assertTrue(self.manifest.is_file())
        self.assertEqual(manifest_data["schema_version"], 1)
        self.assertEqual(manifest_data["commit"], "abcdef1234567890")
        self.assertEqual(manifest_data["profile"], "x86_64-clang")
        self.assertEqual(manifest_data["variant"], "normal")
        self.assertEqual(manifest_data["size_bytes"], len(b"TEST_IMAGE_DATA_1234567890"))

        # Verification must pass
        ci_artifact.verify_artifact(
            manifest_path=self.manifest,
            image_path=self.image,
            expected_commit="abcdef1234567890",
            expected_profile="x86_64-clang",
            expected_variant="normal",
        )

    def test_verify_sha256_mismatch(self) -> None:
        ci_artifact.create_manifest(
            image_path=self.image,
            profile="x86_64-clang",
            variant="normal",
            commit="abcdef1234567890",
            output_path=self.manifest,
        )
        # Corrupt image bytes while keeping size identical
        self.image.write_bytes(b"TEST_IMAGE_DATA_123456789X")
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=self.manifest,
                image_path=self.image,
                expected_commit="abcdef1234567890",
                expected_profile="x86_64-clang",
                expected_variant="normal",
            )
        self.assertIn("checksum mismatch", str(ctx.exception))

    def test_verify_size_mismatch(self) -> None:
        ci_artifact.create_manifest(
            image_path=self.image,
            profile="x86_64-clang",
            variant="normal",
            commit="abcdef1234567890",
            output_path=self.manifest,
        )
        self.image.write_bytes(b"SHORT_DATA")
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=self.manifest,
                image_path=self.image,
                expected_commit="abcdef1234567890",
                expected_profile="x86_64-clang",
                expected_variant="normal",
            )
        self.assertIn("size mismatch", str(ctx.exception))

    def test_verify_commit_mismatch(self) -> None:
        ci_artifact.create_manifest(
            image_path=self.image,
            profile="x86_64-clang",
            variant="normal",
            commit="commit_AAA",
            output_path=self.manifest,
        )
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=self.manifest,
                image_path=self.image,
                expected_commit="commit_BBB",
                expected_profile="x86_64-clang",
                expected_variant="normal",
            )
        self.assertIn("commit mismatch", str(ctx.exception))

    def test_verify_profile_mismatch(self) -> None:
        ci_artifact.create_manifest(
            image_path=self.image,
            profile="x86_64-clang",
            variant="normal",
            commit="abcdef1234567890",
            output_path=self.manifest,
        )
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=self.manifest,
                image_path=self.image,
                expected_commit="abcdef1234567890",
                expected_profile="aarch64-clang",
                expected_variant="normal",
            )
        self.assertIn("profile mismatch", str(ctx.exception))

    def test_verify_variant_mismatch(self) -> None:
        ci_artifact.create_manifest(
            image_path=self.image,
            profile="x86_64-clang",
            variant="normal",
            commit="abcdef1234567890",
            output_path=self.manifest,
        )
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=self.manifest,
                image_path=self.image,
                expected_commit="abcdef1234567890",
                expected_profile="x86_64-clang",
                expected_variant="systest",
            )
        self.assertIn("variant mismatch", str(ctx.exception))

    def test_verify_missing_manifest(self) -> None:
        missing_manifest = self.dir / "nonexistent.json"
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=missing_manifest,
                image_path=self.image,
                expected_commit="abcdef",
                expected_profile="x86_64-clang",
                expected_variant="normal",
            )
        self.assertIn("manifest not found", str(ctx.exception))

    def test_verify_missing_image(self) -> None:
        ci_artifact.create_manifest(
            image_path=self.image,
            profile="x86_64-clang",
            variant="normal",
            commit="abcdef1234567890",
            output_path=self.manifest,
        )
        self.image.unlink()
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=self.manifest,
                image_path=self.image,
                expected_commit="abcdef1234567890",
                expected_profile="x86_64-clang",
                expected_variant="normal",
            )
        self.assertIn("image file not found", str(ctx.exception))

    def test_verify_corrupted_json(self) -> None:
        self.manifest.write_text("not json content", encoding="utf-8")
        with self.assertRaises(ci_artifact.ArtifactVerificationError) as ctx:
            ci_artifact.verify_artifact(
                manifest_path=self.manifest,
                image_path=self.image,
                expected_commit="abcdef1234567890",
                expected_profile="x86_64-clang",
                expected_variant="normal",
            )
        self.assertIn("corrupted JSON", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
