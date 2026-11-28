#!/usr/bin/env python3
"""Host-side RPi 5 RP1 UART0 IRQ & USB MSC verification script.

Uses POSIX termios at 115200 8N1 (no external pyserial dependency), bounded waits,
bidirectional logging, GPT LBA 1024 validation, pre-arm boundary byte check,
single/burst RX checks, idle stability, and [rp1-irq-usb-read] correlation.
"""
import argparse
import hashlib
import os
import re
import select
import struct
import sys
import termios
import time
from pathlib import Path


def open_serial(port: str, baud: int = 115200) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    baud_map = {115200: termios.B115200}
    speed = baud_map[baud]
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def verify_gpt_lba_1024(image_path: Path) -> bytes:
    with image_path.open("rb") as f:
        f.seek(512)
        hdr = f.read(92)
        if len(hdr) != 92 or hdr[:8] != b"EFI PART":
            raise RuntimeError("missing GPT header at LBA 1")
        part_entry_lba, num_parts, entry_sz = struct.unpack("<QII", hdr[72:88])
        first_usable_lba = struct.unpack("<Q", hdr[40:48])[0]
        f.seek(part_entry_lba * 512)
        min_start = None
        for _ in range(num_parts):
            ent = f.read(entry_sz)
            if len(ent) < 56:
                break
            if any(ent[:16]):
                s_lba = struct.unpack("<Q", ent[32:40])[0]
                if min_start is None or s_lba < min_start:
                    min_start = s_lba
        if min_start is None or not (first_usable_lba <= 1024 < min_start):
            raise RuntimeError(
                f"LBA 1024 not in gap [{first_usable_lba}, {min_start})"
            )
        f.seek(1024 * 512)
        sec = f.read(512)
        if len(sec) != 512:
            raise RuntimeError("short read of LBA 1024 from image")
        return sec


class SerialSession:
    def __init__(self, fd: int, log_path: Path):
        self.fd = fd
        self.buf = bytearray()
        self.cursor = 0
        self.log = log_path.open("wb")

    def close(self) -> None:
        self.log.close()
        os.close(self.fd)

    def send(self, data: bytes) -> None:
        self.log.write(b"[TX] " + data + b"\n")
        self.log.flush()
        os.write(self.fd, data)

    def read_until(
        self, pattern: str, timeout_s: float, flags: int = 0
    ) -> tuple[str, re.Match]:
        deadline = time.monotonic() + timeout_s
        rx = re.compile(pattern, flags)
        while time.monotonic() < deadline:
            text = self.buf[self.cursor:].decode("utf-8", errors="replace")
            m = rx.search(text)
            if m:
                self.cursor += len(text[: m.end()].encode("utf-8", errors="replace"))
                return text, m
            rem = max(0.05, deadline - time.monotonic())
            r, _, _ = select.select([self.fd], [], [], rem)
            if r:
                chunk = os.read(self.fd, 4096)
                if chunk:
                    self.log.write(chunk)
                    self.log.flush()
                    self.buf.extend(chunk)
        raise TimeoutError(f"Timed out waiting for {pattern!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--image", required=True, type=Path)
    ap.add_argument("--log", required=True, type=Path)
    ap.add_argument("--mode", choices=["irq", "poll"], required=True)
    args = ap.parse_args()

    expected_sec = verify_gpt_lba_1024(args.image)
    expected_sha = hashlib.sha256(expected_sec).hexdigest()
    print(f"[host] image={args.image} lba1024_sha256={expected_sha}")

    fd = open_serial(args.port)
    sess = SerialSession(fd, args.log)
    try:
        if args.mode == "irq":
            sess.read_until(r"\[rp1-rx\] pre-arm", 30.0)
            sess.send(b"Z")
            sess.read_until(r"PL011 console RX armed via RP1", 10.0)
            sess.read_until(
                r"\[rp1-rx\] irq=\d+ irq_bytes=\d+ poll_bytes=0 sample_n=[1-8] sample=.*5a",
                15.0,
            )
        else:
            sess.read_until(r"using 100 Hz poll fallback", 30.0)

        # Wait for USB root disk marker & BusyBox shell prompt
        _, m_disk = sess.read_until(r"\[xhci-msc\] disk=(\S+) rp1-usb", 30.0)
        disk_name = m_disk.group(1)
        sess.read_until(r"BusyBox", 30.0)

        # Clear any residual pre-arm byte at shell prompt
        sess.send(b"\n")
        time.sleep(0.5)

        _, m0 = sess.read_until(
            r"\[rp1-rx\] irq=(\d+) irq_bytes=(\d+) poll_bytes=(\d+)", 5.0
        )
        prev_irq, prev_ib, prev_pb = int(m0.group(1)), int(m0.group(2)), int(m0.group(3))

        for seq in [b"A", b"RP1-BURST-1234567890"]:
            cmd = f"/bin/rp1-irq-probe rx {len(seq)}\n".encode()
            sess.send(cmd)
            sess.read_until(r"RP1-PROBE READY", 5.0)
            sess.send(seq)
            _, m = sess.read_until(r"RP1-PROBE RX ([0-9a-f]+)", 5.0)
            if m.group(1) != seq.hex():
                raise RuntimeError(f"RX mismatch: got {m.group(1)} expected {seq.hex()}")

        _, m1 = sess.read_until(
            r"\[rp1-rx\] irq=(\d+) irq_bytes=(\d+) poll_bytes=(\d+)", 5.0
        )
        cur_irq, cur_ib, cur_pb = int(m1.group(1)), int(m1.group(2)), int(m1.group(3))
        if args.mode == "irq":
            if cur_ib <= prev_ib or cur_pb != 0:
                raise RuntimeError(f"IRQ byte delta failed: {prev_ib}->{cur_ib}, pb={cur_pb}")
        else:
            if cur_irq != 0 or cur_pb <= prev_pb:
                raise RuntimeError(f"Poll delta failed: irq={cur_irq}, {prev_pb}->{cur_pb}")

        # Idle 3s stability check
        time.sleep(3.0)
        _, m2 = sess.read_until(
            r"\[rp1-rx\] irq=(\d+) irq_bytes=(\d+) poll_bytes=(\d+)", 5.0
        )
        if int(m2.group(1)) != cur_irq or int(m2.group(2)) != cur_ib:
            if args.mode == "irq":
                raise RuntimeError("Unexpected IRQ/byte growth while idle")

        # USB root device read at LBA 1024
        sess.send(f"/bin/rp1-irq-probe block /dev/{disk_name} 1024\n".encode())
        _, m = sess.read_until(
            rf"\[rp1-irq-usb-read\] disk={re.escape(disk_name)} .*?RP1-PROBE BLOCK ([0-9a-f]{{1024}})",
            10.0,
            flags=re.DOTALL,
        )
        if m.group(1) != expected_sec.hex():
            raise RuntimeError("LBA 1024 block mismatch")
        print("[host] PASS")
    finally:
        sess.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
