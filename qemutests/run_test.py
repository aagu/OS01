#!/usr/bin/env python3
"""OS01 test runner — launches QEMU with serial pipe, feeds commands, checks output."""

import sys
import os
import subprocess
import re
import time
import argparse
import tempfile
import http.server
import socketserver
import threading

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
DISK_IMG = os.environ.get("DISK_IMG", "disk.img")
TIMEOUT = int(os.environ.get("TEST_TIMEOUT", "60"))

# The x86 UEFI firmware is profile-private (build/<profile>/firmware/OVMF.fd)
# and must be passed explicitly by the caller. There is deliberately NO
# fallback to the source-tree boot/uefi/OVMF.fd. The check runs at module
# load — before any QEMU process can start.
OVMF_FIRMWARE = os.environ.get("OVMF_FIRMWARE")
if not OVMF_FIRMWARE or not os.path.isfile(OVMF_FIRMWARE):
    raise SystemExit("OVMF_FIRMWARE must name a readable firmware file")

class TestRunner:
    def __init__(self, disk_img, timeout=TIMEOUT):
        self.disk_img = disk_img
        self.timeout = timeout
        self.proc = None
        self.serial_log = None
        self.serial_path = None

    def start_qemu(self, network=False):
        """Launch QEMU with serial output to a temp file."""
        self.serial_log = tempfile.NamedTemporaryFile(
            prefix="os01_serial_", suffix=".log", delete=False)
        self.serial_path = self.serial_log.name
        self.serial_log.close()  # QEMU will write to it; we open separately for reading

        args = [
            QEMU,
            "-M", "q35",
            "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_FIRMWARE}",
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
        if network:
            args += ["-netdev", "user,id=net0,dhcpstart=10.0.2.20",
                     "-device", "e1000e,netdev=net0"]
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
        # If this was a systest run, surface the last test that completed so a
        # hang is attributable to a specific test name.
        last = [l for l in buf.splitlines()
                if '[PASS]' in l or '[FAIL]' in l or 'SYS TEST] RESULT' in l]
        if last:
            print(f"[TEST] Last completed test: {last[-1]}")
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

    # Wait for the test summary, but ALSO race against a wrong-mode boot.
    # If the disk was built without OS01_SYSTEST (init spawns /bin/terminal
    # and a BusyBox prompt appears) there will never be a "[SYS TEST] RESULT:"
    # line — without this the runner spins the full 60s timeout, which reads
    # as a "hang" to a user.  Match either outcome so we return as soon as
    # one happens.  read_until() returns the re.Match for a regex pattern;
    # use the matched group to tell which outcome fired.
    pat = re.compile(
        r'(\[SYS TEST\] RESULT:|\'/bin/terminal|BusyBox v)')
    m = tester.read_until(pat, timeout=60)
    if not m:
        print("FAIL: systest did not complete")
        return False
    matched = m.group(0)
    if "'/bin/terminal" in matched or "BusyBox v" in matched:
        print("FAIL: disk.img is not a systest build (booted /bin/terminal "
              "instead of /bin/systest). Rebuild with: make OS01_SYSTEST=1 test-syscall")
        return False

    # RESULT line matched — drain whatever remains so the parse regex has a
    # stable snapshot (the matched group only holds the prefix match).
    time.sleep(2)
    output = tester._read_available().decode('utf-8', errors='replace')

    # Parse: "[SYS TEST] RESULT: N passed, M failed"
    m2 = re.search(r'\[SYS TEST\] RESULT:\s*(\d+)\s*passed,\s*(\d+)\s*failed', output)
    if not m2:
        print(f"FAIL: could not parse result line. output={output[-200:]!r}")
        return False

    passed, failed = int(m2.group(1)), int(m2.group(2))
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


class _EchoTCPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(4096)
            if not data:
                return
            delay_ms = self.server.echo_delay_ms
            if delay_ms:
                time.sleep(delay_ms / 1000.0)
            self.request.sendall(data)


class _PayloadHTTPHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        payload = b"OS01 network test\n"
        if self.path != "/payload":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        pass


class _ReusableTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


class NetworkServices:
    """Deterministic host endpoints reachable from QEMU as 10.0.2.2."""
    def __init__(self):
        self.stop_event = threading.Event()
        self.servers = []
        self.threads = []
        self.udp_socket = None
        self.tcp_echo_delay_ms = int(os.environ.get("OS01_TCP_ECHO_DELAY_MS", "0"))
        if self.tcp_echo_delay_ms < 0:
            raise ValueError("OS01_TCP_ECHO_DELAY_MS must be non-negative")

    def start(self):
        endpoints = [
            (10002, _EchoTCPHandler),
            (18080, _PayloadHTTPHandler),
        ]
        for port, handler in endpoints:
            server = _ReusableTCPServer(("127.0.0.1", port), handler)
            if port == 10002:
                server.echo_delay_ms = self.tcp_echo_delay_ms
            else:
                server.echo_delay_ms = 0
            self.servers.append(server)
            server.daemon_threads = True
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.threads.append(thread)

        import socket
        self.udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp_socket.bind(("127.0.0.1", 10001))
        self.udp_socket.settimeout(0.25)

        def udp_echo():
            while not self.stop_event.is_set():
                try:
                    data, peer = self.udp_socket.recvfrom(4096)
                    self.udp_socket.sendto(data, peer)
                except socket.timeout:
                    continue
                except OSError:
                    break

        thread = threading.Thread(target=udp_echo, daemon=True)
        thread.start()
        self.threads.append(thread)

    def close(self):
        self.stop_event.set()
        if self.udp_socket:
            self.udp_socket.close()
        for server in self.servers:
            server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)


def test_network(tester):
    """Exercise DHCP, UDP, DNS, TCP, and wget through QEMU user networking."""
    services = NetworkServices()
    try:
        services.start()
        tester.start_qemu(network=True)
        output = tester.read_until("[NET TEST] RESULT:", timeout=tester.timeout)
        if not output:
            print("FAIL: network test did not complete")
            return False

        time.sleep(1)
        output = tester._read_available().decode('utf-8', errors='replace')
        match = re.search(r'\[NET TEST\] RESULT:\s*(\d+)\s*passed,\s*(\d+)\s*failed', output)
        if not match:
            print("FAIL: could not parse network result")
            return False
        passed, failed = int(match.group(1)), int(match.group(2))
        if passed != 6 or failed:
            print(f"FAIL: network regression: {passed} passed, {failed} failed")
            return False
        print("PASS: DHCP, UDP, DNS, TCP, wget, and socket_exit")
        return True
    finally:
        services.close()


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
        elif args.test_name == "network":
            result = test_network(tester)
        else:
            print(f"Unknown test: {args.test_name}")
            result = False
    finally:
        tester.cleanup()

    sys.exit(0 if result else 1)


if __name__ == "__main__":
    main()
