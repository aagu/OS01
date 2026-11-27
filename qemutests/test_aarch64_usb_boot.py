#!/usr/bin/env python3
"""Contract tests for the aarch64 QEMU USB boot acceptance runner."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

TEST_MANIFEST = {
    "id": "test_aarch64_usb_boot",
    "layer": "harness",
    "subsystem": "drivers",
    "timing": "strict",
    "tier": "pr",
    "scenarios": ["drivers.usb.aarch64.boot"],
}

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import aarch64_usb_boot as aub  # noqa: E402
from qemutests.harness.qemu_cmd import build_qemu_aarch64  # noqa: E402

ESP_GUID = "b81ca0b2-c753-9866-0a06-dba28212d425"
OTHER_GUID = "11111111-2222-3333-4444-555555555555"
ROOT_MARKER = "A64_USB_ROOT_MARKER_0001"
ESP_MARKER = "A64_USB_ESP_MARKER_0002"


def _positive_serial(
    *,
    case: str = "usb-only",
    guid_line: str = f"UEFI: ESP GUID=UEFI: ESP GUID={ESP_GUID}{ESP_GUID}",
    first_disk: str | None = None,
    include_msc: bool = True,
    include_echo_only: bool = False,
    include_second_prompt: bool = True,
    late_fatal: str = "",
) -> str:
    if first_disk is None:
        first_disk = "usba" if case == "usb-only" else "hda"
    lines = [
        guid_line,
        "[xhci] device bdf=00:02.0 root=1 route=0x00000 slot=0 parent=1 port=0 speed=0 speed_id=3 iface=08:06:50",
    ]
    if include_msc:
        lines.append("usb_msc: usba: 393216 sectors, 512-byte blocks (QEMU QEMU HARDDISK 2.5+)")
        lines.append("[xhci-msc] disk=usba bdf=00:02.0 slot=1")
    lines.extend([
        f"fs: boot scan first_disk={first_disk} candidates={1 if case == 'usb-only' else 2}",
        "fs: boot disk selected by GUID (usba), root=usba2 boot=usba1 (/boot ok)",
        "BusyBox v1.36.1 (2026-11-27 23:00:00 CST) built-in shell (ash)",
        "",
        f"# /bin/busybox cat /usb-root.txt /boot/USBESP.TXT",
    ])
    if include_echo_only:
        lines[-1] = f"# /bin/busybox cat /usb-root.txt /boot/USBESP.TXT # {ROOT_MARKER} {ESP_MARKER}"
    else:
        lines.append(ROOT_MARKER)
        lines.append(ESP_MARKER)
    if include_second_prompt:
        lines.append("# ")
    if late_fatal:
        lines.append(late_fatal)
    return "\n".join(lines) + "\n"


def _wrong_esp_serial(
    *,
    guid_line: str = f"UEFI: ESP GUID=UEFI: ESP GUID={ESP_GUID}{ESP_GUID}",
    first_disk: str = "hda",
    include_msc: bool = True,
    include_shell: bool = False,
    extra_fatal: str = "",
) -> str:
    lines = [
        guid_line,
        "[xhci] device bdf=00:02.0 root=1 route=0x00000 slot=0 parent=1 port=0 speed=0 speed_id=3 iface=08:06:50",
    ]
    if include_msc:
        lines.append("usb_msc: usba: 393216 sectors, 512-byte blocks (QEMU QEMU HARDDISK 2.5+)")
    lines.extend([
        f"fs: boot scan first_disk={first_disk} candidates=2",
        "fs: no disk matched firmware boot ESP GUID",
        "[boot-3] FATAL stage=fs component=root rc=-2 phase=4 cpu=0",
    ])
    if extra_fatal:
        lines.append(extra_fatal)
    if include_shell:
        lines.append("BusyBox v1.36.1 built-in shell (ash)")
        lines.append("# ")
    return "\n".join(lines) + "\n"


class Aarch64UsbQemuCmdTests(unittest.TestCase):
    def test_usb_only_argv_has_single_disk_and_xhci(self) -> None:
        argv = build_qemu_aarch64(
            firmware="/fw/QEMU_EFI.fd",
            image="/img/usb.img",
            cpus=1,
            dtb="/tmp/virt.dtb",
            storage="usb",
        )
        self.assertIn("qemu-xhci,id=xhci,p2=4,p3=0", argv)
        self.assertIn("usb-storage,drive=usb,bus=xhci.0,port=1,bootindex=0", argv)
        self.assertNotIn("virtio-blk-device,drive=disk", argv)
        self.assertNotIn("virtio-blk-device,drive=decoy,bootindex=1", argv)
        self.assertIn("if=pflash,format=raw,readonly=on,file=/fw/QEMU_EFI.fd", argv)
        self.assertIn("if=none,file=/img/usb.img,format=raw,readonly=on,id=usb", argv)

    def test_dual_argv_has_usb_bootindex0_and_decoy_bootindex1(self) -> None:
        argv = build_qemu_aarch64(
            firmware="/fw/QEMU_EFI.fd",
            image="/img/usb.img",
            cpus=1,
            dtb="/tmp/virt.dtb",
            storage="usb+virtio",
            decoy_image="/img/decoy.img",
        )
        self.assertIn("usb-storage,drive=usb,bus=xhci.0,port=1,bootindex=0", argv)
        self.assertIn("virtio-blk-device,drive=decoy,bootindex=1", argv)
        self.assertIn("if=none,file=/img/decoy.img,format=raw,readonly=on,id=decoy", argv)


class Aarch64UsbCheckerTests(unittest.TestCase):
    def test_adjacent_prefix_and_guid_passes(self) -> None:
        serial = _positive_serial(case="usb-only")
        self.assertEqual(
            aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER),
            [],
        )

    def test_dual_passes_with_hda_first_disk_and_usba_root(self) -> None:
        serial = _positive_serial(case="dual", first_disk="hda")
        self.assertEqual(
            aub.check_case("dual", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER),
            [],
        )

    def test_adjacent_second_guid_mismatch_fails(self) -> None:
        serial = _positive_serial(
            guid_line=f"UEFI: ESP GUID=UEFI: ESP GUID={ESP_GUID}{OTHER_GUID}"
        )
        self.assertTrue(aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER))

    def test_trailing_junk_on_guid_line_fails(self) -> None:
        serial = _positive_serial(
            guid_line=f"UEFI: ESP GUID=UEFI: ESP GUID={ESP_GUID}{ESP_GUID}junk"
        )
        self.assertTrue(aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER))

    def test_separate_line_different_guid_fails(self) -> None:
        serial = _positive_serial(
            guid_line=f"UEFI: ESP GUID={ESP_GUID}\nUEFI: ESP GUID={OTHER_GUID}"
        )
        self.assertTrue(aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER))

    def test_missing_msc_fails(self) -> None:
        serial = _positive_serial(include_msc=False)
        self.assertTrue(aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER))

    def test_markers_only_in_command_echo_fails(self) -> None:
        serial = _positive_serial(include_echo_only=True)
        self.assertTrue(aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER))

    def test_missing_second_prompt_fails(self) -> None:
        serial = _positive_serial(include_second_prompt=False)
        self.assertTrue(aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER))

    def test_late_fatal_after_prompt_fails(self) -> None:
        serial = _positive_serial(late_fatal="[kernel panic] late failure")
        self.assertTrue(aub.check_case("usb-only", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER))

    def test_wrong_esp_type_negative_case_passes(self) -> None:
        serial = _wrong_esp_serial()
        self.assertEqual(
            aub.check_case("wrong-esp-type", serial, ESP_GUID, ROOT_MARKER, ESP_MARKER),
            [],
        )

    def test_wrong_esp_type_reaching_shell_or_other_fatal_fails(self) -> None:
        self.assertTrue(
            aub.check_case(
                "wrong-esp-type",
                _wrong_esp_serial(include_shell=True),
                ESP_GUID,
                ROOT_MARKER,
                ESP_MARKER,
            )
        )
        self.assertTrue(
            aub.check_case(
                "wrong-esp-type",
                _wrong_esp_serial(extra_fatal="[kernel panic] boom"),
                ESP_GUID,
                ROOT_MARKER,
                ESP_MARKER,
            )
        )


if __name__ == "__main__":
    unittest.main()
