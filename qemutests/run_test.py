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

# Number of guest CPUs passed to QEMU -smp (default: single CPU, the
# historical behavior). Validated at module load — before any QEMU
# process can start.
QEMU_SMP = os.environ.get("QEMU_SMP", "1")
if not QEMU_SMP.isdigit() or int(QEMU_SMP) < 1:
    raise SystemExit(
        f"QEMU_SMP must be a positive integer, got {QEMU_SMP!r}")

class TestRunner:
    def __init__(self, disk_img, timeout=TIMEOUT):
        self.disk_img = disk_img
        self.timeout = timeout
        self.proc = None
        self.serial_log = None
        self.serial_path = None
        self._serial_stdout_fp = None  # serial-stdio mode writes via this

    def start_qemu(self, network=False, serial_stdio=False):
        """Launch QEMU.

        With ``serial_stdio=False`` (default) the historical
        file-serial contract is preserved: serial output is captured
        to a temp file via ``-serial file:<path>`` and stdin is
        ``DEVNULL`` (the suite never types into QEMU).

        With ``serial_stdio=True`` the runner uses ``-serial stdio``
        so its own stdin/stdout ARE the serial port: the runner can
        type shell commands at the BusyBox prompt.  Serial output is
        also tee'd to a temp file (via ``tee``) so the existing
        log-reading helpers still work — but the primary transport
        for the gfx suite is the writable stdin pipe below.
        """
        self.serial_log = tempfile.NamedTemporaryFile(
            prefix="os01_serial_", suffix=".log", delete=False)
        self.serial_path = self.serial_log.name
        self.serial_log.close()  # QEMU will write to it; we open separately for reading

        if serial_stdio:
            # The shell needs a writable stdin, so QEMU's stdin is a
            # PIPE; the serial READ side is captured to the log file
            # via a direct stdout redirect.  We open the log file in
            # append+line-buffered mode so the existing
            # ``_read_available()`` polling loop keeps working
            # unchanged.  No ``tee`` wrapper — that would deadlock on
            # the pipe because tee's stdout side fills up while no
            # Python reader drains it.
            serial_arg = "stdio"
            stdin_target = subprocess.PIPE
        else:
            serial_arg = f"file:{self.serial_path}"
            stdin_target = subprocess.DEVNULL

        args = [
            QEMU,
            "-M", "q35",
            "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_FIRMWARE}",
            "-drive", f"file={self.disk_img},format=raw,if=none,id=disk",
            "-device", "ahci,id=ahci",
            "-device", "ide-hd,drive=disk,bus=ahci.0",
            # Issue AAGU-2 §1: provide real entropy at boot so the
            # kernel's CSPRNG can seed itself in QEMU (no RDRAND/
            # RDSEED on default CPU). OVMF exposes this via
            # EFI_RNG_PROTOCOL, which the UEFI bootloader fetches
            # into boot_context.boot_entropy.
            "-object", "rng-random,filename=/dev/urandom,id=rng0",
            "-device", "virtio-rng-pci,rng=rng0",
            "-m", "512",
            "-smp", QEMU_SMP,
            "-serial", serial_arg,
            "-display", "none",
            "-no-reboot",
            "-no-shutdown",
        ]
        if network:
            args += ["-netdev", "user,id=net0,dhcpstart=10.0.2.20",
                     "-device", "e1000e,netdev=net0"]
        if serial_stdio:
            # Redirect QEMU's stdout (the serial READ side under
            # -serial stdio) into the log file directly — line-buffered
            # so the polling reader sees data promptly.  Stderr is
            # dropped: nothing in QEMU's stderr matters for this suite.
            # We keep the handle on ``self`` so cleanup() can close it
            # before unlinking the path on Windows (and to release the
            # inode on Linux too).
            self._serial_stdout_fp = open(self.serial_path, "ab", buffering=0)
            self.proc = subprocess.Popen(
                args,
                stdin=subprocess.PIPE,
                stdout=self._serial_stdout_fp,
                stderr=subprocess.DEVNULL,
            )
        else:
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
        """Send a raw string to the QEMU stdin pipe (serial-stdio mode).

        Raises if QEMU was started in file-serial mode (no writable
        stdin).  The bytes are flushed before returning so a
        subsequent wait_for_prompt() can rely on QEMU having
        received them.
        """
        if not self.proc or not self.proc.stdin or self.proc.stdin is subprocess.DEVNULL:
            raise RuntimeError(
                "send() requires serial_stdio=True (stdin pipe is closed in file-serial mode)")
        self.proc.stdin.write(text.encode("utf-8"))
        self.proc.stdin.flush()

    def send_line(self, text):
        """Send a command line (text + ``\\n``) and flush.

        Used by the gfx suite to invoke ``/bin/test_gfx`` after the
        shell prompt is observed.  Other suites never call this.
        """
        self.send(text + "\n")

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
        if self._serial_stdout_fp is not None:
            try:
                self._serial_stdout_fp.close()
            except OSError:
                pass
            self._serial_stdout_fp = None
        if self.serial_path and os.path.exists(self.serial_path):
            try:
                os.unlink(self.serial_path)
            except OSError:
                pass


def test_boot(tester):
    """Phase 0 test: verify kernel boots, boot-log markers appear in
    order, and the shell runs."""
    tester.start_qemu()

    # Wait for evidence of boot — init banner. read_until() returns the
    # whole buffer, so the ordered boot-marker assertion below runs on
    # everything printed up to (and including) the banner.
    booted = tester.read_until("OS01 Init v1.0", timeout=25)
    if not booted:
        print("FAIL: Kernel did not boot")
        return False

    # Boot-log markers, in serial-output order.
    #
    # Post-Task 6: BSP per-CPU registration runs BEFORE the
    # filesystem/TTY phase at every SMP count, so the "percpu: N
    # CPU(s) registered" marker always precedes TTY creation, which
    # precedes the /dev/null probe, which precedes the init banner.
    # Both single- and multi-CPU branches enforce this ordering —
    # BSP percpu runs even at SMP=1 (num_cpus=1 after the BSP loop).
    if int(QEMU_SMP) > 1:
        markers = (
            rf"percpu: {int(QEMU_SMP)} CPU\(s\) registered"
            r".*tty: console TTY created"
            r".*devfs: /dev/null read=0 write=4"
            r".*OS01 Init v1\.0"
        )
    else:
        markers = (
            rf"percpu: {int(QEMU_SMP)} CPU\(s\) registered"
            r".*tty: console TTY created"
            r".*devfs: /dev/null read=0 write=4"
            r".*OS01 Init v1\.0"
        )
    if not re.search(markers, booted, re.DOTALL):
        print("FAIL: boot-log markers missing or out of order "
              f"(QEMU_SMP={QEMU_SMP})")
        return False

    # Multi-CPU boot: the kernel must have registered the configured
    # CPU count (line format: "percpu: %u CPU(s) registered (%u in MADT)").
    if int(QEMU_SMP) > 1:
        percpu = f"percpu: {QEMU_SMP} CPU(s) registered"
        if percpu not in booted:
            print(f"FAIL: missing {percpu!r} in boot log")
            return False

    # Wait for shell prompt
    prompt = tester.read_until("# ", timeout=15)
    if not prompt:
        print("FAIL: No shell prompt")
        return False

    print("PASS: Kernel booted, boot markers verified, shell prompt appeared")
    return True


def test_systest(tester):
    """Run systest, supplying deterministic serial input for COW TTY reads."""
    tester.start_qemu(serial_stdio=True)
    deadline = time.monotonic() + 60
    # Each region's child signals only after fork, immediately before read.
    # Exact markers prevent replaying input for a previously handled region.
    result_pattern = r"(\[SYS TEST\] RESULT:|'/bin/terminal|BusyBox v)"
    m = None
    for region in range(4):
        pattern = re.compile(rf"\[COW TTY READY {region}\]|{result_pattern}")
        m = tester.read_until(pattern, timeout=max(0, deadline - time.monotonic()))
        if not m:
            print("FAIL: systest did not complete")
            return False
        if "COW TTY READY" not in m.group(0):
            break
        tester.send("COW!")
    if m and "COW TTY READY" in m.group(0):
        m = tester.read_until(re.compile(result_pattern),
                              timeout=max(0, deadline - time.monotonic()))
    if not m:
        print("FAIL: systest did not complete")
        return False
    matched = m.group(0)
    if "'/bin/terminal" in matched or "BusyBox v" in matched:
        print("FAIL: disk.img is not a systest build (booted /bin/terminal "
              "instead of /bin/systest). Rebuild with: make OS01_SYSTEST=1 test-qemu SUITE=systest")
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


def test_gfx(tester):
    """Ring-3 gfx E2E (Task 5 of the 2D graphics API plan + Task 8 acceptance).

    The caller (``main()`` -> ``test_gfx`` dispatch) is responsible
    for invoking ``tester.start_qemu(serial_stdio=True)`` BEFORE this
    function runs (the gfx suite uses the same ``TestRunner`` instance
    as the other suites, and ``main()`` does not pre-start QEMU).
    This function is the shell-side flow: wait for the BusyBox ``#``
    prompt, then run ``/bin/test_gfx`` (the full-screen view E2E)
    AND ``/bin/tetris smoke`` (one-shot full-screen render+present).
    Both must print their PASS markers; either failure aborts the
    suite with FAIL.

    The test programs run the substantive assertion logic.  This
    runner is just the transport + marker gate, by design.

    On success, each binary prints exactly one PASS line; on any
    failure the binary prints a ``FAIL: <reason>`` line with the
    same prefix and exits non-zero.  We treat any non-PASS outcome
    as failure (including timeouts).
    """
    # If the caller forgot to start QEMU, we start it here too — but
    # only if no proc exists.  Calling start_qemu twice would clobber
    # the serial log file the first invocation already opened.
    if not tester.proc:
        tester.start_qemu(serial_stdio=True)

    # Wait for the BusyBox ash prompt.  read_until() matches on the
    # serial log file (the tee side of the stdio pipe), so a normal
    # boot + login + ash start all complete before this returns.
    # The TestRunner already owns the timeout budget; we honour it
    # for both the prompt and the marker so the runner's deadline is
    # visible to the test (and so unit tests can shorten it).
    prompt = tester.read_until("# ", timeout=tester.timeout)
    if not prompt:
        print("FAIL: gfx runner never saw the shell prompt")
        return False

    # ── Step A: /bin/test_gfx — full-screen view E2E ───────
    # Task 5 of the 2D graphics API plan, extended by Task 8 to
    # use the actual framebuffer dimensions and assert a >= 5.18
    # MB pixels buffer + heap headroom.
    tester.send_line("/bin/test_gfx")

    passed = tester.read_until("[GFX TEST] PASS", timeout=tester.timeout)
    if passed is None:
        # Drain a moment so a slow FAIL line still makes it into the
        # log before we report the cause.
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[GFX TEST\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/test_gfx did not produce PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [GFX TEST] PASS marker observed")

    # ── Step B: /bin/tetris smoke — one-shot full-screen E2E ─
    # Task 8 of the user-heap/ELF-isolation plan: a non-interactive
    # tetris path that allocates the same full-screen pixels buffer,
    # performs one real render and present, then prints
    # ``[TETRIS] SMOKE PASS`` and exits cleanly.  The marker must
    # reach stdout / the serial port unmodified, so the smoke path
    # intentionally bypasses the alt-screen terminal mode.
    tester.send_line("/bin/tetris smoke")

    passed = tester.read_until("[TETRIS] SMOKE PASS", timeout=tester.timeout)
    if passed is None:
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[TETRIS\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/tetris smoke did not produce SMOKE PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [TETRIS] SMOKE PASS marker observed")

    # ── Step C: /bin/test_terminal_screen — visual screen E2E ─
    tester.send_line("/bin/test_terminal_screen")

    passed = tester.read_until("[TERM SCREEN TEST] PASS", timeout=tester.timeout)
    if passed is None:
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[TERM SCREEN TEST\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/test_terminal_screen did not produce PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [TERM SCREEN TEST] PASS marker observed")

    # ── Step D: /bin/desktop smoke — GUI desktop smoke E2E ─
    tester.send_line("/bin/desktop smoke")

    passed = tester.read_until("[DESKTOP] SMOKE PASS", timeout=tester.timeout)
    if passed is None:
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[DESKTOP\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/desktop smoke did not produce PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [DESKTOP] SMOKE PASS marker observed")
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
        elif args.test_name == "network":
            result = test_network(tester)
        elif args.test_name == "gfx":
            result = test_gfx(tester)
        else:
            print(f"Unknown test: {args.test_name}")
            result = False
    finally:
        tester.cleanup()

    sys.exit(0 if result else 1)


if __name__ == "__main__":
    main()
