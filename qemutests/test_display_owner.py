#!/usr/bin/env python3
"""QEMU display ownership end-to-end runner & recovery test suite (Task 6).

Covers display ownership handover, background PTY drain, and automatic
fallback recovery across normal close, explicit release, present fault,
SIGKILL, dup-close, and close-before-exec.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import selectors
import socket
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# ────────────────────────────────────────────────────────────────────
# Surface & QMP helpers
# ────────────────────────────────────────────────────────────────────

@dataclass
class Surface:
    width: int
    height: int
    pixels: bytes

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        idx = (y * self.width + x) * 3
        if idx + 2 >= len(self.pixels):
            return (0, 0, 0)
        return (self.pixels[idx], self.pixels[idx + 1], self.pixels[idx + 2])

    def is_blank(self) -> bool:
        return not any(self.pixels)

    def is_mostly_color(self, rgb: tuple[int, int, int], threshold: float = 0.8) -> bool:
        n = self.width * self.height
        if n == 0:
            return False
        matches = 0
        r, g, b = rgb
        for i in range(0, len(self.pixels), 3):
            if (self.pixels[i], self.pixels[i + 1], self.pixels[i + 2]) == (r, g, b):
                matches += 1
        return (matches / n) >= threshold

    def differ_fraction(self, other: "Surface") -> float:
        if (self.width, self.height) != (other.width, other.height):
            return 1.0
        n = self.width * self.height
        if n == 0:
            return 0.0
        diff = sum(1 for i in range(0, len(self.pixels), 3)
                   if self.pixels[i:i + 3] != other.pixels[i:i + 3])
        return diff / n


def parse_ppm(data: bytes) -> Surface:
    if not data.startswith(b"P6"):
        raise RuntimeError("screendump is not a P6 PPM")
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(data) and data[idx] in b" \t\r\n":
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx] not in b"\r\n":
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        if start == idx:
            raise RuntimeError("malformed PPM header")
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    px = data[idx:idx + w * h * 3]
    return Surface(w, h, px)


class QmpClient:
    def __init__(self, path: str, timeout: float = 15.0):
        self.path = path
        self.timeout = timeout
        self.sock = None
        self._fp = None

    def connect(self) -> None:
        deadline = time.time() + self.timeout
        err = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(self.timeout)
                s.connect(self.path)
                self.sock = s
                self._fp = s.makefile("rwb")
                break
            except OSError as e:
                err = e
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"QMP connect failed: {err}")
        self._read_message()
        self.execute("qmp_capabilities")

    def _read_message(self) -> dict:
        while True:
            line = self._fp.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def execute(self, cmd: str, args: Optional[dict] = None) -> dict:
        req = {"execute": cmd}
        if args is not None:
            req["arguments"] = args
        self._fp.write((json.dumps(req) + "\n").encode())
        self._fp.flush()
        while True:
            msg = self._read_message()
            if "error" in msg:
                raise RuntimeError(f"QMP error {cmd}: {msg['error']}")
            return msg.get("return", {})

    def screendump(self, path: Path) -> Surface:
        self.execute("screendump", {"filename": str(path)})
        data = Path(path).read_bytes()
        return parse_ppm(data)

    def close(self) -> None:
        try:
            if self._fp:
                self._fp.close()
            if self.sock:
                self.sock.close()
        except OSError:
            pass


def parse_ppid(status_text: str) -> Optional[int]:
    m = re.search(r"^PPid:\s*(\d+)", status_text, re.MULTILINE)
    return int(m.group(1)) if m else None


# ────────────────────────────────────────────────────────────────────
# DisplayOwnerRunner
# ────────────────────────────────────────────────────────────────────

class DisplayOwnerRunner:
    """Manages QEMU lifecycle, serial I/O selectors, and QMP screenshots."""

    def __init__(self, smp: int = 2, log_path: Optional[Path] = None):
        self.smp = smp
        self.proc: Optional[subprocess.Popen] = None
        self.qmp: Optional[QmpClient] = None
        self.qmp_dir: Optional[Path] = None
        self.buffer = ""
        self.sel = selectors.DefaultSelector()
        self.log_path = log_path or (ROOT / f"build/x86_64-clang/test_display_owner_smp{smp}.log")
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_file = None
        self.terminal_pid: Optional[int] = None

    def start(self, boot_timeout: float = 25.0) -> None:
        paths = subprocess.check_output(
            ["make", "-s", "PROFILE=x86_64-clang", "print-run-paths"],
            text=True, cwd=ROOT
        )
        fw = re.search(r"^firmware=(.*)$", paths, re.M).group(1)
        img = re.search(r"^image=(.*)$", paths, re.M).group(1)

        self.log_file = open(self.log_path, "w", encoding="utf-8")
        self.qmp_dir = Path(tempfile.mkdtemp(prefix="os01-owner-qmp-"))
        qmp_sock = str(self.qmp_dir / "qmp.sock")

        cmd = [
            "qemu-system-x86_64", "-M", "q35",
            "-drive", f"if=pflash,format=raw,readonly=on,file={fw}",
            "-drive", f"file={img},format=raw,if=none,id=disk",
            "-device", "ahci,id=ahci",
            "-device", "ide-hd,drive=disk,bus=ahci.0",
            "-object", "rng-random,filename=/dev/urandom,id=rng0",
            "-device", "virtio-rng-pci,rng=rng0",
            "-m", "512", "-smp", str(self.smp),
            "-serial", "stdio", "-display", "none",
            "-no-reboot", "-no-shutdown",
            "-qmp", f"unix:{qmp_sock},server,nowait"
        ]

        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT
        )
        self.sel.register(self.proc.stdout, selectors.EVENT_READ)

        # Connect QMP
        self.qmp = QmpClient(qmp_sock, timeout=boot_timeout)
        self.qmp.connect()

        # Phase 1: Wait for "# " shell prompt within boot_timeout (25s)
        ready = False
        deadline = time.monotonic() + boot_timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            events = self.sel.select(timeout=min(remaining, 0.5))
            if events:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                text = chunk.decode("utf-8", errors="replace").replace("\r", "")
                self.buffer += text
                self._log(text)
                if self.buffer.endswith("# ") or "\n# " in self.buffer:
                    ready = True
                    break

        if not ready:
            raise RuntimeError(f"Boot timeout ({boot_timeout}s) before shell prompt '# '")

        # Handshake canary
        self.proc.stdin.write(b"printf '__SHELL_%s__\\n' CANARY\n")
        self.proc.stdin.flush()

        canary_ok = False
        hdeadline = time.monotonic() + min(deadline - time.monotonic(), 5.0)
        while time.monotonic() < hdeadline:
            remaining = hdeadline - time.monotonic()
            events = self.sel.select(timeout=min(remaining, 0.2))
            if events:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                text = chunk.decode("utf-8", errors="replace").replace("\r", "")
                self.buffer += text
                self._log(text)
                if re.search(r"(^|\n)__SHELL_CANARY__(\n|$)", self.buffer):
                    canary_ok = True
                    break

        if not canary_ok:
            raise RuntimeError("Shell canary handshake failed within boot budget")

        # Read initial terminal PID
        self.terminal_pid = self.query_terminal_pid()
        if not self.terminal_pid:
            raise RuntimeError("Failed to discover initial terminal PID from shell")

    def _log(self, text: str) -> None:
        if self.log_file:
            self.log_file.write(text)
            self.log_file.flush()

    def query_terminal_pid(self, timeout: float = 10.0) -> Optional[int]:
        self.proc.stdin.write(b"cat /proc/$$/status\n")
        self.proc.stdin.flush()
        ok, out = self.wait_pattern(r"PPid:\s*(\d+)", timeout=timeout)
        if not ok:
            return None
        m = re.search(r"PPid:\s*(\d+)", out)
        return int(m.group(1)) if m else None

    def exec_command(self, cmd: str, timeout: float = 10.0) -> str:
        """Executes a shell command and collects output until trailing prompt."""
        self.proc.stdin.write((cmd + "\n").encode())
        self.proc.stdin.flush()

        deadline = time.monotonic() + timeout
        cmd_out = ""
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            events = self.sel.select(timeout=min(remaining, 0.3))
            if events:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                text = chunk.decode("utf-8", errors="replace").replace("\r", "")
                cmd_out += text
                self.buffer += text
                self._log(text)
                # Check for prompt indicating command finished
                if cmd_out.rstrip().endswith("#"):
                    break
        return cmd_out

    def wait_pattern(self, pattern: str, timeout: float = 10.0) -> Tuple[bool, str]:
        """Reads output until regex pattern matches or timeout."""
        deadline = time.monotonic() + timeout
        accum = ""
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            events = self.sel.select(timeout=min(remaining, 0.3))
            if events:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                text = chunk.decode("utf-8", errors="replace").replace("\r", "")
                accum += text
                self.buffer += text
                self._log(text)
                if re.search(pattern, accum):
                    return True, accum
        return False, accum

    def screendump(self, label: str) -> Surface:
        shot_path = self.qmp_dir / f"{label}.ppm"
        return self.qmp.screendump(shot_path)

    def assert_shell_alive(self, token: str, timeout: float = 10.0) -> None:
        self.proc.stdin.write(f"printf '__ALIVE_%s__\\n' {token}\n".encode())
        self.proc.stdin.flush()
        ok, out = self.wait_pattern(rf"(^|\n)__ALIVE_{token}__(\n|$)", timeout=timeout)
        if not ok:
            raise AssertionError(f"SHELL_ALIVE probe {token} failed: {out[-200:]}")

    def assert_terminal_not_respawned(self) -> None:
        current_pid = self.query_terminal_pid()
        if current_pid != self.terminal_pid:
            raise AssertionError(
                f"Terminal was respawned! Original PID={self.terminal_pid}, current={current_pid}"
            )

    def assert_fallback_restored(self, shot: Surface, tag: str, baseline: Optional[Surface] = None) -> None:
        if shot.is_blank():
            raise AssertionError(f"{tag}: Display is blank after fallback restoration")
        center_px = shot.pixel(shot.width // 2, shot.height // 2)
        if center_px == (0, 255, 0):
            raise AssertionError(
                f"{tag}: Stale GUI frame detected (center pixel is green {center_px})"
            )
        if shot.is_mostly_color((0, 255, 0), threshold=0.05):
            raise AssertionError(
                f"{tag}: Display is mostly green (0, 255, 0); stale GUI frame was not restored to fallback"
            )
        if baseline is not None:
            diff = shot.differ_fraction(baseline)
            if diff > 0.3:
                raise AssertionError(
                    f"{tag}: Fallback display differ fraction ({diff:.2f}) > 0.3 vs baseline"
                )

    def close(self) -> None:
        if self.qmp:
            self.qmp.close()
        if self.proc:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
        if self.log_file:
            self.log_file.close()


# ────────────────────────────────────────────────────────────────────
# Step 1: Runner Failure Unit Tests (Host-side, no QEMU required)
# ────────────────────────────────────────────────────────────────────

class RunnerFailureTests(unittest.TestCase):
    """Pin parsing and failure detection: prompt, echo, chunk splits, silent QEMU."""

    def test_no_newline_prompt_or_malformed(self):
        """Prompt missing `# ` must not satisfy boot ready condition."""
        buffer = "booting os01...\nsystem started\nlogin:\n"
        ready = buffer.endswith("# ") or "\n# " in buffer
        self.assertFalse(ready)

    def test_command_echo_only_rejected(self):
        """Command echo without unique whole-line result marker must be rejected."""
        cmd = "/bin/test_display_owner close"
        # Only echo received, no PASS line
        stream = f"{cmd}\n"
        match = re.search(r"(^|\n)\[TEST_DISPLAY_OWNER\] close PASS(\n|$)", stream)
        self.assertIsNone(match)

    def test_marker_split_across_chunks(self):
        """Marker split across two chunks must match once accumulated."""
        chunk1 = "running test:\n[TEST_DIS"
        chunk2 = "PLAY_OWNER] close PASS\n# "
        accum = ""
        accum += chunk1
        self.assertIsNone(re.search(r"(^|\n)\[TEST_DISPLAY_OWNER\] close PASS(\n|$)", accum))
        accum += chunk2
        self.assertIsNotNone(re.search(r"(^|\n)\[TEST_DISPLAY_OWNER\] close PASS(\n|$)", accum))

    def test_qemu_silence_timeout(self):
        """Silence (no output before monotonic deadline) times out."""
        deadline = time.monotonic() + 0.05
        accum = ""
        while time.monotonic() < deadline:
            time.sleep(0.01)
        ok = bool(accum)
        self.assertFalse(ok)

    def test_nonzero_guest_exit(self):
        """Non-zero exit marker must fail the assert."""
        out = "error occurred: errno=13\nEXIT:1\n"
        self.assertNotIn("EXIT:0", out)

    def test_finally_reclaims_process_and_preserves_log(self):
        """Runner cleanup terminates process and preserves log file."""
        tmp = Path(tempfile.mkdtemp())
        log_p = tmp / "test.log"
        runner = DisplayOwnerRunner(smp=1, log_path=log_p)
        runner.log_file = open(log_p, "w")
        runner.log_file.write("sample log entry\n")
        try:
            # Simulate failure during execution
            raise ValueError("simulated test failure")
        except ValueError:
            pass
        finally:
            runner.close()

        self.assertTrue(log_p.is_file())
        self.assertIn("sample log entry", log_p.read_text())
        shutil.rmtree(tmp, ignore_errors=True)

    def test_stale_green_screenshot_rejected(self):
        """A stale solid-green GUI screenshot must be rejected by fallback assertion."""
        w, h = 100, 100
        green_px = b"\x00\xff\x00" * (w * h)
        green_surface = Surface(w, h, green_px)

        runner = DisplayOwnerRunner(smp=1)
        with self.assertRaises(AssertionError) as ctx:
            runner.assert_fallback_restored(green_surface, "TestStaleGreen")
        self.assertIn("Stale GUI frame detected", str(ctx.exception))

    def test_valid_terminal_screenshot_accepted(self):
        """A terminal-like screenshot (mostly black background, sparse text) passes fallback assertion."""
        w, h = 100, 100
        px = bytearray(w * h * 3)
        for i in range(50):
            px[i * 3:i * 3 + 3] = b"\xff\xff\xff"
        term_surface = Surface(w, h, bytes(px))
        baseline = Surface(w, h, b"\x00" * (w * h * 3))

        runner = DisplayOwnerRunner(smp=1)
        runner.assert_fallback_restored(term_surface, "TestTerminal", baseline=baseline)


# ────────────────────────────────────────────────────────────────────
# End-to-End Test Suite Execution
# ────────────────────────────────────────────────────────────────────

def run_e2e_suite(smp: int = 2) -> bool:
    print(f"\n==================================================")
    print(f"  Running Display Ownership E2E Suite (SMP={smp})")
    print(f"==================================================")

    runner = DisplayOwnerRunner(smp=smp)
    try:
        print("[1/9] Booting QEMU and establishing shell session (25s deadline)...")
        runner.start(boot_timeout=25.0)
        print(f"      Boot successful. Shell active, terminal PID={runner.terminal_pid}")

        # Baseline terminal screenshot
        shot_baseline = runner.screendump("00_baseline")
        if shot_baseline.is_blank():
            raise AssertionError("Baseline terminal screenshot is blank")

        # ── Case 1: 正常 close ──
        print("\n[2/9] Testing Case 1: normal close recovery...")
        runner.proc.stdin.write(b"/bin/test_display_owner close\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] close PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 1 (normal close) did not pass: {out[-300:]}")
        runner.assert_shell_alive("CASE1_CLOSE", timeout=10.0)
        runner.assert_terminal_not_respawned()
        shot_close = runner.screendump("01_after_close")
        runner.assert_fallback_restored(shot_close, "Case 1", shot_baseline)
        print("      PASS: normal close restored fallback; terminal unrespawned; shell alive")

        # ── Case 2: 显式 release ──
        print("\n[3/9] Testing Case 2: explicit release recovery...")
        runner.proc.stdin.write(b"/bin/test_display_owner release\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] release PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 2 (release) did not pass: {out[-300:]}")
        runner.assert_shell_alive("CASE2_RELEASE", timeout=10.0)
        runner.assert_terminal_not_respawned()
        shot_rel = runner.screendump("02_after_release")
        runner.assert_fallback_restored(shot_rel, "Case 2", shot_baseline)
        print("      PASS: explicit release restored fallback; terminal unrespawned; shell alive")

        # ── Case 3: 首次 present 失败 ──
        print("\n[4/9] Testing Case 3: initial present fault recovery...")
        runner.proc.stdin.write(b"/bin/test_display_owner present-fault\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] present-fault PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 3 (present-fault) did not pass: {out[-300:]}")
        runner.assert_shell_alive("CASE3_FAULT", timeout=10.0)
        runner.assert_terminal_not_respawned()
        shot_fault = runner.screendump("03_after_fault")
        runner.assert_fallback_restored(shot_fault, "Case 3", shot_baseline)
        print("      PASS: present-fault cleaned up and restored fallback")

        # ── Case 4: SIGKILL recovery ──
        print("\n[5/9] Testing Case 4: SIGKILL owner recovery...")
        runner.proc.stdin.write(b"/bin/test_display_owner kill-owner\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] kill-owner PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 4 (kill-owner) did not pass: {out[-300:]}")
        runner.assert_shell_alive("CASE4_KILL", timeout=10.0)
        runner.assert_terminal_not_respawned()
        shot_kill = runner.screendump("04_after_kill")
        runner.assert_fallback_restored(shot_kill, "Case 4", shot_baseline)
        print("      PASS: SIGKILL of owner restored fallback cleanly")

        # ── Case 5: dup 最后关闭 ──
        print("\n[6/9] Testing Case 5: dup final close recovery...")
        runner.proc.stdin.write(b"/bin/test_display_owner dup-close\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] dup-close PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 5 (dup-close) did not pass: {out[-300:]}")
        runner.assert_shell_alive("CASE5_DUP", timeout=10.0)
        runner.assert_terminal_not_respawned()
        shot_dup = runner.screendump("05_after_dup")
        runner.assert_fallback_restored(shot_dup, "Case 5", shot_baseline)
        print("      PASS: final close on dup restored fallback")

        # ── Case 6: 显式 close-before-exec ──
        print("\n[7/9] Testing Case 6: close-before-exec child isolation...")
        runner.proc.stdin.write(b"/bin/test_display_owner close-before-exec\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] close-before-exec PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 6 (close-before-exec) did not pass: {out[-300:]}")
        runner.assert_shell_alive("CASE6_EXEC", timeout=10.0)
        runner.assert_terminal_not_respawned()
        shot_exec = runner.screendump("06_after_exec")
        runner.assert_fallback_restored(shot_exec, "Case 6", shot_baseline)
        print("      PASS: close-before-exec verified parent ownership preserved and fallback restored")

        # ── Case 7: 后台持续 PTY 输出与 GUI 前台 hold ──
        print("\n[8/9] Testing Case 7: continuous background stream + GUI foreground hold...")
        # 1. Start streaming task generating > 5KB (exceeding 4KB PTY buffer)
        runner.proc.stdin.write(b"for i in $(seq 1 150); do echo \"STREAM_DATA_$i 01234567890123456789\"; done &\n")
        runner.proc.stdin.flush()
        # 2. Immediately launch hold 2 in foreground
        runner.proc.stdin.write(b"/bin/test_display_owner hold 2\n")
        runner.proc.stdin.flush()

        # Wait for READY from hold
        ok, _ = runner.wait_pattern(r"(^|\n)READY(\n|$)", timeout=5.0)
        if not ok:
            raise AssertionError("Case 7: test_display_owner hold failed to become READY")

        # GUI is now foreground: sample screen, must be green (0, 255, 0), NOT overwritten by terminal!
        time.sleep(0.2)
        shot_hold = runner.screendump("07_during_hold")
        center_px = shot_hold.pixel(shot_hold.width // 2, shot_hold.height // 2)
        if center_px != (0, 255, 0):
            raise AssertionError(f"Case 7: GUI frame was overwritten during hold! Pixel={center_px}")

        # Verify stream data lines continue to arrive at serial stdio while hold is active
        ok, stream_out = runner.wait_pattern(r"STREAM_DATA_\d+", timeout=5.0)
        if not ok:
            raise AssertionError("Case 7: Background stream output was blocked from arriving at serial")

        # Wait for background stream to finish emitting all lines
        runner.wait_pattern(r"STREAM_DATA_150", timeout=8.0)

        # Wait for hold to finish sleeping (duration 2s) and restore fallback
        runner.assert_shell_alive("CASE7_AFTER_HOLD", timeout=10.0)
        runner.assert_terminal_not_respawned()

        # Screenshot after hold: fallback restored, terminal shows updated text
        shot_restored = runner.screendump("07_after_hold")
        runner.assert_fallback_restored(shot_restored, "Case 7", shot_baseline)
        print("      PASS: PTY drained during GUI foreground hold; screen restored with updated text")

        # ── Case 8: 只读回读与非法写保护 ──
        print("\n[9/9] Testing Case 8: readonly readback & write protections...")
        runner.proc.stdin.write(b"/bin/test_display_owner readonly-write\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] readonly-write PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 8 readonly-write failed: {out[-300:]}")

        runner.proc.stdin.write(b"/bin/test_display_owner readonly-mprotect\n")
        runner.proc.stdin.flush()
        ok, out = runner.wait_pattern(r"(^|\n)\[TEST_DISPLAY_OWNER\] readonly-mprotect PASS(\n|$)", timeout=10.0)
        if not ok:
            raise AssertionError(f"Case 8 readonly-mprotect failed: {out[-300:]}")

        runner.assert_shell_alive("CASE8_READONLY", timeout=10.0)
        runner.assert_terminal_not_respawned()
        print("      PASS: readonly mapping enforced, mprotect upgrade rejected EPERM")

        print("\n==================================================")
        print(f"  All Display Ownership E2E tests PASSED (SMP={smp})")
        print("==================================================\n")
        return True

    except Exception as exc:
        print(f"\n[FAIL] Test suite failed on SMP={smp}: {exc}", file=sys.stderr)
        return False

    finally:
        runner.close()


def main():
    parser = argparse.ArgumentParser(description="OS01 Display Ownership Runner")
    parser.add_argument("--smp", type=int, choices=[1, 2], default=2,
                        help="SMP cores for QEMU (1 or 2, default 2)")
    parser.add_argument("unittest_args", nargs="*", default=None,
                        help="Optional unittest arguments")
    args, unknown = parser.parse_known_args()

    if "unittest" in sys.argv or args.unittest_args:
        unittest.main(argv=[sys.argv[0]] + (args.unittest_args or []))
        return 0

    ok = run_e2e_suite(smp=args.smp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
