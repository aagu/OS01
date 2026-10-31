#!/usr/bin/env python3
"""OS01 aarch64 boot-logo acceptance suite.

Boots the aarch64 UEFI image with ``-device ramfb`` so the edk2 firmware
exposes a GOP framebuffer, lets the kernel draw the OS01 wordmark, and
asserts the wordmark pixels on a QMP screendump.

Checks
------
fb-visible : the framebuffer is observable at all — the existing
              "[fb] aarch64 color_printk active" banner is on screen.
logo       : the OS01 wordmark is drawn with the EXACT expected pixel
              counts (the glyph bitmaps are deterministic), the arch
              caption paints the EXACT number of pixels its glyphs
              imply, and the ink pixels stay inside the two ink glyph
              cells.
no-fb      : without ramfb the kernel skips the logo instead of crashing,
              and still reaches its idle loop.

Every expected pixel count is DERIVED from the repo at test time — the
wordmark bitmaps from kernel/driver/logo.c, the caption glyphs from
kernel/driver/font.psf — so a changed or misspelled caption cannot pass.

Exit codes: 0 = PASS, 1 = FAIL, 2 = configuration/environment ERROR.

``--self-test`` runs a QEMU-free self-test of the suite's own pure
helpers (geometry, PPM parsing, RGB counting) against synthetic data and
exits 0.  It needs neither ``--firmware`` nor ``--image``.
"""

from __future__ import annotations

import argparse
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
from datetime import datetime, timezone
from functools import lru_cache
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover — environment ERROR, not a FAIL
    ProcessSession = None  # type: ignore[assignment]

try:
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover — archiving is optional evidence
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]

SUITE = "aarch64-boot-logo"

# ── Logo constants mirrored from kernel/driver/logo.c ──────────
LOGO_INK = (0xE8, 0xF0, 0xF7)       # 0x00e8f0f7
LOGO_ACCENT = (0x3B, 0xD6, 0xC6)    # 0x003bd6c6
LOGO_MUTED = (0x68, 0x78, 0x86)     # 0x00687886
BANNER_BLUE = (0x00, 0x00, 0xFF)    # color_printk(WHITE, BLUE, ...) background

# The wordmark is drawn from the driver's OWN 5x7 bitmaps in
# kernel/driver/logo.c — NOT from the PSF2 font ("drawn as solid geometry,
# without font assets").  The per-glyph set-bit counts are parsed from
# that source at test time (see ``logo_glyph_bits``), so the expected ink
# and accent pixel counts cannot drift away from the drawing.
INK_GLYPHS = 2
ACCENT_GLYPHS = 2

FONT_W, FONT_H = 8, 16              # PSF2 metrics of kernel/driver/font.psf
# The caption the kernel draws under the wordmark (the aarch64 kernel's
# ARCH_BOOT_LOGO_LABEL): "aarch64" + 2 spaces + "/" + 2 spaces + "KERNEL"
# is 18 characters — a hard-coded 17 would drop the last character cell
# and let a truncated caption pass.
LOGO_LABEL = "aarch64  /  KERNEL"
LABEL_LEN = len(LOGO_LABEL)

# The repo files the expectations are derived from.
FONT_PATH = _ROOT / "kernel" / "driver" / "font.psf"
LOGO_SOURCE_PATH = _ROOT / "kernel" / "driver" / "logo.c"

LOGO_DRAWN_MARKER = "[logo] boot logo drawn"
LOGO_SKIPPED_MARKER = "[logo] skipped: no framebuffer"
FB_READY_MARKER = "[fb] frame buffer remap succeed"
FB_ABSENT_MARKER = "[fb] no framebuffer in handoff"
IDLE_MARKER = "[sched] idle-ready"

# Bound of the optional SECONDARY wait (see ``_await_marker``).  The kernel
# prints the logo marker BEFORE the [fb] banner marker
# (kernel/arch/aarch64/boot/main.c:389-399), so the logo marker is already
# buffered when the primary wait returns: this wait is a no-op today and
# only guards against a future reordering of the two markers.  The wait is
# bounded, and a missing marker stays a checker failure — never a timeout.
SECONDARY_WAIT_BOUND_S = 5.0


class Psf2Font:
    """The PSF2 font the kernel draws captions with (kernel/driver/font.psf).

    The layout mirrors the kernel's own ``psf2_t``
    (kernel/include/driver/font.h): eight little-endian u32 header fields,
    then the glyph table.  The kernel renders one glyph as ``height`` bytes
    starting at ``headersize + index * bytesperglyph`` and tests the top
    ``width`` bits of every byte, so the number of pixels a character cell
    paints is the popcount of those bytes.

    The magic is deliberately NOT checked: this repo stores the PSF2 magic
    big-endian (0x864ab572), so a "PSF2" prefix test would reject the real
    file.
    """

    def __init__(self, data: bytes) -> None:
        if len(data) < 32:
            raise RuntimeError(
                f"font.psf is shorter than its 32-byte header ({len(data)} "
                "bytes)")
        (self.magic, self.version, self.headersize, self.flags,
         self.numglyph, self.bytesperglyph, self.height,
         self.width) = struct.unpack_from("<8I", data, 0)
        table_size = self.numglyph * self.bytesperglyph
        table = data[self.headersize:self.headersize + table_size]
        if len(table) != table_size:
            raise RuntimeError(
                f"font.psf glyph table is truncated: expected {table_size} "
                f"bytes, got {len(table)}")
        self._table = table
        # The kernel tests the top ``width`` bits of every row byte.
        self._mask = 0xFF if self.width >= 8 else (0xFF << (8 - self.width)) & 0xFF

    def glyph_bytes(self, index: int) -> bytes:
        """The raw bytes of one glyph entry."""
        start = index * self.bytesperglyph
        return self._table[start:start + self.bytesperglyph]

    def glyph_index(self, char: str) -> int:
        """The glyph the kernel picks for ``char``.

        putchar_at_snap() indexes the table with the character code itself
        (0 < code < numglyph) and falls back to glyph 0.
        """
        code = ord(char)
        return code if 0 < code < self.numglyph else 0

    def glyph_pixels(self, char: str) -> int:
        """The pixels one character cell of ``char`` paints."""
        entry = self.glyph_bytes(self.glyph_index(char))[:self.height]
        return sum(bin(byte & self._mask).count("1") for byte in entry)

    def caption_pixels(self, label: str) -> int:
        """The pixels the whole caption ``label`` paints."""
        return sum(self.glyph_pixels(char) for char in label)


@lru_cache(maxsize=1)
def load_font() -> Psf2Font:
    """The kernel's caption font, parsed from the repo (never hard-coded)."""
    try:
        data = FONT_PATH.read_bytes()
    except OSError as error:
        raise RuntimeError(f"cannot read {FONT_PATH}: {error}") from error
    return Psf2Font(data)


def _logo_define(source: str, name: str) -> int:
    """The value of a ``#define`` in kernel/driver/logo.c."""
    match = re.search(rf"#define\s+{name}\s+(\d+)\b", source)
    if match is None:
        raise RuntimeError(f"{name} is not defined in kernel/driver/logo.c")
    return int(match.group(1))


@lru_cache(maxsize=1)
def logo_glyph_bits() -> tuple[int, ...]:
    """The set-bit count of every wordmark glyph, parsed from the driver.

    The wordmark is drawn from the driver's own 5x7 bitmaps (NOT from the
    PSF2 font), so the expected ink/accent pixel counts are derived from
    kernel/driver/logo.c instead of being hard-coded in this suite.
    """
    try:
        source = LOGO_SOURCE_PATH.read_text()
    except OSError as error:
        raise RuntimeError(
            f"cannot read {LOGO_SOURCE_PATH}: {error}") from error

    glyphs = _logo_define(source, "LOGO_GLYPHS")
    rows = _logo_define(source, "GLYPH_ROWS")
    cols = _logo_define(source, "GLYPH_COLS")

    match = re.search(
        r"static\s+const\s+unsigned\s+char\s+glyphs\s*\[[^\]]*\]\s*"
        r"\[[^\]]*\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if match is None:
        raise RuntimeError(
            "the glyphs[] table is not found in kernel/driver/logo.c")

    groups = re.findall(r"\{([^{}]*)\}", match.group(1))
    if len(groups) != glyphs:
        raise RuntimeError(
            f"expected {glyphs} glyph bitmaps in kernel/driver/logo.c, "
            f"found {len(groups)}")

    # The kernel draws bits GLYPH_COLS-1 .. 0 of every row byte.
    mask = (1 << cols) - 1
    counts = []
    for group in groups:
        values = re.findall(r"0x([0-9a-fA-F]+)", group)
        if len(values) != rows:
            raise RuntimeError(
                f"expected {rows} rows per glyph bitmap in "
                f"kernel/driver/logo.c, found {len(values)}")
        counts.append(
            sum(bin(int(value, 16) & mask).count("1") for value in values))
    return tuple(counts)


def logo_scale(xres: int, yres: int) -> int:
    """The kernel's scale rule (kernel/driver/logo.c)."""
    scale = 14 if xres >= 800 else (11 if xres >= 480 else 8)
    if yres < 300:
        scale = 7
    return scale


def logo_geometry(xres: int, yres: int) -> dict:
    """Expected geometry and pixel counts for a framebuffer of this size."""
    scale = logo_scale(xres, yres)
    left = 32 if xres >= 400 else 16
    top = 24
    advance = 7 * scale
    width = 3 * advance + 5 * scale
    line_y = top + 7 * scale + 14
    label_row = (line_y + 12 + FONT_H - 1) // FONT_H
    label_col = left // FONT_W
    glyph_bits = logo_glyph_bits()
    ink_bits = sum(glyph_bits[:INK_GLYPHS])
    accent_bits = sum(glyph_bits[INK_GLYPHS:INK_GLYPHS + ACCENT_GLYPHS])
    return {
        "scale": scale,
        "left": left,
        "top": top,
        "advance": advance,
        "width": width,
        "line_y": line_y,
        "label_row": label_row,
        "label_x0": label_col * FONT_W,
        "label_y0": label_row * FONT_H,
        "label_y1": (label_row + 1) * FONT_H,
        # The ink glyphs ("O", "S") occupy the first two cells; the accent
        # glyphs ("0", "1") the last two.  Each glyph cell is 5*scale wide,
        # so the cell boundary sits exactly at left + 2*advance.
        "ink_x1": left + INK_GLYPHS * advance,
        "wordmark_y1": top + 7 * scale,
        "ink_px": ink_bits * scale * scale,
        "accent_glyph_px": accent_bits * scale * scale,
        "accent_underline_px": 3 * scale * 2,
        # logo_draw() leaves the console cursor two character rows BELOW the
        # caption (Pos.YPosition = label_row + 2), so the [fb] banner that
        # follows lands on that row — the blue banner band.
        "banner_row": label_row + 2,
        "banner_y0": (label_row + 2) * FONT_H,
        "banner_y1": (label_row + 3) * FONT_H,
    }


def parse_ppm(data: bytes) -> tuple[int, int, bytes]:
    """Parse a binary P6 PPM into (width, height, rgb-bytes)."""
    if not data.startswith(b"P6"):
        raise RuntimeError("screendump is not a P6 PPM")
    idx = 2
    fields: list[int] = []
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
    width, height, maxval = fields
    if maxval != 255:
        raise RuntimeError(
            f"unsupported PPM maxval {maxval} (only 255 is supported)")
    expected = width * height * 3
    payload = data[idx:idx + expected]
    if len(payload) != expected:
        # A truncated dump must not reach count_rgb(): an IndexError there
        # would be mis-classified as an environment error.
        raise RuntimeError(
            f"truncated PPM payload: expected {expected} bytes "
            f"({width}x{height}x3), got {len(payload)}")
    return width, height, payload


def count_rgb(pixels: bytes, width: int, height: int,
              rgb: tuple[int, int, int],
              box: tuple[int, int, int, int] | None = None) -> int:
    """Count pixels equal to ``rgb``; ``box`` is (x0, y0, x1, y1) inclusive-exclusive."""
    if box is None:
        x0, y0, x1, y1 = 0, 0, width, height
    else:
        x0, y0, x1, y1 = box
    x0 = max(0, x0)
    y0 = max(0, y0)
    x1 = min(width, x1)
    y1 = min(height, y1)
    red, green, blue = rgb
    count = 0
    for y in range(y0, y1):
        row = y * width
        for x in range(x0, x1):
            idx = (row + x) * 3
            if (pixels[idx] == red and pixels[idx + 1] == green
                    and pixels[idx + 2] == blue):
                count += 1
    return count


class Qmp:
    """Minimal QMP client: connect, execute one command, close."""

    def __init__(self, path: str, timeout: float = 20.0) -> None:
        self.path = path
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self.fp = None

    def connect(self) -> None:
        deadline = time.time() + self.timeout
        err = None
        while time.time() < deadline:
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(self.timeout)
                sock.connect(self.path)
                self.sock = sock
                self.fp = sock.makefile("rwb")
                break
            except OSError as error:
                err = error
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError(f"QMP connect failed: {err}")
        self._read()
        self.execute("qmp_capabilities")

    def _read(self) -> dict:
        if self.fp is None:
            raise RuntimeError("QMP not connected")
        while True:
            line = self.fp.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            message = json.loads(line)
            if "event" in message:
                continue
            return message

    def execute(self, command: str, arguments: dict | None = None) -> dict:
        if self.fp is None:
            raise RuntimeError("QMP not connected")
        request = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        self.fp.write((json.dumps(request) + "\n").encode())
        self.fp.flush()
        while True:
            message = self._read()
            if "error" in message:
                raise RuntimeError(f"QMP error {command}: {message['error']}")
            return message.get("return", {})

    def screendump(self, path: str) -> bytes:
        self.execute("screendump", {"filename": path})
        return Path(path).read_bytes()

    def close(self) -> None:
        if self.fp is not None:
            try:
                self.fp.close()
            except OSError:
                pass
            self.fp = None
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


def qemu_command(args: argparse.Namespace, cpus: int, diagnostic_dtb: str | None,
                 ramfb: bool, qmp_sock: str | None) -> list[str]:
    """Build the QEMU argv for one boot.

    NOTE: this is a near-copy of ``qemu_command`` in
    qemutests/aarch64_uefi_smp.py (whose signature takes no QMP socket
    path) plus the ``-device ramfb`` / ``-qmp`` flags — a change to the
    shared flags must be made in BOTH places.

    ``diagnostic_dtb`` is the ALREADY RESOLVED path (or None): the caller
    materializes it.  The ``--diagnostic-dtb`` option itself accepts a
    path but never uses it as one — any non-empty value means "generate a
    diagnostic DTB" — which matches the sibling's behaviour.
    """
    command = [
        args.qemu, "-M", "virt,gic-version=2" + (",acpi=off" if diagnostic_dtb else ""),
        "-cpu", "cortex-a53", "-smp", str(cpus),
        "-m", str(getattr(args, "ram_mib", 512)),
        "-drive", f"if=pflash,format=raw,file={args.firmware}",
        "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        "-serial", "stdio", "-display", "none", "-no-reboot", "-no-shutdown",
    ]
    if ramfb:
        command += ["-device", "ramfb"]
    if qmp_sock:
        command += ["-qmp", f"unix:{qmp_sock},server,nowait"]
    if diagnostic_dtb:
        command += ["-dtb", diagnostic_dtb]
    return command


def _await_marker(session, marker: str, bound_s: float) -> bool:
    """Poll the serial text for ``marker`` for at most ``bound_s`` seconds.

    Returns True as soon as the marker shows up, False when the bound (or
    the guest's exit) is reached first.  A missing marker is NOT an error
    here: the caller continues and the checker reports it as a failed
    assertion, so a missing feature never turns into a timeout.
    """
    deadline = time.monotonic() + bound_s
    while True:
        if marker in session.text:
            return True
        if session.returncode is not None:
            # The guest is gone: no further serial output can arrive.
            return False
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        session.observe(min(0.25, remaining))


def _boot_and_capture(args: argparse.Namespace, *, ramfb: bool,
                       wait_markers: tuple[str, ...], cpus: int,
                       run_dir: Path,
                       secondary_marker: str | None = None) -> tuple[dict, list[str]]:
    """Boot QEMU, wait for every marker in ``wait_markers``, then screendump.

    The QMP screendump MUST be taken while QEMU is still alive: the
    ProcessSession owns the child and ``stop()`` terminates it, so the dump
    happens inside the live window, before the cleanup in the ``finally``.

    The wait markers are ones the CURRENT kernel already prints, so a
    missing feature shows up as a FAILED ASSERTION with a screenshot, never
    as a timeout with no evidence.

    ``secondary_marker`` (the logo check only) is a BOUNDED extra wait for
    a marker the kernel may print after the primary one, so the screendump
    cannot race the draw.  Today it is a no-op: the kernel prints the logo
    marker BEFORE the [fb] banner marker the primary wait targets, so the
    marker is already buffered when the primary wait returns; the wait
    only guards against a future reordering of the two markers.  If the
    marker does not arrive in time the capture continues and the checker
    reports the missing marker as a failed assertion — never as a timeout.

    Every preparation or launch failure (no QEMU, DTB generation failed,
    QMP refused) is reported as an ENVIRONMENT error: the caller archives it
    as ERROR and exits 2 — never as a FAIL.  A guest that never reaches the
    marker is NOT an environment error: ``wait_for()``'s timeout is a FAIL
    (exit 1), reported by main() together with the serial log captured
    before the deadline.  A guest that DIED is also a FAIL (exit 1): the
    QMP socket of a dead QEMU is gone, and connecting to it would spin and
    then be mis-classified as an environment error.
    """
    result = {"text": "", "shot": None, "width": 0, "height": 0,
              "pixels": b"", "timed_out": False, "error": None,
              "environment_error": False}
    session = None
    socket_dir = None
    command = []
    try:
        # Imported INSIDE the guarded region: a missing helper module is an
        # environment error (exit 2), never an unhandled crash.
        from aarch64_uefi_smp import generate_diagnostic_dtb

        run_dir.mkdir(parents=True, exist_ok=True)
        diagnostic_dtb = None
        if getattr(args, "diagnostic_dtb", None):
            diagnostic_dtb = generate_diagnostic_dtb(
                args.qemu, str(run_dir), cpus, getattr(args, "ram_mib", 512))

        # The QMP socket MUST live on a SHORT path.  A Unix socket path is
        # capped at ~108 bytes (sun_path), while the archive directory
        # (build dir + suite + UTC timestamp + uuid) is ~150 bytes — QEMU
        # would fail with "AF_UNIX path too long".  The screenshot and the
        # logs stay in the archive; only the socket is short-lived.
        if ramfb:
            socket_dir = tempfile.mkdtemp(prefix="os01-a64-")
            qmp_sock = os.path.join(socket_dir, "qmp.sock")
        else:
            qmp_sock = None

        shot_path = str(run_dir / "logo.ppm")
        command = qemu_command(args, cpus, diagnostic_dtb, ramfb, qmp_sock)
        (run_dir / "command.json").write_text(json.dumps(command, indent=2) + "\n")

        session = ProcessSession(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        session.wait_for(
            lambda text: all(marker in text for marker in wait_markers))
        result["timed_out"] = bool(session.timed_out)
        result["text"] = session.text
        if result["timed_out"]:
            # ProcessSession.wait_for() REAPS the child when its deadline
            # trips, so there is no live QEMU to screendump.  Connecting QMP
            # here would raise and be classified as an environment error,
            # masking the real problem: the guest never reached the marker.
            # A timeout is a FAIL (exit 1), reported by main() together with
            # the serial log that was captured before the deadline.
            pass
        else:
            if secondary_marker:
                # BOUNDED secondary wait.  Today this is a NO-OP: the
                # kernel prints the logo marker BEFORE the [fb] banner
                # marker, so the primary wait has already buffered it.
                # The wait only guards against a future reordering of the
                # two markers.  If the marker does not arrive we
                # CONTINUE: the checker reports the missing marker as a
                # failed assertion, never a timeout.
                _await_marker(session, secondary_marker,
                              SECONDARY_WAIT_BOUND_S)
            session.observe(2)
            result["text"] = session.text
            if ramfb and qmp_sock is not None:
                if session.returncode is not None:
                    # The guest is GONE (a panic under -no-reboot kills
                    # QEMU): its QMP socket is gone too.  Connecting would
                    # spin for the full 20 s and then be mis-classified as
                    # an ENVIRONMENT error (exit 2); a dead guest is a
                    # FAIL (exit 1).
                    result["error"] = (
                        f"the guest exited (rc={session.returncode}) before "
                        "the screendump")
                else:
                    qmp = Qmp(qmp_sock)
                    try:
                        qmp.connect()
                        result["shot"] = shot_path
                        (result["width"], result["height"], result["pixels"]) = parse_ppm(
                            qmp.screendump(shot_path))
                    finally:
                        qmp.close()
    except Exception as error:  # noqa: BLE001 — an environment failure
        result["error"] = f"{type(error).__name__}: {error}"
        result["environment_error"] = True
    finally:
        if session is not None:
            try:
                session.stop()
            finally:
                session.close()
        if socket_dir is not None:
            shutil.rmtree(socket_dir, ignore_errors=True)
    return result, command


def check_fb_visible(capture: dict) -> list[str]:
    """The framebuffer is observable: the existing white-on-blue banner is there."""
    failures = []
    if capture.get("error"):
        failures.append(capture["error"])
    if capture["width"] == 0:
        failures.append("no screendump captured")
        return failures
    width, height = capture["width"], capture["height"]
    geo = logo_geometry(width, height)
    # The blue background is counted in the BANNER ROW BAND only: the
    # banner is printed by color_printk on the row the logo left the
    # cursor on (logo_draw sets Pos.YPosition = label_row + 2).  Pure
    # blue is part of the edk2 firmware banner's palette (spec §V3), so
    # an unrestricted screen-wide count could be satisfied by a blue
    # firmware banner; the edk2 banner sits above this band.
    blue = count_rgb(capture["pixels"], width, height, BANNER_BLUE,
                     (0, geo["banner_y0"], width, geo["banner_y1"]))
    if blue < 100:
        failures.append(
            f"expected the [fb] banner background (rgb{BANNER_BLUE}) on screen, "
            f"found {blue} px in the banner band "
            f"y=[{geo['banner_y0']},{geo['banner_y1']})")
    return failures


def check_logo(capture: dict) -> list[str]:
    """The OS01 wordmark is drawn with the EXACT expected pixels.

    The ink glyphs ("O", "S") must occupy the FIRST TWO glyph cells and
    nowhere else; the accent glyphs ("0", "1") the last two; the accent
    underline its own 3*scale x 2 strip.  Every colour is counted both
    inside its own region and across the WHOLE screen, so a wordmark drawn
    in the wrong cells cannot pass.

    The caption is checked by EXACT PIXEL COUNT: the expected number is
    derived from the repo (the caption's glyph set bits in
    kernel/driver/font.psf), never hard-coded.  A caption that is
    truncated, misspelled or drawn from a different label produces a
    different count and fails.
    """
    failures = []
    if capture.get("error"):
        failures.append(capture["error"])
    if LOGO_DRAWN_MARKER not in capture["text"]:
        failures.append(f"missing serial marker {LOGO_DRAWN_MARKER!r}")
    if capture["width"] == 0:
        failures.append("no screendump captured")
        return failures

    width, height = capture["width"], capture["height"]
    pixels = capture["pixels"]
    geo = logo_geometry(width, height)

    ink_cells = (geo["left"], geo["top"], geo["ink_x1"], geo["wordmark_y1"])
    accent_cells = (geo["ink_x1"], geo["top"],
                    geo["left"] + geo["width"], geo["wordmark_y1"])
    underline = (geo["left"], geo["line_y"],
                 geo["left"] + 3 * geo["scale"], geo["line_y"] + 2)

    ink = count_rgb(pixels, width, height, LOGO_INK, ink_cells)
    ink_total = count_rgb(pixels, width, height, LOGO_INK)
    accent = count_rgb(pixels, width, height, LOGO_ACCENT, accent_cells)
    accent_underline = count_rgb(pixels, width, height, LOGO_ACCENT, underline)
    accent_total = count_rgb(pixels, width, height, LOGO_ACCENT)
    muted = count_rgb(pixels, width, height, LOGO_MUTED,
                      (geo["label_x0"], geo["label_y0"],
                       geo["label_x0"] + LABEL_LEN * FONT_W, geo["label_y1"]))
    # The same caption row across the FULL width: a caption LONGER than
    # the expected label paints muted pixels outside the expected cells.
    muted_row = count_rgb(pixels, width, height, LOGO_MUTED,
                           (0, geo["label_y0"], width, geo["label_y1"]))
    expected_caption = load_font().caption_pixels(LOGO_LABEL)

    if ink != geo["ink_px"]:
        failures.append(
            f"ink pixels: expected {geo['ink_px']}, found {ink} "
            f"(scale={geo['scale']})")
    if ink_total != ink:
        failures.append(
            f"ink pixels outside the two ink glyph cells: {ink_total - ink}")
    if accent != geo["accent_glyph_px"]:
        failures.append(
            f"accent glyph pixels: expected {geo['accent_glyph_px']}, "
            f"found {accent} (scale={geo['scale']})")
    if accent_underline != geo["accent_underline_px"]:
        failures.append(
            f"accent underline pixels: expected {geo['accent_underline_px']}, "
            f"found {accent_underline}")
    if accent_total != accent + accent_underline:
        failures.append(
            f"accent pixels outside the wordmark: "
            f"{accent_total - accent - accent_underline}")
    if muted != expected_caption:
        failures.append(
            f"caption pixels: expected {expected_caption} for "
            f"{LOGO_LABEL!r} (derived from kernel/driver/font.psf), found "
            f"{muted} in the caption band "
            f"x=[{geo['label_x0']},{geo['label_x0'] + LABEL_LEN * FONT_W}), "
            f"y=[{geo['label_y0']},{geo['label_y1']})")
    if muted_row != expected_caption:
        failures.append(
            f"caption row pixels: expected {expected_caption} for "
            f"{LOGO_LABEL!r} across the whole caption row, found "
            f"{muted_row} (a longer caption or stray muted pixels)")
    return failures


def check_no_fb(capture: dict) -> list[str]:
    """Without ramfb the kernel skips the logo instead of crashing."""
    failures = []
    if capture.get("error"):
        failures.append(capture["error"])
    text = capture["text"]
    if FB_ABSENT_MARKER not in text:
        failures.append(f"missing serial marker {FB_ABSENT_MARKER!r}")
    if LOGO_SKIPPED_MARKER not in text:
        failures.append(f"missing serial marker {LOGO_SKIPPED_MARKER!r}")
    if LOGO_DRAWN_MARKER in text:
        failures.append(f"unexpected serial marker {LOGO_DRAWN_MARKER!r}")
    if IDLE_MARKER not in text:
        failures.append(f"kernel never reached {IDLE_MARKER!r}")
    return failures


def _sha256_path(path: str | None) -> str | None:
    if path is None:
        return None
    try:
        with open(path, "rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    except (OSError, ValueError):
        return None


def _git_revision() -> tuple[str, bool]:
    try:
        revision = subprocess.run(["git", "rev-parse", "HEAD"],
                                  capture_output=True, text=True,
                                  check=False).stdout.strip()
        status = subprocess.run(["git", "status", "--porcelain"],
                                capture_output=True, text=True,
                                check=False).stdout.strip()
        return revision, bool(status)
    except OSError:
        return "", False


def _write_suite_report(archive, *, argv, profile, cpu, memory_mib, firmware,
                        image, started_monotonic, status, runner_exit_code,
                        errors, extra=None) -> None:
    """Persist a suite-unit RunReport; best-effort (never raises).

    One archive per check, so the three boots never share a directory —
    ProcessSession opens ``stdout.log`` with "w" and would truncate the
    previous boot's evidence.
    """
    if archive is None or RunReport is None:
        return
    try:
        revision, dirty = _git_revision()
        outcomes = [{"errors": list(errors)}]
        if extra:
            outcomes[0].update(extra)
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision=revision,
            git_dirty=dirty,
            profile=profile,
            suite=SUITE,
            request=None,
            declared_ids=None,
            observed_ids=None,
            argv=list(argv),
            cpu_count=int(cpu),
            memory_mib=int(memory_mib or 0),
            tool_versions={},
            firmware_path=firmware or None,
            firmware_sha256_before=None,
            firmware_sha256_after=_sha256_path(firmware),
            image_path=image or None,
            image_sha256_before=None,
            image_sha256_after=_sha256_path(image),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started_monotonic),
            runner_exit_code=runner_exit_code,
            child_exit_code=None,
            stopped_by_runner=True,
            status=status,
            count_unit="suite",
            outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # noqa: BLE001 — evidence must not mask the run
        print(f"[{SUITE}] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


# check -> (ramfb, wait markers, checker, secondary marker).  The wait
# markers are ones the CURRENT kernel already prints, so a missing feature
# fails an ASSERTION (with a screenshot) instead of timing out with no
# evidence.  The optional secondary marker is a BOUNDED extra wait (see
# ``_await_marker``): it only gives the kernel a few more seconds to emit
# it, and its absence stays a checker failure — never a timeout.  Only the
# logo check uses it, and today it is a no-op: the kernel prints the logo
# marker before the [fb] banner marker the primary wait targets.
CHECKS = {
    "fb-visible": (True, (FB_READY_MARKER,), "check_fb_visible", None),
    "logo": (True, (FB_READY_MARKER,), "check_logo", LOGO_DRAWN_MARKER),
    "no-fb": (False, (IDLE_MARKER,), "check_no_fb", None),
}


def _synthetic_ppm(width: int, height: int, body: bytes) -> bytes:
    """Build a binary P6 PPM (header + payload) for synthetic pixel data."""
    return f"P6\n{width} {height}\n255\n".encode() + body


def self_test() -> None:
    """QEMU-free self-test of the suite's own pure helpers.

    The geometry derivation, the PPM parser and the RGB counter are the
    parts of this suite that decide whether a PASS is honest, so the
    harness exercises them against synthetic data — no firmware, no
    image, no guest.
    """
    # -- geometry -----------------------------------------------------
    # The caption is ARCH_BOOT_LOGO_LABEL = "aarch64  /  KERNEL", 18
    # characters; a hard-coded 17 would drop the last character cell.
    assert LABEL_LEN == 18, LABEL_LEN
    assert LABEL_LEN == len(LOGO_LABEL)
    assert LOGO_LABEL == "aarch64  /  KERNEL"

    assert logo_scale(800, 600) == 14
    assert logo_scale(480, 600) == 11
    assert logo_scale(479, 600) == 8
    assert logo_scale(800, 299) == 7      # a short screen shrinks the scale
    assert logo_scale(320, 240) == 7

    geo = logo_geometry(800, 600)
    assert geo["scale"] == 14
    assert geo["left"] == 32
    assert geo["top"] == 24
    assert geo["advance"] == 7 * 14
    assert geo["width"] == 3 * geo["advance"] + 5 * 14
    assert geo["line_y"] == 24 + 7 * 14 + 14
    assert geo["label_row"] == 10
    assert geo["label_x0"] == 32
    assert geo["label_y0"] == 160
    assert geo["label_y1"] == 176
    assert geo["ink_x1"] == geo["left"] + 2 * geo["advance"]
    assert geo["wordmark_y1"] == geo["top"] + 7 * geo["scale"]
    # The glyph set-bit counts come from the driver's own bitmaps
    # (kernel/driver/logo.c), not from a hard-coded tuple.
    bits = logo_glyph_bits()
    assert len(bits) == 4
    assert all(count > 0 for count in bits)
    assert geo["ink_px"] == sum(bits[:INK_GLYPHS]) * 14 * 14
    assert geo["accent_glyph_px"] == sum(
        bits[INK_GLYPHS:INK_GLYPHS + ACCENT_GLYPHS]) * 14 * 14
    assert geo["accent_underline_px"] == 3 * 14 * 2
    # The caption band spans LABEL_LEN character cells (18 cells, not 17).
    assert geo["label_x0"] + LABEL_LEN * FONT_W == 32 + 18 * 8
    # The [fb] banner lands on the row the logo left the cursor on.
    assert geo["banner_row"] == geo["label_row"] + 2
    assert geo["banner_y0"] == 192 and geo["banner_y1"] == 208

    # A small framebuffer uses the smaller scale and the smaller left.
    small = logo_geometry(320, 240)
    assert small["scale"] == 7
    assert small["left"] == 16
    assert small["advance"] == 7 * 7

    # -- caption font (parsed from the repo) -------------------------
    font = load_font()
    assert font.width == FONT_W and font.height == FONT_H
    assert font.numglyph == 128 and font.bytesperglyph == 16
    # The kernel indexes the glyph table with the character code itself.
    assert font.glyph_index("a") == ord("a")
    assert font.glyph_index(" ") == ord(" ")
    # The expected caption count is DERIVED: a truncated or different
    # label must produce a different number of pixels.
    caption = font.caption_pixels(LOGO_LABEL)
    assert caption > 0
    assert font.caption_pixels(LOGO_LABEL[:-1]) < caption
    assert font.caption_pixels("x86_64  /  KERNEL") != caption
    assert font.caption_pixels("aarch64 ") != caption

    # A synthetic font proves the PARSER itself (the repo file only
    # proves that the real file parses): two 2-byte glyphs, 2 rows each.
    synthetic = struct.pack("<8I", 0x864AB572, 0, 32, 0, 2, 2, 2, 8) + \
        bytes([0x0F, 0x00, 0x03, 0x00])
    tiny = Psf2Font(synthetic)
    assert tiny.numglyph == 2 and tiny.bytesperglyph == 2
    assert tiny.glyph_pixels("\x01") == 2      # 0x03 -> two set bits
    assert tiny.glyph_pixels("\x02") == 4      # out of range -> glyph 0 (0x0F)
    for bad, why in (
        (b"", "a short header"),
        (struct.pack("<8I", 0x864AB572, 0, 32, 0, 4, 2, 2, 8) + bytes(4),
         "a truncated glyph table"),
    ):
        try:
            Psf2Font(bad)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"Psf2Font accepted {why}")

    # -- PPM parsing --------------------------------------------------
    red = (255, 0, 0)
    blue = (0, 0, 255)
    body = bytes(red) + bytes(blue) + bytes(red) + bytes(blue)
    width, height, payload = parse_ppm(_synthetic_ppm(2, 2, body))
    assert (width, height) == (2, 2)
    assert payload == body

    # A comment line in the header is legal PPM and must be skipped.
    width, height, payload = parse_ppm(b"P6\n# a comment\n2 2\n255\n" + body)
    assert (width, height) == (2, 2)
    assert payload == body

    for bad, why in (
        (b"P3\n2 2\n255\n" + body, "not a P6 PPM"),
        (b"P6\n2 2\n255\n" + body[:-1], "truncated payload"),
        (b"P6\n2 2\n65535\n" + body, "unsupported maxval"),
    ):
        try:
            parse_ppm(bad)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"parse_ppm accepted {why}")

    # -- RGB counting -------------------------------------------------
    # 2x3 image, row-major: red, blue / red, blue / blue, blue
    pixels = (bytes(red) + bytes(blue) +
              bytes(red) + bytes(blue) +
              bytes(blue) + bytes(blue))
    assert count_rgb(pixels, 2, 3, red) == 2
    assert count_rgb(pixels, 2, 3, blue) == 4
    assert count_rgb(pixels, 2, 3, (1, 2, 3)) == 0
    # The box is inclusive-exclusive.
    assert count_rgb(pixels, 2, 3, red, (0, 0, 1, 3)) == 2
    assert count_rgb(pixels, 2, 3, red, (0, 0, 1, 1)) == 1
    assert count_rgb(pixels, 2, 3, red, (1, 0, 2, 3)) == 0
    # The box is clipped to the framebuffer.
    assert count_rgb(pixels, 2, 3, red, (-5, -5, 99, 99)) == 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true",
                        help="run the QEMU-free self-test and exit")
    parser.add_argument("--check", choices=("fb-visible", "logo", "no-fb", "all"),
                        default="all")
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--log-dir", default="test-results/aarch64-boot-logo",
                        help="evidence directory used ONLY without --build-dir; "
                             "with --build-dir the suite archives under "
                             "<build-dir>/logs/tests/<suite>-<check>/")
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE", "default"))
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--ram-mib", type=int, default=512)
    parser.add_argument("--diagnostic-dtb", default="auto")
    args = parser.parse_args(argv)

    if args.self_test:
        self_test()
        print("aarch64_boot_logo: self-test passed")
        return 0

    # --firmware/--image are required in normal mode, but NOT for the
    # QEMU-free --self-test above: argparse would otherwise exit 2 on a
    # bare ``--self-test`` invocation.
    if not (args.firmware and args.image):
        parser.error("--firmware and --image are required outside --self-test")

    if ProcessSession is None:
        print("ERROR: qemutests.harness.process is unavailable", file=sys.stderr)
        return 2

    checkers = {
        "check_fb_visible": check_fb_visible,
        "check_logo": check_logo,
        "check_no_fb": check_no_fb,
    }
    checks = ("fb-visible", "logo", "no-fb") if args.check == "all" else (args.check,)
    failures: list[str] = []
    environment_errors: list[str] = []
    for check in checks:
        ramfb, wait_markers, checker_name, secondary_marker = CHECKS[check]
        started = time.monotonic()

        archive = None
        if RunArchive is not None and args.build_dir:
            archive = RunArchive.create(Path(args.build_dir), f"{SUITE}-{check}")
            run_dir = archive.run_dir
        else:
            run_dir = Path(args.log_dir) / check
            run_dir.mkdir(parents=True, exist_ok=True)

        capture, command = _boot_and_capture(
            args, ramfb=ramfb, wait_markers=wait_markers,
            cpus=args.cpus, run_dir=run_dir,
            secondary_marker=secondary_marker)
        try:
            problems = checkers[checker_name](capture)
        except Exception as error:  # noqa: BLE001 — a broken expectation is a FAIL
            # The expectations are derived from the repo (the caption font,
            # the wordmark bitmaps); a parse failure is a FAIL of the
            # SUITE, never an environment error and never a traceback.
            problems = [f"{type(error).__name__}: {error}"]
        if capture["timed_out"]:
            problems.append(
                f"timed out waiting for {list(wait_markers)} in the serial log")

        is_environment = bool(capture.get("environment_error"))
        if is_environment:
            environment_errors.extend(f"{check}: {problem}" for problem in problems)
        else:
            failures.extend(f"{check}: {problem}" for problem in problems)

        print(json.dumps({"check": check, "failures": problems,
                          "environment_error": is_environment,
                          "run_dir": str(run_dir),
                          "shot": capture.get("shot")}))

        _write_suite_report(
            archive, argv=command, profile=args.profile, cpu=args.cpus,
            memory_mib=args.ram_mib, firmware=args.firmware, image=args.image,
            started_monotonic=started,
            status=("ERROR" if is_environment
                    else ("PASS" if not problems else "FAIL")),
            runner_exit_code=(2 if is_environment
                              else (0 if not problems else 1)),
            errors=problems,
            extra={"check": check, "timed_out": capture["timed_out"],
                   "environment_error": is_environment})

    if environment_errors:
        print("ERROR: aarch64 boot logo suite (environment failure)",
              file=sys.stderr)
        for error in environment_errors:
            print(f"  - {error}", file=sys.stderr)
        if failures:
            # The sibling checks' assertion failures must reach stderr too:
            # an environment error must not swallow the evidence of the
            # checks that DID run (the exit code stays 2).
            print("  (assertion failures of the other checks:)",
                  file=sys.stderr)
            for failure in failures:
                print(f"  - {failure}", file=sys.stderr)
        return 2
    if failures:
        print("FAIL: aarch64 boot logo suite", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("PASS: aarch64 boot logo suite")
    return 0


if __name__ == "__main__":
    sys.exit(main())
