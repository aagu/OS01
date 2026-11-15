#!/usr/bin/env python3
"""Host-only contract tests for the USB-only HID input + Hub storage E2E runner.

Run with::

    python3 -m unittest qemutests.test_usb_hid_runner
"""

from __future__ import annotations

import json
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

TEST_MANIFEST = {
    "id": "test_usb_hid_runner",
    "layer": "harness",
    "subsystem": "usb",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests.harness.qemu_cmd import build_qemu_x86
from qemutests.harness.qmp import QmpClient
from qemutests import usb_hid as uh


class QmpWaitEventTests(unittest.TestCase):
    """Fake Unix-socket tests for QmpClient.wait_event and bounded async queue."""

    def _start_fake_qmp(self, handler):
        tmpdir = tempfile.TemporaryDirectory()
        sock_path = Path(tmpdir.name) / "qmp.sock"
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(str(sock_path))
        srv.listen(1)

        def _serve():
            conn, _ = srv.accept()
            with conn:
                handler(conn)

        t = threading.Thread(target=_serve, daemon=True)
        t.start()
        return tmpdir, sock_path, srv, t

    def _handshake(self, conn: socket.socket) -> None:
        conn.sendall(
            (json.dumps({"QMP": {"version": {"qemu": {"micro": 0, "minor": 2, "major": 8}}, "capabilities": []}}) + "\n").encode()
        )
        # read qmp_capabilities
        buf = b""
        while b"\n" not in buf:
            buf += conn.recv(4096)
        conn.sendall((json.dumps({"return": {}}) + "\n").encode())

    def test_device_deleted_before_command_return_is_retained(self) -> None:
        def handler(conn: socket.socket) -> None:
            self._handshake(conn)
            buf = b""
            while b"\n" not in buf:
                buf += conn.recv(4096)
            # Send DEVICE_DELETED *before* the device_del return!
            conn.sendall(
                (json.dumps({"event": "DEVICE_DELETED", "data": {"device": "kbd", "path": "/machine/peripheral/kbd"}}) + "\n").encode()
            )
            conn.sendall((json.dumps({"return": {}}) + "\n").encode())

        tmpdir, sock_path, srv, t = self._start_fake_qmp(handler)
        try:
            client = QmpClient(str(sock_path), timeout=5.0)
            client.connect()
            client.execute("device_del", {"id": "kbd"})
            ev = client.wait_event(
                "DEVICE_DELETED",
                lambda e: e.get("data", {}).get("device") == "kbd",
                deadline=time.monotonic() + 2.0,
            )
            self.assertEqual(ev["data"]["device"], "kbd")
            client.close()
        finally:
            srv.close()
            t.join(timeout=2.0)
            tmpdir.cleanup()

    def test_device_deleted_after_command_return_and_wrong_id_skipped(self) -> None:
        def handler(conn: socket.socket) -> None:
            self._handshake(conn)
            buf = b""
            while b"\n" not in buf:
                buf += conn.recv(4096)
            conn.sendall((json.dumps({"return": {}}) + "\n").encode())
            time.sleep(0.02)
            # Wrong device first, then matching device.
            conn.sendall(
                (json.dumps({"event": "DEVICE_DELETED", "data": {"device": "other"}}) + "\n").encode()
            )
            conn.sendall(
                (json.dumps({"event": "DEVICE_DELETED", "data": {"device": "mouse"}}) + "\n").encode()
            )

        tmpdir, sock_path, srv, t = self._start_fake_qmp(handler)
        try:
            client = QmpClient(str(sock_path), timeout=5.0)
            client.connect()
            client.execute("device_del", {"id": "mouse"})
            ev = client.wait_event(
                "DEVICE_DELETED",
                lambda e: e.get("data", {}).get("device") == "mouse",
                deadline=time.monotonic() + 2.0,
            )
            self.assertEqual(ev["data"]["device"], "mouse")
            # Wrong id did not satisfy wait_event and remains queued.
            other = client.wait_event(
                "DEVICE_DELETED",
                lambda e: e.get("data", {}).get("device") == "other",
                deadline=time.monotonic() + 0.5,
            )
            self.assertEqual(other["data"]["device"], "other")
            client.close()
        finally:
            srv.close()
            t.join(timeout=2.0)
            tmpdir.cleanup()

    def test_wrong_id_times_out(self) -> None:
        def handler(conn: socket.socket) -> None:
            self._handshake(conn)
            conn.sendall(
                (json.dumps({"event": "DEVICE_DELETED", "data": {"device": "wrong"}}) + "\n").encode()
            )
            time.sleep(0.5)

        tmpdir, sock_path, srv, t = self._start_fake_qmp(handler)
        try:
            client = QmpClient(str(sock_path), timeout=5.0)
            client.connect()
            with self.assertRaises(TimeoutError):
                client.wait_event(
                    "DEVICE_DELETED",
                    lambda e: e.get("data", {}).get("device") == "kbd",
                    deadline=time.monotonic() + 0.15,
                )
            client.close()
        finally:
            srv.close()
            t.join(timeout=2.0)
            tmpdir.cleanup()

    def test_bounded_64_event_queue(self) -> None:
        def handler(conn: socket.socket) -> None:
            self._handshake(conn)
            buf = b""
            while b"\n" not in buf:
                buf += conn.recv(4096)
            for i in range(70):
                conn.sendall(
                    (json.dumps({"event": "SPAM", "data": {"idx": i}}) + "\n").encode()
                )
            conn.sendall((json.dumps({"return": {}}) + "\n").encode())

        tmpdir, sock_path, srv, t = self._start_fake_qmp(handler)
        try:
            client = QmpClient(str(sock_path), timeout=5.0)
            client.connect()
            client.execute("query-status")
            self.assertEqual(len(client._events), 64)
            # Oldest 6 (0..5) dropped; 6..69 retained.
            self.assertEqual(client._events[0]["data"]["idx"], 6)
            self.assertEqual(client._events[-1]["data"]["idx"], 69)
            client.close()
        finally:
            srv.close()
            t.join(timeout=2.0)
            tmpdir.cleanup()


class TopologyArgvTests(unittest.TestCase):
    """Positive and negative tests for direct/hub QEMU argv and topology checks."""

    def test_direct_argv_contract(self) -> None:
        argv = uh.direct_argv("qemu-system-x86_64", Path("OVMF.fd"), Path("disk.img"), smp=2)
        joined = " ".join(argv)
        self.assertIn("-M q35,i8042=off", joined)
        self.assertIn("ahci,id=ahci", joined)
        self.assertIn("ide-hd,drive=disk,bus=ahci.0", joined)
        self.assertIn("qemu-xhci,id=xhci,p2=4,p3=0", joined)
        self.assertIn("usb-kbd,id=kbd,bus=xhci.0,port=2,usb_version=2", joined)
        self.assertIn("usb-mouse,id=mouse,bus=xhci.0,port=3,usb_version=2", joined)
        self.assertNotIn("usb-tablet", joined)

    def test_hub_argv_contract(self) -> None:
        argv = uh.hub_argv("qemu-system-x86_64", Path("OVMF.fd"), Path("priv.img"), smp=1)
        joined = " ".join(argv)
        self.assertIn("-M q35,i8042=off", joined)
        self.assertIn("qemu-xhci,id=xhci,p2=4,p3=0", joined)
        self.assertIn("usb-hub,bus=xhci.0,port=1", joined)
        self.assertIn("usb-storage,drive=usbdisk,bus=xhci.0,port=1.2,bootindex=0", joined)
        self.assertIn("usb-kbd,id=kbd,bus=xhci.0,port=1.3,usb_version=1", joined)
        self.assertIn("usb-mouse,id=mouse,bus=xhci.0,port=1.4,usb_version=1", joined)
        self.assertNotIn("readonly=on,id=usbdisk", joined)
        self.assertNotIn("id=usbdisk,readonly=on", joined)
        self.assertNotIn("ahci", joined)
        self.assertNotIn("ide-hd", joined)
        self.assertNotIn("virtio-blk", joined)
        self.assertNotIn("usb-tablet", joined)

    def test_off_preserves_legacy_build_qemu_x86(self) -> None:
        a = build_qemu_x86(firmware="OVMF.fd", image="disk.img", smp=2)
        b = build_qemu_x86(firmware="OVMF.fd", image="disk.img", smp=2, usb_input="off")
        self.assertEqual(a, b)
        self.assertNotIn("i8042=off", " ".join(a))

    def test_invalid_usb_input_rejected(self) -> None:
        with self.assertRaises(ValueError):
            build_qemu_x86(firmware="OVMF.fd", image="disk.img", smp=1, usb_input="bogus")
        with self.assertRaises(ValueError):
            build_qemu_x86(
                firmware="OVMF.fd", image="disk.img", smp=1, storage="ahci", usb_input="hub"
            )

    def test_qmp_info_usb_and_guest_topology_validation(self) -> None:
        info_direct = (
            "  Device 0.1, Port 2, Speed 480 Mb/s, Product QEMU USB Keyboard, ID: kbd\n"
            "  Device 0.2, Port 3, Speed 480 Mb/s, Product QEMU USB Mouse, ID: mouse\n"
        )
        serial_direct = (
            "[xhci-hid] bound bdf=00:04.0 root=2 route=0x00000 slot=1 iface=0 dci=3 kind=kbd\n"
            "[xhci-hid] bound bdf=00:04.0 root=3 route=0x00000 slot=2 iface=0 dci=3 kind=mouse\n"
        )
        self.assertTrue(uh.topology_evidence("direct", info_direct, serial_direct))
        # Wrong speed (12 Mb/s instead of 480 Mb/s) for direct must fail
        self.assertFalse(
            uh.topology_evidence("direct", info_direct.replace("480 Mb/s", "12 Mb/s"), serial_direct)
        )
        # Missing mouse ID must fail
        self.assertFalse(
            uh.topology_evidence("direct", info_direct.replace("ID: mouse", "ID: other"), serial_direct)
        )

        info_hub = (
            "  Device 0.1, Port 1, Speed 480 Mb/s, Product QEMU USB Hub\n"
            "  Device 0.2, Port 1.2, Speed 480 Mb/s, Product QEMU USB HARDDRIVE\n"
            "  Device 0.3, Port 1.3, Speed 12 Mb/s, Product QEMU USB Keyboard, ID: kbd\n"
            "  Device 0.4, Port 1.4, Speed 12 Mb/s, Product QEMU USB Mouse, ID: mouse\n"
        )
        serial_hub = (
            "[xhci-hub] ready bdf=00:04.0 root=1 route=0x00000 slot=1 protocol=usb2 ports=8 ttt=0\n"
            "[xhci-msc] disk=usba bdf=00:04.0 slot=2\n"
            "[xhci-hid] bound bdf=00:04.0 root=1 route=0x00003 slot=3 iface=0 dci=3 kind=kbd\n"
            "[xhci-hid] bound bdf=00:04.0 root=1 route=0x00004 slot=4 iface=0 dci=3 kind=mouse\n"
        )
        self.assertTrue(uh.topology_evidence("hub", info_hub, serial_hub))
        # Hub HID at 480 Mb/s instead of 12 Mb/s must fail
        self.assertFalse(
            uh.topology_evidence("hub", info_hub.replace("12 Mb/s", "480 Mb/s"), serial_hub)
        )
        # Missing guest HID bind must fail
        self.assertFalse(
            uh.topology_evidence("hub", info_hub, serial_hub.replace("kind=mouse", "kind=other"))
        )


class EvidenceNegativePositiveTests(unittest.TestCase):
    """Positive and negative tests for lease, raw edges, PTY isolation, shell, detach, and IO trace."""

    def _valid_round1_serial(self, nonce: str, pty_nonce: str) -> str:
        lines = [
            f"USB_HID_INPUT_READY nonce={nonce} gen=1",
            f"KEY_FRAME nonce={nonce} hex=e04b",
            f"KEY_FRAME nonce={nonce} hex=e0cb",
            f"MOUSE_EVENT nonce={nonce} btn=0 dx=12 dy=-3 wheel=0",
            f"MOUSE_EVENT nonce={nonce} btn=1 dx=0 dy=0 wheel=0",
            f"MOUSE_EVENT nonce={nonce} btn=0 dx=0 dy=0 wheel=0",
            f"MOUSE_EVENT nonce={nonce} btn=2 dx=0 dy=0 wheel=0",
            f"MOUSE_EVENT nonce={nonce} btn=0 dx=0 dy=0 wheel=0",
            f"MOUSE_EVENT nonce={nonce} btn=4 dx=0 dy=0 wheel=0",
            f"MOUSE_EVENT nonce={nonce} btn=0 dx=0 dy=0 wheel=0",
        ]
        # Printable pty_nonce + Enter
        for ch_hex in ("1e", "9e", "30", "b0", "1c", "9c"):
            lines.append(f"KEY_FRAME nonce={nonce} hex={ch_hex}")
        lines.append(f"PTY_ISOLATION nonce={nonce} leaked_bytes=0")
        lines.append(f"USB_HID_INPUT_DONE nonce={nonce} leaked_bytes=0")
        return "\n".join(lines) + "\n"

    def test_round1_input_evidence_positive_and_negative(self) -> None:
        nonce = "N1"
        pty_nonce = "ab"
        expected_pty_frames = ["1e", "9e", "30", "b0", "1c", "9c"]
        good = self._valid_round1_serial(nonce, pty_nonce)
        self.assertTrue(uh.round1_input_evidence(good, nonce, expected_pty_frames))

        # Missing lease marker
        self.assertFalse(
            uh.round1_input_evidence(
                good.replace("USB_HID_INPUT_READY", "NO_READY"), nonce, expected_pty_frames
            )
        )
        # Missing break edge e0cb
        self.assertFalse(
            uh.round1_input_evidence(good.replace("hex=e0cb", "hex=e04b"), nonce, expected_pty_frames)
        )
        # PTY leaked bytes > 0
        self.assertFalse(
            uh.round1_input_evidence(
                good.replace("leaked_bytes=0", "leaked_bytes=1"), nonce, expected_pty_frames
            )
        )

    def test_shell_echo_and_ctrl_c_evidence(self) -> None:
        nonce1 = "ECHO_NONCE_1"
        nonce2 = "ALIVE_NONCE_2"
        good_echo = f"# echo {nonce1}\n{nonce1}\n# "
        self.assertTrue(uh.shell_echo_evidence(good_echo, nonce1))
        # Serial only echoes command line without standalone output line -> FAIL
        echo_only = f"# echo {nonce1}\n# "
        self.assertFalse(uh.shell_echo_evidence(echo_only, nonce1))

        good_ctrlc = (
            "# cat /dev/urandom >/dev/null\n"
            "task 5 killed by signal 2 (default)\n"
            "# \n"
            f"# echo {nonce2}\n"
            f"{nonce2}\n"
            "# "
        )
        self.assertTrue(uh.shell_ctrl_c_evidence(good_ctrlc, nonce2))
        # Missing signal 2 -> FAIL
        self.assertFalse(
            uh.shell_ctrl_c_evidence(
                good_ctrlc.replace("killed by signal 2", "exited"), nonce2
            )
        )

    def test_detach_synthetic_release_evidence(self) -> None:
        nonce = "DETACH1"
        good = "\n".join([
            f"USB_HID_INPUT_READY nonce={nonce} gen=2",
            f"KEY_FRAME nonce={nonce} hex=2a",
            f"MOUSE_EVENT nonce={nonce} btn=1 dx=0 dy=0 wheel=0",
            "[xhci-hid] retired bdf=00:04.0 root=2 route=0x00000 slot=1 iface=0 dci=3 kind=kbd reason=disconnect",
            f"KEY_FRAME nonce={nonce} hex=aa",
            "[xhci-hid] retired bdf=00:04.0 root=3 route=0x00000 slot=2 iface=0 dci=3 kind=mouse reason=disconnect",
            f"MOUSE_EVENT nonce={nonce} btn=0 dx=0 dy=0 wheel=0",
            f"USB_HID_INPUT_DONE nonce={nonce} leaked_bytes=0",
        ]) + "\n"
        self.assertTrue(uh.detach_evidence(good, nonce))
        # Missing synthetic Shift release (hex=aa) -> FAIL
        self.assertFalse(uh.detach_evidence(good.replace("hex=aa", "hex=2a"), nonce))
        # Missing synthetic mouse release (btn=0) -> FAIL
        self.assertFalse(uh.detach_evidence(good.replace("btn=0", "btn=1"), nonce))

    def test_concurrent_io_and_trace_cursor_evidence(self) -> None:
        io_nonce = "IO1"
        in_nonce = "IN1"
        good_serial = "\n".join([
            f"IO_BEGIN nonce={io_nonce}",
            f"IO_PROGRESS nonce={io_nonce} iter=0 bytes=4096",
            f"USB_HID_INPUT_READY nonce={in_nonce} gen=1",
            f"IO_PROGRESS nonce={io_nonce} iter=1 bytes=4096",
            f"KEY_FRAME nonce={in_nonce} hex=e04b",
            f"IO_PROGRESS nonce={io_nonce} iter=2 bytes=4096",
            f"IO_END nonce={io_nonce} iters=3",
        ]) + "\n"
        self.assertTrue(uh.concurrent_io_evidence(good_serial, io_nonce, in_nonce))

        # No IO_PROGRESS after guest input receipt -> not bracketed -> FAIL
        not_bracketed = "\n".join([
            f"IO_BEGIN nonce={io_nonce}",
            f"IO_PROGRESS nonce={io_nonce} iter=0 bytes=4096",
            f"IO_END nonce={io_nonce} iters=1",
            f"KEY_FRAME nonce={in_nonce} hex=e04b",
        ]) + "\n"
        self.assertFalse(uh.concurrent_io_evidence(not_bracketed, io_nonce, in_nonce))

        # Trace cursor segmentation: boot trace has WRITE+SYNC, concurrent slice has none -> FAIL
        boot_trace = (
            "scsi_disk_new_request Command: lun=0 tag=0x1 data=0x2a\n"
            "usb_msd_send_status status 0, tag 0x1, len 13\n"
            "scsi_disk_new_request Command: lun=0 tag=0x2 data=0x35\n"
            "usb_msd_send_status status 0, tag 0x2, len 13\n"
        )
        post_trace = (
            "scsi_disk_new_request Command: lun=0 tag=0x3 data=0x2a\n"
            "usb_msd_send_status status 0, tag 0x3, len 13\n"
            "scsi_disk_new_request Command: lun=0 tag=0x4 data=0x35\n"
            "usb_msd_send_status status 0, tag 0x4, len 13\n"
        )
        full = boot_trace + post_trace
        cursor = len(boot_trace.encode("utf-8"))
        slice_after, next_cursor = uh.read_trace_since(full.encode("utf-8"), cursor)
        self.assertTrue(uh.bot_write_sync_trace_evidence(slice_after))
        self.assertEqual(next_cursor, len(full.encode("utf-8")))
        empty_after, _ = uh.read_trace_since(full.encode("utf-8"), next_cursor)
        self.assertFalse(uh.bot_write_sync_trace_evidence(empty_after))

    def test_early_exit_and_image_modification_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            fw = tmp_path / "OVMF.fd"
            disk = tmp_path / "disk.img"
            probe = tmp_path / "probe.elf"
            fw.write_bytes(b"OVMF")
            disk.write_bytes(b"DISK")
            probe.write_bytes(b"ELF")
            log_dir = tmp_path / "logs"

            args = uh.parse_args([
                "--case", "direct",
                "--qemu", "qemu-system-x86_64",
                "--firmware", str(fw),
                "--disk", str(disk),
                "--probe-elf", str(probe),
                "--smp", "1",
                "--timeout", "0.5",
                "--log-dir", str(log_dir),
            ])

            class EarlyExitSession:
                text = ""
                returncode = 1
                def __init__(self, *a, **kw):
                    pass
                def start(self):
                    pass
                def observe(self, t):
                    pass
                def send(self, data):
                    pass
                def stop(self):
                    pass
                def close(self):
                    pass

            with mock.patch.object(uh.ub, "inject_root_probe"), mock.patch.object(
                uh.ub, "read_gpt_info", return_value={"esp_guid": "00000000-0000-0000-0000-000000000000"}
            ):
                rc = uh.run_case(args, session_factory=EarlyExitSession)
            self.assertEqual(rc, 1)

            # Image modification during run must also fail even if session succeeds
            class MutatingSession:
                text = ""
                returncode = 0
                def __init__(self, *a, **kw):
                    pass
                def start(self):
                    disk.write_bytes(b"MODIFIED")
                def observe(self, t):
                    pass
                def send(self, data):
                    pass
                def stop(self):
                    pass
                def close(self):
                    pass

            with mock.patch.object(uh.ub, "inject_root_probe"), mock.patch.object(
                uh.ub, "read_gpt_info", return_value={"esp_guid": "00000000-0000-0000-0000-000000000000"}
            ):
                rc2 = uh.run_case(args, session_factory=MutatingSession)
            self.assertEqual(rc2, 1)
            self.assertIn("source disk image sha256 changed", (log_dir / "verdict.json").read_text())


if __name__ == "__main__":
    unittest.main()
