#!/usr/bin/env python3
"""CI artifact manifest creation and verification (Task C4).

Generates and validates cryptographic checksums and provenance metadata
(commit SHA, profile, variant) for disk and firmware images reused across
CI jobs, preventing unverified or mismatched binaries from being published.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


class ArtifactVerificationError(Exception):
    """Raised when an artifact manifest is missing, corrupt, or fails verification."""
    pass


def compute_sha256(path: Path) -> str:
    """Compute hex-encoded SHA-256 of file at path."""
    hasher = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


def get_current_commit() -> str:
    """Resolve current git HEAD commit SHA."""
    try:
        proc = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
        )
        return proc.stdout.strip()
    except Exception as e:
        raise ArtifactVerificationError(f"failed to resolve git commit: {e}") from e


def create_manifest(
    image_path: Path,
    profile: str,
    variant: str,
    output_path: Path,
    commit: str | None = None,
) -> dict[str, Any]:
    """Create and write an artifact manifest for image_path."""
    if not image_path.is_file():
        raise ArtifactVerificationError(f"image file not found: {image_path}")

    if commit is None:
        commit = get_current_commit()

    sha256_hex = compute_sha256(image_path)
    size_bytes = image_path.stat().st_size
    created_at = datetime.datetime.now(datetime.timezone.utc).isoformat()

    manifest_data: dict[str, Any] = {
        "schema_version": 1,
        "commit": commit,
        "profile": profile,
        "variant": variant,
        "image_path": str(image_path),
        "sha256": sha256_hex,
        "size_bytes": size_bytes,
        "created_at": created_at,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temp_output = output_path.with_suffix(output_path.suffix + ".tmp")
    temp_output.write_text(json.dumps(manifest_data, indent=2) + "\n", encoding="utf-8")
    temp_output.replace(output_path)
    return manifest_data


def verify_artifact(
    manifest_path: Path,
    image_path: Path,
    expected_commit: str,
    expected_profile: str,
    expected_variant: str,
) -> dict[str, Any]:
    """Verify image_path against manifest_path and expected provenance."""
    if not manifest_path.is_file():
        raise ArtifactVerificationError(f"manifest not found: {manifest_path}")

    try:
        manifest_text = manifest_path.read_text(encoding="utf-8")
        data = json.loads(manifest_text)
    except Exception as e:
        raise ArtifactVerificationError(f"corrupted JSON in manifest: {e}") from e

    schema_version = data.get("schema_version")
    if schema_version != 1:
        raise ArtifactVerificationError(f"unsupported schema_version: {schema_version}")

    actual_commit = data.get("commit")
    if actual_commit != expected_commit:
        raise ArtifactVerificationError(
            f"commit mismatch: manifest has '{actual_commit}', expected '{expected_commit}'"
        )

    actual_profile = data.get("profile")
    if actual_profile != expected_profile:
        raise ArtifactVerificationError(
            f"profile mismatch: manifest has '{actual_profile}', expected '{expected_profile}'"
        )

    actual_variant = data.get("variant")
    if actual_variant != expected_variant:
        raise ArtifactVerificationError(
            f"variant mismatch: manifest has '{actual_variant}', expected '{expected_variant}'"
        )

    if not image_path.is_file():
        raise ArtifactVerificationError(f"image file not found: {image_path}")

    actual_size = image_path.stat().st_size
    expected_size = data.get("size_bytes")
    if actual_size != expected_size:
        raise ArtifactVerificationError(
            f"size mismatch: image has {actual_size} bytes, manifest expects {expected_size} bytes"
        )

    actual_sha = compute_sha256(image_path)
    expected_sha = data.get("sha256")
    if actual_sha != expected_sha:
        raise ArtifactVerificationError(
            f"checksum mismatch: image has sha256 '{actual_sha}', manifest expects '{expected_sha}'"
        )

    return data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Manage CI artifact manifests")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # create
    create_p = subparsers.add_parser("create", help="Create an artifact manifest")
    create_p.add_argument("--image", required=True, type=Path, help="Path to image file")
    create_p.add_argument("--profile", required=True, help="Build profile")
    create_p.add_argument("--variant", required=True, help="Image variant")
    create_p.add_argument("--output", required=True, type=Path, help="Output manifest path")
    create_p.add_argument("--commit", default=None, help="Commit SHA (defaults to git HEAD)")

    # verify
    verify_p = subparsers.add_parser("verify", help="Verify an image against its manifest")
    verify_p.add_argument("--manifest", required=True, type=Path, help="Path to manifest JSON")
    verify_p.add_argument("--image", required=True, type=Path, help="Path to image file")
    verify_p.add_argument("--expected-commit", required=True, help="Expected git commit SHA")
    verify_p.add_argument("--expected-profile", required=True, help="Expected build profile")
    verify_p.add_argument("--expected-variant", required=True, help="Expected image variant")

    args = parser.parse_args(argv)

    if args.command == "create":
        try:
            data = create_manifest(
                image_path=args.image,
                profile=args.profile,
                variant=args.variant,
                output_path=args.output,
                commit=args.commit,
            )
            print(f"[ci_artifact] Created manifest for {args.image} (sha256: {data['sha256'][:16]}...)")
            return 0
        except Exception as e:
            print(f"[ci_artifact] ERROR creating manifest: {e}", file=sys.stderr)
            return 1

    elif args.command == "verify":
        try:
            data = verify_artifact(
                manifest_path=args.manifest,
                image_path=args.image,
                expected_commit=args.expected_commit,
                expected_profile=args.expected_profile,
                expected_variant=args.expected_variant,
            )
            print(f"[ci_artifact] PASS: Verified {args.image} (commit: {data['commit'][:8]}, sha256: {data['sha256'][:16]}...)")
            return 0
        except ArtifactVerificationError as e:
            print(f"[ci_artifact] FAILED verification: {e}", file=sys.stderr)
            return 1
        except Exception as e:
            print(f"[ci_artifact] ERROR during verification: {e}", file=sys.stderr)
            return 2

    return 2


if __name__ == "__main__":
    sys.exit(main())
