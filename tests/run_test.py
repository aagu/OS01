#!/usr/bin/env python3
"""OS01 test runner — launches QEMU with serial pipe, feeds commands, checks output."""

import sys
import os
import subprocess
import re
import time
import argparse
import tempfile

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
DISK_IMG = os.environ.get("DISK_IMG", "disk.img")
TIMEOUT = int(os.environ.get("TEST_TIMEOUT", "60"))

class TestRunner:
    def __init__(self, disk_img, timeout=TIMEOUT):
        self.disk_img = disk_img
        self.timeout = timeout
        self.proc = None
        self.serial_log = None

    def start_qemu(self):
        """Launch QEMU with serial output to a temp file."""
        self.serial_log = tempfile.NamedTemporaryFile(
            prefix="os01_serial_", suffix=".log", delete=False)
        self.serial_path = self.serial_log.name
        self.serial_log.close()  # QEMU will write to it; we open separately for reading

        args = [
            QEMU,
            "-M", "q35",
            "-pflash", "boot/uefi/OVMF.fd",
            "-drive", f"file={self.disk_img},format=raw,if=none,id=disk",
            "-device", "ahci,id=ahci",
            "-device", "ide-hd,drive=disk,bus=ahci.0",
            "-m", "512",
            "-smp", "1",
            "-serial", f"file:{self.serial_path}",
            "-display", "none",
            "-no-reboot",
            "-no-shutdown",
        ]
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def _read_available(self):
        """Read any new data from the serial log file."""
        try:
            with open(self.serial_path, 'rb') as f:
                return f.read()
        except (OSError, IOError):
            return b''

    def read_until(self, pattern, timeout=None):
        """Read serial output until pattern matches. Returns the match or None."""
        if timeout is None:
            timeout = self.timeout
        deadline = time.time() + timeout
        buf = ""
        last_size = 0

        while time.time() < deadline:
            data = self._read_available()
            if len(data) > last_size:
                # New data available
                text = data[last_size:].decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()
                buf += text
                last_size = len(data)

                if isinstance(pattern, str):
                    if pattern in buf:
                        return buf
                else:
                    m = pattern.search(buf)
                    if m:
                        return m

            if self.proc.poll() is not None:
                # QEMU exited — read any remaining output
                data = self._read_available()
                if len(data) > last_size:
                    text = data[last_size:].decode('utf-8', errors='replace')
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    buf += text
                break

            time.sleep(0.5)

        # Timeout: dump what we have
        print(f"\n[TEST] TIMEOUT waiting for pattern: {pattern}")
        print(f"[TEST] Last output: {buf[-500:]}")
        return None

    def send(self, text):
        """Not supported with file-based serial (systest is fully automated)."""
        pass

    def send_line(self, text):
        pass

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
        if self.serial_path and os.path.exists(self.serial_path):
            try:
                os.unlink(self.serial_path)
            except OSError:
                pass


def test_boot(tester):
    """Phase 0 test: verify kernel boots and shell runs."""
    tester.start_qemu()

    # Wait for evidence of boot — init banner
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


def test_systest(tester):
    """System test: runs systest.elf in QEMU, parses results."""
    tester.start_qemu()

    # Wait for test summary line
    output = tester.read_until("[SYS TEST] RESULT:", timeout=60)
    if not output:
        print("FAIL: systest did not complete")
        return False

    # Continue reading for a few more seconds to capture trailing output
    time.sleep(2)
    remaining = tester._read_available()
    if remaining:
        text = remaining.decode('utf-8', errors='replace')
        output += text

    # Parse: "[SYS TEST] RESULT: N passed, M failed"
    m = re.search(r'\[SYS TEST\] RESULT:\s*(\d+)\s*passed,\s*(\d+)\s*failed', output)
    if not m:
        print(f"FAIL: could not parse result line. output={output[-200:]!r}")
        return False

    passed, failed = int(m.group(1)), int(m.group(2))
    if failed > 0:
        print(f"FAIL: {failed} tests failed ({passed} passed)")
        return False
    print(f"PASS: all {passed} syscall tests passed")
    return True


def test_inittab_phase(tester):
    """Verify inittab phase dispatch order and error handling."""
    tester.start_qemu()

    # Wait for the last phase marker. Since SYSINIT and WAIT block
    # before ONCE runs, all three markers must be present by this point.
    buf = tester.read_until("ONCE_DONE", timeout=30)
    if not buf:
        print("FAIL: phase markers not found")
        return False

    # Assert order: SYSINIT_DONE before WAIT_DONE before ONCE_DONE.
    # read_until() re-reads the log from the start each call, so
    # sequential calls only check existence. Single-regex on the
    # returned buffer proves the sequence.
    if not re.search(r'SYSINIT_DONE.*WAIT_DONE.*ONCE_DONE', buf, re.DOTALL):
        print("FAIL: phase dispatch out of order")
        return False

    # Wait for terminal shell prompt
    prompt = tester.read_until("# ", timeout=15)
    if not prompt:
        print("FAIL: terminal not started")
        return False

    # Assert malformed-line warnings are present in the buffer
    if "unknown action 'unknown_action'" not in buf:
        print("FAIL: missing 'unknown action' warning")
        return False
    if "too many fields" not in buf:
        print("FAIL: missing 'too many fields' warning")
        return False

    print("PASS: phase dispatch order verified, error paths exercised")
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
        elif args.test_name == "systest":
            result = test_systest(tester)
        elif args.test_name == "inittab-phase":
            result = test_inittab_phase(tester)
        else:
            print(f"Unknown test: {args.test_name}")
            result = False
    finally:
        tester.cleanup()

    sys.exit(0 if result else 1)


if __name__ == "__main__":
    main()
