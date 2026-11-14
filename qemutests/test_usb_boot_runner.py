#!/usr/bin/env python3
"""Contract tests for the USB boot M0 firmware-gate runner.

The gate proves one narrow thing: the profile OVMF loaded BOOTX64.EFI and
kernel.bin from a USB-only qemu-xhci + usb-storage topology and the kernel
reached device initialisation.  The stock kernel has no USB MSC driver, so
it stops at ``fs_boot_mounts`` with ``rc=-19``; that FATAL line is the
expected pass evidence, not a failure.

Run with::

    python3 -m unittest qemutests.test_usb_boot_runner
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

TEST_MANIFEST = {
    "id": "test_usb_boot_runner",
    "layer": "harness",
    "subsystem": "usb",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests import usb_boot as ub


# ── Serial fragments as the guest actually prints them ──────────────
# The xHCI lines come from kernel/arch/x86_64/driver/xhci.c; the FATAL
# line comes from kernel/core/boot_failure.c and ALWAYS continues with
# phase=... cpu=... — the matcher must therefore use the stable prefix.
SPEED = "[xhci] port 1: connected, reset complete, speed=3\n"
CONFIG = (
    "[xhci] slot 1: configured with config 1 "
    "(iface class 08:06, ep_count=2)\n"
)
FATAL_PREFIX = "[boot-3] FATAL stage=fs component=root rc=-19"
FATAL = FATAL_PREFIX + "\n"
FATAL_REAL = FATAL_PREFIX + " phase=-1 cpu=-1\n"


class FirmwareEvidenceTests(unittest.TestCase):
    def test_positive_in_order(self) -> None:
        self.assertTrue(ub.firmware_evidence(SPEED + CONFIG + FATAL))

    def test_positive_with_real_trailing_fields(self) -> None:
        # boot_failure_stop() appends phase=... cpu=...; a whole-line-only
        # comparison would reject this real line.
        self.assertNotEqual(FATAL_REAL.strip(), FATAL.strip())
        self.assertTrue(ub.firmware_evidence(SPEED + CONFIG + FATAL_REAL))
        self.assertTrue(
            ub.firmware_evidence(SPEED + CONFIG + FATAL_PREFIX + " phase=3 cpu=2\n")
        )

    def test_ovmf_only_text_is_not_evidence(self) -> None:
        ovmf = (
            "BdsDxe: loading Boot0001 \"UEFI QEMU HARDDISK\"\n"
            "BdsDxe: starting Boot0001 \"UEFI QEMU HARDDISK\"\n"
            "UEFI Interactive Shell v2.2\n"
        )
        self.assertFalse(ub.firmware_evidence(ovmf))

    def test_fatal_rc_other_than_minus_19_is_not_evidence(self) -> None:
        for rc in ("-2", "-5", "19", "-190", "-19x"):
            line = f"[boot-3] FATAL stage=fs component=root rc={rc} phase=-1 cpu=-1\n"
            with self.subTest(rc=rc):
                self.assertFalse(ub.firmware_evidence(SPEED + CONFIG + line))

    def test_fatal_stage_or_component_mismatch_is_not_evidence(self) -> None:
        cases = [
            "[boot-3] FATAL stage=fs component=boot rc=-19 phase=-1 cpu=-1\n",
            "[boot-3] FATAL stage=tty component=root rc=-19 phase=-1 cpu=-1\n",
        ]
        for line in cases:
            with self.subTest(line=line):
                self.assertFalse(ub.firmware_evidence(SPEED + CONFIG + line))

    def test_markers_out_of_order_are_rejected(self) -> None:
        self.assertFalse(ub.firmware_evidence(CONFIG + SPEED + FATAL))
        self.assertFalse(ub.firmware_evidence(SPEED + FATAL + CONFIG))
        self.assertFalse(ub.firmware_evidence(FATAL + SPEED + CONFIG))

    def test_speed_must_be_exactly_3(self) -> None:
        self.assertFalse(
            ub.firmware_evidence(
                "[xhci] port 1: connected, reset complete, speed=30\n"
                + CONFIG
                + FATAL
            )
        )

    def test_empty_stream_is_not_evidence(self) -> None:
        self.assertFalse(ub.firmware_evidence(""))


class UsbArgvTests(unittest.TestCase):
    def setUp(self) -> None:
        self.firmware = Path("/tmp/profile/firmware/OVMF.fd")
        self.disk = Path("/tmp/profile/image/disk.img")
        self.argv = ub.usb_argv(
            "qemu-system-x86_64", self.firmware, self.disk, 3
        )

    def _option_value(self, option: str) -> str:
        index = self.argv.index(option)
        return self.argv[index + 1]

    def _drives(self) -> list[str]:
        return [
            self.argv[i + 1]
            for i, arg in enumerate(self.argv)
            if arg == "-drive" and i + 1 < len(self.argv)
        ]

    def test_uses_supplied_qemu_machine_memory_and_smp(self) -> None:
        self.assertEqual(self.argv[0], "qemu-system-x86_64")
        self.assertEqual(self._option_value("-M"), "q35")
        self.assertEqual(self._option_value("-m"), "512")
        self.assertEqual(self._option_value("-smp"), "3")

    def test_usb_xhci_storage_topology(self) -> None:
        joined = " ".join(self.argv)
        self.assertIn("qemu-xhci,id=xhci,p2=4,p3=0", joined)
        self.assertIn(
            "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0", joined
        )

    def test_serial_is_stdio_and_headless(self) -> None:
        self.assertEqual(self._option_value("-serial"), "stdio")
        self.assertEqual(self._option_value("-display"), "none")
        self.assertIn("-no-reboot", self.argv)
        self.assertIn("-no-shutdown", self.argv)

    def test_pflash_is_read_only_and_names_the_firmware(self) -> None:
        pflash = [d for d in self._drives() if d.startswith("if=pflash")]
        self.assertEqual(len(pflash), 1)
        self.assertIn("readonly=on", pflash[0])
        self.assertIn(f"file={self.firmware}", pflash[0])

    def test_exactly_one_non_pflash_drive(self) -> None:
        non_pflash = [d for d in self._drives() if not d.startswith("if=pflash")]
        self.assertEqual(len(non_pflash), 1)
        usb = non_pflash[0]
        self.assertIn(f"file={self.disk}", usb)
        self.assertIn("format=raw", usb)
        self.assertIn("if=none", usb)
        self.assertIn("id=usbdisk", usb)
        self.assertIn("readonly=on", usb)

    def test_rng_is_wired_to_host_urandom(self) -> None:
        joined = " ".join(self.argv)
        self.assertIn(
            "rng-random,filename=/dev/urandom,id=rng0", joined
        )
        self.assertIn("virtio-rng-pci,rng=rng0", joined)

    def test_no_ahci_or_other_boot_disk_leaks_in(self) -> None:
        joined = " ".join(self.argv)
        for banned in ("ahci", "ide-hd", "virtio-blk", "if=ide", "-boot"):
            with self.subTest(banned=banned):
                self.assertNotIn(banned, joined)


class FakeSession:
    """ProcessSession stand-in for runner contract tests."""

    def __init__(
        self,
        stream: str = "",
        *,
        timed_out: bool = False,
        early_exit: int | None = None,
    ) -> None:
        self._stream = stream
        self._timed_out = timed_out
        self._early_exit = early_exit
        self.argv: list[str] = []
        self.run_dir: Path | None = None
        self.timeout_s = 0.0
        self.text = ""
        self.returncode: int | None = None
        self.stopped_by_runner = False
        self.timed_out = False
        self.deadline = float("inf")
        self.observe_log: list[float] = []
        self.stopped = False
        self.closed = False
        self.started = False
        self._cursor = 0

    def start(self) -> None:
        self.started = True
        self._cursor = 0
        assert self.run_dir is not None
        run_dir = self.run_dir
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "stdout.log").write_text(self._stream)
        (run_dir / "stderr.log").write_text("")

    def wait_for(self, predicate) -> str:
        self.text = self._stream
        if self._timed_out:
            self.timed_out = True
            return ""
        if predicate(self.text[self._cursor :]):
            self._cursor = len(self.text)
            return self.text
        if self._early_exit is not None:
            self.returncode = self._early_exit
            self._cursor = len(self.text)
            return ""
        self.timed_out = True
        return ""

    def observe(self, seconds: float) -> str:
        self.observe_log.append(seconds)
        return ""

    def stop(self) -> int | None:
        self.stopped = True
        if self.returncode is None:
            self.stopped_by_runner = True
            self.returncode = -15
        return self.returncode

    def close(self) -> None:
        self.closed = True


def _write_runner_args(
    tmp: Path,
    *,
    firmware_exists: bool = True,
    disk_exists: bool = True,
    case: str = "firmware",
    timeout: float = 5.0,
) -> argparse.Namespace:
    firmware = tmp / "firmware" / "OVMF.fd"
    disk = tmp / "image" / "disk.img"
    firmware.parent.mkdir(parents=True, exist_ok=True)
    disk.parent.mkdir(parents=True, exist_ok=True)
    if firmware_exists:
        firmware.write_bytes(b"OVMF")
    if disk_exists:
        disk.write_bytes(b"disk")
    return argparse.Namespace(
        case=case,
        qemu="qemu-system-x86_64",
        firmware=str(firmware),
        disk=str(disk),
        smp=2,
        log_dir=str(tmp / "run"),
        timeout=timeout,
    )


class RunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="usb-boot-runner-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _run(
        self,
        stream: str,
        *,
        timed_out: bool = False,
        early_exit: int | None = None,
        **kwargs,
    ):
        args = _write_runner_args(self.tmp, **kwargs)
        session = FakeSession(
            stream, timed_out=timed_out, early_exit=early_exit
        )
        calls: list[dict] = []

        def factory(**call_kwargs):
            calls.append(call_kwargs)
            session.argv = call_kwargs["argv"]
            session.run_dir = Path(call_kwargs["run_dir"])
            session.timeout_s = call_kwargs["timeout_s"]
            return session

        rc = ub.run_case(args, session_factory=factory)
        return rc, session, args, calls

    def test_m0_evidence_passes_and_retains_artifacts(self) -> None:
        stream = SPEED + CONFIG + FATAL_REAL
        rc, session, args, calls = self._run(stream)
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 1)
        self.assertTrue(session.stopped)
        self.assertTrue(session.closed)
        self.assertEqual(session.observe_log, [ub.OBSERVE_SECONDS])
        run_dir = Path(args.log_dir)
        self.assertEqual((run_dir / "serial.log").read_text(), stream)
        self.assertEqual(
            json.loads((run_dir / "argv.json").read_text()), session.argv
        )
        meta = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(meta["result"], "PASS")
        self.assertEqual(meta["case"], "firmware")
        self.assertEqual(meta["errors"], [])

    def test_timeout_with_noisy_child_fails_without_extending_deadline(self) -> None:
        noise = "".join(f"spam line {i}\n" for i in range(20000))
        rc, session, args, _ = self._run(noise, timed_out=True)
        self.assertEqual(rc, 1)
        # The observation window must not run after a timeout.
        self.assertEqual(session.observe_log, [])
        self.assertEqual(session.timeout_s, 5.0)
        run_dir = Path(args.log_dir)
        self.assertEqual((run_dir / "serial.log").read_text(), noise)
        meta = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(meta["result"], "FAIL")
        self.assertTrue(
            any("timeout" in err.lower() or "deadline" in err.lower()
                for err in meta["errors"]),
            meta["errors"],
        )

    def test_early_exit_without_evidence_fails(self) -> None:
        rc, session, args, _ = self._run(SPEED, early_exit=0)
        self.assertEqual(rc, 1)
        self.assertTrue(session.stopped)
        self.assertEqual(session.returncode, 0)
        meta = json.loads((Path(args.log_dir) / "metadata.json").read_text())
        self.assertEqual(meta["result"], "FAIL")
        self.assertTrue(
            any("exit" in err.lower() for err in meta["errors"]),
            meta["errors"],
        )

    def test_serial_is_retained_on_failure_and_child_is_reaped(self) -> None:
        stream = "kernel started but no USB evidence\n"
        rc, session, args, _ = self._run(stream, early_exit=1)
        self.assertEqual(rc, 1)
        self.assertTrue(session.stopped)
        self.assertIsNotNone(session.returncode)
        self.assertEqual(
            (Path(args.log_dir) / "serial.log").read_text(), stream
        )

    def test_absent_firmware_fails_before_spawn(self) -> None:
        args = _write_runner_args(self.tmp, firmware_exists=False)
        calls: list[dict] = []
        rc = ub.run_case(args, session_factory=lambda **kw: calls.append(kw))
        self.assertEqual(rc, 2)
        self.assertEqual(calls, [])

    def test_absent_disk_fails_before_spawn(self) -> None:
        args = _write_runner_args(self.tmp, disk_exists=False)
        calls: list[dict] = []
        rc = ub.run_case(args, session_factory=lambda **kw: calls.append(kw))
        self.assertEqual(rc, 2)
        self.assertEqual(calls, [])

    def test_unknown_case_fails_before_spawn(self) -> None:
        args = _write_runner_args(self.tmp, case="bogus")
        calls: list[dict] = []
        rc = ub.run_case(args, session_factory=lambda **kw: calls.append(kw))
        self.assertEqual(rc, 2)
        self.assertEqual(calls, [])


class CliTests(unittest.TestCase):
    def test_parse_args_accepts_the_documented_cli(self) -> None:
        args = ub.parse_args(
            [
                "--case", "firmware",
                "--qemu", "qemu-system-x86_64",
                "--firmware", "/fw/OVMF.fd",
                "--disk", "/img/disk.img",
                "--smp", "2",
                "--log-dir", "/logs/run",
                "--timeout", "120",
            ]
        )
        self.assertEqual(args.case, "firmware")
        self.assertEqual(args.smp, 2)
        self.assertEqual(args.timeout, 120.0)

    def test_parse_args_default_timeout_is_120(self) -> None:
        args = ub.parse_args(
            [
                "--qemu", "qemu-system-x86_64",
                "--firmware", "/fw/OVMF.fd",
                "--disk", "/img/disk.img",
                "--smp", "1",
                "--log-dir", "/logs/run",
            ]
        )
        self.assertEqual(args.case, "firmware")
        self.assertEqual(args.timeout, 120.0)


if __name__ == "__main__":
    unittest.main()
