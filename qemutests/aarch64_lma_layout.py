#!/usr/bin/env python3
# @os01-test
# id: aarch64-lma-layout
# layer: qemu_e2e
# subsystem: boot
# timing: strict
# tier: nightly
# scenarios: boot.lma.uefi
"""Paired UEFI boot layout acceptance (LMA Task 5).

Manages a PRIVATE build root (AARCH64_BUILD_ROOT) — independent from
the default build tree — with fixed ``KERNEL_SELFTEST=1`` and runs the
sequence default → relocated (0x42000000) → default through the
controlled make boundary:

* Every round consumes the round's published
  ``image/selftest/aarch64-uefi.img`` plus ``artifacts/kernel.elf`` and
  ``artifacts/uefi/BOOTAA64.EFI``; the FAT contents are extracted with
  ``mcopy -i`` and compared byte-for-byte with the published products.
* Each round's ELF real entry is asserted (0x40080000 / 0x42080000 /
  0x40080000) and the relocated EFI must differ from the default one.
* The relocated and the restored-default FATs are each booted ONCE
  through the direct runner (``aarch64_uefi_smp.py --expect-lma-base
  <base>``), proving the actual pairing by the run result. The FIRST
  default build is not booted again, and the two default FATs / EFI
  rebuild hashes are never compared.
* The restored default FAT copy gets the relocated kernel.elf swapped
  in and must fail with the dedicated mismatch diagnostic and the
  generic loader failure, with NO kernel takeover evidence inside the
  short observation window (``--expect-lma-mismatch`` keeps the
  direct runner's process recycling and timeout classification).
* The default build tree (``build/aarch64-clang``) must be
  byte-identical before and after the whole run; paths that did not
  exist before must not be created.

QEMU virt, 2 CPUs, 512 MiB. Build failures and QEMU timeouts are never
counted as passes.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASE = 0x40000000
ALT_BASE = 0x42000000
BLOCK_SIZE = 0x200000
HANDOFF_OFFSET = 0x1e0000
ENTRY_OFFSET = 0x80000
DEFAULT_TREE = REPO_ROOT / "build/aarch64-clang"


def log_call(title: str, cmd: list[str], log_path: Path, env=None) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"  [lma-layout] {title}")
    with open(log_path, "wb") as log:
        proc = subprocess.run(cmd, cwd=REPO_ROOT, stdout=log,
                              stderr=subprocess.STDOUT, env=env)
    if proc.returncode != 0:
        tail = log_path.read_text(errors="replace").splitlines()[-30:]
        raise RuntimeError(
            f"{title} failed (exit {proc.returncode}); log: {log_path}\n"
            + "\n".join(tail))


def elf_entry(path: Path) -> int:
    data = path.read_bytes()
    assert data[:6] == b"\x7fELF\x02\x01", f"{path} is not ELF64 LE"
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    assert header[2] == 183, f"{path} is not AArch64"
    return header[4]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def snapshot_default_tree() -> dict[str, str] | None:
    if not DEFAULT_TREE.exists():
        return None
    snapshot = {}
    for path in sorted(DEFAULT_TREE.rglob("*")):
        if path.is_file():
            snapshot[str(path.relative_to(DEFAULT_TREE))] = sha256_file(path)
    return snapshot


class Round:
    def __init__(self, base: int, image: Path, kernel: Path, efi: Path,
                 fat_kernel: Path, fat_efi: Path, log_dir: Path):
        self.base = base
        self.image = image
        self.kernel = kernel
        self.efi = efi
        self.fat_kernel = fat_kernel
        self.fat_efi = fat_efi
        self.log_dir = log_dir


def build_round(root: Path, base: int, firmware_source: str,
                work_dir: Path, tag: str) -> Round:
    """Build one layout round in the private root and audit the pairing."""
    cmd = ["make", "PROFILE=aarch64-clang", "KERNEL_SELFTEST=1",
           f"AARCH64_BUILD_ROOT={root}", "aarch64-uefi"]
    if base != DEFAULT_BASE:
        cmd.append(f"AARCH64_BOOT_BLOCK_BASE={hex(base)}")
    env = dict(os.environ)
    env["AARCH64_UEFI_FIRMWARE_SOURCE"] = firmware_source
    log_call(f"build {tag} (base {hex(base)})", cmd,
             work_dir / f"build-{tag}.log", env=env)

    image = root / "image/selftest/aarch64-uefi.img"
    kernel = root / "artifacts/kernel.elf"
    efi = root / "artifacts/uefi/BOOTAA64.EFI"
    for path in (image, kernel, efi):
        if not path.is_file():
            raise RuntimeError(f"round {tag}: published product missing: {path}")

    # Extract the FAT contents and compare byte-for-byte with the same
    # round's published products.
    fat_dir = work_dir / f"fat-{tag}"
    fat_dir.mkdir(parents=True, exist_ok=True)
    fat_kernel = fat_dir / "kernel.elf"
    fat_efi = fat_dir / "BOOTAA64.EFI"
    log_call(f"extract FAT {tag}",
             ["mcopy", "-i", str(image), "::/kernel.elf", str(fat_kernel)],
             work_dir / f"mcopy-{tag}.log")
    log_call(f"extract EFI {tag}",
             ["mcopy", "-i", str(image), "::/EFI/BOOT/BOOTAA64.EFI",
              str(fat_efi)],
             work_dir / f"mcopy-{tag}.log")
    if sha256_file(fat_kernel) != sha256_file(kernel):
        raise RuntimeError(f"round {tag}: FAT kernel.elf differs from the "
                           f"published artifacts/kernel.elf")
    if sha256_file(fat_efi) != sha256_file(efi):
        raise RuntimeError(f"round {tag}: FAT BOOTAA64.EFI differs from the "
                           f"published artifacts/uefi/BOOTAA64.EFI")

    entry = elf_entry(kernel)
    expected_entry = base + ENTRY_OFFSET
    if entry != expected_entry:
        raise RuntimeError(f"round {tag}: ELF entry {hex(entry)} != "
                           f"configured {hex(expected_entry)}")
    return Round(base, image, kernel, efi, fat_kernel, fat_efi,
                 fat_dir.parent)


def boot_round(root: Path, image: Path, firmware: Path, qemu: str,
               timeout: int, tag: str, base: int | None) -> None:
    """Boot once through the direct runner; a non-zero exit fails the run."""
    cmd = [sys.executable, "qemutests/aarch64_uefi_smp.py",
           "--cpus", "2", "--repeat", "1", "--timeout", str(timeout),
           "--diagnostic-dtb", "auto",
           "--firmware", str(firmware), "--image", str(image),
           "--qemu", qemu,
           "--build-dir", str(root), "--profile", "aarch64-clang",
           "--log-dir", str(root / "lma-logs" / tag)]
    if base is None:
        cmd.append("--expect-lma-mismatch")
    else:
        cmd += ["--expect-lma-base", hex(base)]
    log_call(f"boot {tag}", cmd, root / "lma-logs" / f"boot-{tag}.log")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--timeout", type=int, default=120,
                        help="direct-runner per-boot timeout in seconds "
                             "(default: 120)")
    parser.add_argument("--alt-base", default=hex(ALT_BASE),
                        help="relocated boot block base (default: 0x42000000)")
    args = parser.parse_args()
    alt_base = int(str(args.alt_base), 0)

    work_dir = Path(tempfile.mkdtemp(prefix="aarch64-lma-"))
    root = work_dir / "build"
    root.mkdir(parents=True)
    print(f"  [lma-layout] private build root: {root}")

    before = snapshot_default_tree()

    # Reuse the default tree's firmware when present; otherwise the
    # profile's URL source downloads it into the private root.
    default_firmware = DEFAULT_TREE / "firmware/QEMU_EFI.fd"
    firmware_source = (str(default_firmware)
                       if default_firmware.is_file()
                       else "https://retrage.github.io/edk2-nightly/bin/"
                            "RELEASEAARCH64_QEMU_EFI.fd")

    # Round 1: default layout. Published products only — never booted,
    # never compared against the restored default's rebuild.
    build_round(root, DEFAULT_BASE, firmware_source, work_dir, "default")
    firmware = root / "firmware/QEMU_EFI.fd"
    if not firmware.is_file():
        raise RuntimeError("private-root firmware missing after build")

    # Round 2: relocated layout, booted once (positive pairing).
    relocated = build_round(root, alt_base, firmware_source, work_dir,
                            "relocated")
    relocated_default_efi_digest = sha256_file(relocated.efi)
    boot_round(root, relocated.image, firmware, args.qemu, args.timeout,
               "relocated", alt_base)
    relocated_fat_copy = work_dir / "relocated-fat.img"
    shutil.copyfile(relocated.image, relocated_fat_copy)

    # Round 3: default layout again, restored FAT booted once (the
    # restored-default pairing is proven by this boot).
    restored = build_round(root, DEFAULT_BASE, firmware_source, work_dir,
                           "restored")
    if relocated_default_efi_digest == sha256_file(restored.efi):
        raise RuntimeError("relocated EFI is byte-identical to the default "
                           "EFI; the layout did not reach the loader")
    restored_fat_copy = work_dir / "restored-fat.img"
    shutil.copyfile(restored.image, restored_fat_copy)
    boot_round(root, restored_fat_copy, firmware, args.qemu, args.timeout,
               "restored-default", DEFAULT_BASE)

    # Negative: the restored default FAT with the RELOCATED kernel.elf
    # swapped in. Default loader + relocated ELF must produce the
    # dedicated mismatch diagnostic, no kernel takeover.
    mismatch_img = work_dir / "mismatch.img"
    shutil.copyfile(restored_fat_copy, mismatch_img)
    log_call("swap relocated kernel into default FAT",
             ["mcopy", "-i", str(mismatch_img), "-o",
              str(relocated.fat_kernel), "::/kernel.elf"],
             work_dir / "mcopy-mismatch.log")
    boot_round(root, mismatch_img, firmware, args.qemu, 60,
               "mismatch", None)

    after = snapshot_default_tree()
    if before != after:
        created = sorted(set(after or {}) - set(before or {}))
        raise RuntimeError("default build tree changed during the run; "
                           f"new/changed paths: {created[:10]}")

    print("aarch64_lma_layout: paired default/relocated boots and the "
          "mismatch rejection all passed")

    # Preserve the run evidence outside every build tree: the per-boot
    # RunArchive result.json files plus the runner-level build/boot logs
    # and the extracted FAT pairings. The default build tree stays
    # byte-identical; these copies are the acceptance record.
    stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime()) + f"-{os.getpid()}"
    archive_dest = REPO_ROOT / "test-results/aarch64-lma-layout" / stamp
    archive_dest.mkdir(parents=True, exist_ok=True)
    if (root / "logs").is_dir():
        shutil.copytree(root / "logs", archive_dest / "boots")
    for item in work_dir.iterdir():
        if item.name == "build":
            continue
        if item.is_dir():
            shutil.copytree(item, archive_dest / item.name)
        else:
            shutil.copyfile(item, archive_dest / item.name)
    print(f"  [lma-layout] evidence archived: {archive_dest}")

    if os.environ.get("LMA_KEEP_ROOT"):
        print(f"  [lma-layout] kept private root: {root}")
    else:
        shutil.rmtree(work_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(f"lma-layout FAILED: {error}", file=sys.stderr)
        sys.exit(1)
