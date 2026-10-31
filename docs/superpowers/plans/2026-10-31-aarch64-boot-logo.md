# aarch64 Boot Logo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw the OS01 boot wordmark on the aarch64 framebuffer during boot, using the same single copy of the glyph geometry that x86_64 uses, and prove it with a headless QMP screendump pixel assertion.

**Architecture:** A new arch facade `arch/fb_write.h` hands out an exclusive framebuffer *view* (`struct fb_write_view`) that owns the display-coordinator lease on x86_64 (strong override in `arch/x86_64/platform/fb_write.c`) and draws straight into `Pos.FB_addr` on aarch64 (weak default in `driver/arch_fb_write.c`). `kernel/driver/logo.c` is refactored into a pure `logo_draw()` over that view, so the glyph table stays the only copy. The aarch64 boot path calls `boot_logo_show()` right after the framebuffer is mapped, before the existing `[fb]` banner.

**Tech Stack:** clang/lld (aarch64-none-elf, x86_64), C11 kernel, GNU make (profile-private build dirs), QEMU 11.1.3 `virt` + `-device ramfb` + edk2 `QEMU_EFI.fd`, QMP `screendump` (P6 PPM), Python 3 (`qemutests/harness/process.py` `ProcessSession`).

**Spec:** `docs/superpowers/specs/2026-10-31-aarch64-boot-logo-design.md` (approved `fde7003b`)

**Worktree:** `/home/aagu/OS01/.worktrees/aarch64-boot-logo`, branch `feature/aarch64-boot-logo`, base `fa7d51ac`.

## Global Constraints

- Every task runs in the worktree above. Never commit to `master` from here.
- **The Makefile has no header dependency tracking.** Any task that changes a header consumed by an existing TU MUST start with `make PROFILE=<profile> clean` for every affected profile. `struct fb_write_view` is new and its size is baked into `kernel/driver/logo.c`, `kernel/driver/arch_fb_write.c` and `kernel/arch/x86_64/platform/fb_write.c` — a stale `.o` there is a silent memory-corruption bug.
- Kernel logging policy (`AGENTS.md`): use `log_info` / `log_err` / `log_warn` — never bare `serial_printk` — except for the four whitelisted exceptions. The aarch64 mini-formatter (`kernel/arch/aarch64/runtime/log_impl.c`) supports `%s`, `%u`, `%lu`, `%p` only; every other specifier is echoed verbatim.
- The aarch64 build stays minimal: **do not** add `driver/fb_state.c`, `driver/bga.c`, `driver/fb.c`, `tty/console.c` or any other display-coordinator TU to the aarch64 source list.
- The wordmark geometry, glyph bitmaps and colour values in `kernel/driver/logo.c` MUST NOT change. Only the way the framebuffer is obtained may change.
- x86_64 must keep: the same call site (`kernel/arch/x86_64/memory/boot.c:50`), the same pixels, the same call order, and a byte-identical serial stream. The driver prints nothing.
- The aarch64 serial markers are exactly:
  - `[logo] boot logo drawn (aarch64, 800x600)` — format `[logo] boot logo drawn (%s, %ux%u)`
  - `[logo] skipped: no framebuffer`
- Every task ends with a commit. No task pushes.

---

### Task 1: Headless framebuffer probe + logo acceptance suite

**Files:**
- Create: `qemutests/aarch64_boot_logo.py`
- Modify: `mk/components/run.mk` (add the `test-aarch64-boot-logo` target)

**Interfaces:**
- Consumes: `qemutests/harness/process.py` → `ProcessSession(argv, run_dir, timeout_s)` with `.start()`, `.wait_for(predicate)`, `.observe(seconds)`, `.stop()`, `.close()`, `.text`, `.timed_out`; `qemutests/harness/result.py` → `RunArchive.create(build_dir, suite)` (unique dir per call) and `RunReport`; `qemutests/aarch64_uefi_smp.py` → `generate_diagnostic_dtb(qemu, log_dir, cpus, ram_mib)`.
- Produces (used by every later task and by the make target):
  - `logo_scale(xres: int, yres: int) -> int`
  - `logo_geometry(xres: int, yres: int) -> dict` — keys: `scale`, `left`, `top`, `advance`, `width`, `line_y`, `label_row`, `label_x0`, `label_y0`, `label_y1`, `ink_x1`, `wordmark_y1`, `ink_px`, `accent_glyph_px`, `accent_underline_px`.
  - `parse_ppm(data: bytes) -> tuple[int, int, bytes]`
  - `count_rgb(pixels, width, height, rgb, box=None) -> int`
  - `qemu_command(args, cpus, diagnostic_dtb, ramfb: bool, qmp_sock: str | None) -> list[str]`
  - `_boot_and_capture(args, *, ramfb, wait_markers, cpus, run_dir) -> tuple[dict, list[str]]` — the screendump is taken INSIDE the live QEMU window.
  - `check_fb_visible(capture) / check_logo(capture) / check_no_fb(capture) -> list[str]`
  - CLI: `--check {fb-visible,logo,no-fb,all}` (default `all`), `--firmware`, `--image`, `--qemu`, `--log-dir`, `--build-dir`, `--profile`, `--timeout`, `--cpus`, `--ram-mib`, `--diagnostic-dtb`.

- [ ] **Step 1: Write the suite**

Create `qemutests/aarch64_boot_logo.py`:

```python
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
# The caption the kernel draws under the wordmark (the aarch64 kernel's
# ARCH_BOOT_LOGO_LABEL): "aarch64" + 2 spaces + "/" + 2 spaces + "KERNEL"
# is 18 characters — a hard-coded 17 would drop the last character cell
# and let a truncated caption pass.
LOGO_LABEL = "aarch64  /  KERNEL"
LABEL_LEN = len(LOGO_LABEL)

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
```

- [ ] **Step 2: Add the make target**

In `mk/components/run.mk`, right after the `test-aarch64-m3-probe` target (the
`_test-aarch64-run-m3-probe` recipe ends around line 425), insert:

```make
# ── aarch64 boot logo (framebuffer wordmark) ─────────────────
# Boots the aarch64 image with `-device ramfb` (edk2's RamfbDxe turns it into
# a GOP framebuffer), lets the kernel draw the OS01 wordmark and asserts the
# result on a QMP screendump: exact ink/accent pixel counts, the arch caption
# band, and the no-framebuffer negative case.  Headless: `-display none`.
.PHONY: test-aarch64-boot-logo
test-aarch64-boot-logo:
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	$(MAKE) --no-print-directory aarch64-uefi
	python3 qemutests/aarch64_boot_logo.py \
	  --check all \
	  --diagnostic-dtb=auto \
	  --timeout 120 \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-boot-logo/$$(date -u +%Y%m%dT%H%M%S)-$$$$"
```

- [ ] **Step 3: Build the image the suite will boot**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=aarch64-clang aarch64-uefi
```

Expected: exit 0, `build/aarch64-clang/image/{QEMU_EFI.fd,aarch64-uefi.img}` exist.

- [ ] **Step 4: Run the pipeline check — it must PASS**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
python3 qemutests/aarch64_boot_logo.py \
  --check fb-visible \
  --diagnostic-dtb=auto --timeout 120 \
  --firmware build/aarch64-clang/image/QEMU_EFI.fd \
  --image build/aarch64-clang/image/aarch64-uefi.img \
  --qemu qemu-system-aarch64 \
  --log-dir /tmp/a64-logo-fbvisible
```

Expected: `{"check": "fb-visible", "failures": [], ...}` and
`PASS: aarch64 boot logo suite`.
This proves the whole headless path works: ramfb → GOP handoff → kernel framebuffer
mapping → QMP screendump → PPM parse. The run directory also holds
`stdout.log`, `command.json` and (with `--build-dir`) a `result.json`.

- [ ] **Step 5: Run the logo check — it must FAIL (this is the RED phase)**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
python3 qemutests/aarch64_boot_logo.py \
  --check logo \
  --diagnostic-dtb=auto --timeout 120 \
  --firmware build/aarch64-clang/image/QEMU_EFI.fd \
  --image build/aarch64-clang/image/aarch64-uefi.img \
  --qemu qemu-system-aarch64 \
  --log-dir /tmp/a64-logo-red
```

Expected: exit 1, and the failures name BOTH the missing serial marker and
the missing pixels:

```
  - logo: missing serial marker '[logo] boot logo drawn'
  - logo: ink pixels: expected 7252, found 0 (scale=14)
  - logo: accent glyph pixels: expected 6468, found 0 (scale=14)
  - logo: no caption pixels (rgb(104, 120, 134)) in the label band y=[160,176)
```

The check does NOT time out: it waits for "[fb] frame buffer remap succeed",
which the current kernel already prints, so the screenshot is taken and the
missing logo is reported as a failed ASSERTION. The serial log in
`/tmp/a64-logo-red/logo` must NOT contain `[logo] boot logo drawn`.

- [ ] **Step 6: Commit**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
git add qemutests/aarch64_boot_logo.py mk/components/run.mk
git commit -m "test(aarch64): add the boot logo acceptance suite" \
  -m "Boots the aarch64 image with -device ramfb and asserts the OS01
wordmark on a QMP screendump: exact ink/accent pixel counts derived from
the glyph bitmaps, the arch caption band, and the no-framebuffer negative
case.

The fb-visible check passes today (it proves the headless ramfb -> GOP ->
framebuffer -> screendump path); the logo check is RED until the kernel
draws the wordmark."
```

---

### Task 2: The `fb_write` facade and its two implementations

**Files:**
- Create: `kernel/include/arch/identity.h`
- Create: `kernel/include/arch/x86_64/identity.h`
- Create: `kernel/include/arch/aarch64/identity.h`
- Create: `kernel/include/arch/fb_write.h`
- Create: `kernel/driver/arch_fb_write.c`
- Create: `kernel/arch/x86_64/platform/fb_write.c`

**Interfaces:**
- Consumes: `kernel/include/driver/fb_state.h` → `fb_lease_t`, `fb_snapshot_t`, `fb_writer_begin(lease, generation)`, `fb_writer_end(lease)`; `kernel/include/core/printk.h` → `Pos` (`FB_addr`, `XResolution`, `YResolution`).
- Produces (consumed by Task 3 and Task 5):
  - `struct fb_write_view { fb_lease_t lease; const struct fb_snapshot *snap; uint32_t *addr; uint32_t xres; uint32_t yres; }`
  - `int arch_fb_write_begin(struct fb_write_view *view)` — `0` on success, negative on failure.
  - `void arch_fb_write_end(struct fb_write_view *view)`
  - `ARCH_NAME` (`"x86_64"` / `"aarch64"`), `ARCH_BOOT_LOGO_LABEL` (`"x86_64  /  KERNEL"` / `"aarch64  /  KERNEL"`).

- [ ] **Step 1: Create the identity headers**

`kernel/include/arch/identity.h`:

```c
#ifndef _ARCH_IDENTITY_H
#define _ARCH_IDENTITY_H

/* Arch identity constants.
 *
 *   ARCH_NAME           — short architecture name ("x86_64" / "aarch64")
 *   ARCH_BOOT_LOGO_LABEL — the caption drawn under the OS01 boot wordmark
 *
 * The per-arch headers define the macros; this facade only selects them.
 */

#ifdef __x86_64__
#include <arch/x86_64/identity.h>
#elif defined(__aarch64__)
#include <arch/aarch64/identity.h>
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_IDENTITY_H */
```

`kernel/include/arch/x86_64/identity.h`:

```c
#ifndef _ARCH_X86_64_IDENTITY_H
#define _ARCH_X86_64_IDENTITY_H

#define ARCH_NAME           "x86_64"
#define ARCH_BOOT_LOGO_LABEL "x86_64  /  KERNEL"

#endif /* _ARCH_X86_64_IDENTITY_H */
```

`kernel/include/arch/aarch64/identity.h`:

```c
#ifndef _ARCH_AARCH64_IDENTITY_H
#define _ARCH_AARCH64_IDENTITY_H

#define ARCH_NAME           "aarch64"
#define ARCH_BOOT_LOGO_LABEL "aarch64  /  KERNEL"

#endif /* _ARCH_AARCH64_IDENTITY_H */
```

- [ ] **Step 2: Create the facade header**

`kernel/include/arch/fb_write.h`:

```c
#ifndef _ARCH_FB_WRITE_H
#define _ARCH_FB_WRITE_H

#include <stdint.h>

#include <driver/fb_state.h>   /* fb_lease_t — TYPE ONLY, no link dependency */
#include <arch/identity.h>     /* ARCH_NAME, ARCH_BOOT_LOGO_LABEL */

/* An exclusive view of the framebuffer.
 *
 * Obtained with arch_fb_write_begin() and released with
 * arch_fb_write_end().  The view OWNS the arch-private storage, so every
 * pointer in it stays valid until the matching end() call:
 *
 *   1. the view is caller-owned and MUST outlive begin -> draw -> end;
 *   2. begin() MUST NOT return a pointer to storage that dies when it
 *      returns (the x86_64 override keeps the lease inside the view);
 *   3. begin() and end() are strictly paired, in the same scope, never
 *      nested, and never end() without begin();
 *   4. the caller serialises the pair (the boot logo holds Pos.lock across
 *      it), so no implementation may assume re-entrancy.
 */
struct fb_write_view {
    /* Arch-private storage owned by the view.  The x86_64 override keeps
     * the display-coordinator lease here; the aarch64 weak default leaves
     * it untouched. */
    fb_lease_t lease;

    /* NULL when the arch has no display coordinator (aarch64). */
    const struct fb_snapshot *snap;

    /* Framebuffer base and geometry to draw into. */
    uint32_t *addr;
    uint32_t xres;
    uint32_t yres;
};

/* Acquire exclusive framebuffer write access.  Returns 0 on success,
 * negative on failure (no framebuffer, or no exclusive access). */
int arch_fb_write_begin(struct fb_write_view *view);

/* Release what begin() acquired.  Safe to call after a failed begin(). */
void arch_fb_write_end(struct fb_write_view *view);

#endif /* _ARCH_FB_WRITE_H */
```

- [ ] **Step 3: Create the weak default (aarch64 path)**

`kernel/driver/arch_fb_write.c`:

```c
/* kernel/driver/arch_fb_write.c — weak default arch_fb_write_begin/end.
 *
 * The default describes an arch that has a framebuffer but NO display
 * coordinator: there is no writer lease, no owner and no mode concept, so
 * "exclusive access" is just the caller's Pos.lock.  aarch64 links this
 * default; x86_64 overrides it with the coordinator-backed strong symbol
 * in kernel/arch/x86_64/platform/fb_write.c.
 *
 * Nothing here pulls in the coordinator: the aarch64 build stays minimal.
 */

#include <arch/fb_write.h>
#include <core/printk.h>
#include <errno.h>

__attribute__((weak))
int arch_fb_write_begin(struct fb_write_view *view)
{
    if (!view)
        return -EINVAL;

    view->snap = NULL;
    view->addr = Pos.FB_addr;
    view->xres = (uint32_t)Pos.XResolution;
    view->yres = (uint32_t)Pos.YResolution;

    if (!view->addr || view->xres == 0 || view->yres == 0)
        return -ENODEV;

    return 0;
}

__attribute__((weak))
void arch_fb_write_end(struct fb_write_view *view)
{
    (void)view;
}
```

- [ ] **Step 4: Create the x86_64 strong override**

`kernel/arch/x86_64/platform/fb_write.c`:

```c
/* kernel/arch/x86_64/platform/fb_write.c — strong override of the
 * arch_fb_write_begin/end facade.
 *
 * x86_64 owns the display coordinator (kernel/driver/fb_state.c), so
 * exclusive framebuffer access is a WRITER LEASE, not just Pos.lock.  The
 * lease lives inside the caller's view — never in this function's frame —
 * so the snapshot pointer stays valid until arch_fb_write_end().
 */

#include <arch/fb_write.h>
#include <core/printk.h>
#include <errno.h>

int arch_fb_write_begin(struct fb_write_view *view)
{
    if (!view)
        return -EINVAL;

    if (fb_writer_begin(&view->lease, 0) < 0)
        return -EAGAIN;

    view->snap = &view->lease.snapshot;
    view->addr = view->lease.snapshot.addr;
    view->xres = view->lease.snapshot.state.info.width;
    view->yres = view->lease.snapshot.state.info.height;

    /* Fall back to the live Pos geometry when the coordinator has no
     * published mode info yet. */
    if (!view->xres)
        view->xres = (uint32_t)Pos.XResolution;
    if (!view->yres)
        view->yres = (uint32_t)Pos.YResolution;

    if (!view->addr || view->xres == 0 || view->yres == 0) {
        fb_writer_end(&view->lease);
        view->snap = NULL;
        return -ENODEV;
    }

    return 0;
}

void arch_fb_write_end(struct fb_write_view *view)
{
    if (!view)
        return;
    fb_writer_end(&view->lease);
    view->snap = NULL;
}
```

- [ ] **Step 5: Clean and build both arches**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=x86_64-clang clean
make PROFILE=aarch64-clang clean
make PROFILE=x86_64-clang kernel.bin
make PROFILE=aarch64-clang aarch64-uefi-kernel
```

Expected: both exit 0. (The aarch64 build does not yet link the new TUs — that
is Task 4 — but the headers must not break anything.)

- [ ] **Step 6: Verify the weak/strong resolution on x86_64**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
nm build/x86_64-clang/kernel/kernel.elf | grep -E "arch_fb_write_(begin|end)"
```

Expected: exactly TWO lines, both STRONG (`T`):

```
ffff... T arch_fb_write_begin
ffff... T arch_fb_write_end
```

If either line shows `W` (weak), the strong override did not win — fix the
build wiring before continuing. If the weak default ALSO appears, the two TUs
export duplicate symbols — also a failure.

- [ ] **Step 7: Commit**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
git add kernel/include/arch/identity.h \
        kernel/include/arch/x86_64/identity.h \
        kernel/include/arch/aarch64/identity.h \
        kernel/include/arch/fb_write.h \
        kernel/driver/arch_fb_write.c \
        kernel/arch/x86_64/platform/fb_write.c
git commit -m "feat(arch): add the fb_write facade for exclusive framebuffer access" \
  -m "arch/fb_write.h hands out an fb_write_view that OWNS the arch-private
storage, so the snapshot pointer cannot dangle between begin() and end().

- weak default (kernel/driver/arch_fb_write.c): direct Pos draw, no
  coordinator, no x86-only headers -> this is what aarch64 links
- strong override (kernel/arch/x86_64/platform/fb_write.c): the display
  coordinator writer lease

arch/identity.h carries ARCH_NAME and ARCH_BOOT_LOGO_LABEL."
```

---

### Task 3: Refactor the logo driver onto the facade

**Files:**
- Modify: `kernel/driver/logo.c`
- Modify: `kernel/include/driver/logo.h`

**Interfaces:**
- Consumes: `arch_fb_write_begin/end`, `struct fb_write_view`, `ARCH_BOOT_LOGO_LABEL` (Task 2); `putchar_at_snap(snap, col, row, fg, bg, c)` and `Pos` from `kernel/core/printk.h`; `font` (the PSF2 pointer defined in `kernel/core/printk.c`).
- Produces: `int boot_logo_show(void)` — `0` when the wordmark was drawn, negative when skipped. The glyph table, the geometry and the colour values are UNCHANGED.

- [ ] **Step 1: Rewrite the header**

`kernel/include/driver/logo.h`:

```c
#ifndef _KERNEL_LOGO_H
#define _KERNEL_LOGO_H

// Draw the OS01 boot logo on the framebuffer.
// Must be called after the framebuffer is mapped: on arches with a display
// coordinator a writer lease must be available, on arches without one
// Pos.FB_addr must be valid.
// Advances Pos.YPosition/XPosition below the logo so subsequent
// color_printk output continues beneath it.
// Returns 0 when the logo was drawn, negative when it was skipped.
int boot_logo_show(void);

#endif
```

- [ ] **Step 2: Rewrite the driver**

`kernel/driver/logo.c`:

```c
#include <driver/logo.h>
#include <arch/fb_write.h>
#include <core/printk.h>
#include <driver/font.h>

extern psf2_t *font;

// A small, resolution-aware wordmark for the early 32-bit framebuffer.
// Each five-column glyph is drawn as solid geometry, without font assets.
#define LOGO_GLYPHS 4
#define GLYPH_ROWS 7
#define GLYPH_COLS 5

#define LOGO_INK    0x00e8f0f7
#define LOGO_ACCENT 0x003bd6c6
#define LOGO_MUTED  0x00687886

static const unsigned char glyphs[LOGO_GLYPHS][GLYPH_ROWS] = {
    { 0x0e, 0x1b, 0x11, 0x11, 0x11, 0x1b, 0x0e }, // O
    { 0x0f, 0x18, 0x10, 0x0e, 0x01, 0x03, 0x1e }, // S
    { 0x0e, 0x1b, 0x13, 0x15, 0x19, 0x1b, 0x0e }, // slashed 0
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, // 1
};

// Fill a rectangle in the framebuffer described by `view`.  The geometry is
// clipped to the framebuffer bounds; nothing is drawn outside it.
static void fill_rect(const struct fb_write_view *view, int x, int y,
                      int width, int height, unsigned int color)
{
    uint32_t xres = view->xres;
    uint32_t yres = view->yres;
    uint32_t *fb_base = view->addr;
    if (!fb_base) return;

    int right = x + width;
    int bottom = y + height;

    if (x >= (int)xres || y >= (int)yres ||
        right <= 0 || bottom <= 0)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > (int)xres) right = (int)xres;
    if (bottom > (int)yres) bottom = (int)yres;

    for (int py = y; py < bottom; py++) {
        uint32_t *pixel = fb_base + py * xres + x;
        for (int px = x; px < right; px++)
            *pixel++ = color;
    }
}

// Draw the wordmark and its caption into the framebuffer described by
// `view`.  The geometry is unchanged from the original x86_64-only
// implementation; only the framebuffer source differs per arch.
static void logo_draw(const struct fb_write_view *view, const char *label)
{
    uint32_t xres = view->xres;
    uint32_t yres = view->yres;

    int scale = xres >= 800 ? 14 :
                xres >= 480 ? 11 : 8;
    if (yres < 300)
        scale = 7;

    int left = xres >= 400 ? 32 : 16;
    int top = 24;
    int advance = 7 * scale;
    int width = (LOGO_GLYPHS - 1) * advance + GLYPH_COLS * scale;

    for (int letter = 0; letter < LOGO_GLYPHS; letter++) {
        unsigned int color = letter < 2 ? LOGO_INK : LOGO_ACCENT;
        for (int row = 0; row < GLYPH_ROWS; row++) {
            for (int col = 0; col < GLYPH_COLS; col++) {
                if (glyphs[letter][row] & (1u << (GLYPH_COLS - col - 1)))
                    fill_rect(view, left + letter * advance + col * scale,
                              top + row * scale, scale, scale, color);
            }
        }
    }

    int line_y = top + GLYPH_ROWS * scale + 14;
    fill_rect(view, left, line_y, 3 * scale, 2, LOGO_ACCENT);
    fill_rect(view, left + 3 * scale + 8, line_y,
              width - 3 * scale - 8, 1, LOGO_MUTED);

    int label_row = (line_y + 12 + (int)font->height - 1) / (int)font->height;
    int label_col = left / (int)font->width;
    for (int i = 0; label[i]; i++)
        putchar_at_snap(view->snap, label_col + i, label_row, LOGO_MUTED, BLACK,
                        (unsigned char)label[i]);

    // Leave a blank character row between the wordmark and boot messages.
    Pos.YPosition = label_row + 2;
    Pos.XPosition = 0;
}

int boot_logo_show(void)
{
    struct fb_write_view view;
    uint64_t flags = spin_lock_irqsave(&Pos.lock);

    int rc = arch_fb_write_begin(&view);
    if (rc < 0) {
        spin_unlock_irqrestore(&Pos.lock, flags);
        return rc;
    }

    logo_draw(&view, ARCH_BOOT_LOGO_LABEL);

    arch_fb_write_end(&view);
    spin_unlock_irqrestore(&Pos.lock, flags);
    return 0;
}
```

- [ ] **Step 3: Clean and build x86_64**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang kernel.bin
```

Expected: exit 0.

- [ ] **Step 4: Verify the coordinator moved out of the driver**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
grep -n "fb_writer_begin\|fb_writer_end" kernel/driver/logo.c
```

Expected: NO output (the lease now lives in the x86_64 override).

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
grep -n "fb_writer_begin" kernel/arch/x86_64/platform/fb_write.c
```

Expected: exactly one line, the `fb_writer_begin(&view->lease, 0)` call.

- [ ] **Step 5: Verify the x86_64 symbol set did not change**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
nm build/x86_64-clang/kernel/kernel.elf | grep -E "arch_fb_write_(begin|end)|fb_writer_begin"
```

Expected: `T arch_fb_write_begin`, `T arch_fb_write_end` (strong), plus
`T fb_writer_begin` (still defined once, in `kernel/driver/fb_state.c`).

- [ ] **Step 6: Commit**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
git add kernel/driver/logo.c kernel/include/driver/logo.h
git commit -m "refactor(driver): draw the boot logo through the arch fb_write facade" \
  -m "logo_draw() is now a pure function of an fb_write_view; boot_logo_show()
acquires the view, draws, and releases it.  The glyph table, the geometry and
the colours are unchanged; only the framebuffer source differs per arch.

The x86_64 writer lease moves from kernel/driver/logo.c into the strong
override, so the driver no longer references the display coordinator."
```

---

### Task 4: Wire the logo driver into the aarch64 build

**Files:**
- Modify: `kernel/Makefile` (the aarch64 `KERNEL_C_SOURCES` list, around line 81)

**Interfaces:**
- Consumes: `kernel/driver/logo.c` and `kernel/driver/arch_fb_write.c` (Tasks 2–3).
- Produces: an aarch64 kernel ELF that links `boot_logo_show` and the weak `arch_fb_write_begin`, and still links NO coordinator symbol.

- [ ] **Step 1: Add the two sources to the aarch64 list**

In `kernel/Makefile`, in the `ifeq ($(ARCH),aarch64)` block, change:

```make
                   core/stack_chk.c \
                   core/printk.c \
```

to:

```make
                   core/stack_chk.c \
                   core/printk.c \
                   driver/logo.c \
                   driver/arch_fb_write.c \
```

(The x86_64 branch needs no change: its `$(wildcard driver/*.c)` already picks
up `driver/arch_fb_write.c`, and `ARCH_SOURCE_DIRS` already contains `platform`,
so `arch/x86_64/platform/fb_write.c` is collected by the ARCHDIR wildcard.)

- [ ] **Step 2: Clean and build aarch64**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi-kernel
```

Expected: exit 0.

- [ ] **Step 3: Verify the aarch64 symbol set**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
nm build/aarch64-clang/kernel/kernel.elf | grep -E "arch_fb_write|boot_logo_show|fb_writer_begin|fb_bootstrap_state|fb_publish_initial_mapping"
```

Expected EXACTLY:

```
ffff... T boot_logo_show
ffff... W arch_fb_write_begin
ffff... W arch_fb_write_end
```

and NO line for `fb_writer_begin`, `fb_bootstrap_state` or
`fb_publish_initial_mapping` — the display coordinator must stay out of the
aarch64 build.

- [ ] **Step 4: Commit**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
git add kernel/Makefile
git commit -m "build(aarch64): link the logo driver and the fb_write default" \
  -m "The aarch64 source list is explicit, so driver/logo.c and
driver/arch_fb_write.c are added by hand.  The display coordinator stays out
of the aarch64 build: the ELF links the weak arch_fb_write default only."
```

---

### Task 5: Draw the logo during aarch64 boot

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c` (`aarch64_boot_fb_init()`, around line 359)

**Interfaces:**
- Consumes: `int boot_logo_show(void)` (Task 3); `ARCH_NAME` (Task 2).
- Produces: the serial markers `[logo] boot logo drawn (aarch64, 800x600)` and `[logo] skipped: no framebuffer`; the wordmark on the framebuffer above the `[fb]` banner.

- [ ] **Step 1: Rewrite `aarch64_boot_fb_init()`**

In `kernel/arch/aarch64/boot/main.c`, replace the body of
`aarch64_boot_fb_init()` with:

```c
static void aarch64_boot_fb_init(const struct boot_context *handoff)
{
    /* spin_init(&Pos.lock) MUST run on every boot path — including the
     * no-framebuffer branch — because color_printk callers (slab.c error
     * paths) will spin_lock(&Pos.lock) unconditionally.  BSS zero-init
     * leaves .lock == 0, which means LOCKED per spinlock_T's contract
     * (1=unlocked).  spin_init before the gate keeps the lock usable on
     * every code path. */
    spin_init(&Pos.lock);

    if (!(handoff->flags & BOOT_CONTEXT_HAS_FRAMEBUFFER)) {
        kputs("[fb] no framebuffer in handoff; color_printk disabled\n");
    } else {
        Pos.Phy_addr    = (uint32_t *)handoff->graphics.FrameBufferBase;
        Pos.FB_length   = handoff->graphics.FrameBufferSize;
        Pos.XResolution = handoff->graphics.HorizontalResolution;
        Pos.YResolution = handoff->graphics.VerticalResolution;
        Pos.XPosition   = 0;
        Pos.YPosition   = 0;

        frame_buffer_init();
        if (!Pos.FB_addr)
            kputs("[fb] mapping failed; color_printk disabled\n");
    }

    /* OS01 boot logo.  Drawn FIRST so the [fb] banner below lands under
     * it (the logo advances Pos.YPosition past the wordmark).  The driver
     * is silent; the boot path reports the outcome. */
    if (boot_logo_show() == 0)
        log_info("[logo] boot logo drawn (%s, %ux%u)\n", ARCH_NAME,
                 (unsigned)Pos.XResolution, (unsigned)Pos.YResolution);
    else
        log_info("[logo] skipped: no framebuffer\n");

    if (Pos.FB_addr) {
        color_printk(WHITE, BLUE,
                     "[fb] aarch64 color_printk active (%ux%u)\n",
                     Pos.XResolution, Pos.YResolution);
        kputs("[fb] frame buffer remap succeed (banner on screen)\n");
    }
}
```

Also add the two includes at the top of the file, next to the existing ones:

```c
#include <arch/fb_write.h>            /* ARCH_NAME */
#include <driver/logo.h>              /* boot_logo_show */
```

- [ ] **Step 2: Clean and build**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang aarch64-uefi
```

Expected: exit 0.

- [ ] **Step 3: Run the logo check — it must PASS now**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
python3 qemutests/aarch64_boot_logo.py \
  --check logo \
  --diagnostic-dtb=auto --timeout 120 \
  --firmware build/aarch64-clang/image/QEMU_EFI.fd \
  --image build/aarch64-clang/image/aarch64-uefi.img \
  --qemu qemu-system-aarch64 \
  --log-dir /tmp/a64-logo-green
```

Expected: exit 0, `{"check": "logo", "failures": [], ...}` and
`PASS: aarch64 boot logo suite`. The serial log in
`/tmp/a64-logo-green/logo/stdout.log` must contain
`[logo] boot logo drawn (aarch64, 800x600)`, and the screendump in
`/tmp/a64-logo-green/logo/logo.ppm` must carry exactly 7252 ink pixels and
6552 accent pixels.

- [ ] **Step 4: Run the negative case — it must PASS now**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
python3 qemutests/aarch64_boot_logo.py \
  --check no-fb \
  --diagnostic-dtb=auto --timeout 120 \
  --firmware build/aarch64-clang/image/QEMU_EFI.fd \
  --image build/aarch64-clang/image/aarch64-uefi.img \
  --qemu qemu-system-aarch64 \
  --log-dir /tmp/a64-logo-nofb
```

Expected: exit 0, `{"check": "no-fb", "failures": [], ...}`. The serial log in
`/tmp/a64-logo-nofb/no-fb/stdout.log` must contain
`[logo] skipped: no framebuffer` and `[sched] idle-ready`, and must NOT
contain `[logo] boot logo drawn`. The check waits for `[sched] idle-ready`
(a marker the CURRENT kernel already prints), so it never times out on the
missing skip marker — it reports the missing marker as a failed ASSERTION.

- [ ] **Step 5: Commit**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
git add kernel/arch/aarch64/boot/main.c
git commit -m "feat(aarch64): draw the OS01 boot logo during boot" \
  -m "aarch64_boot_fb_init() now draws the wordmark right after the
framebuffer is mapped, so the existing [fb] banner lands under it.

The driver stays silent; the boot path reports the outcome:
  [logo] boot logo drawn (aarch64, 800x600)
  [logo] skipped: no framebuffer"
```

---

### Task 6: Documentation and the full gate

**Files:**
- Modify: `docs/arch.md` (the facade/override matrix)
- Modify: `AGENTS.md` (the key-files table — add the aarch64 logo line)

**Interfaces:**
- Consumes: everything from Tasks 1–5.
- Produces: the final green gate.

- [ ] **Step 1: Add the facade row to `docs/arch.md`**

In the facade/override matrix table, after the `arch/include/rtc.h` row, add:

```markdown
| `arch/include/fb_write.h` `arch_fb_write_begin/end` | `driver/arch_fb_write.c`（弱默认：直绘 `Pos.FB_addr`，无 coordinator） | `arch/x86_64/platform/fb_write.c`（强覆盖：display coordinator writer lease，lease 内嵌于调用方持有的 `fb_write_view`） | —（用弱默认） | 独占 framebuffer 写入视图；`fb_write_view` 自持 arch 私有存储，`snap` 指针在 begin→end 之间有效 |
```

- [ ] **Step 2: Add the aarch64 logo row to `AGENTS.md`**

In the "Key files" table of `AGENTS.md`, after the `kernel/arch/x86_64/trampoline.S` row, add:

```markdown
| `kernel/driver/logo.c` | OS01 启动字标（arch-neutral，经 `arch/fb_write.h` 取帧缓冲） |
```

and in the "Architecture" code block, after the `Init:` line, add:

```
Logo:    aarch64_boot_fb_init() → frame_buffer_init() → boot_logo_show() → [fb] banner
```

- [ ] **Step 3: Run the whole aarch64 suite through the make target**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=aarch64-clang test-aarch64-boot-logo
```

Expected: exit 0, `PASS: aarch64 boot logo suite`, with all three checks
reported (`fb-visible`, `logo`, `no-fb`).

- [ ] **Step 4: Run the x86_64 gates (no regression)**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=x86_64-clang kernel.bin
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
```

Expected: all exit 0.

- [ ] **Step 5: Run the aarch64 audit gate**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
make PROFILE=aarch64-clang test-aarch64-audit
```

Expected: exit 0.

- [ ] **Step 6: Boot x86_64 for real and assert its serial stream is unchanged**

The source grep below cannot prove the kernel actually boots; this step runs
a real x86_64 boot (OVMF + the normal disk image) and asserts on the serial
stream, which is what spec V5 criterion 1 asks for.

Run this block as a plain **bash script** (not as a make recipe — hence the
single `$`):

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
# 1. The firmware is NOT produced by disk.img — build it explicitly.  The
#    rule downloads RELEASEX64_OVMF.fd; offline, fall back to the copy the
#    main workspace already fetched.
mkdir -p build/x86_64-clang/firmware
make PROFILE=x86_64-clang build/x86_64-clang/firmware/OVMF.fd \
  || cp /home/aagu/OS01/build/x86_64-clang/firmware/OVMF.fd \
       build/x86_64-clang/firmware/OVMF.fd
make PROFILE=x86_64-clang disk.img
FW=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^firmware=//p')
IMG=$(make -s PROFILE=x86_64-clang print-run-paths | sed -n 's/^image=//p')
test -f "$FW" || { echo "ERROR: firmware missing: $FW" >&2; exit 1; }
test -f "$IMG" || { echo "ERROR: image missing: $IMG" >&2; exit 1; }

# 2. Boot.  The kernel idles forever, so `timeout` ends the run on purpose;
#    124 (timeout) and 137 (killed) are the ONLY accepted exit codes.
timeout 120 qemu-system-x86_64 -M q35 \
  -drive if=pflash,format=raw,readonly=on,file="$FW" \
  -drive file="$IMG",format=raw,if=none,id=disk -device ahci,id=ahci \
  -device ide-hd,drive=disk,bus=ahci.0 -m 512 -smp 1 -serial stdio \
  -display none -no-reboot -no-shutdown > /tmp/x86-logo.log 2>&1; rc=$?
if [ "$rc" -ne 124 ] && [ "$rc" -ne 137 ]; then
  echo "ERROR: qemu exited $rc" >&2; tail -20 /tmp/x86-logo.log >&2; exit 1
fi

# 3. Assertions — EVERY one must hold, and any failure exits non-zero.
grep -q "frame buffer remap succeed" /tmp/x86-logo.log \
  || { echo "ERROR: x86_64 never reported the framebuffer remap" >&2; exit 1; }
if grep -q "\[logo\]" /tmp/x86-logo.log; then
  echo "ERROR: x86_64 serial stream gained a [logo] marker" >&2; exit 1
fi
echo "OK: x86_64 serial stream unchanged (fb line present, no [logo] marker)"
```

Expected: `OK: x86_64 serial stream unchanged (fb line present, no [logo] marker)`,
and the recipe's exit status 0. Any missing line, any unexpected `[logo]`
marker, a missing firmware/image, or an unexpected QEMU exit code aborts the
step with a non-zero status — a failed acceptance can never end green.

Note: `make PROFILE=x86_64-clang disk.img` builds BusyBox and the rootfs and
takes several minutes — the same artifacts `make PROFILE=x86_64-clang
test-qemu SUITE=phase-0` uses. Run it once per worktree.

- [ ] **Step 7: Confirm no x86_64 source prints a `[logo]` marker**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
grep -rn "\[logo\]" kernel/arch/x86_64/ kernel/driver/ kernel/core/ || echo "no x86_64 [logo] marker"
```

Expected: `no x86_64 [logo] marker` — the driver prints nothing, so the x86_64
serial stream is byte-identical to before this change. This is the auxiliary
source-level check that complements Step 6's runtime assertion.

- [ ] **Step 8: Commit**

```bash
cd /home/aagu/OS01/.worktrees/aarch64-boot-logo
git add docs/arch.md AGENTS.md
git commit -m "docs: record the fb_write facade and the aarch64 boot logo" \
  -m "Adds the arch/fb_write.h row to the facade/override matrix and the logo
driver to the AGENTS.md key-files table."
```

---

## Self-Review

**Spec coverage** — every spec section maps to a task:

| Spec section | Task |
|---|---|
| §1 facade `arch/fb_write.h` + implementations | Task 2 |
| §1 `arch/identity.h` (ARCH_NAME, ARCH_BOOT_LOGO_LABEL) | Task 2 |
| §2 `kernel/driver/logo.c` refactor (`logo_draw`, `boot_logo_show`) | Task 3 |
| §3 aarch64 call site + markers | Task 5 |
| §4 build wiring (aarch64 list; x86_64 unchanged) | Task 4 (+ Task 2 for the x86_64 wildcard pickup) |
| V1 build gates + `nm` assertions | Task 2 Step 6, Task 3 Step 5, Task 4 Step 3, Task 6 |
| V2 serial markers | Task 5 Steps 3–4 |
| V3 screendump pixel assertions | Task 1 Step 1 (`check_logo`), Task 5 Step 3 |
| V4 negative case | Task 1 Step 1 (`check_no_fb`), Task 5 Step 4 |
| V5 x86_64 no-regression | Task 2 Step 6, Task 3 Steps 4–5, Task 6 Step 6 |
| V6 make target | Task 1 Step 2 |
| Change list (incl. `docs/arch.md`) | Tasks 1–6 |

**Placeholder scan** — every code block contains the complete content; no
"TBD", no "similar to Task N", no unspecified error handling. The two
behavioural branches the driver can take (framebuffer present / absent) are
both specified with their exact serial strings.

**Type consistency** — `struct fb_write_view` (Task 2) has the fields
`lease`, `snap`, `addr`, `xres`, `yres`; Task 3's `logo_draw` reads exactly
`view->xres`, `view->yres`, `view->addr`, `view->snap`; Task 2's two
implementations fill exactly those fields. `boot_logo_show` returns `int` in
Task 3's header, Task 3's definition and Task 5's call site. `ARCH_NAME` and
`ARCH_BOOT_LOGO_LABEL` are defined in Task 2 and consumed in Task 3 (label) and
Task 5 (name). The test module's `logo_geometry()` mirrors the C scale rule and
the C label-row formula (`(line_y + 12 + FONT_H - 1) // FONT_H`) — the same
expression as `kernel/driver/logo.c`.

**Known divergence, intentional** — the test's `GLYPH_BITS = (20, 17, 23, 10)`
is a hand-mirrored copy of the C bitmap's set-bit counts. If the C bitmaps
ever change, the test must be updated in the same commit; the plan states the
counts' source in both files so a reviewer can check them.

**Plan review findings addressed** (review of `ce5a3b72`):

| Finding | Fix |
|---|---|
| [P1] the screendump was taken AFTER `session.stop()`, so QEMU was already dead and every ramfb check would fail | `_boot_and_capture()` now connects QMP, dumps and parses INSIDE the live window; `stop()`/`close()` happen in the `finally` afterwards |
| [P2] the `logo` check waited for a marker the kernel did not have yet, so the RED phase timed out instead of reporting missing pixels | all three checks now wait for markers the CURRENT kernel already prints (`[fb] frame buffer remap succeed`, `[sched] idle-ready`); the missing logo is a failed ASSERTION with a screenshot |
| [P2] the negative case stopped two seconds after the skip marker | it now waits for `[sched] idle-ready` and asserts BOTH the skip marker and the absence of the drawn marker |
| [P2] the ink assertion accepted ink anywhere in the 4-glyph wordmark | `check_logo()` counts ink inside the first TWO glyph cells (`x < left + 2*advance`) and compares against the whole-screen ink count, so ink drawn in the accent cells fails |
| [P2] the three boots shared one log dir (`stdout.log` is opened with "w") and nothing was archived | each check gets its own `RunArchive.create()` directory and a `result.json` RunReport |
| [P2] the x86_64 serial acceptance was a source grep, not a boot | Task 6 Step 6 boots x86_64 for real (OVMF + disk.img) and asserts on the serial stream; the source grep remains as an auxiliary check in Step 7 |

**Second review (`bb14ec67`) — three further findings, all fixed:**

| Finding | Fix |
|---|---|
| [P1] the QMP socket lived inside the archive directory; that path is ~157 bytes and exceeds the ~108-byte `sun_path` limit, so QEMU failed with "AF_UNIX path too long" | the socket is created in a short `tempfile.mkdtemp(prefix="os01-a64-")` directory and removed afterwards; the screenshot and the logs stay in the archive |
| [P2] the x86_64 step did not build the firmware and could end green on a failed acceptance | the firmware is built explicitly (with an offline fallback copy), `test -f` guards both artifacts, only QEMU exit codes 124/137 are accepted, and every assertion aborts the step with a non-zero status |
| [P2] environment failures were archived as FAIL / exit 1, and the DTB generation sat outside the `try`, so it skipped `result.json` | every preparation or launch failure is now flagged `environment_error`, archived with status `ERROR` and runner exit code 2, and the whole run exits 2; the DTB generation moved inside the guarded region |

**Third review (`3ec959d2`) — two further findings, both fixed:**

| Finding | Fix |
|---|---|
| [P2] the x86_64 bash block used the Makefile `$$` escape, so `$?` became the shell PID and the exit-code guard never rejected a failed QEMU | the block is now plain bash (`rc=$?`, `"$rc"`) and the plan states explicitly that it is NOT a make recipe |
| [P2] after a `wait_for()` timeout the child is reaped, so the QMP connect raised and the timeout was misreported as an environment error (exit 2) instead of a FAIL (exit 1) | `_boot_and_capture()` skips the QMP block when `session.timed_out` is set; the serial log is kept and the run is reported as FAIL / TIMEOUT (exit 1) |

Both were verified by executing the plan's module and the bash logic directly:
a simulated timeout now yields `exit=1, status=FAIL, environment_error=false` for
both the `logo` and the `no-fb` check, and the bash guard rejects a failing
command with `REJECTED (rc=1)`.
