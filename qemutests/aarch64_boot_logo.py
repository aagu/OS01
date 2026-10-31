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
              caption band carries muted pixels, and the ink pixels stay
              inside the two ink glyph cells.
no-fb      : without ramfb the kernel skips the logo instead of crashing,
              and still reaches its idle loop.

Exit codes: 0 = PASS, 1 = FAIL, 2 = configuration/environment ERROR.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
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


class SuiteEnvironmentError(RuntimeError):
    """A preparation or launch failure — ERROR (exit 2), never a FAIL."""

# ── Logo constants mirrored from kernel/driver/logo.c ──────────
LOGO_INK = (0xE8, 0xF0, 0xF7)       # 0x00e8f0f7
LOGO_ACCENT = (0x3B, 0xD6, 0xC6)    # 0x003bd6c6
LOGO_MUTED = (0x68, 0x78, 0x86)     # 0x00687886
BANNER_BLUE = (0x00, 0x00, 0xFF)    # color_printk(WHITE, BLUE, ...) background

# Set bits per glyph row of the wordmark bitmaps in kernel/driver/logo.c:
# O, S, 0, 1.  The first two are ink, the last two accent.
GLYPH_BITS = (20, 17, 23, 10)
INK_GLYPHS = 2
ACCENT_GLYPHS = 2

FONT_W, FONT_H = 8, 16              # PSF2 metrics of kernel/driver/font.psf
LABEL_LEN = 17                     # len("aarch64  /  KERNEL")

LOGO_DRAWN_MARKER = "[logo] boot logo drawn"
LOGO_SKIPPED_MARKER = "[logo] skipped: no framebuffer"
FB_READY_MARKER = "[fb] frame buffer remap succeed"
FB_ABSENT_MARKER = "[fb] no framebuffer in handoff"
IDLE_MARKER = "[sched] idle-ready"


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
    ink_bits = sum(GLYPH_BITS[:INK_GLYPHS])
    accent_bits = sum(GLYPH_BITS[INK_GLYPHS:])
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
    width, height, _maxval = fields
    return width, height, data[idx:idx + width * height * 3]


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


def _boot_and_capture(args: argparse.Namespace, *, ramfb: bool,
                       wait_markers: tuple[str, ...], cpus: int,
                       run_dir: Path) -> tuple[dict, list[str]]:
    """Boot QEMU, wait for every marker in ``wait_markers``, then screendump.

    The QMP screendump MUST be taken while QEMU is still alive: the
    ProcessSession owns the child and ``stop()`` terminates it, so the dump
    happens inside the live window, before the cleanup in the ``finally``.

    The wait markers are ones the CURRENT kernel already prints, so a
    missing feature shows up as a FAILED ASSERTION with a screenshot, never
    as a timeout with no evidence.

    Every preparation or launch failure (no QEMU, DTB generation failed, the
    guest never came up, QMP refused) is reported as an ENVIRONMENT error:
    the caller archives it as ERROR and exits 2 — never as a FAIL.
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
            session.observe(2)
            result["text"] = session.text
            if ramfb and qmp_sock is not None:
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
    blue = count_rgb(capture["pixels"], capture["width"], capture["height"],
                     BANNER_BLUE)
    if blue < 100:
        failures.append(
            f"expected the [fb] banner background (rgb{BANNER_BLUE}) on screen, "
            f"found {blue} px")
    return failures


def check_logo(capture: dict) -> list[str]:
    """The OS01 wordmark is drawn with the EXACT expected pixels.

    The ink glyphs ("O", "S") must occupy the FIRST TWO glyph cells and
    nowhere else; the accent glyphs ("0", "1") the last two; the accent
    underline its own 3*scale x 2 strip.  Every colour is counted both
    inside its own region and across the WHOLE screen, so a wordmark drawn
    in the wrong cells cannot pass.
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
    if muted == 0:
        failures.append(
            f"no caption pixels (rgb{LOGO_MUTED}) in the label band "
            f"y=[{geo['label_y0']},{geo['label_y1']})")
    return failures


def check_no_fb(capture: dict) -> list[str]:
    """Without ramfb the kernel skips the logo instead of crashing."""
    failures = []
    if capture.get("error"):
        failures.append(capture["error"])
    text = capture["text"]
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


# check -> (ramfb, wait markers, checker).  The wait markers are ones the
# CURRENT kernel already prints, so a missing feature fails an ASSERTION
# (with a screenshot) instead of timing out with no evidence.
CHECKS = {
    "fb-visible": (True, (FB_READY_MARKER,), "check_fb_visible"),
    "logo": (True, (FB_READY_MARKER,), "check_logo"),
    "no-fb": (False, (IDLE_MARKER,), "check_no_fb"),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", choices=("fb-visible", "logo", "no-fb", "all"),
                        default="all")
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--log-dir", default="test-results/aarch64-boot-logo")
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE", "default"))
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--ram-mib", type=int, default=512)
    parser.add_argument("--diagnostic-dtb", default="auto")
    args = parser.parse_args()

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
        ramfb, wait_markers, checker_name = CHECKS[check]
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
            cpus=args.cpus, run_dir=run_dir)
        problems = checkers[checker_name](capture)
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
                          "run_dir": str(run_dir)}))

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
