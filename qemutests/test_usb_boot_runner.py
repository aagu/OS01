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
import re
import shutil
import sys
import tempfile
import time
import unittest
from unittest import mock

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
    """ProcessSession stand-in that mirrors the real EOF/deadline contract.

    ``wait_for`` on EOF without a predicate match does NOT return early in
    the real session — it keeps polling until the absolute deadline and
    then sets ``timed_out``.  ``observe`` is the API that returns as soon
    as an exited child has been reaped, exposing ``returncode``.
    """

    def __init__(
        self,
        stream: str = "",
        *,
        exited_after: int | None = None,
    ) -> None:
        self._stream = stream
        self._exited_after = exited_after
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
        self.text = ""
        assert self.run_dir is not None
        run_dir = self.run_dir
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "stdout.log").write_text(self._stream)
        (run_dir / "stderr.log").write_text("")
        self.deadline = time.monotonic() + self.timeout_s

    def wait_for(self, predicate) -> str:
        self.text = self._stream
        if predicate(self.text[self._cursor :]):
            self._cursor = len(self.text)
            return self.text
        # Real ProcessSession.wait_for keeps polling until the deadline
        # when EOF arrives without a match.  The fake cannot sleep, so it
        # models the end state directly.
        self.timed_out = True
        return ""

    def observe(self, seconds: float) -> str:
        self.observe_log.append(seconds)
        self.text = self._stream
        # The real observe() returns immediately and reaps the child once
        # it has exited, making returncode available.
        if self._exited_after is not None and self.returncode is None:
            self.returncode = self._exited_after
        return ""

    def send(self, data: bytes) -> None:
        pass

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
        exited_after: int | None = None,
        **kwargs,
    ):
        args = _write_runner_args(self.tmp, **kwargs)
        session = FakeSession(stream, exited_after=exited_after)
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
        # Poll slices precede the single full observation window.
        self.assertEqual(session.observe_log[-1], ub.OBSERVE_SECONDS)
        self.assertTrue(
            all(s <= ub.POLL_SECONDS for s in session.observe_log[:-1])
        )
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
        started = time.monotonic()
        rc, session, args, _ = self._run(noise, timeout=0.05)
        elapsed = time.monotonic() - started
        self.assertEqual(rc, 1)
        # The absolute deadline is authoritative: 20k noisy lines cannot
        # extend it, and the full observation window must not run.
        self.assertLess(elapsed, 1.0)
        self.assertNotIn(ub.OBSERVE_SECONDS, session.observe_log)
        self.assertEqual(session.timeout_s, 0.05)
        run_dir = Path(args.log_dir)
        self.assertEqual((run_dir / "serial.log").read_text(), noise)
        meta = json.loads((run_dir / "metadata.json").read_text())
        self.assertEqual(meta["result"], "FAIL")
        self.assertTrue(meta["timed_out"])
        self.assertTrue(
            any("timeout" in err.lower() or "deadline" in err.lower()
                for err in meta["errors"]),
            meta["errors"],
        )

    def test_early_exit_without_evidence_fails_before_deadline(self) -> None:
        started = time.monotonic()
        rc, session, args, _ = self._run(SPEED, exited_after=0, timeout=5.0)
        elapsed = time.monotonic() - started
        self.assertEqual(rc, 1)
        # Production behavior: observe() reaps the exited child and
        # exposes returncode, so the diagnosis happens long before the
        # 5 s deadline (the old wait_for path burned the whole budget).
        self.assertLess(elapsed, 2.0)
        self.assertTrue(session.stopped)
        self.assertEqual(session.returncode, 0)
        self.assertFalse(session.timed_out)
        meta = json.loads((Path(args.log_dir) / "metadata.json").read_text())
        self.assertEqual(meta["result"], "FAIL")
        self.assertFalse(meta["timed_out"])
        self.assertTrue(
            any("exited early" in err.lower() for err in meta["errors"]),
            meta["errors"],
        )

    def test_real_process_session_detects_early_exit_before_deadline(self) -> None:
        """Exercise the real ProcessSession contract, not a fake.

        The runner's own argv is patched to an immediately-exiting child;
        the default session factory (the real ProcessSession) must reap it
        and produce the early-exit diagnosis well before the 5 s budget.
        """
        args = _write_runner_args(self.tmp, timeout=5.0)
        started = time.monotonic()
        with mock.patch.object(
            ub, "usb_argv", return_value=[sys.executable, "-c", "pass"]
        ):
            rc = ub.run_case(args)
        elapsed = time.monotonic() - started
        self.assertEqual(rc, 1)
        self.assertLess(elapsed, 2.0)
        meta = json.loads((Path(args.log_dir) / "metadata.json").read_text())
        self.assertEqual(meta["result"], "FAIL")
        self.assertFalse(meta["timed_out"])
        self.assertTrue(
            any("exited early" in err.lower() for err in meta["errors"]),
            meta["errors"],
        )
        self.assertEqual(
            (Path(args.log_dir) / "serial.log").read_text(), ""
        )

    def test_serial_is_retained_on_failure_and_child_is_reaped(self) -> None:
        stream = "kernel started but no USB evidence\n"
        rc, session, args, _ = self._run(stream, exited_after=1)
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

    def _assert_pre_spawn_error(self, mutate) -> None:
        args = _write_runner_args(self.tmp)
        mutate(args)
        calls: list[dict] = []
        rc = ub.run_case(args, session_factory=lambda **kw: calls.append(kw))
        self.assertEqual(rc, 2)
        self.assertEqual(calls, [])

    def test_bad_smp_fails_before_spawn(self) -> None:
        for value in (0, -1, "two", None):
            with self.subTest(smp=value):
                self._assert_pre_spawn_error(
                    lambda args, v=value: setattr(args, "smp", v)
                )

    def test_bad_timeout_fails_before_spawn(self) -> None:
        for value in (0, -1.0, "soon", None, float("inf"), float("nan")):
            with self.subTest(timeout=value):
                self._assert_pre_spawn_error(
                    lambda args, v=value: setattr(args, "timeout", v)
                )


class MakeTopologyDriftTests(unittest.TestCase):
    """`run-usb` (Make) and the gate runner must not drift apart."""

    def test_qemu_usb_disk_matches_shared_builder(self) -> None:
        text = (_ROOT / "mk/components/run.mk").read_text()
        match = re.search(
            r"^QEMU_USB_DISK = (.*?)(?:\n\n|\n\.PHONY)",
            text,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(match, "QEMU_USB_DISK assignment not found")
        make_tokens = set(match.group(1).replace("\\\n", " ").split())

        argv = ub.usb_argv(
            "qemu-system-x86_64", Path("/fw/OVMF.fd"), Path("/img/disk.img"), 2
        )
        argv_tokens = set(argv)

        for token in (
            "qemu-xhci,id=xhci,p2=4,p3=0",
            "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0",
            "rng-random,filename=/dev/urandom,id=rng0",
            "virtio-rng-pci,rng=rng0",
        ):
            with self.subTest(token=token):
                self.assertIn(token, make_tokens)
                self.assertIn(token, argv_tokens)

        self.assertTrue(any("id=usbdisk" in t for t in make_tokens))
        self.assertTrue(any("id=usbdisk" in t for t in argv_tokens))
        self.assertTrue(any("readonly=on" in t for t in make_tokens))


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
        self.assertEqual(args.case, "readonly")
        self.assertEqual(args.timeout, 120.0)


class ReadonlyCaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="usb-boot-ro-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_gpt_partition_offset_calculation(self) -> None:
        import struct
        import uuid

        img = self.tmp / "gpt.img"
        data = bytearray(4096 * 512)
        # Protective MBR + GPT header at LBA 1
        esp_guid = uuid.UUID("11111111-2222-3333-4444-555555555555")
        root_guid = uuid.UUID("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
        esp_type = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
        linux_type = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")

        # Write GPT header at LBA 1 (offset 512)
        hdr = bytearray(92)
        hdr[0:8] = b"EFI PART"
        struct.pack_into("<II", hdr, 8, 0x00010000, 92)
        struct.pack_into("<QQQQ", hdr, 24, 1, 4095, 34, 4062)
        struct.pack_into("<QIII", hdr, 72, 2, 128, 128, 0)
        data[512 : 512 + 92] = hdr

        # Entry 0 (ESP): LBA 2048..3071
        e0 = bytearray(128)
        e0[0:16] = esp_type.bytes_le
        e0[16:32] = esp_guid.bytes_le
        struct.pack_into("<QQ", e0, 32, 2048, 3071)
        data[1024 : 1024 + 128] = e0

        # Entry 1 (root): LBA 3072..4061
        e1 = bytearray(128)
        e1[0:16] = linux_type.bytes_le
        e1[16:32] = root_guid.bytes_le
        struct.pack_into("<QQ", e1, 32, 3072, 4061)
        data[1024 + 128 : 1024 + 256] = e1

        img.write_bytes(data)
        info = ub.read_gpt_info(img)
        self.assertEqual(info["esp_guid"], esp_guid)
        self.assertEqual(info["esp_offset"], 2048 * 512)
        self.assertEqual(info["esp_size"], (3071 - 2048 + 1) * 512)
        self.assertEqual(info["root_offset"], 3072 * 512)
        self.assertEqual(info["root_size"], (4061 - 3072 + 1) * 512)

    def test_readonly_requires_partition_content(self) -> None:
        root_marker = "USB_ROOT_MARKER_ALPHA"
        esp_marker = "USB_ESP_MARKER_BETA"

        # 1. Prompt without both standalone marker lines fails
        stream_no_markers = (
            "usb_msc: usba: 131072 sectors, 512-byte blocks\n"
            "# "
        )
        self.assertFalse(
            ub.readonly_evidence(stream_no_markers, root_marker, esp_marker)
        )

        # 2. Input echo containing markers on the command line fails
        stream_echo_only = (
            "usb_msc: usba: 131072 sectors, 512-byte blocks\n"
            "# echo USB_ROOT_MARKER_ALPHA USB_ESP_MARKER_BETA\n"
            "# "
        )
        self.assertFalse(
            ub.readonly_evidence(stream_echo_only, root_marker, esp_marker)
        )

        # 3. Valid sequence: usba log -> prompt -> both standalone marker lines -> fresh prompt
        stream_valid = (
            "usb_msc: usba: 131072 sectors, 512-byte blocks\n"
            "# cat /usb-root.txt /boot/USBESP.TXT\n"
            "USB_ROOT_MARKER_ALPHA\n"
            "USB_ESP_MARKER_BETA\n"
            "# "
        )
        self.assertTrue(
            ub.readonly_evidence(stream_valid, root_marker, esp_marker)
        )


class DualCaseTests(unittest.TestCase):
    def test_parse_args_accepts_dual_and_ahci_with_ahci_disk(self) -> None:
        for case in ("dual", "ahci"):
            args = ub.parse_args(
                [
                    "--case", case,
                    "--qemu", "qemu-system-x86_64",
                    "--firmware", "/fw/OVMF.fd",
                    "--disk", "/img/usb.img",
                    "--ahci-disk", "/img/ahci.img",
                    "--smp", "2",
                    "--log-dir", "/logs/run",
                ]
            )
            self.assertEqual(args.case, case)
            self.assertEqual(args.ahci_disk, "/img/ahci.img")

    def test_dual_argv_topology(self) -> None:
        argv = ub.dual_argv(
            "qemu-system-x86_64",
            Path("/tmp/OVMF.fd"),
            Path("/tmp/usb.img"),
            Path("/tmp/ahci.img"),
            2,
        )
        self.assertIn(
            "file=/tmp/usb.img,format=raw,if=none,id=usbdisk,readonly=on", argv
        )
        self.assertIn(
            "file=/tmp/ahci.img,format=raw,if=none,id=ahcidisk", argv
        )
        self.assertIn(
            "usb-storage,drive=usbdisk,bus=xhci.0,port=1,bootindex=0", argv
        )
        self.assertIn("ide-hd,drive=ahcidisk,bus=ahci.0,bootindex=1", argv)
        self.assertFalse(any(a.startswith("-boot") for a in argv))

    def test_dual_requires_identity_and_content(self) -> None:
        usb_guid = "11111111-2222-3333-4444-555555555555"
        other_guid = "99999999-8888-7777-6666-555555555555"
        root_marker = "USB_ROOT_MARKER_ALPHA"
        esp_marker = "USB_ESP_MARKER_BETA"

        valid = (
            f"UEFI: ESP GUID={usb_guid}\n"
            "fs: boot scan first_disk=hda candidates=2\n"
            "fs: boot disk selected by GUID (usba), root=usba2 boot=usba1 (/boot ok)\n"
            "# cat /usb-root.txt /boot/USBESP.TXT\n"
            f"{root_marker}\n"
            f"{esp_marker}\n"
            "# "
        )
        self.assertTrue(ub.dual_evidence(valid, usb_guid, root_marker, esp_marker))

        # Wrong ESP GUID fails
        self.assertFalse(
            ub.dual_evidence(
                valid.replace(usb_guid, other_guid), usb_guid, root_marker, esp_marker
            )
        )
        # Wrong first_disk (AHCI must be registered first) fails
        self.assertFalse(
            ub.dual_evidence(
                valid.replace("first_disk=hda", "first_disk=usba"),
                usb_guid,
                root_marker,
                esp_marker,
            )
        )
        # Wrong selected disk fails
        self.assertFalse(
            ub.dual_evidence(
                valid.replace(
                    "selected by GUID (usba), root=usba2 boot=usba1",
                    "selected by GUID (hda), root=hda2 boot=hda1",
                ),
                usb_guid,
                root_marker,
                esp_marker,
            )
        )
        # /boot mount failure fails
        self.assertFalse(
            ub.dual_evidence(
                valid.replace("(/boot ok)", "(/boot failed)"),
                usb_guid,
                root_marker,
                esp_marker,
            )
        )
        # Missing marker output fails
        self.assertFalse(
            ub.dual_evidence(
                valid.replace(f"{root_marker}\n", "AHCI_ROOT_MARKER\n"),
                usb_guid,
                root_marker,
                esp_marker,
            )
        )


if __name__ == "__main__":
    unittest.main()
