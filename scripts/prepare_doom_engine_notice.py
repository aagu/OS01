#!/usr/bin/env python3
"""scripts/prepare_doom_engine_notice.py — generate Doom engine license and provenance notice.

Consumes:
  - thirdpart/doomgeneric/LICENSE
  - thirdpart/doomgeneric.manifest (pinned commit, upstream repo, ordered patch list)
  - thirdpart/patches/doomgeneric/*.patch

Produces in $(BUILD_DIR)/doom-notice:
  - LICENSE
  - SOURCE.txt
  - PATCHES.txt
  - patches/<patch_filename>
  - .doom_notice.stamp
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shutil
import sys
import tempfile

DEFAULT_ROOT = Path(__file__).resolve().parents[1]

PATCH_REASONS = {
    "0001-os01-no-zenity.patch": "eliminate Zenity GUI dialog dependency; errors written directly to stderr",
    "0002-os01-quit.patch": "implement clean engine shutdown via I_Quit and exit(0)",
    "0003-os01-config.patch": "fix sign comparison and configuration loading/saving behavior",
    "0004-os01-input-budget.patch": "non-blocking input event queue draining with leased events",
    "0005-os01-test-observe.patch": "isolated test observation hook for gametic and state logging",
}


def sha256_of_file(file_path: Path) -> str:
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def parse_manifest(manifest_path: Path) -> tuple[str, str, list[str]]:
    source_val = ""
    revision_val = ""
    patches_val: list[str] = []

    content = manifest_path.read_text(encoding="utf-8")
    lines = content.splitlines()
    in_patches = False

    for line in lines:
        line_strip = line.strip()
        if not line_strip or line_strip.startswith("#"):
            continue

        if line_strip.startswith("DOOMGENERIC_SOURCE"):
            m = re.search(r":=\s*(.+)", line_strip)
            if m:
                source_val = m.group(1).strip()
        elif line_strip.startswith("DOOMGENERIC_REVISION"):
            m = re.search(r":=\s*(.+)", line_strip)
            if m:
                revision_val = m.group(1).strip()
        elif line_strip.startswith("DOOMGENERIC_PATCHES"):
            in_patches = True
            m = re.search(r":=\s*(.*)", line_strip)
            rest = m.group(1).rstrip("\\").strip() if m else ""
            if rest:
                patches_val.extend(rest.split())
        elif in_patches:
            ends_with_backslash = line_strip.endswith("\\")
            cleaned = line_strip.rstrip("\\").strip()
            if cleaned:
                patches_val.extend(cleaned.split())
            if not ends_with_backslash:
                in_patches = False

    if not source_val:
        raise ValueError("DOOMGENERIC_SOURCE not found in manifest")
    if not revision_val:
        raise ValueError("DOOMGENERIC_REVISION not found in manifest")
    if not patches_val:
        raise ValueError("DOOMGENERIC_PATCHES is empty or not found in manifest")

    return source_val, revision_val, patches_val


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare Doom engine license notice and provenance files.")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_ROOT / "thirdpart" / "doomgeneric.manifest",
                        help="Path to doomgeneric.manifest")
    parser.add_argument("--license", type=Path, default=DEFAULT_ROOT / "thirdpart" / "doomgeneric" / "LICENSE",
                        help="Path to doomgeneric LICENSE file")
    parser.add_argument("--patch-dir", type=Path, default=DEFAULT_ROOT / "thirdpart" / "patches" / "doomgeneric",
                        help="Directory containing doomgeneric patches")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="Output directory for generated notice artifacts")

    args = parser.parse_args()

    manifest_path: Path = args.manifest
    license_path: Path = args.license
    patch_dir: Path = args.patch_dir
    output_dir: Path = args.output_dir

    if not manifest_path.is_file():
        sys.stderr.write(f"ERROR: manifest file not found: {manifest_path}\n")
        return 1
    if not license_path.is_file():
        sys.stderr.write(f"ERROR: license file not found: {license_path}\n")
        return 1
    if not patch_dir.is_dir():
        sys.stderr.write(f"ERROR: patch directory not found: {patch_dir}\n")
        return 1

    try:
        source_val, revision_val, patches = parse_manifest(manifest_path)
    except Exception as e:
        sys.stderr.write(f"ERROR: failed parsing manifest: {e}\n")
        return 1

    # Verify every patch exists before writing anything
    patch_shas: list[tuple[str, str, Path]] = []
    for patch_name in patches:
        p_path = patch_dir / patch_name
        if not p_path.is_file():
            sys.stderr.write(f"ERROR: patch file {patch_name} not found in {patch_dir}\n")
            return 1
        patch_shas.append((patch_name, sha256_of_file(p_path), p_path))

    # Stage outputs atomically in a temp directory first
    output_dir.mkdir(parents=True, exist_ok=True)
    stamp_file = output_dir / ".doom_notice.stamp"

    with tempfile.TemporaryDirectory(dir=output_dir.parent) as tmp_dir_str:
        tmp_dir = Path(tmp_dir_str)
        tmp_patches_dir = tmp_dir / "patches"
        tmp_patches_dir.mkdir(parents=True, exist_ok=True)

        # 1. LICENSE
        shutil.copy2(license_path, tmp_dir / "LICENSE")

        # 2. SOURCE.txt
        source_text = (
            f"Package: doomgeneric (OS01 port)\n"
            f"Upstream Repository: https://github.com/ozkl/doomgeneric\n"
            f"Pinned Revision: {revision_val}\n"
            f"Submodule Path: {source_val}\n"
            f"Architecture: OS01 freestanding userland with libgfx surface and exclusive keyboard lease\n"
        )
        (tmp_dir / "SOURCE.txt").write_text(source_text, encoding="utf-8")

        # 3. PATCHES.txt and copied patch files
        lines = [
            "OS01 Doomgeneric Applied Patches (in application order):",
            "================================================================================",
            f"{'Order':<5} {'Filename':<35} {'SHA256'}",
            "--------------------------------------------------------------------------------",
        ]
        for idx, (p_name, p_sha, p_src) in enumerate(patch_shas, start=1):
            lines.append(f"{idx:02d}    {p_name:<35} {p_sha}")
            shutil.copy2(p_src, tmp_patches_dir / p_name)

        lines.append("--------------------------------------------------------------------------------")
        lines.append("Patch details and descriptions:")
        for idx, (p_name, p_sha, _) in enumerate(patch_shas, start=1):
            reason = PATCH_REASONS.get(p_name, "OS01 platform adaptation patch")
            lines.append(f"{idx:02d}. {p_name}: {reason}")
        lines.append("")

        (tmp_dir / "PATCHES.txt").write_text("\n".join(lines), encoding="utf-8")

        # Copy all staged files into output_dir
        shutil.copy2(tmp_dir / "LICENSE", output_dir / "LICENSE")
        shutil.copy2(tmp_dir / "SOURCE.txt", output_dir / "SOURCE.txt")
        shutil.copy2(tmp_dir / "PATCHES.txt", output_dir / "PATCHES.txt")
        dest_patches_dir = output_dir / "patches"
        dest_patches_dir.mkdir(parents=True, exist_ok=True)
        for p_name, _, p_src in patch_shas:
            shutil.copy2(tmp_patches_dir / p_name, dest_patches_dir / p_name)

        # Touch stamp
        stamp_file.write_text(f"revision={revision_val}\npatches={len(patches)}\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
