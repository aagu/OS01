#!/usr/bin/env python3
"""OS01 aarch64 QEMU USB Mass Storage boot acceptance suite.

Cases:
  usb-only       : Boots aarch64-uefi.img exclusively over qemu-xhci + usb-storage.
  dual           : Boots USB (bootindex=0) alongside a virtio-blk decoy (bootindex=1)
                   with distinct GPT/ESP UUIDs and markers.
  wrong-esp-type : Changes USB partition 1 type GUID to Microsoft Basic Data while
                   keeping the original ESP partition GUID, LBA, and FAT contents
                   identical; verifies GUID-match failure rc=-2 without shell.

Exit codes: 0 = PASS, 1 = FAIL, 2 = ERROR.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import Any

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:
    from qemutests.harness.result import RunArchive
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]

from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb  # noqa: E402
from qemutests.harness.qemu_cmd import build_qemu_aarch64  # noqa: E402
from qemutests.usb_boot import prepare_image, read_gpt_info  # noqa: E402

SUITE = "aarch64-usb-boot"
CASES = ("usb-only", "dual", "wrong-esp-type")
DEFAULT_TIMEOUT = 120.0
OBSERVE_SECONDS = 1.0
_GUID_PREFIX = "UEFI: ESP GUID="
_BASIC_DATA_TYPE_GUID = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

_ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07")
_UUID_RE = re.compile(
    r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"
)
_BOT_RE = re.compile(r"\[xhci\] device bdf=\S+ .*iface=08:06:50\b")
_MSC_RE = re.compile(r"usb_msc: usba: \d+ sectors, 512-byte blocks\b")
_FIRST_DISK_USB_RE = re.compile(r"fs: boot scan first_disk=usba candidates=\d+")
_FIRST_DISK_HDA_RE = re.compile(r"fs: boot scan first_disk=hda candidates=\d+")
_SELECTED_RE = re.compile(
    r"fs: boot disk selected by GUID \(usba\), root=usba2 boot=usba1 \(/boot ok\)"
)
_ASH_RE = re.compile(r"BusyBox .* built-in shell \(ash\)")
_PROMPT_RE = re.compile(r"^# ", re.MULTILINE)
_GUID_FAIL_RE = re.compile(r"fs: no disk matched firmware boot ESP GUID")
_RC2_FATAL_RE = re.compile(r"FATAL.*rc=-2\b")
_FATAL_OR_PANIC_RE = re.compile(r"(?:FATAL|\[kernel panic\]|PANIC:)")


def _sanitize_serial(serial: str) -> str:
    return _ANSI_RE.sub("", serial).replace("\r", "")


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _sha256_slice(path: Path, offset: int, size: int) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        f.seek(offset)
        rem = size
        while rem > 0:
            chunk = f.read(min(1 << 20, rem))
            if not chunk:
                raise RuntimeError("unexpected EOF hashing slice")
            h.update(chunk)
            rem -= len(chunk)
    return h.hexdigest()


def _parse_guid_lines(clean: str, expected_guid: str) -> list[str]:
    violations: list[str] = []
    expected = expected_guid.lower()
    seen_records = 0
    for lineno, line in enumerate(clean.splitlines(), start=1):
        if not line.startswith(_GUID_PREFIX):
            continue
        seen_records += 1
        pos = 0
        line_uuids: list[str] = []
        while pos < len(line):
            if line.startswith(_GUID_PREFIX, pos):
                pos += len(_GUID_PREFIX)
            elif line[pos].isspace():
                pos += 1
            elif pos + 36 <= len(line) and _UUID_RE.fullmatch(line[pos : pos + 36]):
                line_uuids.append(line[pos : pos + 36].lower())
                pos += 36
            else:
                violations.append(
                    f"line {lineno}: undecodable ESP GUID handoff token at offset {pos}: {line!r}"
                )
                break
        if not line_uuids:
            violations.append(f"line {lineno}: ESP GUID record contained no UUID")
        for u in line_uuids:
            if u != expected:
                violations.append(
                    f"line {lineno}: ESP GUID mismatch: got {u}, expected {expected}"
                )
    if seen_records == 0:
        violations.append("missing 'UEFI: ESP GUID=' handoff record")
    return violations


def _check_ordered(clean: str, patterns: list[tuple[str, re.Pattern[str]]]) -> tuple[list[str], int]:
    violations: list[str] = []
    pos = 0
    for label, pat in patterns:
        m = pat.search(clean, pos)
        if not m:
            violations.append(f"missing ordered evidence: {label}")
            return violations, pos
        pos = m.end()
    return violations, pos


def check_case(
    case: str,
    serial: str,
    expected_guid: str,
    root_marker: str,
    esp_marker: str,
) -> list[str]:
    """Validate serial transcript for one aarch64 USB boot case."""
    if case not in CASES:
        return [f"unknown case: {case!r}"]

    clean = _sanitize_serial(serial)
    violations = _parse_guid_lines(clean, expected_guid)

    if case in ("usb-only", "dual"):
        if _FATAL_OR_PANIC_RE.search(clean):
            violations.append("unexpected FATAL or kernel panic in serial output")
        first_disk_pat = _FIRST_DISK_USB_RE if case == "usb-only" else _FIRST_DISK_HDA_RE
        first_disk_lbl = "first_disk=usba" if case == "usb-only" else "first_disk=hda"
        ordered_v, pos = _check_ordered(
            clean,
            [
                ("BOT interface 08:06:50", _BOT_RE),
                ("usb_msc usba 512-byte blocks", _MSC_RE),
                (first_disk_lbl, first_disk_pat),
                ("GUID-selected usba root/boot", _SELECTED_RE),
                ("BusyBox ash banner", _ASH_RE),
                ("initial shell prompt", _PROMPT_RE),
            ],
        )
        violations.extend(ordered_v)
        if not ordered_v:
            post_v, _ = _check_ordered(
                clean[pos:],
                [
                    ("standalone root marker line", re.compile(rf"^{re.escape(root_marker)}$", re.MULTILINE)),
                    ("standalone esp marker line", re.compile(rf"^{re.escape(esp_marker)}$", re.MULTILINE)),
                    ("second shell prompt", _PROMPT_RE),
                ],
            )
            violations.extend(post_v)
        return violations

    # Negative case: wrong-esp-type
    ordered_v, _ = _check_ordered(
        clean,
        [
            ("BOT interface 08:06:50", _BOT_RE),
            ("usb_msc usba 512-byte blocks", _MSC_RE),
            ("first_disk=hda", _FIRST_DISK_HDA_RE),
            ("GUID match failure", _GUID_FAIL_RE),
            ("FATAL rc=-2", _RC2_FATAL_RE),
        ],
    )
    violations.extend(ordered_v)

    if _ASH_RE.search(clean) or _PROMPT_RE.search(clean):
        violations.append("wrong-esp-type unexpectedly reached shell")
    for line in clean.splitlines():
        if _FATAL_OR_PANIC_RE.search(line) and not _RC2_FATAL_RE.search(line):
            violations.append(f"unexpected fatal/panic in wrong-esp-type: {line!r}")
    return violations


def _make_decoy_image(src: Path, dst: Path, root_marker: str, esp_marker: str) -> None:
    prepare_image(src, dst, root_marker, esp_marker)
    subprocess.run(["sfdisk", "--disk-id", str(dst), str(uuid.uuid4())], check=True, capture_output=True)
    subprocess.run(["sfdisk", "--part-uuid", str(dst), "1", str(uuid.uuid4())], check=True, capture_output=True)


def _prepare_wrong_esp_image(src: Path, dst: Path, root_marker: str, esp_marker: str) -> uuid.UUID:
    orig_guid = prepare_image(src, dst, root_marker, esp_marker)
    before = read_gpt_info(dst)
    fat_before = _sha256_slice(dst, before["esp_offset"], before["esp_size"])
    before_json = json.loads(
        subprocess.run(["sfdisk", "--json", str(dst)], check=True, capture_output=True, text=True).stdout
    )
    p1_before = before_json["partitiontable"]["partitions"][0]

    subprocess.run(
        ["sfdisk", "--part-type", str(dst), "1", _BASIC_DATA_TYPE_GUID],
        check=True,
        capture_output=True,
    )
    after_json = json.loads(
        subprocess.run(["sfdisk", "--json", str(dst)], check=True, capture_output=True, text=True).stdout
    )
    p1_after = after_json["partitiontable"]["partitions"][0]
    fat_after = _sha256_slice(dst, before["esp_offset"], before["esp_size"])
    if (
        uuid.UUID(p1_after["uuid"]) != orig_guid
        or p1_after["start"] != p1_before["start"]
        or p1_after["size"] != p1_before["size"]
        or fat_before != fat_after
    ):
        raise RuntimeError("wrong-esp-type sfdisk modified ESP GUID, LBA, or FAT contents")
    return orig_guid


def _run_one_case(
    case: str,
    *,
    qemu: str,
    firmware: Path,
    image: Path,
    build_dir: Path,
    timeout: float,
    profile: str,
) -> int:
    if ProcessSession is None or RunArchive is None:
        print("ERROR: qemutests harness modules unavailable", file=sys.stderr)
        return 2

    archive = RunArchive.create(build_dir, f"{SUITE}-{case}")
    case_dir = archive.run_dir

    fw_hash_before = _sha256_file(firmware)
    img_hash_before = _sha256_file(image)

    usb_marker_root = f"A64_USB_ROOT_{uuid.uuid4().hex[:12]}"
    usb_marker_esp = f"A64_USB_ESP_{uuid.uuid4().hex[:12]}"
    usb_img = case_dir / "usb.img"
    decoy_img: Path | None = None

    if case == "usb-only":
        expected_guid = str(prepare_image(image, usb_img, usb_marker_root, usb_marker_esp))
        storage = "usb"
    elif case == "dual":
        expected_guid = str(prepare_image(image, usb_img, usb_marker_root, usb_marker_esp))
        decoy_img = case_dir / "decoy.img"
        _make_decoy_image(
            image,
            decoy_img,
            f"A64_DECOY_ROOT_{uuid.uuid4().hex[:12]}",
            f"A64_DECOY_ESP_{uuid.uuid4().hex[:12]}",
        )
        storage = "usb+virtio"
    elif case == "wrong-esp-type":
        expected_guid = str(
            _prepare_wrong_esp_image(image, usb_img, usb_marker_root, usb_marker_esp)
        )
        decoy_img = case_dir / "decoy.img"
        _make_decoy_image(
            image,
            decoy_img,
            f"A64_DECOY_ROOT_{uuid.uuid4().hex[:12]}",
            f"A64_DECOY_ESP_{uuid.uuid4().hex[:12]}",
        )
        storage = "usb+virtio"
    else:
        return 2

    dtb_path = generate_diagnostic_dtb(qemu, str(case_dir), cpus=1, ram_mib=512)
    argv = build_qemu_aarch64(
        firmware=str(firmware),
        image=str(usb_img),
        cpus=1,
        dtb=dtb_path,
        storage=storage,
        decoy_image=str(decoy_img) if decoy_img else None,
    )

    started = time.monotonic()
    timed_out = False
    early_exit_code: int | None = None
    session = ProcessSession(
        argv=argv,
        run_dir=case_dir,
        timeout_s=timeout,
        writable_stdin=True,
    )
    try:
        session.start()
        if case in ("usb-only", "dual"):
            if not session.wait_for(lambda s: "# " in s) or session.timed_out:
                timed_out = True
            else:
                cmd = b"/bin/busybox cat /usb-root.txt /boot/USBESP.TXT\n"
                for i in range(0, len(cmd), 8):
                    session.send(cmd[i : i + 8])
                    time.sleep(0.01)
                if not session.wait_for(
                    lambda s: usb_marker_esp in s and re.search(
                        r"(?m)^# ", s.replace("\r", "").split(usb_marker_esp, 1)[1]
                    ) is not None
                ) or session.timed_out:
                    timed_out = True
                else:
                    session.observe(OBSERVE_SECONDS)
        else:
            if not session.wait_for(lambda s: "rc=-2" in s) or session.timed_out:
                timed_out = True
            else:
                session.observe(OBSERVE_SECONDS)

        if session.returncode is not None and case in ("usb-only", "dual"):
            early_exit_code = session.returncode
        serial_text = session.text
    finally:
        try:
            session.stop()
        finally:
            session.close()

    (case_dir / "serial.log").write_text(serial_text, encoding="utf-8")
    violations = check_case(case, serial_text, expected_guid, usb_marker_root, usb_marker_esp)
    if timed_out:
        violations.append(f"timed out waiting for expected serial milestone ({timeout}s)")
    if early_exit_code is not None:
        violations.append(f"QEMU exited early with code {early_exit_code}")
    if _sha256_file(firmware) != fw_hash_before:
        violations.append("firmware file was modified during QEMU run")
    if _sha256_file(image) != img_hash_before:
        violations.append("source image file was modified during QEMU run")

    passed = len(violations) == 0
    meta: dict[str, Any] = {
        "case": case,
        "profile": profile,
        "expected_guid": expected_guid,
        "argv": argv,
        "passed": passed,
        "violations": violations,
        "duration_s": round(time.monotonic() - started, 3),
    }
    (case_dir / "metadata.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    status = "PASS" if passed else "FAIL"
    print(f"[{SUITE}] {case}: {status} ({meta['duration_s']}s)")
    if not passed:
        for v in violations:
            print(f"  - {v}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="aarch64 QEMU USB boot acceptance suite")
    parser.add_argument("--case", choices=("all", *CASES), default="all")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--profile", default="aarch64-clang")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = parser.parse_args(argv)

    firmware = Path(args.firmware)
    image = Path(args.image)
    if not firmware.is_file() or not image.is_file():
        print("ERROR: missing --firmware or --image file", file=sys.stderr)
        return 2
    if shutil.which(args.qemu) is None:
        print(f"ERROR: qemu binary not found: {args.qemu}", file=sys.stderr)
        return 2

    build_dir = Path(args.build_dir)
    cases = CASES if args.case == "all" else (args.case,)
    rc = 0
    for c in cases:
        if _run_one_case(
            c,
            qemu=args.qemu,
            firmware=firmware,
            image=image,
            build_dir=build_dir,
            timeout=args.timeout,
            profile=args.profile,
        ) != 0:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
