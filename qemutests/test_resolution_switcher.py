#!/usr/bin/env python3
"""QEMU resolution-switcher end-to-end runner (plan Task 10).

Binding design:
  docs/superpowers/specs/2026-10-06-qemu-resolution-switcher-design.md §8

This module is two things at once:

  * the ``resolution`` suite driver registered in ``qemutests/run_test.py``
    (``test_resolution(tester) -> bool``); and
  * a host-only ``unittest`` suite (discovered with
    ``python3 -m unittest discover -s qemutests -p 'test_resolution_switcher.py'``)
    that exercises the runner's *pure* logic against fake QMP/serial objects —
    no QEMU is started by the unit tests.

The production configuration (``make ... test-qemu SUITE=resolution``) runs the
surface/session normal cases against the normal rootfs.  The isolated fault
configuration (``make ... FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution``)
runs the fault-injection subset against the ``resolution-test`` rootfs via the
``/dev/fbtest`` guest helper (``user/test_resolution.c``).
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
DEFAULT_SMP = 2
DEFAULT_TIMEOUT = 180
PROMPT = "# "
BOOT_TIMEOUT = 120

# ── Fixed plan values ────────────────────────────────────────
BOOT_MODE = "800x600"              # production round-trip start mode
ROUND_TRIP_MODES = ("1280x720", "640x480")
OFF_WHITELIST_MODE = "1280x960"    # plan-specified off-table boot mode
OFF_WHITELIST_FALLBACKS = ("1600x1200", "1400x1050", "1152x864")

# Unsupported-display-device case (spec §8.2 item 6): QEMU `-vga cirrus`
# (1013:00b8) is driven by OVMF QemuVideoDxe so a trusted GOP framebuffer
# exists, but it does not match the kernel's BGA probe (1234:1111 + legacy VGA
# I/O), so no switchable backend is published.  bochs-display / secondary-vga
# both *are* accepted by the probe here, and virtio-gpu-pci yields no GOP with
# this OVMF, so cirrus is the only suitable device on this host.
NO_BGA_DEVICE = ("-vga", "cirrus")

WHITELIST = {
    (640, 480), (800, 600), (1024, 768), (1280, 720), (1280, 800),
    (1280, 1024), (1440, 900), (1600, 900), (1920, 1080),
}

# EFI System Partition type GUID (canonical, mixed-endian decoded).
ESP_TYPE_GUID = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"

# Test-only environment variables that must never be mixed into a resolution
# run (spec §8: no OS01_SYSTEST / KERNEL_SELFTEST contamination).
FORBIDDEN_FLAGS = (
    "OS01_SYSTEST", "OS01_NETTEST", "KERNEL_SELFTEST",
    "KERNEL_CANARY_SELFTEST", "ARCH9_FAULT",
)


# ═══════════════════════════════════════════════════════════════
#  Errors
# ═══════════════════════════════════════════════════════════════

class ImageIsolationError(RuntimeError):
    """prepare_resolution_image could not produce a clean private copy."""


class QmpError(RuntimeError):
    """QMP transport failed; a screenshot cannot be taken (must FAIL)."""


# ═══════════════════════════════════════════════════════════════
#  GPT / image isolation
# ═══════════════════════════════════════════════════════════════

def _guid_from_bytes(raw: bytes) -> str:
    """Decode a 16-byte GPT GUID (mixed endian) to canonical text."""
    return (f"{int.from_bytes(raw[0:4], 'little'):08x}-"
            f"{int.from_bytes(raw[4:6], 'little'):04x}-"
            f"{int.from_bytes(raw[6:8], 'little'):04x}-"
            f"{raw[8:10].hex()}-{raw[10:16].hex()}")


def parse_gpt_esp_offset(image: Path) -> int:
    """Return the byte offset of the EFI System Partition inside ``image``.

    Raises ImageIsolationError when the GPT is corrupt or has no ESP.  Only
    the primary GPT header (LBA 1) is consulted; the ESP is located by its
    type GUID, never by a hard-coded LBA.
    """
    image = Path(image)
    try:
        fsize = image.stat().st_size
    except OSError as e:
        raise ImageIsolationError(f"cannot stat image {image}: {e}")

    with open(image, "rb") as fp:
        fp.seek(512)
        hdr = fp.read(512)
        if len(hdr) < 92 or hdr[0:8] != b"EFI PART":
            raise ImageIsolationError("GPT signature missing (corrupt image)")
        header_size = struct.unpack_from("<I", hdr, 12)[0]
        if header_size < 92 or header_size > 512:
            raise ImageIsolationError(f"bad GPT header size {header_size}")
        stored_crc = struct.unpack_from("<I", hdr, 16)[0]
        calc = binascii.crc32(
            bytes(hdr[:16]) + b"\x00\x00\x00\x00" + bytes(hdr[20:header_size])
        ) & 0xFFFFFFFF
        if calc != stored_crc:
            raise ImageIsolationError("GPT header CRC mismatch (corrupt image)")

        entry_lba = struct.unpack_from("<Q", hdr, 72)[0]
        nentries = struct.unpack_from("<I", hdr, 80)[0]
        esize = struct.unpack_from("<I", hdr, 84)[0]
        if not (128 <= esize <= 4096) or not (1 <= nentries <= 4096):
            raise ImageIsolationError("implausible GPT partition array")
        if (entry_lba + 1) * 512 > fsize:
            raise ImageIsolationError("GPT partition array out of bounds")

        fp.seek(entry_lba * 512)
        raw = fp.read(nentries * esize)
        if len(raw) < nentries * esize:
            raise ImageIsolationError("truncated GPT partition array")

    for i in range(nentries):
        entry = raw[i * esize:(i + 1) * esize]
        if entry[0:16] == b"\x00" * 16:
            continue
        if _guid_from_bytes(entry[0:16]) == ESP_TYPE_GUID:
            first = struct.unpack_from("<Q", entry, 32)[0]
            if first == 0:
                continue
            return first * 512

    raise ImageIsolationError("no EFI System Partition found")


def prepare_resolution_image(source: Path, mode: str, destination: Path) -> Path:
    """Copy ``source`` to ``destination`` and set its ESP ``config.txt``.

    The copy is private to the caller: the source image is never opened for
    writing and its sha256 must be byte-identical afterwards.  The single
    configuration line written is exactly ``resolution {mode}\\n``.

    Raises ImageIsolationError on a corrupt GPT, a missing ESP, or a
    non-zero ``mcopy`` result — never silently falls back.
    """
    source = Path(source)
    destination = Path(destination)
    if not source.is_file():
        raise ImageIsolationError(f"source image missing: {source}")
    before = _sha256(source)

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)

    offset = parse_gpt_esp_offset(destination)

    cfg = Path(tempfile.mkstemp(prefix="rescfg-", suffix=".txt")[1])
    try:
        cfg.write_bytes(_config_bytes(mode))
        cmd = ["mcopy", "-o", "-i", f"{destination}@@{offset}",
               str(cfg), "::/config.txt"]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise ImageIsolationError(
                f"mcopy failed rc={proc.returncode}: {proc.stderr.strip()}")
        chk = subprocess.run(
            ["mtype", "-i", f"{destination}@@{offset}", "::/config.txt"],
            capture_output=True)
        if chk.returncode != 0 or chk.stdout != _config_bytes(mode):
            raise ImageIsolationError("ESP config.txt content mismatch after mcopy")
    finally:
        cfg.unlink(missing_ok=True)

    if _sha256(source) != before:
        raise ImageIsolationError("source image changed during prepare (not isolated)")
    return destination


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _config_bytes(mode: str) -> bytes:
    return f"resolution {mode}\n".encode("ascii")


# ═══════════════════════════════════════════════════════════════
#  Surface (QMP screendump) helpers
# ═══════════════════════════════════════════════════════════════

@dataclass
class Surface:
    width: int
    height: int
    pixels: bytes          # RGB888, row-major, len == w*h*3

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        idx = (y * self.width + x) * 3
        return (self.pixels[idx], self.pixels[idx + 1], self.pixels[idx + 2])

    def is_blank(self) -> bool:
        return not any(self.pixels)

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
    """Parse a binary P6 PPM (QEMU ``screendump`` output)."""
    if not data.startswith(b"P6"):
        raise QmpError("screendump is not a P6 PPM")
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
            raise QmpError("malformed PPM header")
        fields.append(int(data[start:idx]))
    idx += 1  # exactly one whitespace byte after maxval
    w, h, _maxval = fields
    if w <= 0 or h <= 0 or w * h > (1 << 30):
        raise QmpError(f"implausible PPM dimensions {w}x{h}")
    px = data[idx:idx + w * h * 3]
    if len(px) < w * h * 3:
        raise QmpError("truncated PPM pixel data")
    return Surface(w, h, px)


# ── Pure assertion helpers (each returns None on success, else a reason) ──

def check_surface_dims(expected: tuple[int, int], surface: Optional[Surface]) -> Optional[str]:
    if surface is None:
        return "no_surface"
    if (surface.width, surface.height) != (expected[0], expected[1]):
        return (f"surface_size {surface.width}x{surface.height} != "
                f"{expected[0]}x{expected[1]}")
    return None


def check_sentinel(surface: Surface, x: int, y: int, rgb: tuple[int, int, int],
                   tol: int = 0) -> Optional[str]:
    got = surface.pixel(x, y)
    if any(abs(a - b) > tol for a, b in zip(got, rgb)):
        return f"sentinel_pixel({x},{y})={got} != {rgb}"
    return None


def check_picture_present(surface: Optional[Surface]) -> Optional[str]:
    if surface is None:
        return "no_surface"
    if surface.is_blank():
        return "surface_blank"
    return None


def check_pid_stable(before, after) -> Optional[str]:
    if before != after:
        return f"pid_changed {before}->{after}"
    return None


def check_fault_config_isolated(env: dict) -> Optional[str]:
    bad = [v for v in FORBIDDEN_FLAGS if str(env.get(v, "0")) not in ("", "0", "None")]
    if bad:
        return "fault_config_mixed:" + ",".join(bad)
    return None


# ── CLI output parsing ──────────────────────────────────────────

def parse_setres_list(text: str) -> Optional[dict]:
    """Parse ``setres -l`` output.

    Returns {'modes': [(w,h)..], 'current': (w,h), 'generation': int,
    'stride': int} or None when the expected lines are absent.
    """
    modes = [(int(m.group(1)), int(m.group(2)))
             for m in re.finditer(r"^\s+(\d+)x(\d+) bpp=\d+\s*$", text, re.MULTILINE)]
    cur = re.search(r"Current:\s*(\d+)x(\d+)\s*\(CURRENT\)\s*bpp=(\d+)\s*stride=(\d+)",
                    text)
    if not cur:
        return None
    gen = re.search(r"generation:\s*(\d+)", text)
    return {
        "modes": modes,
        "current": (int(cur.group(1)), int(cur.group(2))),
        "stride": int(cur.group(4)),
        "generation": int(gen.group(1)) if gen else 0,
    }


def parse_resolution_get(text: str) -> Optional[dict]:
    """Parse ``test_resolution get`` / ``snapshot`` output for width/height/gen."""
    m = re.search(r"(?:GET|STATE) width=(\d+) height=(\d+).*?generation=(\d+)",
                  text, re.DOTALL)
    if not m:
        return None
    return {"width": int(m.group(1)), "height": int(m.group(2)),
            "generation": int(m.group(3))}


def parse_test_set(text: str) -> Optional[dict]:
    """Parse a ``test_resolution set/arm-*`` mnemonic + SET result line."""
    m = re.search(r"SET (\d+)x(\d+) rc=(-?\d+) errno=(\d+)", text)
    if not m:
        return None
    return {"width": int(m.group(1)), "height": int(m.group(2)),
            "rc": int(m.group(3)), "errno": int(m.group(4))}


def parse_resjson(text: str) -> list:
    """Collect every ``RESJSON {...}`` line emitted by the guest helper."""
    out = []
    for m in re.finditer(r"RESJSON (\{.*?\})\s*$", text, re.MULTILINE):
        try:
            out.append(json.loads(m.group(1)))
        except json.JSONDecodeError:
            continue
    return out


def parse_term_status(text: str) -> Optional[dict]:
    """Parse ``test_resolution terminal-status`` output."""
    m = re.search(r"TERM_STATUS pending=(\d+) consumed=(\d+)", text)
    if not m:
        return None
    return {"pending": int(m.group(1)), "consumed": int(m.group(2))}


def parse_ppid(text: str) -> Optional[int]:
    """Parse the ``PPid:`` field of a /proc/<pid>/status dump."""
    m = re.search(r"PPid:\s*(\d+)", text)
    return int(m.group(1)) if m else None


# ═══════════════════════════════════════════════════════════════
#  QMP client
# ═══════════════════════════════════════════════════════════════

class QmpClient:
    """Minimal QMP client over a unix socket.

    A failed connection or failed ``screendump`` raises QmpError: the runner
    must FAIL, never skip the screenshot.
    """

    def __init__(self, path: str, timeout: float = 20.0):
        self.path = path
        self.timeout = timeout
        self.sock = None
        self._fp = None
        self._id = 0

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
            except OSError as e:  # not listening yet
                err = e
                time.sleep(0.2)
        if self.sock is None:
            raise QmpError(f"QMP connect failed ({self.path}): {err}")
        # Greeting, then capabilities handshake.
        self._read_message()
        self.execute("qmp_capabilities")

    def _read_message(self) -> dict:
        while True:
            line = self._fp.readline()
            if not line:
                raise QmpError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def execute(self, cmd: str, args: Optional[dict] = None) -> dict:
        self._id += 1
        req = {"execute": cmd}
        if args is not None:
            req["arguments"] = args
        self._fp.write((json.dumps(req) + "\n").encode())
        self._fp.flush()
        while True:
            msg = self._read_message()
            if "error" in msg:
                raise QmpError(f"QMP {cmd} error: {msg['error']}")
            return msg.get("return", {})

    def screendump(self, path: Path) -> Surface:
        self.execute("screendump", {"filename": str(path)})
        # screendump is synchronous; the file is complete on return.
        try:
            data = Path(path).read_bytes()
        except OSError as e:
            raise QmpError(f"screendump file unreadable: {e}")
        return parse_ppm(data)

    def close(self) -> None:
        try:
            if self._fp:
                self._fp.close()
            if self.sock:
                self.sock.close()
        except OSError:
            pass


# ═══════════════════════════════════════════════════════════════
#  Live session (QEMU + serial pipe + QMP)
# ═══════════════════════════════════════════════════════════════

class ResolutionSession:
    """A running QEMU instance driven over a writable serial pipe + QMP.

    The QEMU process boundary is owned by ``ProcessSession`` (spec
    section 5.3).  QMP is a separate concern (a unix socket) and
    continues to be managed by ``QmpClient`` directly.

    Process boundary delegation:
      * ``start()`` constructs a ``ProcessSession`` via an
        injectable ``session_factory`` (default: ``ProcessSession``).
        Tests install a stand-in to bypass the real QEMU spawn.
      * Serial output is captured by the ProcessSession's stdout
        pipe (``text``); the legacy ``_pump()`` / ``serial.log``
        file path is preserved as a backward-compat shim that
        reads from ``process.text``.
      * ``send_line`` writes via ``process.send``.  Lifecycle /
        cleanup delegate to ``process.close``.
    """

    def __init__(self, firmware: Path, image: Path, results_dir: Path,
                 smp: int = DEFAULT_SMP, timeout: int = DEFAULT_TIMEOUT,
                 qemu: str = QEMU, serial_only: bool = False,
                 display_device=None, session_factory=None,
                 run_archive=None):
        self.firmware = Path(firmware)
        self.image = Path(image)
        self.results_dir = Path(results_dir)
        self.smp = smp
        self.timeout = timeout
        self.qemu = qemu
        self.serial_only = serial_only
        # Video device argv tokens; defaults to the supported `-vga std` (BGA).
        self.display_device = (list(display_device) if display_device
                               else ["-vga", "std"])
        # ProcessSession factory: tests install a stand-in (see
        # FakeProcessSession in qemutests/test_run_test_harness.py).
        if session_factory is None:
            from qemutests.harness.process import ProcessSession
            session_factory = ProcessSession
        self._session_factory = session_factory
        self.proc = None            # legacy alias for `process._proc`
        self.process = None         # ProcessSession instance
        self.qmp: Optional[QmpClient] = None
        self.serial_path = self.results_dir / "serial.log"
        self._serial_fp = None
        self._qmp_dir = None
        self._buf = ""
        # RunArchive (spec §7.2): when the suite dispatcher wired one
        # in via OS01_BUILD_DIR, the session lives under
        # ``build/<profile>/logs/tests/<suite>/<UTC>-<uuid>/``;
        # ``None`` means the legacy results_dir path is used.
        self.run_archive = run_archive

    def start(self) -> None:
        self.results_dir.mkdir(parents=True, exist_ok=True)
        # A unix socket path is limited to ~108 bytes; the per-run results
        # directory can exceed that, so the socket lives in a short temp dir.
        self._qmp_dir = Path(tempfile.mkdtemp(prefix="os01qmp-"))
        qmp_path = str(self._qmp_dir / "q")
        try:
            os.unlink(qmp_path)
        except OSError:
            pass
        # Backward-compat serial.log shim — the live harness still
        # creates the shim so legacy readers (debug scripts) find
        # the file.  The primary serial source is now the
        # ProcessSession's stdout pipe.
        self._serial_fp = open(self.serial_path, "wb", buffering=0)
        args = [
            self.qemu, "-M", "q35",
            "-drive", f"if=pflash,format=raw,readonly=on,file={self.firmware}",
            "-drive", f"file={self.image},format=raw,if=none,id=disk",
            "-device", "ahci,id=ahci",
            "-device", "ide-hd,drive=disk,bus=ahci.0",
            "-object", "rng-random,filename=/dev/urandom,id=rng0",
            "-device", "virtio-rng-pci,rng=rng0",
            *self.display_device,
            "-m", "512",
            "-smp", str(self.smp),
            "-serial", "stdio",
            "-display", "none",
            "-no-reboot", "-no-shutdown",
            "-qmp", f"unix:{qmp_path},server,nowait",
        ]
        # Construct ProcessSession (argv-only, monotonic deadline,
        # process-group kill, persistent log files).  writable_stdin
        # is True because ResolutionSession types shell commands.
        run_dir = self.results_dir / "process"
        self.process = self._session_factory(
            argv=args,
            run_dir=run_dir,
            timeout_s=float(self.timeout),
            writable_stdin=True,
        )
        self.process.start()
        self.proc = self.process  # legacy alias
        # Mirror initial serial output to the legacy serial.log file
        # so external debug readers can still tail it.
        if self._serial_fp is not None:
            try:
                self._serial_fp.write(self.process.text.encode("utf-8", errors="replace"))
                self._serial_fp.flush()
            except OSError:
                pass
        self.qmp = QmpClient(qmp_path)
        self.qmp.connect()   # QmpError -> FAIL (never skip)

    # ── serial transport ──
    def _pump(self) -> None:
        # The serial source is the ProcessSession's stdout pipe; the
        # legacy serial.log file is mirrored for debug readers.  We
        # always re-read ``process.text`` so a freshly-drained chunk
        # is visible to the next wait_for call.
        try:
            data = self.process.text.encode("utf-8", errors="replace")
        except (OSError, AttributeError):
            data = b""
        self._buf = data.decode("utf-8", errors="replace")
        if self._serial_fp is not None:
            try:
                self._serial_fp.write(data)
                self._serial_fp.flush()
            except OSError:
                pass

    def _send(self, text: str) -> None:
        if not self.process:
            raise RuntimeError("session not started")
        self.process.send(text.encode("utf-8"))

    def send_line(self, text: str) -> None:
        self._send(text + "\n")

    def wait_for(self, pattern: str, timeout: float = 20.0,
                 start: Optional[str] = None) -> bool:
        rx = re.compile(pattern)
        # With no explicit start, search the whole buffer: a single
        # serial read can deliver several events at once, so
        # "already present" must count.
        base = "" if start is None else start
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump()
            region = self._buf[len(base):]
            if rx.search(region):
                return True
            if self.process and self.process._proc and self.process._proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def mark(self) -> str:
        """Snapshot the serial buffer *after* draining pending output."""
        self._pump()
        return self._buf

    def run(self, cmd: str, until: str = PROMPT, timeout: float = 20.0) -> str:
        start = self.mark()
        self.send_line(cmd)
        self.wait_for(until, timeout=timeout, start=start)
        return self._buf[len(start):]

    def output(self) -> str:
        self._pump()
        return self._buf

    # ── queries ──
    def screen(self, label: str = "shot") -> Surface:
        if self.qmp is None:
            raise QmpError("no QMP client")
        return self.qmp.screendump(self.results_dir / f"{label}.ppm")

    def setres_list(self) -> Optional[dict]:
        text = self.run("/bin/setres -l")
        return parse_setres_list(text)

    def mode(self) -> Optional[tuple[int, int]]:
        st = self.setres_list()
        return st["current"] if st else None

    def generation(self) -> Optional[int]:
        st = self.setres_list()
        return st["generation"] if st else None

    def shell_pid(self) -> Optional[int]:
        m = re.search(r"PID=(\d+)", self.run('echo "PID=$$"'))
        return int(m.group(1)) if m else None

    def pty(self) -> Optional[str]:
        m = re.search(r"(/dev/pts\d+)", self.run("cat /proc/self/fd/0"))
        return m.group(1) if m else None

    def terminal_pid(self) -> Optional[int]:
        """PID of the terminal process that owns this shell's PTY.

        The interactive shell is the ash child forked by terminal.elf
        (user/terminal.c), so its parent PID is the terminal PID.  Reading
        it from /proc avoids hard-coding a PID the test must not assume.
        """
        return parse_ppid(self.run("cat /proc/$$/status"))

    def close(self) -> None:
        if self.qmp:
            self.qmp.close()
        if self.process is not None:
            try:
                self.process.close()
            except Exception:
                pass
            self.process = None
            self.proc = None
        if self._serial_fp:
            try:
                self._serial_fp.close()
            except OSError:
                pass
            self._serial_fp = None
        if self._qmp_dir:
            shutil.rmtree(self._qmp_dir, ignore_errors=True)


# ═══════════════════════════════════════════════════════════════
#  Case evaluation (shared by live + fake sessions)
# ═══════════════════════════════════════════════════════════════

def evaluate_round_trip(session, initial: str = BOOT_MODE) -> list:
    """Surface + session round-trip checks over a session-like object.

    Returns a list of failure reason strings (empty means pass).  Works
    against both a live ResolutionSession and a fake test double.
    """
    fail: list = []

    def expect(w: int, h: int, label: str) -> None:
        surf = session.screen(label)
        r = check_surface_dims((w, h), surf)
        if r:
            fail.append(f"{label}:{r}")
            return
        r = check_picture_present(surf)
        if r:
            fail.append(f"{label}:{r}")

    expect(800, 600, "boot")
    if session.mode() != (800, 600):
        fail.append(f"boot_mode {session.mode()} != (800, 600)")

    # echo-but-no-picture: the shell echoes the command to serial, but the
    # framebuffer must also change (a redraw happened).  The present is
    # throttled, so poll for up to ~1 s before declaring the screen dead.
    before = session.screen("pre-echo")
    out = session.run("echo RES_ECHO_PROBE")
    echoed = "RES_ECHO_PROBE" in out
    painted = False
    deadline = time.time() + 1.0
    while True:
        after = session.screen("post-echo")
        if after is not None and not after.is_blank() and (
                before is None or after.differ_fraction(before) > 0.0):
            painted = True
            break
        if time.time() >= deadline:
            break
        time.sleep(0.05)
    if echoed and not painted:
        fail.append("echo_without_picture")

    pid0, pty0 = session.shell_pid(), session.pty()

    for spec in ROUND_TRIP_MODES:
        w, h = (int(x) for x in spec.split("x"))
        session.run(f"setres {spec}")
        expect(w, h, f"m{w}x{h}")
        if session.mode() != (w, h):
            fail.append(f"mode_after_{spec} {session.mode()} != ({w}, {h})")

    # Restore the initial mode and require a *new* generation (ABA-safe).
    gen_before = session.generation()
    session.run(f"setres {initial}")
    expect(800, 600, "restore")
    if session.mode() != (800, 600):
        fail.append(f"restore_mode {session.mode()} != (800, 600)")
    gen_after = session.generation()
    if gen_before is not None and gen_after is not None and gen_after <= gen_before:
        fail.append(f"generation_not_advanced {gen_before}->{gen_after}")

    # Same-mode SET: no hardware write, no clear, no generation bump.
    g1 = session.generation()
    s1 = session.screen("same-pre")
    session.run(f"setres {initial}")
    g2 = session.generation()
    s2 = session.screen("same-post")
    if g1 is not None and g2 is not None and g2 != g1:
        fail.append(f"same_mode_generation {g1}->{g2}")
    if s1 is not None and s2 is not None:
        if s2.is_blank():
            fail.append("same_mode_cleared")
        elif s1.differ_fraction(s2) > 0.02:
            fail.append(f"same_mode_changed {s1.differ_fraction(s2):.3f}")

    # Session identity must survive every switch.
    r = check_pid_stable(pid0, session.shell_pid())
    if r:
        fail.append("spid_" + r)
    if pty0 != session.pty():
        fail.append(f"pty_changed {pty0}->{session.pty()}")

    return fail


def evaluate_off_whitelist(session) -> tuple[bool, list, Optional[str]]:
    """Query-only check for a boot mode outside the candidate table.

    Returns (ok, failures, env_block).  When the actual boot mode equals the
    requested off-table mode, GET_MODES must not list it; otherwise the case
    is an environment block (the firmware did not provide that mode).
    """
    fail: list = []
    mode = session.mode()
    if mode is None:
        return False, ["off_whitelist: no current mode"], None
    if mode in WHITELIST:
        return False, [], (f"OVMF did not provide {OFF_WHITELIST_MODE}; "
                           f"booted {mode[0]}x{mode[1]} instead")
    st = session.setres_list()
    if st is None:
        return False, ["off_whitelist: setres -l unparsable"], None
    if (mode[0], mode[1]) in st["modes"]:
        fail.append(f"off_whitelist {mode} listed in GET_MODES")
    surf = session.screen("offwl")
    r = check_surface_dims((mode[0], mode[1]), surf)
    if r:
        fail.append(f"off_whitelist:{r}")
    return (not fail), fail, None


# ═══════════════════════════════════════════════════════════════
#  Suite entry points
# ═══════════════════════════════════════════════════════════════

def _results_dir(image: Path) -> Path:
    env = os.environ.get("OS01_RESOLUTION_RESULT_DIR")
    if env:
        return Path(env)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return Path(tempfile.mkdtemp(prefix=f"resolution-{stamp}-"))


def _env_path(name: str) -> Optional[Path]:
    v = os.environ.get(name)
    return Path(v) if v else None


def run_production_suite(firmware: Path, image: Path, results_dir: Path,
                         log=print) -> bool:
    """Production configuration: surface/session normal cases.

    Boots a private 800x600 copy, verifies the QMP surface + GET_STATE follow
    every switch, exercises the idle background-switch session continuity,
    the off-table boot-mode query, and the three-boot image isolation.
    """
    failures: list = []
    copy800 = results_dir / "disk-800.img"
    try:
        prepare_resolution_image(image, BOOT_MODE, copy800)
    except ImageIsolationError as e:
        log(f"FAIL: prepare_resolution_image({BOOT_MODE}): {e}")
        return False

    sess = _boot_session(firmware, copy800, results_dir / "round-trip", log)
    if sess is None:
        return False
    try:
        failures += evaluate_round_trip(sess)
        failures += _idle_switch_case(sess, log)
        failures += _prod_cli_case(sess, log)
        failures += _client_estale_case(sess, log)
    finally:
        sess.close()

    failures += _off_whitelist_case(firmware, image, results_dir, log)
    failures += _isolation_case(firmware, image, copy800, results_dir, log)
    failures += _no_bga_case(firmware, image, results_dir, log, test_build=False)

    return _report("production", failures, log)


def run_test_suite(firmware: Path, image: Path, results_dir: Path,
                   log=print) -> bool:
    """FB_RESOLUTION_TEST=1 configuration: isolated fault/compat subset."""
    failures: list = []
    copy800 = results_dir / "disk-r800.img"
    try:
        prepare_resolution_image(image, BOOT_MODE, copy800)
    except ImageIsolationError as e:
        log(f"FAIL: prepare_resolution_image({BOOT_MODE}): {e}")
        return False

    # Non-destructive fault cases share one boot.
    sess = _boot_session(firmware, copy800, results_dir / "fault", log)
    if sess is None:
        return False
    try:
        failures += _snapshot_case(sess, log)
        failures += _mismatch_case(sess, log)
        failures += _drain_case(sess, log)
        failures += _terminal_enomem_case(sess, log)
        failures += _client_eagain_case(sess, log)
        failures += _pty_watch_case(sess, log)
        failures += _capacity_cli_case(sess, log)
    finally:
        sess.close()

    # Destructive cases each get their own boot, ordered last.
    failures += _no_bga_case(firmware, image, results_dir, log, test_build=True)
    failures += _mmap_case(firmware, copy800, results_dir, log)
    failures += _rollback_case(firmware, copy800, results_dir, log)

    return _report("test", failures, log)


# ── live orchestration helpers ──────────────────────────────────

def _no_bga_case(firmware: Path, image: Path, results_dir: Path, log,
                 test_build: bool) -> list:
    """Unsupported display device with a trusted GOP (spec §8.2 item 6).

    Boots on ``NO_BGA_DEVICE`` (cirrus): OVMF provides a GOP framebuffer, but
    the kernel finds no BGA backend, so SET and enumerate return ENODEV while
    the legacy read / surrender path and the serial shell survive.  With the
    test build the read (GET_STATE) and FBIOSURRENDER are asserted directly.
    """
    fail: list = []
    try:
        copy = _private_copy(image, results_dir / "disk-nobga.img")
    except ImageIsolationError as e:
        return [f"nobga_prepare: {e}"]

    sess = ResolutionSession(firmware, copy, results_dir / "nobga",
                             display_device=list(NO_BGA_DEVICE))
    try:
        sess.start()
    except QmpError as e:
        log(f"FAIL: {e}")
        return ["nobga_qmp_failed"]
    try:
        if not sess.wait_for(re.escape(PROMPT), timeout=BOOT_TIMEOUT):
            fail.append("nobga_no_shell")
            return fail

        out = _guest(sess, "/bin/setres -l")
        if "ENODEV" not in out:
            fail.append("nobga_enumerate_not_enodev")
        out = _guest(sess, "/bin/setres 1280x720")
        if "ENODEV" not in out:
            fail.append("nobga_set_not_enodev")

        # A trusted GOP framebuffer must still exist (real surface, sane size).
        surf = sess.screen("nobga")
        if surf is None or not (640 <= surf.width <= 1920 and 480 <= surf.height <= 1200):
            fail.append("nobga_no_trusted_gop_surface")

        # Legacy read / surrender are still usable (test build has the helper).
        if test_build:
            st = parse_resolution_get(_guest(sess, "/bin/test_resolution get"))
            if st is None or st["width"] == 0 or st["height"] == 0:
                fail.append("nobga_read_query_failed")
            sur = _guest(sess, "/bin/test_resolution surrender")
            if "SURRENDER rc=0" not in sur:
                fail.append("nobga_surrender_failed")

        # The serial session survives.
        out = _guest(sess, "echo RES_NOBGA_ALIVE")
        if "RES_NOBGA_ALIVE" not in out:
            fail.append("nobga_session_dead")
    finally:
        sess.close()
    return fail


def _report(flavor: str, failures: list, log) -> bool:
    for f in failures:
        log(f"FAIL: {f}")
    if failures:
        log(f"resolution {flavor} suite: {len(failures)} failure(s)")
        return False
    log(f"resolution {flavor} suite: PASS")
    return True


def _private_copy(source: Path, dest: Path) -> Path:
    """Copy an image into a per-run private path, asserting the source is
    byte-identical afterwards (never boot the profile's real image)."""
    before = _sha256(source)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, dest)
    if _sha256(source) != before:
        raise ImageIsolationError("source image changed during copy")
    return dest


def _boot_session(firmware: Path, image: Path, results_dir: Path, log):
    sess = ResolutionSession(firmware, image, results_dir)
    try:
        sess.start()
    except QmpError as e:
        log(f"FAIL: {e}")
        return None
    if not sess.wait_for(re.escape(PROMPT), timeout=BOOT_TIMEOUT):
        log("FAIL: no shell prompt")
        log("serial tail:\n" + sess.output()[-800:])
        sess.close()
        return None
    return sess


def _idle_switch_case(sess, log) -> list:
    """Idle background setres -> new frame <1s; PID/PTY stable; Ctrl-C works."""
    fail: list = []
    pid0, pty0 = sess.shell_pid(), sess.pty()
    t0 = time.time()
    sess.send_line("setres 1280x720 &")
    ok = False
    elapsed = None
    while time.time() - t0 < 2.5:
        surf = sess.screen("idle")
        if (surf.width, surf.height) == (1280, 720):
            ok = True
            elapsed = time.time() - t0
            break
        time.sleep(0.02)
    if not ok:
        fail.append("idle_switch_no_frame")
    elif elapsed is not None and elapsed > 1.0:
        fail.append(f"idle_switch_slow {elapsed:.2f}s")
    r = check_pid_stable(pid0, sess.shell_pid())
    if r:
        fail.append("idle_" + r)
    if pty0 != sess.pty():
        fail.append(f"idle_pty_changed {pty0}->{sess.pty()}")

    # Ctrl-C must still reach ash and yield a fresh prompt.
    mark = sess.mark()
    sess._send("\x03")
    if not sess.wait_for(re.escape(PROMPT), timeout=8, start=mark):
        fail.append("ctrl_c_no_prompt")
    if sess.shell_pid() != pid0:
        fail.append("ctrl_c_shell_changed")
    sess.run(f"setres {BOOT_MODE}")
    return fail


def _prod_cli_case(sess, log) -> list:
    """Production `/bin/setres` CLI presence and error contract."""
    fail: list = []
    out = _guest(sess, "/bin/setres -h")
    if "usage: setres" not in out:
        fail.append("prod_setres_help_missing")
    out = _guest(sess, "/bin/setres 999x999")
    if "not a supported mode" not in out:
        fail.append("prod_setres_unsupported_not_reported")
    return fail


def _start_client(sess) -> bool:
    """Background a real test_lvgl client that reports its exit status.

    The client's own exit code is emitted by the guest shell wrapper
    (``RES_LVGL_EXIT=$?``) so the runner never has to guess a host-side PID.
    Returns True once the client has a live view and is in its present loop.
    """
    sess.send_line("{ /bin/test_lvgl wait; echo RES_LVGL_EXIT=$?; } &")
    return sess.wait_for(r"LVGL compatibility smoke test succeeded", timeout=25)


def _client_estale_case(sess, log) -> list:
    """A real long-running gfx client (test_lvgl) survives start, then a real
    layout switch invalidates its view: it must release its graphics view and
    exit non-zero with the stale-view diagnostic (spec §8.2 item 9).

    Runs a *real* app in the guest (not the host stand-in loop) and switches
    the mode for real, so the client observes ESTALE from libgfx.  The client
    exit status is captured from the guest shell itself (background block),
    not a host-side PID guess.  Runs last among the shared-boot cases because
    the client leaves the terminal's alt screen active.
    """
    fail: list = []
    if not _start_client(sess):
        return ["client_estale_never_presenting"]

    before = sess.mark()
    _guest(sess, "setres 1280x720")

    # The client must report the stale view ...
    if not sess.wait_for(r"display mode changed, restart the app", timeout=10,
                         start=before):
        fail.append("client_estale_no_diagnostic")
    # ... and exit non-zero (captured by the guest shell wrapper).
    sess.wait_for(r"RES_LVGL_EXIT=\d+", timeout=10, start=before)
    em = re.search(r"RES_LVGL_EXIT=(-?\d+)", sess.output()[len(before):])
    if em is None:
        fail.append("client_estale_no_exit_status")
    elif int(em.group(1)) == 0:
        fail.append("client_estale_exited_zero")

    # The session/terminal must survive the client's exit.
    if sess.shell_pid() is None:
        fail.append("client_estale_session_dead")
    if "RES_CLIENT_ALIVE" not in _guest(sess, "echo RES_CLIENT_ALIVE"):
        fail.append("client_estale_shell_alive")

    # Leave the terminal's alt screen (the client died holding it) and restore.
    sess.send_line("printf '\\033[?1049l'")
    _guest(sess, "setres 800x600")
    return fail


def _off_whitelist_case(firmware: Path, image: Path, results_dir: Path, log) -> list:
    """Boot an out-of-table mode and verify GET_STATE/CLI report it."""
    fail: list = []
    for mode in (OFF_WHITELIST_MODE,) + OFF_WHITELIST_FALLBACKS:
        copy = results_dir / f"disk-{mode}.img"
        try:
            prepare_resolution_image(image, mode, copy)
        except ImageIsolationError as e:
            fail.append(f"off_whitelist_prepare {mode}: {e}")
            continue
        sess = _boot_session(firmware, copy, results_dir / f"offwl-{mode}", log)
        if sess is None:
            continue
        try:
            ok, sub, envblock = evaluate_off_whitelist(sess)
        finally:
            sess.close()
        if envblock:
            log(f"ENVIRONMENT BLOCK: {envblock}")
            continue
        if ok:
            log(f"off-whitelist boot mode verified at {mode}")
            return fail
        fail += sub
        return fail
    log("ENVIRONMENT BLOCK: no off-table GOP mode provided by this firmware "
        f"(tried {OFF_WHITELIST_MODE} + {', '.join(OFF_WHITELIST_FALLBACKS)})")
    return fail


def _isolation_case(firmware: Path, image: Path, copy800: Path,
                    results_dir: Path, log) -> list:
    """original -> 800 copy -> original: source sha256 and modes unchanged."""
    fail: list = []
    before = _sha256(image)
    modes: dict = {}
    for label, img in (("orig1", image), ("copy800", copy800), ("orig2", image)):
        sess = _boot_session(firmware, img, results_dir / f"iso-{label}", log)
        if sess is None:
            fail.append(f"iso_{label}_boot_failed")
            continue
        try:
            modes[label] = sess.mode()
        finally:
            sess.close()
    if modes.get("orig1") != modes.get("orig2"):
        fail.append(f"iso_orig_mode_drift {modes.get('orig1')} vs {modes.get('orig2')}")
    if _sha256(image) != before:
        fail.append("iso_source_changed")
    if modes.get("copy800") != (800, 600):
        fail.append(f"iso_copy800_mode {modes.get('copy800')}")
    return fail


# ── test-build fault cases ──────────────────────────────────────

def _guest(sess, cmd, until=PROMPT, timeout=20) -> str:
    return sess.run(cmd, until=until, timeout=timeout)


def _snapshot_case(sess, log) -> list:
    """DISPI register snapshot must be consistent with GET_STATE."""
    fail: list = []
    text = _guest(sess, "/bin/test_resolution snapshot")
    st = parse_resolution_get(text)
    if st is None:
        return ["snapshot_unparsable"]
    regs = re.search(r"REGS((?:\s+\d+){11})", text)
    if not regs:
        return ["snapshot_regs_missing"]
    r = [int(x) for x in regs.group(1).split()]
    if r[1] != st["width"] or r[2] != st["height"]:
        fail.append(f"snapshot_regs_dims {r[1]}x{r[2]} != {st['width']}x{st['height']}")
    if r[3] != 32:
        fail.append(f"snapshot_bpp {r[3]} != 32")
    if r[8] != 0 or r[9] != 0:
        fail.append(f"snapshot_offsets {r[8]},{r[9]} != 0")
    return fail


def _mismatch_case(sess, log) -> list:
    """Fault the new-mode readback: SET -> EIO with old layout restored."""
    fail: list = []
    before = _guest(sess, "/bin/test_resolution get")
    st0 = parse_resolution_get(before)
    text = _guest(sess, "/bin/test_resolution arm-mismatch 0 1 1024 768")
    res = parse_test_set(text)
    if res is None:
        return ["mismatch_set_unparsable"]
    if res["rc"] >= 0 or res["errno"] != 5:  # EIO
        fail.append(f"mismatch_not_eio rc={res['rc']} errno={res['errno']}")
    after = _guest(sess, "/bin/test_resolution get")
    st1 = parse_resolution_get(after)
    if st0 and st1:
        if (st1["width"], st1["height"]) != (st0["width"], st0["height"]):
            fail.append(f"mismatch_layout_not_restored {st1['width']}x{st1['height']}")
        if st1["generation"] <= st0["generation"]:
            fail.append("mismatch_generation_unchanged")
    surf = sess.screen("mismatch")
    r = check_surface_dims((st0["width"], st0["height"]), surf) if st0 else None
    if r:
        fail.append("mismatch_" + r)
    return fail


def _drain_case(sess, log) -> list:
    """Held writer lease -> SET times out (-EBUSY); lease then released."""
    fail: list = []
    sess.send_line("/bin/test_resolution hold 4 &")
    if not sess.wait_for(r"HOLD ok", timeout=8):
        return ["drain_hold_not_acquired"]
    out = _guest(sess, f"setres 1280x720")
    if "EBUSY" not in out:
        fail.append(f"drain_no_ebusy: {out.strip()[-120:]}")
    if not sess.wait_for(r"RELEASE ok", timeout=8):
        fail.append("drain_release_missing")
    if sess.mode() != (800, 600):
        fail.append(f"drain_mode_changed {sess.mode()}")
    return fail


def _terminal_enomem_case(sess, log) -> list:
    """Faulted terminal resize prepare recovers and keeps the session.

    The terminal PID is discovered from /proc (the shell's parent), never
    hard-coded — a wrong PID would leave the fault unconsumed.  The guest
    ``terminal-status`` consumed-delta proves the armed ENOMEM was actually
    taken by the terminal, so the case cannot pass vacuously.
    """
    fail: list = []
    pid0 = sess.shell_pid()
    term_pid = sess.terminal_pid()
    if not term_pid or term_pid <= 0:
        return ["term_enomem_no_terminal_pid"]
    if term_pid == pid0:
        return [f"term_enomem_terminal_is_shell {term_pid}"]
    log(f"terminal-ENOMEM target PID={term_pid} (shell PID={pid0})")

    baseline = parse_term_status(_guest(sess, "/bin/test_resolution terminal-status"))
    if baseline is None:
        return ["term_enomem_status_unparsable"]
    c0 = baseline["consumed"]

    arm = _guest(sess, f"/bin/test_resolution arm-terminal {term_pid} 7")
    if "ARM_TERMINAL" not in arm or "ok" not in arm:
        return [f"term_enomem_arm_failed: {arm.strip()[-120:]}"]

    _guest(sess, "setres 1280x720")
    if sess.mode() != (1280, 720):
        fail.append(f"term_enomem_mode {sess.mode()}")
    # The terminal must recover within a retry window and keep ash alive.
    ok = False
    deadline = time.time() + 6
    while time.time() < deadline:
        surf = sess.screen("term-recover")
        if (surf.width, surf.height) == (1280, 720) and not surf.is_blank():
            ok = True
            break
        time.sleep(0.2)
    if not ok:
        fail.append("term_enomem_no_recovery_frame")

    # The fault must have been *consumed* by the terminal: a wrong PID (or a
    # broken hook) leaves the consumed count unchanged and pending set.
    after = parse_term_status(_guest(sess, "/bin/test_resolution terminal-status"))
    if after is None:
        fail.append("term_enomem_status_after_unparsable")
    else:
        if after["consumed"] != c0 + 1:
            fail.append(f"term_enomem_not_consumed c0={c0} c1={after['consumed']}")
        if after["pending"] != 0:
            fail.append("term_enomem_fault_still_pending")
            # Do not leak a stuck fault into later cases.
            _guest(sess, f"/bin/test_resolution consume-terminal {term_pid}")

    final = sess.shell_pid()
    if final != pid0:
        fail.append(f"term_enomem_shell_changed {pid0}->{final}")
    _guest(sess, "setres 800x600")
    return fail


def _client_eagain_case(sess, log) -> list:
    """A real gfx client (test_lvgl) hits a transient EAGAIN while a mode SET
    drains behind a held writer lease, stays alive, and keeps its view valid
    (spec §8.2 item 5).

    The held lease makes ``fb_transition_begin`` set ``transitioning`` for a
    full ~1 s before its drain times out (-EBUSY); every gfx present in that
    window fails EAGAIN.  The generation never changes, so a present after the
    window must succeed again: the client is proven to still hold a live view
    by driving a *real* switch afterwards, which now yields the ESTALE
    contract.
    """
    fail: list = []
    sess.send_line("/bin/test_resolution hold 4 &")
    if not sess.wait_for(r"HOLD ok", timeout=8):
        return ["eagain_hold_not_acquired"]

    if not _start_client(sess):
        return ["eagain_client_never_presenting"]
    # Everything examined from here is strictly newer than the client start,
    # so a stale RELEASE/clients marker from an earlier case cannot match.
    mark = sess.mark()

    # This SET must block in the drain and time out: generation unchanged.
    out = _guest(sess, "setres 1280x720")
    if "EBUSY" not in out:
        fail.append(f"eagain_set_not_ebusy: {out.strip()[-120:]}")

    # The client must NOT have exited on the transient failure, and must not
    # have taken the stale-view path (which only a generation change triggers).
    seen = sess.output()[len(mark):]
    if re.search(r"RES_LVGL_EXIT=", seen):
        fail.append("eagain_client_exited_on_transient")
    if "display mode changed, restart the app" in seen:
        fail.append("eagain_client_estale_during_transient")

    # The lease releases after hold expires.  A *real* switch now must yield
    # ESTALE: only possible while the client still holds its (unchanged,
    # no-longer-stale) view, so this proves it re-presented after the EAGAIN.
    if not sess.wait_for(r"RELEASE ok", timeout=10, start=mark):
        fail.append("eagain_release_missing")
    before = sess.mark()
    _guest(sess, "setres 1280x720")
    if not sess.wait_for(r"display mode changed, restart the app", timeout=10,
                         start=before):
        fail.append("eagain_client_view_lost")
    sess.wait_for(r"RES_LVGL_EXIT=\d+", timeout=10, start=before)
    em = re.search(r"RES_LVGL_EXIT=(-?\d+)", sess.output()[len(before):])
    if em is None:
        fail.append("eagain_client_no_estale_exit")
    elif int(em.group(1)) == 0:
        fail.append("eagain_client_estale_exited_zero")

    sess.send_line("printf '\\033[?1049l'")
    _guest(sess, "setres 800x600")
    return fail


def _pty_watch_case(sess, log) -> list:
    """Foreground program observes SIGWINCH + four winsize fields.

    The observer runs in the foreground (so fd 0 is the PTY slave) and drives
    the mode switch itself, then reports the master-delivered SIGWINCH and the
    TIOCGWINSZ geometry both before and after the switch.
    """
    fail: list = []
    # Foreground: run() waits for the trailing prompt, so the whole 6 s watch
    # window (and every JSON event) is captured before returning.
    out = _guest(sess, "/bin/test_resolution pty-watch 6 1280x720", timeout=25)
    events = parse_resjson(out)
    if not events:
        return ["pty_watch_not_started"]
    init = [e for e in events if e.get("sigwinch") == 0]
    win = [e for e in events if e.get("sigwinch") == 1]
    if not init:
        fail.append("pty_watch_no_initial_event")
    elif init[-1].get("xpixel") != 800 or init[-1].get("ypixel") != 600:
        fail.append(f"pty_watch_initial_pixel "
                    f"{init[-1].get('xpixel')}x{init[-1].get('ypixel')}")
    if not win:
        fail.append("pty_watch_no_sigwinch")
    else:
        ev = win[-1]
        for k in ("row", "col", "xpixel", "ypixel"):
            if not isinstance(ev.get(k), int) or ev.get(k) <= 0:
                fail.append(f"pty_watch_bad_field {k}={ev.get(k)}")
        if ev.get("xpixel") != 1280 or ev.get("ypixel") != 720:
            fail.append(f"pty_watch_pixel {ev.get('xpixel')}x{ev.get('ypixel')}")
    _guest(sess, "setres 800x600")
    return fail


def _capacity_cli_case(sess, log) -> list:
    """CLI/argument errors from the guest (host covers the fine grain)."""
    fail: list = []
    out = _guest(sess, "/bin/setres 999x999")
    if "not a supported mode" not in out:
        fail.append("cli_unsupported_mode_not_reported")
    out = _guest(sess, "/bin/setres -h")
    if "usage: setres" not in out:
        fail.append("cli_help_missing")
    out = _guest(sess, "/bin/test_resolution set 1 1")
    res = parse_test_set(out)
    if res is None or res["rc"] >= 0:
        fail.append("capacity_invalid_set_not_rejected")
    return fail


def _mmap_case(firmware: Path, image: Path, results_dir: Path, log) -> list:
    """A raw /dev/fb mmap makes every layout SET fail with EBUSY (sticky)."""
    fail: list = []
    sess = _boot_session(firmware, image, results_dir / "mmap", log)
    if sess is None:
        return ["mmap_boot_failed"]
    try:
        out = _guest(sess, "/bin/test_resolution mmap")
        if "RESJSON" not in out or '"op": "mmap"' not in out:
            fail.append(f"mmap_helper_failed: {out.strip()[-120:]}")
        out = _guest(sess, "setres 1280x720")
        if "EBUSY" not in out:
            fail.append("mmap_set_not_ebusy")
        if sess.mode() != (800, 600):
            fail.append(f"mmap_mode_changed {sess.mode()}")
    finally:
        sess.close()
    return fail


def _rollback_case(firmware: Path, image: Path, results_dir: Path, log) -> list:
    """Rollback-failure fault -> EIO/serial-only; run LAST (needs a reboot)."""
    fail: list = []
    sess = _boot_session(firmware, image, results_dir / "rollback", log)
    if sess is None:
        return ["rollback_boot_failed"]
    try:
        text = _guest(sess, "/bin/test_resolution arm-rollback 0 1 1024 768")
        res = parse_test_set(text)
        if res is None or res["rc"] >= 0:
            fail.append(f"rollback_set_expected_error: {text.strip()[-120:]}")
        # The session must survive in serial-only mode: the shell still runs
        # (a plain, non-fb command), while fb/gfx queries now report EIO.
        out = _guest(sess, "echo RES_ROLLBACK_ALIVE")
        if "RES_ROLLBACK_ALIVE" not in out:
            fail.append("rollback_serial_session_dead")
        out2 = _guest(sess, "/bin/setres 1280x720")
        if "errno=EIO" not in out2:
            fail.append("rollback_set_not_eio")
    finally:
        sess.close()
    return fail


def test_resolution(tester=None) -> bool:
    """run_test.py dispatcher entry: returns True on success."""
    log = print
    env = dict(os.environ)
    mix = check_fault_config_isolated(env)
    if mix:
        log(f"FAIL: resolution suite launched with forbidden flags ({mix})")
        return False

    firmware = _env_path("OVMF_FIRMWARE")
    image = _env_path("DISK_IMG") or (Path(tester.disk_img) if tester else None)
    if not firmware or not firmware.is_file():
        log("FAIL: OVMF_FIRMWARE is not set to a readable file")
        return False
    if not image or not Path(image).is_file():
        log("FAIL: DISK_IMG is not set to a readable file")
        return False

    # RunArchive integration (spec §7.2): when OS01_BUILD_DIR is set
    # the suite creates an archive and writes result.json after the
    # run.  When unset (host unit tests), fall back to the legacy
    # results_dir.
    archive = None
    build_dir = env.get("OS01_BUILD_DIR")
    if build_dir:
        try:
            from qemutests.harness.result import RunArchive
            archive = RunArchive.create(
                build_dir=Path(build_dir), suite="resolution")
        except Exception:
            archive = None
    results_dir = _results_dir(Path(image))
    if archive is not None:
        # Make the archive's run_dir the canonical results_dir for
        # this run so the suite writes its logs into the archive.
        results_dir = archive.run_dir
    log(f"resolution suite: firmware={firmware} image={image} results={results_dir}")
    ok = False
    try:
        if str(env.get("FB_RESOLUTION_TEST", "0")) == "1":
            ok = run_test_suite(firmware, Path(image), results_dir, log)
        else:
            ok = run_production_suite(firmware, Path(image), results_dir, log)
    finally:
        if archive is not None:
            try:
                from qemutests.harness.result import RunReport
                report = RunReport(
                    schema_version=1,
                    run_id=archive.run_dir.name,
                    git_revision="unknown",
                    git_dirty=False,
                    profile=env.get("OS01_PROFILE", "default"),
                    suite="resolution",
                    request=None,
                    declared_ids=None,
                    observed_ids=None,
                    argv=[],
                    cpu_count=2,
                    memory_mib=512,
                    tool_versions={},
                    firmware_path=str(firmware),
                    firmware_sha256_before=None,
                    firmware_sha256_after=None,
                    image_path=str(image),
                    image_sha256_before=None,
                    image_sha256_after=None,
                    utc_started_at=datetime.now(timezone.utc).isoformat(),
                    duration_s=0.0,
                    runner_exit_code=0 if ok else 1,
                    child_exit_code=None,
                    stopped_by_runner=False,
                    status="PASS" if ok else "FAIL",
                    count_unit="case",
                    outcomes=[],
                    stdout_log=str(archive.run_dir / "stdout.log"),
                    stderr_log=str(archive.run_dir / "stderr.log"),
                )
                archive.write(report)
            except Exception as exc:
                log(f"  [run_test] warning: failed to write RunReport: {exc}")
    return ok


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="OS01 resolution-switcher runner")
    parser.add_argument("case", nargs="?", default="all",
                        help="all | production | test | round-trip | off-whitelist")
    args = parser.parse_args(argv)
    ok = test_resolution(None)
    return 0 if ok else 1


# ═══════════════════════════════════════════════════════════════
#  Host unit tests (no QEMU) — Step 1 RED target
# ═══════════════════════════════════════════════════════════════

class FakeSurface(Surface):
    pass


def _mk_surface(w: int, h: int, fill: tuple[int, int, int] = (0, 0, 0)) -> Surface:
    return Surface(w, h, bytes(fill) * (w * h))


class FakeSession:
    """Scripted stand-in for ResolutionSession for host unit tests."""

    def __init__(self):
        self.fail_boot = False
        self.echo_paints = True          # False -> echo but no picture
        self.mode_now = (800, 600)
        self.gen = 1
        self.pid = 3
        self.pty_path = "/dev/pts0"
        self._frames = {}
        self.calls = []

    def screen(self, label="shot"):
        if self.fail_boot and label == "boot":
            return _mk_surface(640, 480)
        if label == "pre-echo":
            return _mk_surface(*self.mode_now, fill=(1, 1, 1))
        if label == "post-echo":
            if self.echo_paints:
                return _mk_surface(*self.mode_now, fill=(2, 2, 2))
            return _mk_surface(self.mode_now[0], self.mode_now[1])
        if label in ("same-pre", "same-post"):
            return _mk_surface(*self.mode_now, fill=(2, 2, 2))
        return _mk_surface(*self.mode_now, fill=(2, 2, 2))

    def mode(self):
        return self.mode_now

    def generation(self):
        return self.gen

    def shell_pid(self):
        return self.pid

    def pty(self):
        return self.pty_path

    def run(self, cmd, until=PROMPT, timeout=5):
        self.calls.append(cmd)
        if cmd.startswith("echo RES_ECHO_PROBE"):
            return "RES_ECHO_PROBE\n# "
        if cmd.startswith("setres "):
            spec = cmd.split()[1]
            w, h = (int(x) for x in spec.split("x"))
            if (w, h) != self.mode_now:
                self.mode_now = (w, h)
                self.gen += 1
        return "# "


class PrepareImageIsolationTest(unittest.TestCase):
    """test_prepare_image_isolation (Step 1)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="resetest-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _make_image(self, name="disk.img", esp=True, corrupt=False, fat=True) -> Path:
        esp_lba = 2048
        esp_sectors = 8192
        size = (esp_lba + esp_sectors) * 512
        img = self.tmp / name
        with open(img, "wb") as fp:
            fp.truncate(size)
        with open(img, "r+b") as fp:
            fp.seek(512)
            hdr = bytearray(512)
            hdr[0:8] = b"EFI PART"
            struct.pack_into("<I", hdr, 8, 0x00010000)     # revision
            struct.pack_into("<I", hdr, 12, 92)            # header size
            struct.pack_into("<Q", hdr, 24, 1)             # current LBA
            struct.pack_into("<Q", hdr, 32, 1)             # backup LBA (unused)
            struct.pack_into("<Q", hdr, 72, 2)             # partition array LBA
            struct.pack_into("<I", hdr, 80, 128)           # entries
            struct.pack_into("<I", hdr, 84, 128)           # entry size
            if corrupt:
                hdr[0:8] = b"XXXXXXXX"
            struct.pack_into("<I", hdr, 16, 0)
            crc = binascii.crc32(bytes(hdr[:92])) & 0xFFFFFFFF
            struct.pack_into("<I", hdr, 16, crc)
            fp.write(hdr)
            fp.seek(2 * 512)
            entries = bytearray(128 * 128)
            if esp:
                raw = bytes.fromhex(
                    "28732ac11ff8d211ba4b00a0c93ec93b")  # EFI System Partition
                entries[0:16] = raw
                struct.pack_into("<Q", entries, 32, esp_lba)
                struct.pack_into("<Q", entries, 40, esp_lba + esp_sectors - 1)
            fp.write(entries)
        if fat and not corrupt:
            subprocess.run(
                ["mformat", "-i", f"{img}@@{esp_lba * 512}", "::"],
                check=True, capture_output=True)
        return img

    def test_source_sha256_unchanged(self):
        src = self._make_image()
        before = _sha256(src)
        dst = self.tmp / "copy.img"
        prepare_resolution_image(src, "800x600", dst)
        self.assertEqual(before, _sha256(src))

    def test_config_written_only_to_destination(self):
        src = self._make_image()
        before = _sha256(src)
        dst = self.tmp / "copy.img"
        prepare_resolution_image(src, "800x600", dst)
        # The destination ESP now carries the exact config bytes ...
        out = subprocess.run(
            ["mtype", "-i", f"{dst}@@{2048 * 512}", "::/config.txt"],
            capture_output=True)
        self.assertEqual(out.stdout, b"resolution 800x600\n")
        # ... and the source is byte-identical (no in-place pollution).
        self.assertEqual(before, _sha256(src))

    def test_esp_offset_not_hardcoded(self):
        img = self._make_image()
        self.assertEqual(parse_gpt_esp_offset(img), 2048 * 512)

    def test_corrupt_gpt_fails(self):
        img = self._make_image(corrupt=True)
        with self.assertRaises(ImageIsolationError):
            parse_gpt_esp_offset(img)

    def test_missing_esp_fails(self):
        img = self._make_image(esp=False)
        with self.assertRaises(ImageIsolationError):
            parse_gpt_esp_offset(img)

    def test_mcopy_nonzero_fails(self):
        # Valid GPT + ESP offset, but the partition is not a FAT volume, so
        # mcopy fails -> prepare must raise, never fall back to a default size.
        img = self._make_image(fat=False)
        with self.assertRaises(ImageIsolationError):
            prepare_resolution_image(img, "800x600", self.tmp / "copy.img")

    def test_config_exact_bytes(self):
        self.assertEqual(_config_bytes("800x600"), b"resolution 800x600\n")


class CheckHelpersTest(unittest.TestCase):
    """surface/session assertion failures (Step 1)."""

    def test_surface_size_mismatch_fails(self):
        s = _mk_surface(640, 480, (1, 1, 1))
        self.assertIsNotNone(check_surface_dims((800, 600), s))

    def test_sentinel_mismatch_fails(self):
        s = _mk_surface(800, 600, (0, 0, 0))
        self.assertIsNotNone(check_sentinel(s, 0, 0, (255, 0, 0)))

    def test_blank_surface_fails_picture(self):
        self.assertIsNotNone(check_picture_present(_mk_surface(800, 600)))

    def test_pid_change_fails(self):
        self.assertIsNotNone(check_pid_stable(3, 4))

    def test_fault_config_mixing_fails(self):
        self.assertIsNotNone(check_fault_config_isolated({"OS01_SYSTEST": "1"}))
        self.assertIsNotNone(check_fault_config_isolated({"KERNEL_SELFTEST": "1"}))
        self.assertIsNone(check_fault_config_isolated({}))

    def test_round_trip_pass(self):
        self.assertEqual(evaluate_round_trip(FakeSession()), [])

    def test_round_trip_surface_fail(self):
        fs = FakeSession()
        fs.fail_boot = True
        self.assertTrue(evaluate_round_trip(fs))

    def test_round_trip_echo_without_picture(self):
        fs = FakeSession()
        fs.echo_paints = False
        self.assertIn("echo_without_picture", evaluate_round_trip(fs))

    def test_round_trip_pid_change(self):
        fs = FakeSession()
        seq = iter([3, 4])          # pid0 vs final
        fs.shell_pid = lambda: next(seq)
        self.assertTrue(any("pid_changed" in f for f in evaluate_round_trip(fs)))


class ParseHelpersTest(unittest.TestCase):
    def test_parse_setres_list(self):
        text = ("Available modes (capacity 16, 9 listed):\n"
                "  640x480 bpp=32\n  800x600 bpp=32\n"
                "Current: 800x600 (CURRENT) bpp=32 stride=3200\n"
                "generation: 2\n# ")
        st = parse_setres_list(text)
        self.assertEqual(st["current"], (800, 600))
        self.assertEqual(st["generation"], 2)
        self.assertIn((640, 480), st["modes"])

    def test_parse_test_set(self):
        st = parse_test_set("SET 1024x768 rc=-1 errno=5\n")
        self.assertEqual((st["width"], st["height"]), (1024, 768))
        self.assertEqual(st["errno"], 5)

    def test_parse_term_status(self):
        st = parse_term_status("TERM_STATUS pending=0 consumed=3\n")
        self.assertEqual(st, {"pending": 0, "consumed": 3})
        self.assertIsNone(parse_term_status("nope"))

    def test_parse_ppid(self):
        self.assertEqual(parse_ppid("Name:\ttask\nPid:\t42\nPPid:\t7\n"), 7)
        self.assertIsNone(parse_ppid("no parent here"))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "unittest":
        sys.argv = sys.argv[:1]
        unittest.main(module=__name__, exit=True)
    else:
        sys.exit(main())
