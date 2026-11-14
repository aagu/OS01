#!/usr/bin/env python3
"""USB boot M0 firmware-gate runner.

Purpose (design spec §5.9, plan M0): prove that the profile OVMF loaded
``BOOTX64.EFI`` and ``kernel.bin`` from a **USB-only** qemu-xhci +
usb-storage topology, i.e. that the guest reached kernel device
initialisation.  The stock kernel has no USB MSC driver yet, so it stops
at ``fs_boot_mounts`` with ``rc=-19`` (-ENODEV) — that FATAL line is the
expected pass evidence for M0, not a failure.

This runner deliberately does not reuse ``qemutests/run_test.py``'s argv
builder: that builder unconditionally adds an AHCI root disk, which would
make the USB boot unprovable.  ``usb_argv()`` is the whole topology.

CLI (the Make entry point is ``test-qemu SUITE=usb-boot``)::

    python3 qemutests/test_usb_boot.py --case firmware \
        --qemu PATH --firmware FILE --disk FILE \
        --smp N --log-dir DIR [--timeout 120]

Exit codes: 0 = PASS, 1 = FAIL (evidence missing / timeout / early exit),
2 = ERROR (bad configuration, missing input, spawn failure).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path
from typing import Any

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover - exercised only on broken installs
    ProcessSession = None


CASE_FIRMWARE = "firmware"
CASES = (CASE_FIRMWARE,)
DEFAULT_TIMEOUT = 120.0
OBSERVE_SECONDS = 1.0
MEMORY_MIB = 512

# ── M0 evidence ─────────────────────────────────────────────────────
# Markers must appear in this order:
#   1. xHCI port connected at speed=3 (high speed; xHCI speed ID 3)
#   2. the mass-storage interface configured (class 08:06, two bulk EPs)
#   3. fs_boot_mounts failing with -ENODEV (the stock kernel has no USB
#      MSC block driver, so there is no BLOCK_DISK to mount).
_XHCI_SPEED_RE = re.compile(
    r"\[xhci\] port \d+: connected, reset complete, speed=3(?:\s|$)"
)
_XHCI_CONFIG_RE = re.compile(
    r"\[xhci\] slot \d+: configured with config \d+ "
    r"\(iface class 08:06, ep_count=2\)"
)
# The real line from boot_failure_stop() continues with
# " phase=... cpu=...".  Match the stable prefix, never the whole line.
_FATAL_PREFIX_RE = re.compile(
    r"^\[boot-3\] FATAL stage=fs component=root rc=-19(?=\s|$)"
)


def firmware_evidence(serial: str) -> bool:
    """True iff the serial stream contains the M0 firmware evidence.

    The three markers must occur in order (an OVMF banner alone, a FATAL
    with a different rc, or markers out of order are not evidence).
    """
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


def usb_argv(
    qemu: str, firmware: Path, disk: Path, smp: int
) -> list[str]:
    """Build the USB-only QEMU command line.

    Exactly one non-pflash drive (the usb-storage backed image) is
    present: no AHCI, no implicit IDE/virtio disk and no ``-boot order``.
    """
    return [
        str(qemu),
        "-M", "q35",
        "-m", str(MEMORY_MIB),
        "-smp", str(smp),
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"file={disk},format=raw,if=none,id=usbdisk,readonly=on",
        "-device", "qemu-xhci,id=xhci,p2=4,p3=0",
        "-device", "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-no-shutdown",
    ]


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


def run_case(args: argparse.Namespace, *, session_factory=None) -> int:
    """Run one case and judge it.

    Returns 0 (PASS), 1 (FAIL) or 2 (configuration / environment ERROR).
    """
    started = time.monotonic()
    case = getattr(args, "case", None)
    if case not in CASES:
        print(f"[usb-boot] ERROR: unknown case {case!r}")
        return 2
    smp = getattr(args, "smp", 0)
    if not isinstance(smp, int) or smp <= 0:
        print(f"[usb-boot] ERROR: invalid smp {smp!r}")
        return 2
    timeout = getattr(args, "timeout", DEFAULT_TIMEOUT)
    if timeout is None or timeout <= 0:
        print(f"[usb-boot] ERROR: invalid timeout {timeout!r}")
        return 2

    firmware = Path(args.firmware)
    disk = Path(args.disk)
    for label, path in (("firmware", firmware), ("disk", disk)):
        if not path.is_file():
            print(f"[usb-boot] ERROR: {label} input does not exist: {path}")
            return 2

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("[usb-boot] ERROR: ProcessSession is unavailable")
        return 2

    run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    argv = usb_argv(args.qemu, firmware, disk, smp)
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

    session = None
    errors: list[str] = []
    spawn_error: Exception | None = None
    try:
        session = session_factory(
            argv=argv, run_dir=run_dir, timeout_s=timeout
        )
        session.start()
        # Absolute monotonic deadline owned by the session: every wait
        # below consumes it; nothing resets or extends it.
        deadline = session.deadline

        session.wait_for(firmware_evidence)

        if session.timed_out:
            errors.append("timeout before M0 firmware evidence")
        elif firmware_evidence(session.text):
            if deadline - time.monotonic() < OBSERVE_SECONDS:
                errors.append("timeout: insufficient observation budget")
            else:
                session.observe(OBSERVE_SECONDS)
                if deadline - time.monotonic() < 0:
                    errors.append("overall deadline expired")
        else:
            returncode = session.returncode
            if returncode is not None:
                errors.append(
                    "QEMU exited early "
                    f"(rc={returncode}) without M0 firmware evidence"
                )
            else:
                errors.append("M0 firmware evidence missing")
    except Exception as exc:  # noqa: BLE001 - surface spawn failures as ERROR
        spawn_error = exc
    finally:
        if session is not None:
            # ProcessSession.stop() owns the bounded teardown:
            # SIGTERM the whole process group, wait <=5 s, then SIGKILL.
            try:
                session.stop()
            except Exception:  # noqa: BLE001 - never mask the real error
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001 - finalizer must not raise
                pass

    serial = session.text if session is not None else ""
    (run_dir / "serial.log").write_text(serial)

    if spawn_error is not None:
        print(f"[usb-boot] spawn error: {spawn_error}")
        metadata.update(
            {
                "elapsed_seconds": time.monotonic() - started,
                "result": "ERROR",
                "errors": [f"spawn error: {spawn_error}"],
            }
        )
        _write_json(run_dir / "metadata.json", metadata)
        return 2

    timed_out = bool(session is not None and session.timed_out)
    passed = (
        not errors
        and not timed_out
        and firmware_evidence(serial)
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
        description="USB boot M0 firmware-gate runner"
    )
    parser.add_argument(
        "--case", choices=CASES, default=CASE_FIRMWARE,
        help="gate case (default: firmware; later plans add success cases)",
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
