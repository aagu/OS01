#!/usr/bin/env python3
"""USB-only HID input & shared Hub storage QEMU E2E runner.

CLI (the Make entry point is ``test-qemu SUITE=usb-hid``)::

    python3 qemutests/usb_hid.py --case direct|hub \\
        --qemu PATH --firmware FILE --disk FILE --probe-elf FILE \\
        --smp N --timeout 180 --log-dir DIR

Exit codes: 0 = PASS, 1 = FAIL (evidence missing / timeout / early exit),
2 = ERROR (bad configuration, missing input, spawn failure).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
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

from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb
from qemutests.harness.qemu_cmd import build_qemu_aarch64, build_qemu_x86
from qemutests.harness.qmp import QmpClient
from qemutests import usb_boot as ub


CASE_DIRECT = "direct"
CASE_HUB = "hub"
CASES = (CASE_DIRECT, CASE_HUB)
DEFAULT_TIMEOUT = 180.0
POLL_SECONDS = 0.05
MEMORY_MIB = 512

_PROMPT_TEXT = "# "

# Scancode Set 1 map for lowercase letters and digits (used for pty_nonce).
_CHAR_TO_QCODE_AND_SC: dict[str, tuple[str, int]] = {
    "a": ("a", 0x1E), "b": ("b", 0x30), "c": ("c", 0x2E), "d": ("d", 0x20),
    "e": ("e", 0x12), "f": ("f", 0x21), "g": ("g", 0x22), "h": ("h", 0x23),
    "i": ("i", 0x17), "j": ("j", 0x24), "k": ("k", 0x25), "l": ("l", 0x26),
    "m": ("m", 0x32), "n": ("n", 0x31), "o": ("o", 0x18), "p": ("p", 0x19),
    "q": ("q", 0x10), "r": ("r", 0x13), "s": ("s", 0x1F), "t": ("t", 0x14),
    "u": ("u", 0x16), "v": ("v", 0x2F), "w": ("w", 0x11), "x": ("x", 0x2D),
    "y": ("y", 0x15), "z": ("z", 0x2C),
    "0": ("0", 0x0B), "1": ("1", 0x02), "2": ("2", 0x03), "3": ("3", 0x04),
    "4": ("4", 0x05), "5": ("5", 0x06), "6": ("6", 0x07), "7": ("7", 0x08),
    "8": ("8", 0x09), "9": ("9", 0x0A),
    " ": ("spc", 0x39), "-": ("minus", 0x0C), "_": ("minus", 0x0C),
    "=": ("equal", 0x0D), "/": ("slash", 0x35), ".": ("dot", 0x34),
    ">": ("dot", 0x34),
}


def direct_argv(
    qemu: str,
    firmware: Path,
    disk: Path,
    smp: int,
    *,
    arch: str = "x86_64",
    dtb: Path | None = None,
    qmp_sock: Path | None = None,
) -> list[str]:
    """Build the direct USB HID + root disk QEMU command line."""
    extra: list[str] = []
    if qmp_sock is not None:
        extra.extend(["-qmp", f"unix:{qmp_sock},server=on,wait=off"])
    if arch == "aarch64":
        extra.extend(["-device", "bochs-display"])
        return build_qemu_aarch64(
            qemu=str(qemu),
            firmware=str(firmware),
            image=str(disk),
            cpus=smp,
            dtb=str(dtb) if dtb is not None else None,
            memory=MEMORY_MIB,
            storage="virtio",
            usb_input="direct",
            extra=extra,
        )
    return build_qemu_x86(
        qemu=str(qemu),
        firmware=str(firmware),
        image=str(disk),
        smp=smp,
        serial="stdio",
        memory=MEMORY_MIB,
        storage="ahci",
        usb_input="direct",
        extra=extra,
    )


def hub_argv(
    qemu: str,
    firmware: Path,
    disk: Path,
    smp: int,
    *,
    arch: str = "x86_64",
    dtb: Path | None = None,
    qmp_sock: Path | None = None,
    trace_events: Path | None = None,
    trace_file: Path | None = None,
) -> list[str]:
    """Build the shared USB Hub (storage@1.2, kbd@1.3, mouse@1.4) QEMU command line."""
    extra: list[str] = []
    if qmp_sock is not None:
        extra.extend(["-qmp", f"unix:{qmp_sock},server=on,wait=off"])
    if trace_events is not None and trace_file is not None:
        extra.extend(["-trace", f"events={trace_events},file={trace_file}"])
    if arch == "aarch64":
        extra.extend(["-device", "bochs-display"])
        return build_qemu_aarch64(
            qemu=str(qemu),
            firmware=str(firmware),
            image=str(disk),
            cpus=smp,
            dtb=str(dtb) if dtb is not None else None,
            memory=MEMORY_MIB,
            storage="usb",
            usb_readonly=False,
            usb_hub=True,
            usb_input="hub",
            extra=extra,
        )
    return build_qemu_x86(
        qemu=str(qemu),
        firmware=str(firmware),
        image=str(disk),
        smp=smp,
        serial="stdio",
        memory=MEMORY_MIB,
        storage="usb",
        usb_speed="2.0",
        usb_readonly=False,
        usb_hub=True,
        usb_input="hub",
        extra=extra,
    )


def topology_evidence(case: str, info_usb: str, serial: str) -> bool:
    """Validate QMP ``info usb`` ports/speeds/ids jointly with guest bindings."""
    if not info_usb or not serial:
        return False
    if case == CASE_DIRECT:
        kbd_qmp = re.search(
            r"Port 2,\s*Speed 480 Mb/s,.*ID:\s*kbd\b", info_usb
        )
        mouse_qmp = re.search(
            r"Port 3,\s*Speed 480 Mb/s,.*ID:\s*mouse\b", info_usb
        )
        kbd_guest = re.search(
            r"\[xhci-hid\] bound bdf=\S+ root=2 route=0x[0-9a-fA-F]+ slot=\d+ iface=\d+ dci=\d+ kind=kbd\b",
            serial,
        )
        mouse_guest = re.search(
            r"\[xhci-hid\] bound bdf=\S+ root=3 route=0x[0-9a-fA-F]+ slot=\d+ iface=\d+ dci=\d+ kind=mouse\b",
            serial,
        )
        return bool(kbd_qmp and mouse_qmp and kbd_guest and mouse_guest)

    if case == CASE_HUB:
        hub_qmp = re.search(r"Port 1,\s*Speed (?:480|12) Mb/s,.*Hub", info_usb)
        disk_qmp = re.search(r"Port 1\.2,\s*Speed (?:480|12) Mb/s,", info_usb)
        kbd_qmp = re.search(
            r"Port 1\.3,\s*Speed 12 Mb/s,.*ID:\s*kbd\b", info_usb
        )
        mouse_qmp = re.search(
            r"Port 1\.4,\s*Speed 12 Mb/s,.*ID:\s*mouse\b", info_usb
        )
        hub_guest = re.search(
            r"\[xhci-hub\] ready bdf=\S+ root=1 route=0x00000 slot=\d+ protocol=usb2\b",
            serial,
        )
        msc_guest = re.search(r"\[xhci-msc\] disk=usba bdf=\S+ slot=\d+\b", serial)
        kbd_guest = re.search(
            r"\[xhci-hid\] bound bdf=\S+ root=1 route=0x0*[1-9a-fA-F][0-9a-fA-F]* slot=\d+ iface=\d+ dci=\d+ kind=kbd\b",
            serial,
        )
        mouse_guest = re.search(
            r"\[xhci-hid\] bound bdf=\S+ root=1 route=0x0*[1-9a-fA-F][0-9a-fA-F]* slot=\d+ iface=\d+ dci=\d+ kind=mouse\b",
            serial,
        )
        return bool(
            hub_qmp
            and disk_qmp
            and kbd_qmp
            and mouse_qmp
            and hub_guest
            and msc_guest
            and kbd_guest
            and mouse_guest
        )

    return False


def _ordered_subseq(lines: list[str], patterns: list[str]) -> bool:
    idx = 0
    for line in lines:
        if idx < len(patterns) and patterns[idx] in line:
            idx += 1
    return idx == len(patterns)


def round1_input_evidence(
    serial_slice: str, nonce: str, expected_pty_frames: list[str]
) -> bool:
    """Validate lease READY, E0 left make/break, mouse move/buttons, PTY isolation, and DONE."""
    if not serial_slice or not nonce:
        return False
    lines = [ln.strip() for ln in serial_slice.splitlines()]
    if not any(
        re.match(rf"^USB_HID_INPUT_READY nonce={re.escape(nonce)} gen=[1-9]\d*$", ln)
        for ln in lines
    ):
        return False

    required = [
        f"KEY_FRAME nonce={nonce} hex=e04b",
        f"KEY_FRAME nonce={nonce} hex=e0cb",
        f"MOUSE_EVENT nonce={nonce} btn=0 dx=12 dy=-3 wheel=0",
        f"MOUSE_EVENT nonce={nonce} btn=1 ",
        f"MOUSE_EVENT nonce={nonce} btn=0 ",
        f"MOUSE_EVENT nonce={nonce} btn=2 ",
        f"MOUSE_EVENT nonce={nonce} btn=0 ",
        f"MOUSE_EVENT nonce={nonce} btn=4 ",
        f"MOUSE_EVENT nonce={nonce} btn=0 ",
    ]
    for hx in expected_pty_frames:
        required.append(f"KEY_FRAME nonce={nonce} hex={hx}")
    required.append(f"PTY_ISOLATION nonce={nonce} leaked_bytes=0")
    required.append(f"USB_HID_INPUT_DONE nonce={nonce} leaked_bytes=0")
    return _ordered_subseq(lines, required)


def shell_echo_evidence(serial_slice: str, nonce: str) -> bool:
    """True iff serial shows ``echo <nonce>`` followed by standalone ``<nonce>`` line and prompt."""
    if not serial_slice or not nonce:
        return False
    stage = 0
    for raw_line in serial_slice.splitlines():
        stripped = raw_line.strip()
        if stage == 0:
            if f"echo {nonce}" in raw_line:
                stage = 1
        elif stage == 1:
            if stripped == nonce:
                stage = 2
        else:
            if _PROMPT_TEXT in raw_line:
                return True
    return False


def shell_ctrl_c_evidence(serial_slice: str, alive_nonce: str) -> bool:
    """True iff ``killed by signal 2`` -> prompt -> ``echo alive_nonce`` -> standalone output -> prompt."""
    if not serial_slice or not alive_nonce:
        return False
    stage = 0
    for raw_line in serial_slice.splitlines():
        stripped = raw_line.strip()
        if stage == 0:
            if "killed by signal 2" in raw_line:
                stage = 1
        elif stage == 1:
            if _PROMPT_TEXT in raw_line:
                stage = 2
                if f"echo {alive_nonce}" in raw_line:
                    stage = 3
        elif stage == 2:
            if f"echo {alive_nonce}" in raw_line:
                stage = 3
        elif stage == 3:
            if stripped == alive_nonce:
                stage = 4
        else:
            if _PROMPT_TEXT in raw_line:
                return True
    return False


def detach_evidence(serial_slice: str, nonce: str) -> bool:
    """Validate held Shift + left mouse -> retire -> synthetic releases -> DONE."""
    if not serial_slice or not nonce:
        return False
    lines = [ln.strip() for ln in serial_slice.splitlines()]
    if not any(
        re.match(rf"^USB_HID_INPUT_READY nonce={re.escape(nonce)} gen=[1-9]\d*$", ln)
        for ln in lines
    ):
        return False

    # Require held make/press before synthetic releases, plus retire logs and DONE.
    has_kbd_retire = any("[xhci-hid] retired " in ln and "kind=kbd" in ln for ln in lines)
    has_mouse_retire = any("[xhci-hid] retired " in ln and "kind=mouse" in ln for ln in lines)
    if not (has_kbd_retire and has_mouse_retire):
        return False

    kbd_ok = _ordered_subseq(
        lines,
        [
            f"KEY_FRAME nonce={nonce} hex=2a",
            f"KEY_FRAME nonce={nonce} hex=aa",
            f"USB_HID_INPUT_DONE nonce={nonce} leaked_bytes=0",
        ],
    )
    mouse_ok = _ordered_subseq(
        lines,
        [
            f"MOUSE_EVENT nonce={nonce} btn=1 ",
            f"MOUSE_EVENT nonce={nonce} btn=0 ",
            f"USB_HID_INPUT_DONE nonce={nonce} leaked_bytes=0",
        ],
    )
    return kbd_ok and mouse_ok


def concurrent_io_evidence(serial_slice: str, io_nonce: str, in_nonce: str) -> bool:
    """True iff IO_BEGIN -> IO_PROGRESS -> guest input receipt -> IO_PROGRESS -> IO_END."""
    if not serial_slice or not io_nonce or not in_nonce:
        return False
    lines = [ln.strip() for ln in serial_slice.splitlines()]
    stage = 0
    for ln in lines:
        if stage == 0:
            if f"IO_BEGIN nonce={io_nonce}" in ln:
                stage = 1
        elif stage == 1:
            if f"IO_PROGRESS nonce={io_nonce} " in ln:
                stage = 2
        elif stage == 2:
            if f"KEY_FRAME nonce={in_nonce} " in ln or f"MOUSE_EVENT nonce={in_nonce} " in ln:
                stage = 3
        elif stage == 3:
            if f"IO_PROGRESS nonce={io_nonce} " in ln:
                stage = 4
        elif stage == 4:
            if f"IO_END nonce={io_nonce} " in ln:
                return True
    return False


def read_trace_since(trace_bytes: bytes, cursor: int) -> tuple[str, int]:
    """Return complete lines from ``trace_bytes[cursor:]`` and the advanced byte cursor."""
    if cursor < 0:
        cursor = 0
    if cursor >= len(trace_bytes):
        return "", cursor
    chunk = trace_bytes[cursor:]
    last_nl = chunk.rfind(b"\n")
    if last_nl < 0:
        return "", cursor
    complete = chunk[: last_nl + 1]
    return complete.decode("utf-8", errors="replace"), cursor + last_nl + 1


def bot_write_sync_trace_evidence(trace_text: str) -> bool:
    """True iff trace proves WRITE(10) -> SYNCHRONIZE CACHE(10) with status=0 CSWs."""
    return ub.persistence_trace_evidence(trace_text)


def _file_sha256(path: Path) -> str | None:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            while chunk := handle.read(1024 * 1024):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def _write_json(path: Path, payload: Any) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def _qmp_key(qmp: QmpClient, qcode: str, down: bool, *, deadline: float) -> None:
    qmp.input_events(
        [{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}],
        deadline=deadline,
    )


def _qmp_key_tap(qmp: QmpClient, qcode: str, *, deadline: float, delay: float = 0.03) -> None:
    _qmp_key(qmp, qcode, True, deadline=deadline)
    time.sleep(delay)
    _qmp_key(qmp, qcode, False, deadline=deadline)
    time.sleep(delay)


def _qmp_mouse_rel(qmp: QmpClient, dx: int, dy: int, *, deadline: float) -> None:
    qmp.input_events(
        [
            {"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}},
        ],
        deadline=deadline,
    )


def _qmp_mouse_btn(qmp: QmpClient, button: str, down: bool, *, deadline: float) -> None:
    qmp.input_events(
        [{"type": "btn", "data": {"down": down, "button": button}}],
        deadline=deadline,
    )


def _qmp_type_text(qmp: QmpClient, text: str, *, deadline: float) -> None:
    for ch in text:
        if "A" <= ch <= "Z":
            qcode = ch.lower()
            _qmp_key(qmp, "shift", True, deadline=deadline)
            time.sleep(0.02)
            _qmp_key_tap(qmp, qcode, deadline=deadline, delay=0.02)
            _qmp_key(qmp, "shift", False, deadline=deadline)
            time.sleep(0.02)
        elif ch in (">", "_"):
            _qmp_key(qmp, "shift", True, deadline=deadline)
            time.sleep(0.02)
            _qmp_key_tap(qmp, "dot" if ch == ">" else "minus", deadline=deadline, delay=0.02)
            _qmp_key(qmp, "shift", False, deadline=deadline)
            time.sleep(0.02)
        else:
            qcode, _ = _CHAR_TO_QCODE_AND_SC[ch]
            _qmp_key_tap(qmp, qcode, deadline=deadline, delay=0.02)


def _wait_for_serial(
    session,
    predicate,
    *,
    deadline: float,
    start_offset: int = 0,
    label: str,
) -> None:
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if predicate(session.text[start_offset:]):
                return
            raise TimeoutError(f"timeout waiting for {label}")
        session.observe(min(POLL_SECONDS, remaining))
        if predicate(session.text[start_offset:]):
            return
        rc = session.returncode
        if rc is not None:
            session.observe(0.05)
            if predicate(session.text[start_offset:]):
                return
            raise RuntimeError(f"QEMU exited early (rc={rc}) while waiting for {label}")


def _run_direct_or_hub(
    args: argparse.Namespace,
    *,
    session_factory,
) -> int:
    case = args.case
    qemu = args.qemu
    firmware = Path(args.firmware)
    disk = Path(args.disk)
    probe_elf = Path(args.probe_elf)
    smp = int(args.smp)
    timeout = float(args.timeout)
    run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)

    source_hash_before = _file_sha256(disk)
    private_disk = run_dir / f"usb-hid-{case}.img"
    shutil.copyfile(disk, private_disk)
    ub.inject_root_probe(private_disk, probe_elf, "usb-hid-probe")

    esp_guid = str(ub.read_gpt_info(private_disk)["esp_guid"])
    qmp_tmpdir = Path(tempfile.mkdtemp(prefix="os01-qmp-"))
    qmp_sock = qmp_tmpdir / "q.sock"
    events_file = run_dir / "trace-events.txt"
    trace_file = run_dir / "trace.log"
    arch = getattr(args, "arch", "x86_64")
    dtb: Path | None = None
    if arch == "aarch64":
        dtb = Path(generate_diagnostic_dtb(qemu, str(qmp_tmpdir), cpus=smp, ram_mib=MEMORY_MIB))

    if case == CASE_HUB:
        events_file.write_text("scsi_disk_new_request\nusb_msd_send_status\n")
        argv = hub_argv(
            qemu,
            firmware,
            private_disk,
            smp,
            arch=arch,
            dtb=dtb,
            qmp_sock=qmp_sock,
            trace_events=events_file,
            trace_file=trace_file,
        )
    else:
        argv = direct_argv(
            qemu,
            firmware,
            private_disk,
            smp,
            arch=arch,
            dtb=dtb,
            qmp_sock=qmp_sock,
        )

    _write_json(run_dir / "argv.json", argv)

    started = time.monotonic()
    deadline = started + timeout
    qmp = QmpClient(str(qmp_sock), timeout=timeout)
    qmp.set_transcript(run_dir / "qmp-transcript.jsonl")
    device_del_log: list[dict[str, Any]] = []
    info_usb_text = ""
    errors: list[str] = []
    session = None

    try:
        session = session_factory(
            argv=argv,
            run_dir=run_dir,
            timeout_s=timeout,
            writable_stdin=True,
        )
        session.start()
        qmp.connect(deadline=deadline)
        _wait_for_serial(
            session,
            lambda s: _PROMPT_TEXT in s,
            deadline=deadline,
            label="initial shell prompt",
        )

        info_usb_text = qmp.execute(
            "human-monitor-command",
            {"command-line": "info usb"},
            deadline=deadline,
        )
        (run_dir / "info-usb.txt").write_text(info_usb_text)
        if not topology_evidence(case, info_usb_text, session.text):
            errors.append(f"topology_evidence failed for case={case}")

        if case == CASE_HUB:
            if esp_guid.lower() not in session.text.lower():
                errors.append(f"ESP GUID {esp_guid} missing in hub boot serial")
            if "usb_msc: usba:" not in session.text:
                errors.append("usba block device missing in hub boot serial")

        # ── Round 1: leased input + PTY isolation + normal unbind ─────────
        nonce1 = f"R1_{uuid.uuid4().hex[:8]}"
        pty_nonce = f"pty{uuid.uuid4().hex[:4]}"
        expected_pty_frames: list[str] = []
        for ch in pty_nonce:
            _, sc = _CHAR_TO_QCODE_AND_SC[ch]
            expected_pty_frames.append(f"{sc:02x}")
            expected_pty_frames.append(f"{sc | 0x80:02x}")
        expected_pty_frames.extend(["1c", "9c"])

        r1_start = len(session.text)
        session.send(f"/bin/usb-hid-probe input {nonce1}\n".encode("ascii"))
        _wait_for_serial(
            session,
            lambda s: f"USB_HID_INPUT_READY nonce={nonce1} " in s,
            deadline=deadline,
            start_offset=r1_start,
            label="round1 READY",
        )

        # E0 Left Arrow make/break
        _qmp_key_tap(qmp, "left", deadline=deadline)
        _wait_for_serial(
            session,
            lambda s: f"KEY_FRAME nonce={nonce1} hex=e0cb" in s,
            deadline=deadline,
            start_offset=r1_start,
            label="round1 left break",
        )

        # Mouse dx=+12, dy=-3
        _qmp_mouse_rel(qmp, 12, -3, deadline=deadline)
        _wait_for_serial(
            session,
            lambda s: f"MOUSE_EVENT nonce={nonce1} btn=0 dx=12 dy=-3 wheel=0" in s,
            deadline=deadline,
            start_offset=r1_start,
            label="round1 mouse rel",
        )

        # Mouse left, right, middle press/release
        for btn, btn_mask in (("left", 1), ("right", 2), ("middle", 4)):
            _qmp_mouse_btn(qmp, btn, True, deadline=deadline)
            _wait_for_serial(
                session,
                lambda s, m=btn_mask: f"MOUSE_EVENT nonce={nonce1} btn={m} " in s,
                deadline=deadline,
                start_offset=r1_start,
                label=f"round1 {btn} down",
            )
            _qmp_mouse_btn(qmp, btn, False, deadline=deadline)
            _wait_for_serial(
                session,
                lambda s: s.count(f"MOUSE_EVENT nonce={nonce1} btn=0 ") >= 2,
                deadline=deadline,
                start_offset=r1_start,
                label=f"round1 {btn} up",
            )

        # Printable pty_nonce + Enter while leased
        _qmp_type_text(qmp, pty_nonce, deadline=deadline)
        _qmp_key_tap(qmp, "ret", deadline=deadline)
        _wait_for_serial(
            session,
            lambda s: f"KEY_FRAME nonce={nonce1} hex=9c" in s,
            deadline=deadline,
            start_offset=r1_start,
            label="round1 pty_nonce enter",
        )

        # End Round 1 cleanly via Esc (tests normal unbind + TTY recovery)
        _qmp_key_tap(qmp, "esc", deadline=deadline)
        _wait_for_serial(
            session,
            lambda s: f"USB_HID_INPUT_DONE nonce={nonce1} " in s and _PROMPT_TEXT in s.split(f"USB_HID_INPUT_DONE nonce={nonce1}", 1)[1],
            deadline=deadline,
            start_offset=r1_start,
            label="round1 DONE + prompt",
        )
        if not round1_input_evidence(session.text[r1_start:], nonce1, expected_pty_frames):
            errors.append("round1_input_evidence failed")

        # ── Case-specific middle phase ────────────────────────────────────
        if case == CASE_DIRECT:
            # Shell echo with Shift + intentional typo + Backspace + Enter
            echo_nonce = f"Hid_{uuid.uuid4().hex[:6]}"
            sh_start = len(session.text)
            _qmp_type_text(qmp, f"echo {echo_nonce}X", deadline=deadline)
            _qmp_key_tap(qmp, "backspace", deadline=deadline)
            _qmp_key_tap(qmp, "ret", deadline=deadline)
            _wait_for_serial(
                session,
                lambda s: shell_echo_evidence(s, echo_nonce),
                deadline=deadline,
                start_offset=sh_start,
                label="direct shell echo",
            )

            # Ctrl-C against cat /dev/urandom >/dev/null + liveness echo
            ctrl_start = len(session.text)
            _qmp_type_text(qmp, "cat /dev/urandom >/dev/null", deadline=deadline)
            _qmp_key_tap(qmp, "ret", deadline=deadline)
            time.sleep(0.5)
            _qmp_key(qmp, "ctrl", True, deadline=deadline)
            time.sleep(0.03)
            _qmp_key_tap(qmp, "c", deadline=deadline)
            _qmp_key(qmp, "ctrl", False, deadline=deadline)
            _wait_for_serial(
                session,
                lambda s: "killed by signal 2" in s and _PROMPT_TEXT in s.split("killed by signal 2", 1)[1],
                deadline=deadline,
                start_offset=ctrl_start,
                label="direct Ctrl-C kill",
            )
            alive_nonce = f"ALIVE_{uuid.uuid4().hex[:6]}"
            _qmp_type_text(qmp, f"echo {alive_nonce}", deadline=deadline)
            _qmp_key_tap(qmp, "ret", deadline=deadline)
            _wait_for_serial(
                session,
                lambda s: shell_ctrl_c_evidence(s, alive_nonce),
                deadline=deadline,
                start_offset=ctrl_start,
                label="direct shell alive after Ctrl-C",
            )

        else:  # CASE_HUB
            trace_cursor = trace_file.stat().st_size if trace_file.exists() else 0
            io_nonce = f"IO_{uuid.uuid4().hex[:6]}"
            in_nonce = f"IN_{uuid.uuid4().hex[:6]}"
            conc_start = len(session.text)
            session.send(f"/bin/usb-hid-probe io {io_nonce} 15 &\n".encode("ascii"))
            _wait_for_serial(
                session,
                lambda s: f"IO_PROGRESS nonce={io_nonce} " in s,
                deadline=deadline,
                start_offset=conc_start,
                label="hub concurrent IO_PROGRESS start",
            )
            session.send(f"/bin/usb-hid-probe input {in_nonce}\n".encode("ascii"))
            _wait_for_serial(
                session,
                lambda s: f"USB_HID_INPUT_READY nonce={in_nonce} " in s,
                deadline=deadline,
                start_offset=conc_start,
                label="hub concurrent input READY",
            )
            _qmp_key_tap(qmp, "left", deadline=deadline)
            _qmp_mouse_rel(qmp, 5, -2, deadline=deadline)
            _wait_for_serial(
                session,
                lambda s: f"KEY_FRAME nonce={in_nonce} hex=e0cb" in s,
                deadline=deadline,
                start_offset=conc_start,
                label="hub concurrent input receipt",
            )
            _qmp_key_tap(qmp, "esc", deadline=deadline)
            _wait_for_serial(
                session,
                lambda s: f"IO_END nonce={io_nonce} " in s and f"USB_HID_INPUT_DONE nonce={in_nonce} " in s,
                deadline=deadline,
                start_offset=conc_start,
                label="hub concurrent IO_END",
            )
            if not concurrent_io_evidence(session.text[conc_start:], io_nonce, in_nonce):
                errors.append("concurrent_io_evidence failed")

            # Wait briefly for trace flush and verify BOT WRITE(10)->SYNC(10) from trace_cursor
            conc_trace = ""
            while time.monotonic() < deadline:
                if trace_file.exists():
                    conc_trace, next_cursor = read_trace_since(
                        trace_file.read_bytes(), trace_cursor
                    )
                    if bot_write_sync_trace_evidence(conc_trace):
                        trace_cursor = next_cursor
                        break
                time.sleep(POLL_SECONDS)
            else:
                errors.append("hub concurrent BOT WRITE(10)->SYNCHRONIZE CACHE(10) trace missing")

        # ── Round 2: held Shift + left mouse -> device_del -> synthetic release ──
        nonce2 = f"R2_{uuid.uuid4().hex[:8]}"
        r2_start = len(session.text)
        session.send(f"/bin/usb-hid-probe input {nonce2}\n".encode("ascii"))
        _wait_for_serial(
            session,
            lambda s: f"USB_HID_INPUT_READY nonce={nonce2} " in s,
            deadline=deadline,
            start_offset=r2_start,
            label="round2 READY",
        )
        _qmp_key(qmp, "shift", True, deadline=deadline)
        _qmp_mouse_btn(qmp, "left", True, deadline=deadline)
        _wait_for_serial(
            session,
            lambda s: f"KEY_FRAME nonce={nonce2} hex=2a" in s and f"MOUSE_EVENT nonce={nonce2} btn=1 " in s,
            deadline=deadline,
            start_offset=r2_start,
            label="round2 held Shift + left mouse",
        )

        del_kbd_ret = qmp.execute("device_del", {"id": "kbd"}, deadline=deadline)
        ev_kbd = qmp.wait_event(
            "DEVICE_DELETED",
            lambda e: e.get("data", {}).get("device") == "kbd",
            deadline=deadline,
        )
        device_del_log.append({"id": "kbd", "return": del_kbd_ret, "event": ev_kbd})
        _wait_for_serial(
            session,
            lambda s: f"KEY_FRAME nonce={nonce2} hex=aa" in s,
            deadline=deadline,
            start_offset=r2_start,
            label="round2 synthetic Shift release",
        )

        del_mouse_ret = qmp.execute("device_del", {"id": "mouse"}, deadline=deadline)
        ev_mouse = qmp.wait_event(
            "DEVICE_DELETED",
            lambda e: e.get("data", {}).get("device") == "mouse",
            deadline=deadline,
        )
        device_del_log.append({"id": "mouse", "return": del_mouse_ret, "event": ev_mouse})
        _wait_for_serial(
            session,
            lambda s: f"MOUSE_EVENT nonce={nonce2} btn=0 " in s,
            deadline=deadline,
            start_offset=r2_start,
            label="round2 synthetic mouse release",
        )

        # Exit Round 2 probe cleanly via serial Ctrl-C
        session.send(b"\x03")
        _wait_for_serial(
            session,
            lambda s: f"USB_HID_INPUT_DONE nonce={nonce2} " in s and _PROMPT_TEXT in s.split(f"USB_HID_INPUT_DONE nonce={nonce2}", 1)[1],
            deadline=deadline,
            start_offset=r2_start,
            label="round2 DONE + prompt",
        )
        if not detach_evidence(session.text[r2_start:], nonce2):
            errors.append("detach_evidence failed")

        # ── Hub post-detach storage check ─────────────────────────────────
        if case == CASE_HUB:
            post_nonce = f"POST_{uuid.uuid4().hex[:6]}"
            post_start = len(session.text)
            session.send(f"/bin/usb-hid-probe io {post_nonce} 1\n".encode("ascii"))
            _wait_for_serial(
                session,
                lambda s: f"IO_END nonce={post_nonce} " in s and _PROMPT_TEXT in s.split(f"IO_END nonce={post_nonce}", 1)[1],
                deadline=deadline,
                start_offset=post_start,
                label="hub post-detach IO_END",
            )
            post_trace = ""
            while time.monotonic() < deadline:
                if trace_file.exists():
                    post_trace, next_cursor = read_trace_since(
                        trace_file.read_bytes(), trace_cursor
                    )
                    if bot_write_sync_trace_evidence(post_trace):
                        trace_cursor = next_cursor
                        break
                time.sleep(POLL_SECONDS)
            else:
                errors.append("hub post-detach BOT WRITE(10)->SYNCHRONIZE CACHE(10) trace missing")

            post_info_usb = qmp.execute(
                "human-monitor-command",
                {"command-line": "info usb"},
                deadline=deadline,
            )
            if not re.search(r"Port 1\.2,\s*Speed (?:480|12) Mb/s,", post_info_usb):
                errors.append("hub storage missing in post-detach info usb")

    except Exception as exc:  # noqa: BLE001
        errors.append(str(exc))
    finally:
        qmp.close()
        if session is not None:
            try:
                session.stop()
            except Exception:  # noqa: BLE001
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001
                pass
        shutil.rmtree(qmp_tmpdir, ignore_errors=True)

    source_hash_after = _file_sha256(disk)
    if source_hash_before != source_hash_after:
        errors.append("source disk image sha256 changed")

    serial_text = session.text if session is not None else ""
    (run_dir / "serial.log").write_text(serial_text)
    _write_json(run_dir / "device-del.json", device_del_log)
    verdict = "PASS" if not errors else "FAIL"
    _write_json(
        run_dir / "verdict.json",
        {
            "case": case,
            "smp": smp,
            "verdict": verdict,
            "errors": errors,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "firmware": str(firmware),
            "firmware_sha256": _file_sha256(firmware),
            "disk": str(disk),
            "disk_sha256_before": source_hash_before,
            "disk_sha256_after": source_hash_after,
            "probe_elf": str(probe_elf),
            "probe_elf_sha256": _file_sha256(probe_elf),
            "esp_guid": esp_guid,
            "info_usb": info_usb_text,
        },
    )
    print(f"[usb-hid] case={case} smp={smp} result={verdict} errors={len(errors)} log_dir={run_dir}")
    for err in errors:
        print(f"[usb-hid]   - {err}", file=sys.stderr)
    return 0 if not errors else 1


def run_case(args: argparse.Namespace, *, session_factory=None) -> int:
    if args.case not in CASES:
        print(f"[usb-hid] ERROR: unknown case {args.case!r}", file=sys.stderr)
        return 2
    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("[usb-hid] ERROR: ProcessSession unavailable", file=sys.stderr)
        return 2
    for label, raw in (("firmware", args.firmware), ("disk", args.disk), ("probe-elf", args.probe_elf)):
        if not raw or not Path(raw).is_file():
            print(f"[usb-hid] ERROR: {label} does not exist: {raw}", file=sys.stderr)
            return 2
    try:
        smp = int(args.smp)
        timeout = float(args.timeout)
    except (TypeError, ValueError):
        print("[usb-hid] ERROR: invalid smp or timeout", file=sys.stderr)
        return 2
    if smp <= 0 or timeout <= 0:
        print("[usb-hid] ERROR: smp and timeout must be positive", file=sys.stderr)
        return 2
    return _run_direct_or_hub(args, session_factory=session_factory)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="USB-only HID input & Hub storage E2E runner")
    parser.add_argument("--case", required=True, choices=CASES)
    parser.add_argument("--arch", default="x86_64", choices=("x86_64", "aarch64"))
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--probe-elf", required=True)
    parser.add_argument("--smp", required=True, type=int)
    parser.add_argument("--timeout", default=DEFAULT_TIMEOUT, type=float)
    parser.add_argument("--log-dir", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return run_case(args)


if __name__ == "__main__":
    sys.exit(main())
