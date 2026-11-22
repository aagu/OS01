#!/usr/bin/env python3
"""USB boot E2E runner (M1b readonly mount proof and later sub-cases).

CLI (the Make entry point is ``test-qemu SUITE=usb-boot`` or
``test-qemu-usb-boot``)::

    python3 qemutests/usb_boot.py --case readonly \\
        --qemu PATH --firmware FILE --disk FILE \\
        --smp N --log-dir DIR [--timeout 120]

Exit codes: 0 = PASS, 1 = FAIL (evidence missing / timeout / early exit),
2 = ERROR (bad configuration, missing input, spawn failure).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover - exercised only on broken installs
    ProcessSession = None

from qemutests.harness.qemu_cmd import build_qemu_x86


CASE_READONLY = "readonly"
CASE_DUAL = "dual"
CASE_AHCI = "ahci"
CASE_PERSISTENCE = "persistence"
CASE_SUPERSPEED = "superspeed"
CASE_HUB = "hub"
CASES = (
    CASE_READONLY,
    CASE_DUAL,
    CASE_AHCI,
    CASE_PERSISTENCE,
    CASE_SUPERSPEED,
    CASE_HUB,
)
DEFAULT_TIMEOUT = 120.0
OBSERVE_SECONDS = 1.0
POLL_SECONDS = 0.1
MEMORY_MIB = 512

_SECTOR_SIZE = 512
_ESP_TYPE_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
_LINUX_FS_TYPE_GUID = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")

# ── xHCI enumeration evidence ────────────────────────────────────────
# The xHCI lines come from kernel/arch/x86_64/driver/xhci.c.  They are
# the M0 checkpoint's positive proof that OVMF loaded BOOTX64.EFI and
# kernel.bin over a USB-only qemu-xhci topology and that the kernel
# enumerated the MSC interface — they used to be followed by the
# pre-MSC-driver expected failure, which no longer occurs.
_XHCI_SPEED_RE = re.compile(
    r"\[xhci\] port \d+: connected, reset complete, speed=3(?:\s|$)"
)
_XHCI_CONFIG_RE = re.compile(
    r"\[xhci\] slot \d+: configured with config \d+ "
    r"\(iface class 08:06, ep_count=2\)"
)

# ── M1b readonly evidence ───────────────────────────────────────────
_MSC_USBA_RE = re.compile(
    r"usb_msc: usba: \d+ sectors, 512-byte blocks(?:\s|$)"
)
_PROMPT_TEXT = "# "

# ── M3 SuperSpeed & persistence evidence ────────────────────────────
_XHCI_SS_SPEED_RE = re.compile(
    r"\[xhci\] port \d+: (?:USB 3 already enabled in U0|connected, (?:warm )?reset complete), speed=4(?:\s|$)"
)
_XHCI_HUB_READY_RE = re.compile(
    r"\[xhci-hub\] ready bdf=([0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.\d+) "
    r"root=(\d+) route=0x([0-9a-fA-F]{5}) slot=(\d+) protocol=usb2\b"
)
_XHCI_HUB_DEV_RE = re.compile(
    r"\[xhci\] device bdf=([0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.\d+) "
    r"root=(\d+) route=0x([0-9a-fA-F]{5}) slot=(\d+) parent=(\d+) port=(\d+) "
    r"speed=\d+ speed_id=\d+ iface=08:06:50\b"
)
_XHCI_MSC_DISK_RE = re.compile(
    r"\[xhci-msc\] disk=usba bdf=([0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.\d+) slot=(\d+)\b"
)
_SCSI_REQ_RE = re.compile(
    r"scsi_disk_new_request Command: lun=(\d+) tag=(0x[0-9a-fA-F]+) data=(?:0x)?([0-9a-fA-F]+)"
)
_USB_MSD_STATUS_RE = re.compile(
    r"usb_msd_send_status status (\d+), tag (0x[0-9a-fA-F]+),"
)


def superspeed_evidence(serial: str, root_marker: str, esp_marker: str) -> bool:
    """True iff serial proves SuperSpeed (speed=4) and both mount sentinels."""
    if not serial or not _XHCI_SS_SPEED_RE.search(serial):
        return False
    return _mount_marker_evidence(serial, root_marker, esp_marker)


def hub_evidence(
    serial: str, expected_guid: str, root_marker: str, esp_marker: str
) -> bool:
    """True iff serial proves ordered UEFI GUID -> USB2 hub -> BOT child -> usba -> markers."""
    if not serial or not expected_guid or not root_marker or not esp_marker:
        return False
    if "FATAL stage=" in serial:
        return False
    target_guid = expected_guid.lower()
    stage = 0
    hub_bdf = ""
    hub_root = ""
    hub_slot = ""
    dev_slot = ""
    for raw_line in serial.splitlines():
        stripped = raw_line.strip()
        if stage == 0:
            m = _UEFI_ESP_GUID_RE.search(stripped)
            if m and m.group(1).lower() == target_guid:
                stage = 1
        elif stage == 1:
            m = _XHCI_HUB_READY_RE.search(stripped)
            if m:
                hub_bdf = m.group(1).lower()
                hub_root = m.group(2)
                hub_slot = m.group(4)
                stage = 2
        elif stage == 2:
            m = _XHCI_HUB_DEV_RE.search(stripped)
            if (
                m
                and m.group(1).lower() == hub_bdf
                and m.group(2) == hub_root
                and m.group(3) != "00000"
                and m.group(5) == hub_slot
                and int(m.group(6)) >= 1
            ):
                dev_slot = m.group(4)
                stage = 3
        elif stage == 3:
            if _MSC_USBA_RE.search(stripped):
                stage = 4
        elif stage == 4:
            m = _XHCI_MSC_DISK_RE.search(stripped)
            if m and m.group(1).lower() == hub_bdf and m.group(2) == dev_slot:
                stage = 5
                break
    if stage != 5:
        return False
    return _guid_boot_evidence(
        serial, expected_guid, "usba", "usba", "usba2", "usba1", root_marker, esp_marker
    )


def persistence_boot1_evidence(serial: str) -> bool:
    """True iff boot1 serial proves probe emitted USB_WRITE_SYNC_OK and prompt."""
    if not serial:
        return False
    saw_ok = False
    for raw_line in serial.splitlines():
        stripped = raw_line.strip()
        if not saw_ok:
            if stripped == "USB_WRITE_SYNC_OK":
                saw_ok = True
        elif _PROMPT_TEXT in raw_line:
            return True
    return False


def persistence_boot2_evidence(serial: str, marker: str) -> bool:
    """True iff boot2 serial proves cat /usb-persist.txt output standalone marker and prompt."""
    if not serial or not marker:
        return False
    stage = 0
    for raw_line in serial.splitlines():
        stripped = raw_line.strip()
        if stage == 0:
            if "cat /usb-persist.txt" in raw_line:
                stage = 1
        elif stage == 1:
            if stripped == marker:
                stage = 2
        else:
            if _PROMPT_TEXT in raw_line:
                return True
    return False


def persistence_trace_evidence(trace_text: str) -> bool:
    """True iff QEMU trace proves WRITE(10) (0x2a) then SYNCHRONIZE CACHE(10) (0x35) on LUN 0 with status=0 CSWs."""
    if not trace_text:
        return False
    write_tag: str | None = None
    write_csw_ok = False
    sync_tag: str | None = None
    sync_csw_ok = False

    for line in trace_text.splitlines():
        req_m = _SCSI_REQ_RE.search(line)
        if req_m:
            lun = int(req_m.group(1))
            tag = req_m.group(2).lower()
            opcode = int(req_m.group(3), 16)
            if lun == 0:
                if not write_csw_ok and opcode == 0x2A:
                    write_tag = tag
                elif write_csw_ok and not sync_csw_ok and opcode == 0x35:
                    sync_tag = tag
            continue

        csw_m = _USB_MSD_STATUS_RE.search(line)
        if csw_m:
            status = int(csw_m.group(1))
            tag = csw_m.group(2).lower()
            if status == 0:
                if write_tag is not None and not write_csw_ok and tag == write_tag:
                    write_csw_ok = True
                elif sync_tag is not None and not sync_csw_ok and tag == sync_tag:
                    sync_csw_ok = True
                    return True

    return write_csw_ok and sync_csw_ok


def _xhci_enumeration_evidence(serial: str) -> bool:
    """True iff the serial proves an xHCI port reset at speed=3 and the MSC
    interface configured — the M0 checkpoint's positive proof that OVMF
    loaded BOOTX64.EFI + kernel.bin over a USB-only topology.  The port
    reset must precede the interface configuration."""
    if not serial:
        return False
    stage = 0
    for raw_line in serial.splitlines():
        line = raw_line.strip()
        if stage == 0:
            if _XHCI_SPEED_RE.search(line):
                stage = 1
        elif stage == 1:
            if _XHCI_CONFIG_RE.search(line):
                stage = 2
    return stage == 2


def _readonly_prompt_ready(serial: str) -> bool:
    """True iff usba was published and the initial shell prompt appeared."""
    if not serial:
        return False
    saw_usba = False
    for line in serial.splitlines():
        if not saw_usba:
            if _MSC_USBA_RE.search(line):
                saw_usba = True
        elif _PROMPT_TEXT in line:
            return True
    return False


def _mount_marker_evidence(
    serial: str, root_marker: str, esp_marker: str
) -> bool:
    """True iff serial proves usba publication, prompt, both markers, and fresh prompt."""
    if not serial or not root_marker or not esp_marker:
        return False
    stage = 0
    saw_root = False
    saw_esp = False
    for raw_line in serial.splitlines():
        stripped = raw_line.strip()
        if stage == 0:
            if _MSC_USBA_RE.search(stripped):
                stage = 1
        elif stage == 1:
            if _PROMPT_TEXT in raw_line:
                stage = 2
        elif stage == 2:
            if stripped == root_marker:
                saw_root = True
            elif stripped == esp_marker:
                saw_esp = True
            if saw_root and saw_esp:
                stage = 3
        else:
            if _PROMPT_TEXT in raw_line:
                return True
    return False


_UEFI_ESP_GUID_RE = re.compile(
    r"UEFI:\s+ESP\s+GUID=([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})"
)
_BOOT_SCAN_RE = re.compile(r"fs:\s+boot\s+scan\s+first_disk=(\S+)\s+candidates=\d+")
_BOOT_SELECTED_RE = re.compile(
    r"fs:\s+boot\s+disk\s+selected\s+by\s+GUID\s+\(([^)]+)\),\s+root=(\S+)\s+boot=(\S+)\s+\(/boot\s+ok\)"
)


def _guid_boot_evidence(
    serial: str,
    expected_guid: str,
    expected_first_disk: str,
    expected_selected_disk: str,
    expected_root: str,
    expected_boot: str,
    root_marker: str,
    esp_marker: str,
) -> bool:
    if not serial or not expected_guid or not root_marker or not esp_marker:
        return False
    target_guid = expected_guid.lower()
    stage = 0
    saw_root = False
    saw_esp = False
    for raw_line in serial.splitlines():
        stripped = raw_line.strip()
        if stage == 0:
            m = _UEFI_ESP_GUID_RE.search(stripped)
            if m and m.group(1).lower() == target_guid:
                stage = 1
        elif stage == 1:
            m = _BOOT_SCAN_RE.search(stripped)
            if m and m.group(1) == expected_first_disk:
                stage = 2
        elif stage == 2:
            m = _BOOT_SELECTED_RE.search(stripped)
            if (
                m
                and m.group(1) == expected_selected_disk
                and m.group(2) == expected_root
                and m.group(3) == expected_boot
            ):
                stage = 3
        elif stage == 3:
            if _PROMPT_TEXT in raw_line:
                stage = 4
        elif stage == 4:
            if stripped == root_marker:
                saw_root = True
            elif stripped == esp_marker:
                saw_esp = True
            if saw_root and saw_esp:
                stage = 5
        else:
            if _PROMPT_TEXT in raw_line:
                return True
    return False


def _msc_published_before_selection(serial: str) -> bool:
    """True iff the MSC driver published the usba disk before the kernel
    selected it — the read half of the MSC -> GUID-selection chain."""
    saw_msc = False
    for raw_line in serial.splitlines():
        stripped = raw_line.strip()
        if not saw_msc:
            if _MSC_USBA_RE.search(stripped):
                saw_msc = True
        elif _BOOT_SELECTED_RE.search(stripped):
            return True
    return False


def dual_evidence(
    serial: str, usb_guid: str, root_marker: str, esp_marker: str
) -> bool:
    """True iff serial proves firmware ESP GUID, AHCI-first scan, usba selection, and USB markers."""
    return _guid_boot_evidence(
        serial, usb_guid, "hda", "usba", "usba2", "usba1", root_marker, esp_marker
    )


def readonly_evidence(
    serial: str, esp_guid: str, root_marker: str, esp_marker: str
) -> bool:
    """True iff serial proves the whole USB-only chain.

    This is the M1b gate and, since the M0 firmware checkpoint was
    retired, it now carries the checkpoint's positive proof as well:
    xHCI enumeration (port reset at speed=3 + MSC interface configured),
    the MSC driver publishing the usba disk, the firmware ESP GUID the
    kernel booted from, the boot scan over the USB-only disk set, the
    GUID-selected usba root mount, and finally both partition markers
    read back.
    """
    if not esp_guid:
        return False
    if not _xhci_enumeration_evidence(serial):
        return False
    if not _msc_published_before_selection(serial):
        return False
    return _guid_boot_evidence(
        serial, esp_guid, "usba", "usba", "usba2", "usba1", root_marker, esp_marker
    )


def ahci_evidence(
    serial: str, ahci_guid: str, root_marker: str, esp_marker: str
) -> bool:
    """True iff serial proves firmware ESP GUID, hda selection, and AHCI markers."""
    return _guid_boot_evidence(
        serial, ahci_guid, "hda", "hda", "hda2", "hda1", root_marker, esp_marker
    )


def _guid_prompt_ready(serial: str, selected_disk: str) -> bool:
    if not serial:
        return False
    saw_selected = False
    for line in serial.splitlines():
        if not saw_selected:
            m = _BOOT_SELECTED_RE.search(line)
            if m and m.group(1) == selected_disk:
                saw_selected = True
        elif _PROMPT_TEXT in line:
            return True
    return False


def _validate_marker(label: str, value: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} must be a non-empty string")
    if value != value.strip():
        raise ValueError(f"{label} must not have leading/trailing whitespace")
    if any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in value):
        raise ValueError(f"{label} must be printable ASCII")
    return value


def read_gpt_info(image_path: Path) -> dict[str, Any]:
    """Read ESP/root partition byte extents and ESP UniquePartitionGUID from GPT."""
    file_size = image_path.stat().st_size
    if file_size < 2 * _SECTOR_SIZE:
        raise ValueError(f"image too small for GPT: {file_size}")

    with image_path.open("rb") as f:
        f.seek(_SECTOR_SIZE)
        hdr = f.read(92)
        if len(hdr) < 92 or hdr[0:8] != b"EFI PART":
            raise ValueError("invalid GPT header signature")
        hdr_size = struct.unpack_from("<I", hdr, 12)[0]
        if hdr_size < 92 or hdr_size > _SECTOR_SIZE:
            raise ValueError(f"invalid GPT header size: {hdr_size}")
        entries_lba, num_entries, entry_size = struct.unpack_from("<QII", hdr, 72)
        if entry_size < 128 or num_entries == 0 or num_entries > 1024:
            raise ValueError(f"invalid GPT entry table: {num_entries}x{entry_size}")

        table_offset = entries_lba * _SECTOR_SIZE
        table_bytes = num_entries * entry_size
        if table_offset + table_bytes > file_size:
            raise ValueError("GPT partition table exceeds image size")

        f.seek(table_offset)
        table = f.read(table_bytes)

    esp_info: dict[str, Any] | None = None
    root_info: dict[str, Any] | None = None
    for idx in range(num_entries):
        ent = table[idx * entry_size : (idx + 1) * entry_size]
        type_guid = uuid.UUID(bytes_le=ent[0:16])
        if type_guid.int == 0:
            continue
        uniq_guid = uuid.UUID(bytes_le=ent[16:32])
        first_lba, last_lba = struct.unpack_from("<QQ", ent, 32)
        if last_lba < first_lba:
            raise ValueError(f"GPT entry {idx} has last_lba < first_lba")
        offset = first_lba * _SECTOR_SIZE
        size = (last_lba - first_lba + 1) * _SECTOR_SIZE
        if offset + size > file_size:
            raise ValueError(f"GPT entry {idx} exceeds image size")

        if type_guid == _ESP_TYPE_GUID and esp_info is None:
            esp_info = {"guid": uniq_guid, "offset": offset, "size": size}
        elif type_guid == _LINUX_FS_TYPE_GUID and root_info is None:
            root_info = {"guid": uniq_guid, "offset": offset, "size": size}

    if esp_info is None or root_info is None:
        raise ValueError("GPT missing required ESP or Linux filesystem partition")

    return {
        "esp_guid": esp_info["guid"],
        "esp_offset": esp_info["offset"],
        "esp_size": esp_info["size"],
        "root_offset": root_info["offset"],
        "root_size": root_info["size"],
    }


def prepare_image(
    source: Path, destination: Path, root_marker: str, esp_marker: str
) -> uuid.UUID:
    """Copy source image to destination and inject verified root + ESP markers."""
    _validate_marker("root_marker", root_marker)
    _validate_marker("esp_marker", esp_marker)

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    info = read_gpt_info(destination)

    esp_payload = (esp_marker + "\n").encode("ascii")
    root_payload = (root_marker + "\n").encode("ascii")

    with tempfile.TemporaryDirectory(prefix="usb-boot-prep-") as tmpdir:
        tmp = Path(tmpdir)
        esp_file = tmp / "USBESP.TXT"
        root_file = tmp / "usb-root.txt"
        root_img = tmp / "root.ext2"
        esp_file.write_bytes(esp_payload)
        root_file.write_bytes(root_payload)

        esp_spec = f"{destination}@@{info['esp_offset']}"
        subprocess.run(
            ["mcopy", "-o", "-i", esp_spec, str(esp_file), "::/USBESP.TXT"],
            check=True,
            capture_output=True,
        )
        esp_verify = subprocess.run(
            ["mcopy", "-i", esp_spec, "::/USBESP.TXT", "-"],
            check=True,
            capture_output=True,
        )
        if esp_verify.stdout != esp_payload:
            raise RuntimeError("ESP marker readback mismatch")

        with destination.open("rb") as src_f:
            src_f.seek(info["root_offset"])
            remaining = info["root_size"]
            with root_img.open("wb") as out_f:
                while remaining > 0:
                    chunk = src_f.read(min(1 << 20, remaining))
                    if not chunk:
                        raise RuntimeError("unexpected EOF extracting ext2 partition")
                    out_f.write(chunk)
                    remaining -= len(chunk)

        subprocess.run(
            ["debugfs", "-w", str(root_img), "-R", "rm /usb-root.txt"],
            check=False,
            capture_output=True,
        )
        subprocess.run(
            ["debugfs", "-w", str(root_img), "-R", f"write {root_file} /usb-root.txt"],
            check=True,
            capture_output=True,
        )
        root_verify = subprocess.run(
            ["debugfs", str(root_img), "-R", "cat /usb-root.txt"],
            check=True,
            capture_output=True,
        )
        if root_verify.stdout != root_payload:
            raise RuntimeError("root marker readback mismatch")

        if root_img.stat().st_size != info["root_size"]:
            raise RuntimeError("ext2 partition size changed unexpectedly")

        with root_img.open("rb") as in_f, destination.open("r+b") as dst_f:
            dst_f.seek(info["root_offset"])
            remaining = info["root_size"]
            while remaining > 0:
                chunk = in_f.read(min(1 << 20, remaining))
                if not chunk:
                    raise RuntimeError("unexpected EOF reinserting ext2 partition")
                dst_f.write(chunk)
                remaining -= len(chunk)

    return info["esp_guid"]


def inject_root_probe(image: Path, probe_elf: Path, guest_basename: str) -> None:
    """Inject ``probe_elf`` into ``image``'s ext2 root at ``/bin/<guest_basename>`` (0100755) and verify readback."""
    if not guest_basename or guest_basename in (".", "..") or not re.fullmatch(
        r"[A-Za-z0-9._-]+", guest_basename
    ):
        raise ValueError(f"invalid guest_basename: {guest_basename!r}")
    if not probe_elf.is_file():
        raise FileNotFoundError(f"probe_elf does not exist: {probe_elf}")
    probe_bytes = probe_elf.read_bytes()
    if not probe_bytes:
        raise ValueError(f"probe_elf is empty: {probe_elf}")

    info = read_gpt_info(image)
    img_size = image.stat().st_size
    root_offset = int(info["root_offset"])
    root_size = int(info["root_size"])
    if root_offset < 0 or root_size <= 0 or root_offset + root_size > img_size:
        raise ValueError(
            f"root partition bounds invalid: offset={root_offset} size={root_size} img_size={img_size}"
        )
    if len(probe_bytes) >= root_size:
        raise ValueError(
            f"probe_elf ({len(probe_bytes)} bytes) exceeds root partition size ({root_size} bytes)"
        )

    guest_path = f"/bin/{guest_basename}"
    with tempfile.TemporaryDirectory(prefix="usb-probe-inject-") as tmpdir:
        root_img = Path(tmpdir) / "root.ext2"
        with image.open("rb") as src_f:
            src_f.seek(root_offset)
            remaining = root_size
            with root_img.open("wb") as out_f:
                while remaining > 0:
                    chunk = src_f.read(min(1 << 20, remaining))
                    if not chunk:
                        raise RuntimeError("unexpected EOF extracting ext2 partition")
                    out_f.write(chunk)
                    remaining -= len(chunk)

        subprocess.run(
            ["debugfs", "-w", str(root_img), "-R", f"rm {guest_path}"],
            check=False,
            capture_output=True,
        )
        subprocess.run(
            ["debugfs", "-w", str(root_img), "-R", f"write {probe_elf} {guest_path}"],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["debugfs", "-w", str(root_img), "-R", f"set_inode_field {guest_path} mode 0100755"],
            check=True,
            capture_output=True,
        )
        verify = subprocess.run(
            ["debugfs", str(root_img), "-R", f"cat {guest_path}"],
            check=True,
            capture_output=True,
        )
        if verify.stdout != probe_bytes:
            raise RuntimeError(f"{guest_path} readback mismatch")

        if root_img.stat().st_size != root_size:
            raise RuntimeError("ext2 partition size changed unexpectedly")

        with root_img.open("rb") as in_f, image.open("r+b") as dst_f:
            dst_f.seek(root_offset)
            remaining = root_size
            while remaining > 0:
                chunk = in_f.read(min(1 << 20, remaining))
                if not chunk:
                    raise RuntimeError("unexpected EOF reinserting ext2 partition")
                dst_f.write(chunk)
                remaining -= len(chunk)


def prepare_persistence_image(
    source: Path,
    destination: Path,
    root_marker: str,
    esp_marker: str,
    probe_elf: Path,
) -> uuid.UUID:
    """Prepare a writable USB image with /bin/usb-boot-probe (0100755) and no /usb-persist.txt."""
    esp_guid = prepare_image(source, destination, root_marker, esp_marker)
    info = read_gpt_info(destination)
    with tempfile.TemporaryDirectory(prefix="usb-boot-probe-") as tmpdir:
        root_img = Path(tmpdir) / "root.ext2"
        with destination.open("rb") as src_f:
            src_f.seek(info["root_offset"])
            remaining = info["root_size"]
            with root_img.open("wb") as out_f:
                while remaining > 0:
                    chunk = src_f.read(min(1 << 20, remaining))
                    if not chunk:
                        raise RuntimeError("unexpected EOF extracting ext2 partition")
                    out_f.write(chunk)
                    remaining -= len(chunk)

        subprocess.run(
            ["debugfs", "-w", str(root_img), "-R", "rm /usb-persist.txt"],
            check=False,
            capture_output=True,
        )

        if root_img.stat().st_size != info["root_size"]:
            raise RuntimeError("ext2 partition size changed unexpectedly")

        with root_img.open("rb") as in_f, destination.open("r+b") as dst_f:
            dst_f.seek(info["root_offset"])
            remaining = info["root_size"]
            while remaining > 0:
                chunk = in_f.read(min(1 << 20, remaining))
                if not chunk:
                    raise RuntimeError("unexpected EOF reinserting ext2 partition")
                dst_f.write(chunk)
                remaining -= len(chunk)

    inject_root_probe(destination, probe_elf, "usb-boot-probe")
    return esp_guid


def usb_argv(
    qemu: str, firmware: Path, disk: Path, smp: int, *, readonly: bool = True
) -> list[str]:
    """Build the USB-only QEMU command line via the shared builder."""
    return build_qemu_x86(
        qemu=str(qemu),
        firmware=str(firmware),
        image=str(disk),
        smp=smp,
        serial="stdio",
        memory=MEMORY_MIB,
        storage="usb",
        usb_readonly=readonly,
    )


def superspeed_argv(
    qemu: str, firmware: Path, disk: Path, smp: int
) -> list[str]:
    """Build the USB 3.0 SuperSpeed QEMU command line via the shared builder."""
    return build_qemu_x86(
        qemu=str(qemu),
        firmware=str(firmware),
        image=str(disk),
        smp=smp,
        serial="stdio",
        memory=MEMORY_MIB,
        storage="usb",
        usb_speed="3.0",
        usb_readonly=True,
    )


def hub_argv(
    qemu: str, firmware: Path, disk: Path, smp: int
) -> list[str]:
    """Build the USB 2.0 Hub QEMU command line via the shared builder."""
    return build_qemu_x86(
        qemu=str(qemu),
        firmware=str(firmware),
        image=str(disk),
        smp=smp,
        serial="stdio",
        memory=MEMORY_MIB,
        storage="usb",
        usb_speed="2.0",
        usb_readonly=True,
        usb_hub=True,
    )


def dual_argv(
    qemu: str, firmware: Path, usb_disk: Path, ahci_disk: Path, smp: int
) -> list[str]:
    """Build the dual-disk (USB bootindex=0, AHCI bootindex=1) QEMU command line."""
    return build_qemu_x86(
        qemu=str(qemu),
        firmware=str(firmware),
        image=str(usb_disk),
        ahci_image=str(ahci_disk),
        smp=smp,
        serial="stdio",
        memory=MEMORY_MIB,
        storage="dual",
    )


def ahci_argv(
    qemu: str, firmware: Path, ahci_disk: Path, smp: int
) -> list[str]:
    """Build the AHCI-only QEMU command line via the shared builder."""
    return build_qemu_x86(
        qemu=str(qemu),
        firmware=str(firmware),
        image=str(ahci_disk),
        smp=smp,
        serial="stdio",
        memory=MEMORY_MIB,
        storage="ahci",
    )


def _file_sha256(path: Path) -> str | None:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 16), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def _write_json(path: Path, payload: Any) -> None:
    path.write_text(json.dumps(payload, indent=2) + "\n")


def _parse_positive_int(value: Any, label: str) -> int | None:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        print(f"[usb-boot] ERROR: invalid {label} {value!r}", file=sys.stderr)
        return None
    if isinstance(value, bool) or parsed <= 0:
        print(f"[usb-boot] ERROR: invalid {label} {value!r}", file=sys.stderr)
        return None
    return parsed


def _parse_positive_timeout(value: Any) -> float | None:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        print(f"[usb-boot] ERROR: invalid timeout {value!r}", file=sys.stderr)
        return None
    if not math.isfinite(parsed) or parsed <= 0:
        print(f"[usb-boot] ERROR: invalid timeout {value!r}", file=sys.stderr)
        return None
    return parsed


def _run_session_until(
    *,
    session_factory,
    argv: list[str],
    run_dir: Path,
    timeout: float,
    prompt_ready,
    command: bytes,
    evidence_fn,
    case_label: str,
) -> tuple[str, list[str], bool, bool, Exception | None]:
    """Run one QEMU session, send command on prompt, and wait for evidence."""
    session = None
    errors: list[str] = []
    timed_out = False
    stopped_by_runner = False
    spawn_error: Exception | None = None
    try:
        session = session_factory(
            argv=argv,
            run_dir=run_dir,
            timeout_s=timeout,
            writable_stdin=True,
        )
        session.start()
        deadline = session.deadline
        early_returncode: int | None = None
        sent_command = False

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            session.observe(min(POLL_SECONDS, remaining))
            if not sent_command and prompt_ready(session.text):
                session.send(command)
                sent_command = True
            if sent_command and evidence_fn(session.text):
                break
            returncode = session.returncode
            if returncode is not None:
                early_returncode = returncode
                break

        evidence_ok = evidence_fn(session.text)
        if timed_out:
            errors.append(f"timeout before {case_label} evidence")
        elif early_returncode is not None:
            errors.append(
                f"QEMU exited early (rc={early_returncode}) without {case_label} evidence"
            )
        elif not evidence_ok:
            errors.append(f"{case_label} evidence missing")
        elif deadline - time.monotonic() < OBSERVE_SECONDS:
            errors.append("timeout: insufficient observation budget")
        else:
            session.observe(OBSERVE_SECONDS)
            if deadline - time.monotonic() < 0:
                errors.append("overall deadline expired")
    except Exception as exc:  # noqa: BLE001
        spawn_error = exc
    finally:
        if session is not None:
            try:
                session.stop()
            except Exception:  # noqa: BLE001
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001
                pass
            stopped_by_runner = bool(getattr(session, "stopped_by_runner", False))

    serial = session.text if session is not None else ""
    return serial, errors, timed_out, stopped_by_runner, spawn_error


def _run_persistence_case(
    *,
    args: argparse.Namespace,
    firmware: Path,
    disk: Path,
    smp: int,
    timeout: float,
    run_dir: Path,
    session_factory,
    started: float,
) -> int:
    probe_raw = getattr(args, "probe_elf", None)
    if not probe_raw:
        print("[usb-boot] ERROR: --case persistence requires --probe-elf", file=sys.stderr)
        return 2
    probe_elf = Path(probe_raw)
    if not probe_elf.is_file():
        print(f"[usb-boot] ERROR: probe-elf does not exist: {probe_elf}", file=sys.stderr)
        return 2

    root_marker = f"USB_ROOT_MARKER_{uuid.uuid4().hex[:12]}"
    esp_marker = f"USB_ESP_MARKER_{uuid.uuid4().hex[:12]}"
    persist_marker = f"USB_PERSIST_MARKER_{uuid.uuid4().hex[:12]}"
    private_disk = run_dir / "usb-persist.img"

    try:
        esp_guid = prepare_persistence_image(
            disk, private_disk, root_marker, esp_marker, probe_elf
        )
    except Exception as exc:  # noqa: BLE001
        print(f"[usb-boot] ERROR: failed to prepare persistence image: {exc}", file=sys.stderr)
        if private_disk.exists():
            private_disk.unlink(missing_ok=True)
        return 2

    events_file = run_dir / "trace-events.txt"
    events_file.write_text("scsi_disk_new_request\nusb_msd_send_status\n")
    trace_file = run_dir / "trace-boot1.log"

    argv1 = usb_argv(args.qemu, firmware, private_disk, smp, readonly=False) + [
        "-trace",
        f"events={events_file},file={trace_file}",
    ]
    argv2 = usb_argv(args.qemu, firmware, private_disk, smp, readonly=True)
    _write_json(run_dir / "argv-boot1.json", argv1)
    _write_json(run_dir / "argv-boot2.json", argv2)

    metadata: dict[str, Any] = {
        "case": CASE_PERSISTENCE,
        "qemu": str(args.qemu),
        "firmware": str(firmware.resolve()),
        "firmware_sha256": _file_sha256(firmware),
        "disk": str(disk.resolve()),
        "disk_sha256": _file_sha256(disk),
        "probe_elf": str(probe_elf.resolve()),
        "probe_elf_sha256": _file_sha256(probe_elf),
        "esp_guid": str(esp_guid),
        "root_marker": root_marker,
        "esp_marker": esp_marker,
        "persist_marker": persist_marker,
        "smp": smp,
        "timeout_seconds": timeout,
        "argv_boot1": argv1,
        "argv_boot2": argv2,
        "serial_boot1_log": "serial-boot1.log",
        "serial_boot2_log": "serial-boot2.log",
        "trace_boot1_log": "trace-boot1.log",
        "observe_seconds": OBSERVE_SECONDS,
    }

    errors: list[str] = []
    timed_out = False
    boot1_serial = ""
    boot2_serial = ""
    spawn_error: Exception | None = None

    try:
        boot1_serial, err1, to1, stop1, se1 = _run_session_until(
            session_factory=session_factory,
            argv=argv1,
            run_dir=run_dir,
            timeout=timeout,
            prompt_ready=_readonly_prompt_ready,
            command=f"/bin/usb-boot-probe {persist_marker}\n".encode("ascii"),
            evidence_fn=persistence_boot1_evidence,
            case_label="persistence-boot1",
        )
        (run_dir / "serial-boot1.log").write_text(boot1_serial)
        if se1 is not None:
            spawn_error = se1
            raise se1
        errors.extend(err1)
        timed_out = timed_out or to1
        metadata["boot1"] = {
            "timed_out": to1,
            "stopped_by_runner": stop1,
            "serial_bytes": len(boot1_serial),
            "errors": err1,
        }

        trace_text = trace_file.read_text(errors="replace") if trace_file.is_file() else ""
        trace_ok = persistence_trace_evidence(trace_text)
        metadata["trace_ok"] = trace_ok
        if not trace_file.is_file():
            errors.append("missing boot1 QEMU trace file")
        elif not trace_ok:
            errors.append("boot1 QEMU trace missing WRITE(10)->SYNCHRONIZE CACHE(10) LUN 0 status=0 CSWs")

        if not errors:
            boot2_serial, err2, to2, stop2, se2 = _run_session_until(
                session_factory=session_factory,
                argv=argv2,
                run_dir=run_dir,
                timeout=timeout,
                prompt_ready=_readonly_prompt_ready,
                command=b"cat /usb-persist.txt\n",
                evidence_fn=lambda text: persistence_boot2_evidence(text, persist_marker),
                case_label="persistence-boot2",
            )
            (run_dir / "serial-boot2.log").write_text(boot2_serial)
            if se2 is not None:
                spawn_error = se2
                raise se2
            errors.extend(err2)
            timed_out = timed_out or to2
            metadata["boot2"] = {
                "timed_out": to2,
                "stopped_by_runner": stop2,
                "serial_bytes": len(boot2_serial),
                "errors": err2,
            }
    except Exception:
        pass
    finally:
        if private_disk.exists():
            private_disk.unlink(missing_ok=True)

    (run_dir / "serial.log").write_text(
        f"=== BOOT 1 ===\n{boot1_serial}\n=== BOOT 2 ===\n{boot2_serial}\n"
    )

    if spawn_error is not None:
        print(f"[usb-boot] spawn error: {spawn_error}", file=sys.stderr)
        metadata.update(
            {
                "elapsed_seconds": time.monotonic() - started,
                "result": "ERROR",
                "errors": [f"spawn error: {spawn_error}"],
            }
        )
        _write_json(run_dir / "metadata.json", metadata)
        return 2

    passed = (
        not errors
        and not timed_out
        and persistence_boot1_evidence(boot1_serial)
        and persistence_boot2_evidence(boot2_serial, persist_marker)
    )
    metadata.update(
        {
            "elapsed_seconds": time.monotonic() - started,
            "timed_out": timed_out,
            "result": "PASS" if passed else "FAIL",
            "errors": errors,
        }
    )
    _write_json(run_dir / "metadata.json", metadata)

    for error in errors:
        print(f"[usb-boot] FAIL: {error}")
    print(
        f"[usb-boot] case={CASE_PERSISTENCE} result={'PASS' if passed else 'FAIL'} "
        f"firmware_sha256={metadata['firmware_sha256']} "
        f"disk_sha256={metadata['disk_sha256']} log_dir={run_dir}"
    )
    return 0 if passed else 1


def run_case(args: argparse.Namespace, *, session_factory=None) -> int:
    """Run one case and judge it.

    Returns 0 (PASS), 1 (FAIL) or 2 (configuration / environment ERROR).
    """
    started = time.monotonic()
    case = getattr(args, "case", None)
    if case not in CASES:
        print(f"[usb-boot] ERROR: unknown case {case!r}", file=sys.stderr)
        return 2
    smp = _parse_positive_int(getattr(args, "smp", None), "smp")
    if smp is None:
        return 2
    timeout = _parse_positive_timeout(getattr(args, "timeout", DEFAULT_TIMEOUT))
    if timeout is None:
        return 2

    firmware = Path(args.firmware)
    disk = Path(args.disk)
    for label, path in (("firmware", firmware), ("disk", disk)):
        if not path.is_file():
            print(
                f"[usb-boot] ERROR: {label} input does not exist: {path}",
                file=sys.stderr,
            )
            return 2

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("[usb-boot] ERROR: ProcessSession is unavailable", file=sys.stderr)
        return 2

    run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    if case == CASE_PERSISTENCE:
        return _run_persistence_case(
            args=args,
            firmware=firmware,
            disk=disk,
            smp=smp,
            timeout=timeout,
            run_dir=run_dir,
            session_factory=session_factory,
            started=started,
        )

    private_disk: Path | None = None
    private_ahci: Path | None = None
    esp_guid_str: str | None = None
    ahci_guid_str: str | None = None
    root_marker = f"USB_ROOT_MARKER_{uuid.uuid4().hex[:12]}"
    esp_marker = f"USB_ESP_MARKER_{uuid.uuid4().hex[:12]}"
    ahci_root_marker = f"AHCI_ROOT_MARKER_{uuid.uuid4().hex[:12]}"
    ahci_esp_marker = f"AHCI_ESP_MARKER_{uuid.uuid4().hex[:12]}"
    boot_disk = disk

    if case in (CASE_DUAL, CASE_AHCI):
        ahci_disk_raw = getattr(args, "ahci_disk", None)
        if not ahci_disk_raw:
            print(f"[usb-boot] ERROR: --case {case} requires --ahci-disk", file=sys.stderr)
            return 2
        ahci_disk = Path(ahci_disk_raw)
        if not ahci_disk.is_file():
            print(
                f"[usb-boot] ERROR: ahci-disk input does not exist: {ahci_disk}",
                file=sys.stderr,
            )
            return 2
        private_ahci = run_dir / "ahci-prepared.img"
        try:
            ahci_guid = prepare_image(
                ahci_disk, private_ahci, ahci_root_marker, ahci_esp_marker
            )
            ahci_guid_str = str(ahci_guid)
        except Exception as exc:  # noqa: BLE001
            print(f"[usb-boot] ERROR: failed to prepare AHCI image: {exc}", file=sys.stderr)
            if private_ahci.exists():
                private_ahci.unlink(missing_ok=True)
            return 2

    if case in (CASE_READONLY, CASE_DUAL, CASE_SUPERSPEED, CASE_HUB):
        private_disk = run_dir / "usb-readonly.img"
        try:
            esp_guid = prepare_image(disk, private_disk, root_marker, esp_marker)
            esp_guid_str = str(esp_guid)
            boot_disk = private_disk
        except Exception as exc:  # noqa: BLE001
            print(f"[usb-boot] ERROR: failed to prepare USB image: {exc}", file=sys.stderr)
            if private_disk.exists():
                private_disk.unlink(missing_ok=True)
            if private_ahci is not None and private_ahci.exists():
                private_ahci.unlink(missing_ok=True)
            return 2

    if case == CASE_DUAL:
        if esp_guid_str == ahci_guid_str:
            print(
                f"[usb-boot] ERROR: USB and AHCI ESP GUIDs must be distinct ({esp_guid_str})",
                file=sys.stderr,
            )
            if private_disk is not None:
                private_disk.unlink(missing_ok=True)
            if private_ahci is not None:
                private_ahci.unlink(missing_ok=True)
            return 2
        assert private_ahci is not None
        argv = dual_argv(args.qemu, firmware, boot_disk, private_ahci, smp)
    elif case == CASE_AHCI:
        assert private_ahci is not None
        argv = ahci_argv(args.qemu, firmware, private_ahci, smp)
    elif case == CASE_SUPERSPEED:
        argv = superspeed_argv(args.qemu, firmware, boot_disk, smp)
    elif case == CASE_HUB:
        argv = hub_argv(args.qemu, firmware, boot_disk, smp)
    else:
        argv = usb_argv(args.qemu, firmware, boot_disk, smp)
    _write_json(run_dir / "argv.json", argv)

    metadata: dict[str, Any] = {
        "case": case,
        "qemu": str(args.qemu),
        "firmware": str(firmware.resolve()),
        "firmware_sha256": _file_sha256(firmware),
        "disk": str(disk.resolve()),
        "disk_sha256": _file_sha256(disk),
        "smp": smp,
        "timeout_seconds": timeout,
        "argv": argv,
        "serial_log": "serial.log",
        "observe_seconds": OBSERVE_SECONDS,
    }
    if case in (CASE_READONLY, CASE_DUAL, CASE_SUPERSPEED, CASE_HUB):
        metadata["esp_guid"] = esp_guid_str
        metadata["root_marker"] = root_marker
        metadata["esp_marker"] = esp_marker
    if case in (CASE_DUAL, CASE_AHCI):
        metadata["ahci_guid"] = ahci_guid_str
        metadata["ahci_root_marker"] = ahci_root_marker
        metadata["ahci_esp_marker"] = ahci_esp_marker

    def _case_prompt_ready(text: str) -> bool:
        if case in (CASE_READONLY, CASE_SUPERSPEED):
            return _readonly_prompt_ready(text)
        if case in (CASE_DUAL, CASE_HUB):
            return _guid_prompt_ready(text, "usba")
        if case == CASE_AHCI:
            return _guid_prompt_ready(text, "hda")
        return False

    def _case_evidence(text: str) -> bool:
        if case == CASE_READONLY:
            assert esp_guid_str is not None
            return readonly_evidence(text, esp_guid_str, root_marker, esp_marker)
        if case == CASE_SUPERSPEED:
            return superspeed_evidence(text, root_marker, esp_marker)
        if case == CASE_HUB:
            assert esp_guid_str is not None
            return hub_evidence(text, esp_guid_str, root_marker, esp_marker)
        if case == CASE_DUAL:
            assert esp_guid_str is not None
            return dual_evidence(text, esp_guid_str, root_marker, esp_marker)
        assert ahci_guid_str is not None
        return ahci_evidence(text, ahci_guid_str, ahci_root_marker, ahci_esp_marker)

    session = None
    errors: list[str] = []
    timed_out = False
    spawn_error: Exception | None = None
    try:
        session = session_factory(
            argv=argv,
            run_dir=run_dir,
            timeout_s=timeout,
            writable_stdin=True,
        )
        session.start()
        deadline = session.deadline

        early_returncode: int | None = None
        sent_command = False
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            session.observe(min(POLL_SECONDS, remaining))
            if not sent_command and _case_prompt_ready(session.text):
                session.send(b"cat /usb-root.txt /boot/USBESP.TXT\n")
                sent_command = True
            if sent_command and _case_evidence(session.text):
                break
            returncode = session.returncode
            if returncode is not None:
                early_returncode = returncode
                break

        evidence_ok = _case_evidence(session.text)

        if timed_out:
            errors.append(f"timeout before {case} evidence")
        elif early_returncode is not None:
            errors.append(
                "QEMU exited early "
                f"(rc={early_returncode}) without {case} evidence"
            )
        elif not evidence_ok:
            errors.append(f"{case} evidence missing")
        elif deadline - time.monotonic() < OBSERVE_SECONDS:
            errors.append("timeout: insufficient observation budget")
        else:
            session.observe(OBSERVE_SECONDS)
            if deadline - time.monotonic() < 0:
                errors.append("overall deadline expired")
    except Exception as exc:  # noqa: BLE001
        spawn_error = exc
    finally:
        if session is not None:
            try:
                session.stop()
            except Exception:  # noqa: BLE001
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001
                pass
        if private_disk is not None and private_disk.exists():
            private_disk.unlink(missing_ok=True)
        if private_ahci is not None and private_ahci.exists():
            private_ahci.unlink(missing_ok=True)

    serial = session.text if session is not None else ""
    (run_dir / "serial.log").write_text(serial)

    if spawn_error is not None:
        print(f"[usb-boot] spawn error: {spawn_error}", file=sys.stderr)
        metadata.update(
            {
                "elapsed_seconds": time.monotonic() - started,
                "result": "ERROR",
                "errors": [f"spawn error: {spawn_error}"],
            }
        )
        _write_json(run_dir / "metadata.json", metadata)
        return 2

    passed = (
        not errors
        and not timed_out
        and _case_evidence(serial)
    )

    metadata.update(
        {
            "elapsed_seconds": time.monotonic() - started,
            "timed_out": timed_out,
            "stopped_by_runner": bool(
                session is not None and session.stopped_by_runner
            ),
            "serial_bytes": len(serial),
            "result": "PASS" if passed else "FAIL",
            "errors": errors,
        }
    )
    _write_json(run_dir / "metadata.json", metadata)

    for error in errors:
        print(f"[usb-boot] FAIL: {error}")
    print(
        f"[usb-boot] case={case} result={'PASS' if passed else 'FAIL'} "
        f"firmware_sha256={metadata['firmware_sha256']} "
        f"disk_sha256={metadata['disk_sha256']} log_dir={run_dir}"
    )
    return 0 if passed else 1


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="USB boot E2E runner"
    )
    parser.add_argument(
        "--case", choices=CASES, default=CASE_READONLY,
        help="gate case (default: readonly)",
    )
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--ahci-disk", default=None)
    parser.add_argument("--probe-elf", default=None)
    parser.add_argument("--smp", required=True, type=int)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    return run_case(parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
