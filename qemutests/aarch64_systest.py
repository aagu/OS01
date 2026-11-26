#!/usr/bin/env python3
"""Run OS01 systest E2E suite on aarch64 QEMU virt."""

import argparse
import os
import pty
import re
import select
import subprocess
import sys
import tempfile
import time


def main() -> int:
    ap = argparse.ArgumentParser(description="Run OS01 systest on aarch64 QEMU")
    ap.add_argument("--image", required=True)
    ap.add_argument("--firmware", required=True)
    ap.add_argument("--qemu", default="qemu-system-aarch64")
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--memory", type=int, default=512)
    ap.add_argument("--timeout", type=int, default=150)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        sparse = os.path.join(tmp, "virt.dtb.sparse")
        packed = os.path.join(tmp, "virt.dtb")
        subprocess.run(
            [
                args.qemu,
                "-M", "virt,gic-version=2",
                "-cpu", "cortex-a53",
                "-smp", str(args.cpus),
                "-m", str(args.memory),
                "-machine", f"dumpdtb={sparse}",
                "-display", "none",
            ],
            check=True,
        )
        subprocess.run(
            ["dtc", "-I", "dtb", "-O", "dtb", "-o", packed, sparse],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        master, slave = pty.openpty()
        proc = subprocess.Popen(
            [
                args.qemu,
                "-M", "virt,gic-version=2,acpi=off",
                "-cpu", "cortex-a53",
                "-smp", str(args.cpus),
                "-m", str(args.memory),
                "-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware}",
                "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
                "-device", "virtio-blk-device,drive=disk",
                "-device", "virtio-rng-device",
                "-device", "ramfb",
                "-dtb", packed,
                "-nographic",
                "-no-reboot",
            ],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
        )
        os.close(slave)

        buf = b""
        cow_sent = 0
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            r, _, _ = select.select([master], [], [], 0.2)
            if master in r:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk
                while cow_sent < 4 and f"[COW TTY READY {cow_sent}]".encode() in buf:
                    time.sleep(0.05)
                    os.write(master, b"COW!")
                    cow_sent += 1
                if re.search(rb"\[TEST\] END suite=systest [^\r\n]*\r?\n", buf):
                    break
            if proc.poll() is not None:
                break

        proc.kill()
        proc.wait()
        os.close(master)

    text = buf.decode("utf-8", errors="replace")
    m = re.search(
        r"\[TEST\] END suite=systest total=(\d+) passed=(\d+) failed=(\d+) skipped=(\d+)",
        text,
    )
    if not m:
        print("FAIL: aarch64 systest did not emit [TEST] END summary", file=sys.stderr)
        print("\n".join(text.splitlines()[-30:]), file=sys.stderr)
        return 1

    total, passed, failed, skipped = (int(x) for x in m.groups())
    for line in text.splitlines():
        if "[TEST] FAIL" in line or "[FAIL]" in line:
            print(line, file=sys.stderr)
    if failed != 0:
        print(
            f"FAIL: aarch64 systest total={total} passed={passed} failed={failed} skipped={skipped}",
            file=sys.stderr,
        )
        return 1

    print(
        f"PASS: aarch64 systest total={total} passed={passed} failed={failed} skipped={skipped}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
