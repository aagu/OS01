#!/usr/bin/env python3
"""USB boot E2E runner (M0 firmware diagnostic & M1b readonly mount proof).

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


CASE_FIRMWARE = "firmware"
CASE_READONLY = "readonly"
CASES = (CASE_FIRMWARE, CASE_READONLY)
DEFAULT_TIMEOUT = 120.0
OBSERVE_SECONDS = 1.0
POLL_SECONDS = 0.1
MEMORY_MIB = 512

_SECTOR_SIZE = 512
_ESP_TYPE_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
_LINUX_FS_TYPE_GUID = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")

# ── M0 evidence ─────────────────────────────────────────────────────
_XHCI_SPEED_RE = re.compile(
    r"\[xhci\] port \d+: connected, reset complete, speed=3(?:\s|$)"
)
_XHCI_CONFIG_RE = re.compile(
    r"\[xhci\] slot \d+: configured with config \d+ "
    r"\(iface class 08:06, ep_count=2\)"
)
_FATAL_PREFIX_RE = re.compile(
    r"^\[boot-3\] FATAL stage=fs component=root rc=-19(?=\s|$)"
)

# ── M1b readonly evidence ───────────────────────────────────────────
_MSC_USBA_RE = re.compile(
    r"usb_msc: usba: \d+ sectors, 512-byte blocks(?:\s|$)"
)
_PROMPT_TEXT = "# "


def firmware_evidence(serial: str) -> bool:
    """True iff the serial stream contains the M0 firmware evidence."""
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
        else:
            if _FATAL_PREFIX_RE.match(line):
                return True
    return False


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


def readonly_evidence(serial: str, root_marker: str, esp_marker: str) -> bool:
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


def usb_argv(
    qemu: str, firmware: Path, disk: Path, smp: int
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

    private_disk: Path | None = None
    esp_guid_str: str | None = None
    root_marker = f"USB_ROOT_MARKER_{uuid.uuid4().hex[:12]}"
    esp_marker = f"USB_ESP_MARKER_{uuid.uuid4().hex[:12]}"
    boot_disk = disk

    if case == CASE_READONLY:
        private_disk = run_dir / "usb-readonly.img"
        try:
            esp_guid = prepare_image(disk, private_disk, root_marker, esp_marker)
            esp_guid_str = str(esp_guid)
            boot_disk = private_disk
        except Exception as exc:  # noqa: BLE001
            print(f"[usb-boot] ERROR: failed to prepare USB image: {exc}", file=sys.stderr)
            if private_disk.exists():
                private_disk.unlink(missing_ok=True)
            return 2

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
    if case == CASE_READONLY:
        metadata["esp_guid"] = esp_guid_str
        metadata["root_marker"] = root_marker
        metadata["esp_marker"] = esp_marker

    session = None
    errors: list[str] = []
    timed_out = False
    spawn_error: Exception | None = None
    try:
        session = session_factory(
            argv=argv,
            run_dir=run_dir,
            timeout_s=timeout,
            writable_stdin=(case == CASE_READONLY),
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
            if case == CASE_FIRMWARE:
                if firmware_evidence(session.text):
                    break
            else:
                if not sent_command and _readonly_prompt_ready(session.text):
                    session.send(b"cat /usb-root.txt /boot/USBESP.TXT\n")
                    sent_command = True
                if sent_command and readonly_evidence(
                    session.text, root_marker, esp_marker
                ):
                    break
            returncode = session.returncode
            if returncode is not None:
                early_returncode = returncode
                break

        evidence_ok = (
            firmware_evidence(session.text)
            if case == CASE_FIRMWARE
            else readonly_evidence(session.text, root_marker, esp_marker)
        )

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
        and (
            firmware_evidence(serial)
            if case == CASE_FIRMWARE
            else readonly_evidence(serial, root_marker, esp_marker)
        )
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
    parser.add_argument("--smp", required=True, type=int)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    return parser.parse_args(argv)


def main(argv=None) -> int:
    return run_case(parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
