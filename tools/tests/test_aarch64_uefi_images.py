#!/usr/bin/env python3
"""Verify AArch64 dual-platform UEFI images (qemu-virt & rpi5) and build gates."""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ESP_TYPE_GUID = bytes([
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
])


def find_esp_offset(image_path: Path) -> int:
    with image_path.open("rb") as f:
        f.seek(512)
        gpt = f.read(92)
        if len(gpt) < 92 or gpt[:8] != b"EFI PART":
            raise AssertionError(f"missing GPT header in {image_path}")
        part_lba, num_parts, part_sz = struct.unpack_from("<QII", gpt, 72)
        f.seek(part_lba * 512)
        for _ in range(num_parts):
            ent = f.read(part_sz)
            if len(ent) < 56:
                break
            if ent[:16] == ESP_TYPE_GUID:
                first_lba = struct.unpack_from("<Q", ent, 32)[0]
                return first_lba * 512
    raise AssertionError(f"ESP partition not found in {image_path}")


def read_esp_file(image_path: Path, esp_off: int, fat_path: str) -> bytes:
    res = subprocess.run(
        ["mcopy", "-n", "-i", f"{image_path}@@{esp_off}", fat_path, "-"],
        check=True,
        capture_output=True,
    )
    return res.stdout


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_make_n(build_root: Path, extra_args: list[str]) -> subprocess.CompletedProcess:
    cmd = [
        "make", "-n",
        "PROFILE=aarch64-clang",
        f"AARCH64_BUILD_ROOT={build_root}",
        *extra_args,
    ]
    return subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-root", required=True)
    args = parser.parse_args()
    build_root = Path(args.build_root).resolve()

    qemu_img = build_root / "image" / "aarch64-uefi.img"
    rpi5_img = build_root / "image" / "rpi5" / "aarch64-uefi.img"
    assert qemu_img.is_file(), f"missing {qemu_img}"
    assert rpi5_img.is_file(), f"missing {rpi5_img}"

    qemu_off = find_esp_offset(qemu_img)
    rpi5_off = find_esp_offset(rpi5_img)

    qemu_efi = read_esp_file(qemu_img, qemu_off, "::/EFI/BOOT/BOOTAA64.EFI")
    rpi5_efi = read_esp_file(rpi5_img, rpi5_off, "::/EFI/BOOT/BOOTAA64.EFI")
    assert qemu_efi == rpi5_efi, (
        f"BOOTAA64.EFI differs: {sha256(qemu_efi)} vs {sha256(rpi5_efi)}"
    )

    qemu_kern = read_esp_file(qemu_img, qemu_off, "::/kernel.elf")
    rpi5_kern = read_esp_file(rpi5_img, rpi5_off, "::/kernel.elf")
    assert qemu_kern == rpi5_kern, (
        f"kernel.elf differs: {sha256(qemu_kern)} vs {sha256(rpi5_kern)}"
    )

    qemu_cfg = read_esp_file(qemu_img, qemu_off, "::/config.txt").decode("utf-8")
    rpi5_cfg = read_esp_file(rpi5_img, rpi5_off, "::/config.txt").decode("utf-8")
    assert qemu_cfg == "platform qemu-virt\nconsole dtb\n", repr(qemu_cfg)
    assert rpi5_cfg == "platform rpi5\nconsole rp1-header\n", repr(rpi5_cfg)

    # Parse-time validation gates
    for bad in (
        ["AARCH64_PLATFORM=bogus", "aarch64-uefi"],
        ["AARCH64_PLATFORM=", "aarch64-uefi"],
        ["AARCH64_PLATFORM=qemu-virt rpi5", "aarch64-uefi"],
        ["AARCH64_PLATFORM=rpi5", "KERNEL_SELFTEST=1", "aarch64-uefi"],
    ):
        r = run_make_n(build_root, bad)
        assert r.returncode != 0, f"expected failure for {bad}, got 0"

    # QEMU-only targets must fail at parse time before any recipe output
    for target in (
        ["run-aarch64-uefi"],
        ["test-aarch64"],
        ["test-aarch64-boot-logo"],
        ["test-aarch64-pcie"],
        ["test-aarch64", "MODE=boot-contract"],
    ):
        r = run_make_n(build_root, ["AARCH64_PLATFORM=rpi5", *target])
        assert r.returncode != 0, f"expected failure for rpi5 {target}"
        combined = r.stdout + r.stderr
        for forbidden in ("QEMU_EFI.fd", "qemu-system-aarch64", "mkdisk"):
            assert forbidden not in combined, (
                f"rpi5 {target} leaked {forbidden!r} before failing:\n{combined}"
            )

    # RPi5 image dry-run must not depend on QEMU_EFI.fd
    r = run_make_n(
        build_root,
        ["AARCH64_PLATFORM=rpi5", "AARCH64_UEFI_FIRMWARE_SOURCE=/no-such-fw", "aarch64-uefi"],
    )
    assert r.returncode == 0, f"rpi5 dry-run failed:\n{r.stderr}"
    assert "QEMU_EFI.fd" not in r.stdout, f"rpi5 dry-run touched QEMU_EFI.fd:\n{r.stdout}"

    print("PASS: test_aarch64_uefi_images")


if __name__ == "__main__":
    main()
