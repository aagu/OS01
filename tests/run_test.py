#!/usr/bin/env python3
"""OS01 test runner — launches QEMU with serial pipe, feeds commands, checks output."""

import sys
import os
import subprocess
import re
import time
import argparse
import signal

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
DISK_IMG = os.environ.get("DISK_IMG", "disk.img")
TIMEOUT = int(os.environ.get("TEST_TIMEOUT", "30"))

class TestRunner:
    def __init__(self, disk_img, timeout=TIMEOUT):
        self.disk_img = disk_img
        self.timeout = timeout
        self.proc = None
        self.output_lines = []

    def start_qemu(self):
        """Launch QEMU with serial on a pty pair."""
        import pty
        master_fd, slave_fd = pty.openpty()
        self.master = master_fd
        self.slave = slave_fd

        # Use -serial with the slave
        args = [
            QEMU,
            "-M", "q35",
            "-pflash", "boot/uefi/OVMF.fd",
            "-hda", f"{self.disk_img}",
            "-m", "512",
            "-smp", "1",
            "-serial", f"/dev/fd/{slave_fd}",
            "-display", "none",
            "-no-reboot",
            "-no-shutdown",
        ]
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            pass_fds=[slave_fd],
        )
        os.close(slave_fd)

    def read_until(self, pattern, timeout=None):
        """Read serial output until pattern matches. Returns the match or None."""
        if timeout is None:
            timeout = self.timeout
        deadline = time.time() + timeout
        buf = ""
        while time.time() < deadline:
            try:
                import select
                r, _, _ = select.select([self.master], [], [], 1.0)
                if r:
                    data = os.read(self.master, 4096)
                    if not data:
                        break
                    text = data.decode('utf-8', errors='replace')
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    buf += text
                    if isinstance(pattern, str):
                        if pattern in buf:
                            return buf
                    else:
                        m = pattern.search(buf)
                        if m:
                            return m
                elif self.proc.poll() is not None:
                    break
            except Exception as e:
                print(f"\n[TEST] exception: {e}")
                break
        # Timeout: dump what we have
        print(f"\n[TEST] TIMEOUT waiting for pattern: {pattern}")
        print(f"[TEST] Last output: {buf[-500:]}")
        return None

    def send(self, text):
        """Send a string to the serial port."""
        data = text.encode('utf-8')
        os.write(self.master, data)

    def send_line(self, text):
        """Send a line (with newline) to the serial port."""
        self.send(text + "\r\n")

    def wait_for_prompt(self, timeout=None):
        """Wait for the shell prompt."""
        return self.read_until("# ", timeout=timeout)

    def cleanup(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if hasattr(self, 'master'):
            try:
                os.close(self.master)
            except OSError:
                pass


def test_boot(tester):
    """Phase 0 test: verify kernel boots and shell runs."""
    tester.start_qemu()

    # Wait for evidence of boot — init.elf banner
    booted = tester.read_until("OS01 Init v1.0", timeout=25)
    if not booted:
        print("FAIL: Kernel did not boot")
        return False

    # Wait for shell prompt
    prompt = tester.read_until("# ", timeout=15)
    if not prompt:
        print("FAIL: No shell prompt")
        return False

    print("PASS: Kernel booted and shell prompt appeared")
    return True


def main():
    parser = argparse.ArgumentParser(description="OS01 test runner")
    parser.add_argument("--disk", default=DISK_IMG, help="Disk image to test")
    parser.add_argument("--timeout", type=int, default=TIMEOUT, help="Timeout in seconds")
    parser.add_argument("test_name", nargs="?", default="boot", help="Test to run")
    args = parser.parse_args()

    tester = TestRunner(args.disk, args.timeout)

    try:
        if args.test_name == "boot" or args.test_name == "phase-0":
            result = test_boot(tester)
        else:
            print(f"Unknown test: {args.test_name}")
            result = False
    finally:
        tester.cleanup()

    sys.exit(0 if result else 1)


if __name__ == "__main__":
    main()
