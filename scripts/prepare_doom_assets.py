#!/usr/bin/env python3
"""scripts/prepare_doom_assets.py — validate and package local Doom IWAD assets.

Usage:
    Explicit CLI mode:
        python3 prepare_doom_assets.py --iwad <path> --sha256 <hex> \
            --license <path> --source <text> --output-dir <path>

    Environment mode (mutually exclusive with the 4 asset arguments):
        python3 prepare_doom_assets.py --from-env --output-dir <path>
        Reads:
            OS01_DOOM_ASSET_IWAD
            OS01_DOOM_ASSET_SHA256
            OS01_DOOM_ASSET_LICENSE
            OS01_DOOM_ASSET_SOURCE
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import sys
import tempfile


def validate_path_string(name: str, path_str: str) -> None:
    if not path_str:
        sys.stderr.write(f"ERROR: {name} path cannot be empty\n")
        sys.exit(1)
    for bad in (" ", "\t", "\r", "\n", ":"):
        if bad in path_str:
            sys.stderr.write(
                f"ERROR: {name} path '{path_str}' contains forbidden character {bad!r}; "
                "rootfs manifest encoding does not support whitespace or colons in paths.\n"
            )
            sys.exit(1)
    if "\0" in path_str:
        sys.stderr.write(f"ERROR: {name} path contains null byte\n")
        sys.exit(1)


def sha256_of_file(file_path: Path) -> str:
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate and package local Doom IWAD assets for OS01 rootfs.",
        add_help=True,
    )
    parser.add_argument("--iwad", help="Path to local IWAD file")
    parser.add_argument("--sha256", help="Expected 64-char hex SHA256 of the IWAD file")
    parser.add_argument("--license", help="Path to IWAD license text file")
    parser.add_argument("--source", help="Provenance description or URL (single line)")
    parser.add_argument("--from-env", action="store_true", help="Read asset configuration from environment")
    parser.add_argument("--output-dir", required=True, help="Destination directory for packaged assets")

    args = parser.parse_args()

    # Mutual exclusivity check
    has_cli_asset_args = any(x is not None for x in (args.iwad, args.sha256, args.license, args.source))
    if args.from_env:
        if has_cli_asset_args:
            sys.stderr.write("ERROR: --from-env cannot be combined with explicit --iwad/--sha256/--license/--source arguments\n")
            return 1
        iwad = os.environ.get("OS01_DOOM_ASSET_IWAD")
        sha256_expected = os.environ.get("OS01_DOOM_ASSET_SHA256")
        license_path = os.environ.get("OS01_DOOM_ASSET_LICENSE")
        source_text = os.environ.get("OS01_DOOM_ASSET_SOURCE")

        missing = []
        if not iwad:
            missing.append("OS01_DOOM_ASSET_IWAD")
        if not sha256_expected:
            missing.append("OS01_DOOM_ASSET_SHA256")
        if not license_path:
            missing.append("OS01_DOOM_ASSET_LICENSE")
        if not source_text:
            missing.append("OS01_DOOM_ASSET_SOURCE")
        if missing:
            sys.stderr.write(f"ERROR: missing required environment variables: {', '.join(missing)}\n")
            return 1
    else:
        missing = []
        if not args.iwad:
            missing.append("--iwad")
        if not args.sha256:
            missing.append("--sha256")
        if not args.license:
            missing.append("--license")
        if not args.source:
            missing.append("--source")
        if missing:
            sys.stderr.write(f"ERROR: missing required arguments: {', '.join(missing)}\n")
            return 1
        iwad = args.iwad
        sha256_expected = args.sha256
        license_path = args.license
        source_text = args.source

    # Validate output-dir
    output_dir_str = args.output_dir
    validate_path_string("output-dir", output_dir_str)

    # Validate paths
    validate_path_string("iwad", iwad)
    validate_path_string("license", license_path)

    # Validate files exist and are regular files
    iwad_p = Path(iwad)
    if not iwad_p.is_file():
        sys.stderr.write(f"ERROR: IWAD path '{iwad}' is not a regular file or does not exist\n")
        return 1

    license_p = Path(license_path)
    if not license_p.is_file():
        sys.stderr.write(f"ERROR: License path '{license_path}' is not a regular file or does not exist\n")
        return 1

    # Validate source text (single line, no newlines/nulls)
    if "\n" in source_text or "\r" in source_text or "\0" in source_text:
        sys.stderr.write("ERROR: Source description must be a single line without embedded newlines or null bytes\n")
        return 1

    # Validate SHA256 format
    clean_sha = sha256_expected.strip().lower()
    if len(clean_sha) != 64 or not all(c in "0123456789abcdef" for c in clean_sha):
        sys.stderr.write(f"ERROR: Invalid SHA256 string '{sha256_expected}', expected 64 hex characters\n")
        return 1

    # Verify SHA256 matches file
    actual_sha = sha256_of_file(iwad_p)
    if actual_sha != clean_sha:
        sys.stderr.write(
            f"ERROR: SHA256 checksum mismatch for IWAD '{iwad}':\n"
            f"  Expected: {clean_sha}\n"
            f"  Actual:   {actual_sha}\n"
        )
        return 1

    # Atomic publication into output_dir
    out_dir = Path(output_dir_str).resolve()
    out_dir.parent.mkdir(parents=True, exist_ok=True)

    tmp_dir = Path(tempfile.mkdtemp(prefix=".doom-assets-tmp-", dir=str(out_dir.parent)))
    try:
        dest_iwad = tmp_dir / "iwad.wad"
        dest_license = tmp_dir / "IWAD-LICENSE.txt"
        dest_source = tmp_dir / "IWAD-SOURCE.txt"
        dest_stamp = tmp_dir / ".doom_assets.stamp"

        shutil.copy2(iwad_p, dest_iwad)
        shutil.copy2(license_p, dest_license)
        dest_source.write_text(source_text + "\n", encoding="utf-8")

        license_sha = sha256_of_file(dest_license)
        stamp_content = (
            f"iwad_sha256={actual_sha}\n"
            f"license_sha256={license_sha}\n"
            f"source={source_text}\n"
        )
        dest_stamp.write_text(stamp_content, encoding="utf-8")

        # Move to output_dir
        out_dir.mkdir(parents=True, exist_ok=True)
        for fname in ("iwad.wad", "IWAD-LICENSE.txt", "IWAD-SOURCE.txt", ".doom_assets.stamp"):
            (tmp_dir / fname).replace(out_dir / fname)
    finally:
        if tmp_dir.exists():
            shutil.rmtree(tmp_dir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
