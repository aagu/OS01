#!/usr/bin/env python3
"""Unit tests for qemutests/harness/qemu_cmd.py."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "test_harness_qemu_cmd",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from qemutests.harness.qemu_cmd import build_qemu_aarch64, build_qemu_x86  # noqa: E402


class BuildQemuX86Tests(unittest.TestCase):
    def test_x86_typical_snapshot(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/disk.img",
            smp=2,
            serial="stdio",
            snapshot=True,
        )
        self.assertEqual(
            argv,
            [
                "qemu-system-x86_64",
                "-M", "q35",
                "-drive", "if=pflash,format=raw,readonly=on,file=/fw/OVMF.fd",
                "-drive", "file=/img/disk.img,format=raw,if=none,id=disk",
                "-device", "ahci,id=ahci",
                "-device", "ide-hd,drive=disk,bus=ahci.0",
                "-object", "rng-random,filename=/dev/urandom,id=rng0",
                "-device", "virtio-rng-pci,rng=rng0",
                "-m", "512",
                "-smp", "2",
                "-serial", "stdio",
                "-display", "none",
                "-no-reboot",
                "-no-shutdown",
                "-snapshot",
            ],
        )

    def test_x86_serial_stdio_has_no_file_serial(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/disk.img",
            smp=1,
            serial="stdio",
        )
        self.assertIn("stdio", argv)
        self.assertFalse(
            any(a.startswith("file:") for a in argv),
            f"unexpected file: serial arg in {argv!r}",
        )
        self.assertNotIn("-snapshot", argv)

    def test_x86_serial_file(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/disk.img",
            smp=2,
            serial="file",
            serial_path="/tmp/serial.log",
        )
        idx = argv.index("-serial")
        self.assertEqual(argv[idx + 1], "file:/tmp/serial.log")

    def test_x86_machine_pc_uses_virtio_blk_and_keeps_rng_pair(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/disk.img",
            smp=2,
            serial="stdio",
            machine="pc",
            extra=["-nic", "none"],
        )
        self.assertIn("pc", argv)
        self.assertIn("virtio-blk-pci,drive=disk", argv)
        self.assertNotIn("ahci,id=ahci", argv)
        self._assert_rng_pair(argv)
        self.assertEqual(argv[-2:], ["-nic", "none"])

    def test_x86_rng_always_paired(self) -> None:
        for machine in ("q35", "pc"):
            argv = build_qemu_x86(
                firmware="/fw/OVMF.fd",
                image="/img/disk.img",
                smp=1,
                serial="stdio",
                machine=machine,
            )
            self._assert_rng_pair(argv)

    def test_x86_usb_only_topology(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/disk.img",
            smp=1,
            serial="stdio",
            storage="usb",
        )
        self.assertIn("qemu-xhci,id=xhci,p2=4,p3=0", argv)
        self.assertIn(
            "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0", argv
        )
        self.assertIn(
            "file=/img/disk.img,format=raw,if=none,id=usbdisk,readonly=on",
            argv,
        )
        self.assertNotIn("ahci,id=ahci", argv)
        self.assertFalse(any("virtio-blk" in a or "ide-hd" in a for a in argv))
        self.assertFalse(any(a.startswith("-boot") for a in argv))
        drives = [
            a for a in argv
            if a.startswith("file=") or a.startswith("if=pflash")
        ]
        self.assertEqual(len(drives), 2)
        self.assertEqual(
            sum(1 for a in drives if not a.startswith("if=pflash")), 1
        )
        self._assert_rng_pair(argv)

    def test_x86_dual_topology_usb_bootindex_0_ahci_bootindex_1(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/usb.img",
            ahci_image="/img/ahci.img",
            smp=2,
            serial="stdio",
            storage="dual",
        )
        self.assertIn("if=pflash,format=raw,readonly=on,file=/fw/OVMF.fd", argv)
        self.assertIn(
            "file=/img/usb.img,format=raw,if=none,id=usbdisk,readonly=on", argv
        )
        self.assertIn(
            "file=/img/ahci.img,format=raw,if=none,id=ahcidisk", argv
        )
        self.assertIn("qemu-xhci,id=xhci,p2=4,p3=0", argv)
        self.assertIn(
            "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0", argv
        )
        self.assertIn("ahci,id=ahci", argv)
        self.assertIn("ide-hd,drive=ahcidisk,bus=ahci.0,bootindex=1", argv)
        self.assertFalse(any(a.startswith("-boot") for a in argv))
        self._assert_rng_pair(argv)

    def test_x86_dual_requires_ahci_image(self) -> None:
        with self.assertRaises(ValueError):
            build_qemu_x86(
                firmware="/fw/OVMF.fd",
                image="/img/usb.img",
                smp=2,
                serial="stdio",
                storage="dual",
            )

    def test_x86_rejects_unknown_storage(self) -> None:
        with self.assertRaises(ValueError):
            build_qemu_x86(
                firmware="/fw/OVMF.fd",
                image="/img/disk.img",
                smp=1,
                serial="stdio",
                storage="scsi",
            )

    def test_x86_usb_superspeed(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/usb.img",
            smp=2,
            serial="stdio",
            storage="usb",
            usb_speed="3.0",
        )
        self.assertIn("qemu-xhci,id=xhci,p2=0,p3=4", argv)
        self.assertNotIn("qemu-xhci,id=xhci,p2=4,p3=0", argv)
        self.assertIn(
            "file=/img/usb.img,format=raw,if=none,id=usbdisk,readonly=on", argv
        )
        self.assertIn(
            "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0", argv
        )

    def test_x86_usb_writable(self) -> None:
        argv = build_qemu_x86(
            firmware="/fw/OVMF.fd",
            image="/img/usb.img",
            smp=2,
            serial="stdio",
            storage="usb",
            usb_readonly=False,
        )
        self.assertIn("file=/img/usb.img,format=raw,if=none,id=usbdisk", argv)
        self.assertNotIn(
            "file=/img/usb.img,format=raw,if=none,id=usbdisk,readonly=on", argv
        )

    def test_x86_rejects_unknown_usb_speed(self) -> None:
        with self.assertRaises(ValueError):
            build_qemu_x86(
                firmware="/fw/OVMF.fd",
                image="/img/usb.img",
                smp=1,
                serial="stdio",
                storage="usb",
                usb_speed="1.1",
            )

    def _assert_rng_pair(self, argv: list[str]) -> None:
        self.assertIn("rng-random,filename=/dev/urandom,id=rng0", argv)
        self.assertIn("virtio-rng-pci,rng=rng0", argv)


class BuildQemuAarch64Tests(unittest.TestCase):
    def test_aarch64_typical_snapshot(self) -> None:
        argv = build_qemu_aarch64(
            firmware="/fw/AAVMF.fd",
            image="/img/disk.img",
            cpus=4,
            dtb="/tmp/virt.dtb",
            extra=["-snapshot"],
        )
        self.assertEqual(
            argv,
            [
                "qemu-system-aarch64",
                "-M", "virt,gic-version=2,acpi=off",
                "-cpu", "cortex-a53",
                "-smp", "4",
                "-m", "512",
                "-drive", "if=pflash,format=raw,file=/fw/AAVMF.fd",
                "-drive", "if=none,file=/img/disk.img,format=raw,readonly=on,id=disk",
                "-device", "virtio-blk-device,drive=disk",
                "-object", "rng-random,filename=/dev/urandom,id=rng0",
                "-device", "virtio-rng-pci,rng=rng0",
                "-serial", "stdio",
                "-display", "none",
                "-no-reboot",
                "-no-shutdown",
                "-dtb", "/tmp/virt.dtb",
                "-snapshot",
            ],
        )

    def test_aarch64_without_dtb_and_rng_always_paired(self) -> None:
        argv = build_qemu_aarch64(
            firmware="/fw/AAVMF.fd",
            image="/img/disk.img",
            cpus=2,
            dtb=None,
        )
        self.assertIn("virt,gic-version=2", argv)
        self.assertNotIn("-dtb", argv)
        self.assertIn("rng-random,filename=/dev/urandom,id=rng0", argv)
        self.assertIn("virtio-rng-pci,rng=rng0", argv)


if __name__ == "__main__":
    unittest.main()
