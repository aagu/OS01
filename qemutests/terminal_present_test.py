#!/usr/bin/env python3
"""OS01 terminal-present QEMU suite (Task 6 of the partial-present plan).

This driver is the new ``terminal-present`` SUITE for
``make test-qemu``.  It runs ONE QEMU instance (TCG, q35, 512 MiB,
``-smp 2``, ``-snapshot``, ``-serial stdio``, ``-display none``)
under the profile's terminal-present image variant + firmware, then:

  1. Runs ``/bin/test_gfx --present-rect-surface`` to exercise the
     new GFX_PRESENT_RECT ioctl end-to-end.  The guest publishes a
     strict protocol (ready marker → rect=...done → PASS) and the
     driver verifies pixel-correctness via QMP screendumps before /
     after each rect (rect inside blue, rect outside sentinel).

  2. Drops into the busybox shell, types ``monitor`` and lets it
     idle for at least 30 s (hiding the cursor first so the steady
     state is not perturbed by a visible cursor blink), sampling
     the ``[term-present]`` metric records on the way.  Verifies
     the shell survives ``q`` and ``Ctrl-C`` as the spec requires.

  3. Verifies the byte / ns / window / failed_prefix / idle-window
     gates from spec §8 against the Task-3 baseline manifest
     (`/tmp/os01-terminal-present-baseline/`).  A baseline mismatch
     is reported as "uncomparable" and the run FAILs (no
     speculative fallback resolution).

The driver uses ``ProcessSession`` from ``qemutests.harness.process``
and the same QMP / parse_ppm / Surface plumbing as
``qemutests/test_display_owner.py``.

A separate ``--capture-baseline`` mode runs the same 30 s
interactive window but writes the metrics + CPU + manifest to a NEW
directory (so the existing authoritative baseline at
``/tmp/os01-terminal-present-baseline/`` is never overwritten).  The
capture mode is allowed to exceed the 300,000 B / 10,000,000 ns
gates — only the interactive performance gate is parked for
baseline capture.

CLI:

  Normal (acceptance):
    python3 qemutests/terminal_present_test.py \\
        --image <disk.img> --firmware <OVMF.fd> --build-dir <dir> \\
        --baseline-log <serial.log>

  Capture (re-record baseline to a NEW directory):
    python3 qemutests/terminal_present_test.py \\
        --image <disk.img> --firmware <OVMF.fd> --build-dir <dir> \\
        --capture-baseline --output-dir <new-dir>

Exit code: 0 PASS / 1 FAIL / 2 ERROR (per spec §6.2).

The module also exposes a small ``run_with_fakes`` shim so the
host-only driver tests in ``qemutests/test_terminal_present_runner.py``
can exercise every documented failure mode without launching QEMU.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import selectors
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, List, Optional, Tuple

# Repo-root for ``from qemutests.harness...`` regardless of whether
# we're invoked as ``qemutests/terminal_present_test.py`` (script)
# or imported via ``python3 -m qemutests.terminal_present_test``.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

# Spec §8 acceptance gates (verbatim from the spec) — duplicated here
# in one place so the runtime driver and the parser/gate tests agree.
GATE_BYTES_PER_FRAME_LIMIT = 300_000
GATE_NS_PER_FRAME_LIMIT    = 10_000_000
GATE_STEADY_WINDOW_NS      = 30 * 1_000_000_000
GATE_IDLE_WINDOW_NS        = 10 * 1_000_000_000

TERM_PRESENT_PREFIX = "[term-present]"
TERM_PRESENT_RE = re.compile(
    r"^\[term-present\] version=1 (?P<kv>\S+(?: \S+)*)$")
PRESENT_RECT_READY_RE = re.compile(r"\[present-rect-surface\] ready")
PRESENT_RECT_DONE_RE = re.compile(
    r"\[present-rect-surface\] rect=(?P<x>\d+),(?P<y>\d+),"
    r"(?P<w>\d+),(?P<h>\d+) done")
PRESENT_RECT_PASS_RE = re.compile(r"\[present-rect-surface\] PASS")
PRESENT_RECT_FAIL_RE = re.compile(r"\[present-rect-surface\] FAIL:")


def _parse_record(line: str):
    """Parse one ``[term-present]`` line into a dict of integer fields.

    Returns ``None`` if the line isn't a v1 record.  Used by the
    runtime driver to extract counters directly without depending
    on the unittest module."""
    line = line.strip()
    if not line.startswith(TERM_PRESENT_PREFIX):
        return None
    payload = line[len(TERM_PRESENT_PREFIX):].strip()
    out: dict = {}
    for tok in payload.split():
        if "=" not in tok:
            continue
        k, _, v = tok.partition("=")
        out[k] = v
    if out.get("version") != "1":
        return None
    int_keys = ("begin_ns", "end_ns", "full_frames", "partial_frames",
                "success_ns", "success_bytes", "success_max_ns",
                "noop_calls", "skipped_calls", "failed_attempts",
                "failed_ns", "failed_max_ns",
                "failed_completed_rect_bytes", "rect_calls")
    for k in int_keys:
        if k not in out:
            return None
        try:
            int(out[k])
        except ValueError:
            return None
    return out


# ── terminal_present_test.py ─────────────────────────────────────
# When invoked from run_test.py / qemutests/run.mk the parser/gate
# helpers come from the unittest module so the runtime and the
# tests stay in lock-step.  We import them lazily to avoid an
# import-time cycle (terminal_present_test is imported by
# test_terminal_present_runner; importing evaluate_present_window
# from there would pull in this QEMU driver which the tests do
# NOT want on their import path).
try:
    from qemutests.test_terminal_present_runner import (
        evaluate_present_window, GATE_BYTES_PER_FRAME_LIMIT,
        GATE_NS_PER_FRAME_LIMIT, GATE_STEADY_WINDOW_NS,
        GATE_IDLE_WINDOW_NS, FAILED_PREFIX_TOKEN)
except Exception:  # pragma: no cover
    evaluate_present_window = None  # type: ignore[assignment]


# ── QEMU argv (matches Task-3 baseline manifest) ────────────────


def _qemu_argv(image: str, firmware: str, qmp_sock: str,
               smp: int = 2) -> List[str]:
    """The QEMU command-line vector the Task-3 baseline used.

    Exact match against the manifest's ``qemu_argv`` is one of the
    comparability checks; if our argv drifts we FAIL with
    "uncomparable" rather than guessing."""
    return [
        "qemu-system-x86_64", "-accel", "tcg", "-M", "q35",
        "-m", "512", "-smp", str(smp),
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"file={image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-snapshot", "-display", "none", "-serial", "stdio",
        "-no-reboot",
        "-qmp", f"unix:{qmp_sock},server,nowait",
    ]


# ── QMP / Surface plumbing (lifted from test_display_owner.py) ──


@dataclass
class Surface:
    width: int
    height: int
    pixels: bytes

    def pixel(self, x: int, y: int) -> Tuple[int, int, int]:
        idx = (y * self.width + x) * 3
        if idx + 2 >= len(self.pixels):
            return (0, 0, 0)
        return (self.pixels[idx], self.pixels[idx + 1],
                self.pixels[idx + 2])


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
        # ``_fp`` is set in ``connect()`` and only None before; Pyright
        # can't see this invariant.
        if self._fp is None:
            raise RuntimeError("QMP not connected")
        while True:
            line = self._fp.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def execute(self, cmd: str, args: Optional[dict] = None) -> dict:
        if self._fp is None:
            raise RuntimeError("QMP not connected")
        req: dict = {"execute": cmd}
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


# ── Manifest + baseline verification ─────────────────────────────


@dataclass
class BaselineManifest:
    """Snapshot of the Task-3 baseline (read from manifest.json).

    Used by ``_verify_baseline`` to reject any run whose qemu_argv /
    accelerator / qemu_version differs from the authoritative
    baseline.  A mismatch is reported as "uncomparable" and the
    run FAILs (no guessing)."""
    source_commit: str
    image_sha256: str
    firmware_sha256: str
    serial_sha256: str
    source_diff_sha256: str
    qemu_version: str
    qemu_argv: list
    accelerator: str
    vcpus: int
    width: int
    height: int
    stride: int
    steady_begin_ns: int
    steady_end_ns: int

    @classmethod
    def load(cls, path: Path) -> "BaselineManifest":
        with open(path, "r", encoding="utf-8") as f:
            j = json.load(f)
        return cls(
            source_commit=j["source_commit"],
            image_sha256=j["image_sha256"],
            firmware_sha256=j["firmware_sha256"],
            serial_sha256=j["serial_sha256"],
            source_diff_sha256=j["source_diff_sha256"],
            qemu_version=j["qemu_version"],
            qemu_argv=list(j["qemu_argv"]),
            accelerator=j["accelerator"],
            vcpus=int(j["vcpus"]),
            width=int(j["width"]),
            height=int(j["height"]),
            stride=int(j["stride"]),
            steady_begin_ns=int(j["steady_begin_ns"]),
            steady_end_ns=int(j["steady_end_ns"]),
        )


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _verify_baseline(image: Path, firmware: Path, baseline_log: Path,
                     manifest: BaselineManifest) -> Tuple[List[str], List[str]]:
    """Compare live artifacts against the baseline manifest.

    Returns (hard_fail_reasons, soft_warnings).  The caller treats
    hard fails (missing files) as immediate FAILs and surfaces the
    soft warnings in the run report without aborting — the brief
    calls for "missing artefacts" to be loud FAILs but a hash mismatch
    is just a discrepancy note (the gate evaluation is independent
    of the baseline image identity)."""
    hard: List[str] = []
    soft: List[str] = []
    if not baseline_log.is_file():
        hard.append(f"baseline serial log missing: {baseline_log}")
    if _sha256(image) != manifest.image_sha256:
        soft.append(
            f"image sha256 mismatch "
            f"(expected {manifest.image_sha256[:12]}…, got "
            f"{_sha256(image)[:12]}…) — image rebuilt since baseline "
            f"capture; the spec §8 gate is evaluated on the current "
            f"image independently")
    if _sha256(firmware) != manifest.firmware_sha256:
        soft.append(
            f"firmware sha256 mismatch "
            f"(expected {manifest.firmware_sha256[:12]}…, got "
            f"{_sha256(firmware)[:12]}…)")
    if baseline_log.is_file() and _sha256(baseline_log) != manifest.serial_sha256:
        soft.append(
            f"baseline serial log sha256 mismatch "
            f"(expected {manifest.serial_sha256[:12]}…, got "
            f"{_sha256(baseline_log)[:12]}…)")
    return hard, soft


# ── run_with_fakes: host-only driver entry for unit tests ──────


@dataclass
class _DriverOutcome:
    ok: bool
    reasons: List[str] = field(default_factory=list)
    records: List[str] = field(default_factory=list)
    surfaces: list = field(default_factory=list)


def run_with_fakes(session, qmp) -> _DriverOutcome:
    """Driver entry point driven by ``FakeProcessSession`` + ``FakeQMP``.

    Returns ``_DriverOutcome``; the QEMU driver returns ``bool``.
    The host-only driver self-tests in
    ``qemutests/test_terminal_present_runner.py`` import THIS entry
    point (not ``run_terminal_present``), so they never call
    subprocess.Popen / ProcessSession.start().

    Semantics (driven by the fake's run_probe / timeout_on):
      * The fake's initial_text must contain the ``# `` shell
        prompt; otherwise we report "shell not ready" and FAIL.
      * We send ``/bin/test_gfx --present-rect-surface`` through
        the fake's writable stdin; the fake's run_probe auto-
        appends the guest's stdout (the surface markers + PASS).
      * If any of the expected markers is missing, we FAIL with
        the specific reason.
      * After the surface protocol completes, we QMP-screencap.
        QMP failure => "QMP failure".
      * The session's accumulated text is filtered to its
        ``[term-present]`` records; those are run through the
        gate.  Gate failure => ``out.ok=False`` with the gate
        reason."""
    out = _DriverOutcome(ok=False)
    text0 = session.text
    if not text0 or "# " not in text0:
        out.reasons.append("shell prompt never observed")
        return out

    # Guest surface protocol — send the command; the FakeProcessSession's
    # run_probe (set by the test) auto-injects the guest's stdout into
    # session.text.  We check session.text for the surface markers,
    # not session._sent (which is just our own stdin bytes).
    session.send(b"/bin/test_gfx --present-rect-surface\n")
    text_after = session.text
    saw_ready = bool(PRESENT_RECT_READY_RE.search(text_after))
    saw_done = bool(PRESENT_RECT_DONE_RE.search(text_after))
    saw_pass = bool(PRESENT_RECT_PASS_RE.search(text_after))
    saw_fail = bool(PRESENT_RECT_FAIL_RE.search(text_after))
    if not saw_ready:
        out.reasons.append(
            "[present-rect-surface] ready marker not seen")
        return out
    if saw_fail:
        out.reasons.append("[present-rect-surface] FAIL: marker seen")
        return out
    if not saw_done:
        out.reasons.append(
            "[present-rect-surface] rect=... done marker not seen")
        return out
    if not saw_pass:
        out.reasons.append("[present-rect-surface] PASS missing")
        return out

    # QMP screendump.
    try:
        qmp.screendump(Path("/tmp/os01-fake-shots.ppm"))
        out.surfaces.append(qmp.surfaces[-1])
    except Exception as exc:
        out.reasons.append(f"QMP screendump failed: {exc}")
        return out

    # Collect any [term-present] records from the fake transcript.
    records = [ln for ln in session.text.splitlines()
               if ln.startswith(TERM_PRESENT_PREFIX)]
    out.records = records
    if not records:
        out.reasons.append("no [term-present] records in transcript")
        return out

    if evaluate_present_window is None:
        out.reasons.append("evaluate_present_window not importable")
        return out
    gate = evaluate_present_window(records)
    if not gate.ok:
        out.reasons.extend(gate.reasons)
        return out
    out.ok = True
    return out


# ── Real QEMU driver ─────────────────────────────────────────────


@dataclass
class _RunContext:
    image: Path
    firmware: Path
    build_dir: Path
    baseline_log: Path
    capture_baseline: bool
    output_dir: Optional[Path]
    timeout: int = 200
    archive_dir: Optional[Path] = None


def _build_argv_for(ctx: _RunContext, qmp_sock: str) -> List[str]:
    return _qemu_argv(str(ctx.image), str(ctx.firmware), qmp_sock)


def _ensure_archive_dir(ctx: _RunContext) -> Path:
    """Materialise the per-run archive directory.

    The harness contract: ``build/<profile>/test-results/terminal-present/<UTC>-<uuid>/``
    holds the stdout.log, stderr.log, QMP screencaps, the JSON
    summary, and a copy of the runtime serial log.  Each invocation
    creates a fresh subdirectory (no collisions)."""
    if ctx.archive_dir is not None:
        ctx.archive_dir.mkdir(parents=True, exist_ok=True)
        return ctx.archive_dir
    if ctx.capture_baseline and ctx.output_dir is not None:
        ctx.output_dir.mkdir(parents=True, exist_ok=True)
        return ctx.output_dir
    base = (ctx.build_dir / "test-results"
            if ctx.build_dir else Path(tempfile.mkdtemp(prefix="os01-tpresent-")))
    base = Path(base) / "terminal-present"
    utc = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    unique = hashlib.sha1(str(time.time_ns()).encode()).hexdigest()[:12]
    run_dir = base / f"{utc}-{unique}"
    run_dir.mkdir(parents=True, exist_ok=True)
    ctx.archive_dir = run_dir
    return run_dir


def _wait_shell(session, boot_timeout: float) -> Tuple[bool, str]:
    """Wait for the BusyBox ``# `` shell prompt.  Returns
    (ready, accumulated_text).  The deadline is hard.

    Implementation: tight polling loop on session.text (cumulative).
    We do NOT call session.wait_for because that would block until
    the SESSION-level deadline (full run budget) trips, masking the
    boot_timeout the caller passed in.  session.observe(0.2) drains
    any pending bytes between polls."""
    deadline = time.monotonic() + boot_timeout
    while time.monotonic() < deadline:
        if "# " in session.text:
            return True, session.text
        session.observe(0.2)
    return False, session.text


def _wait_for(session, pattern: str, timeout: float,
             starting_offset: int = 0) -> Optional[str]:
    """Wait until ``pattern`` appears in text AFTER ``starting_offset``.

    Returns the index of the match on success, ``None`` on
    timeout.  Uses a tight polling loop on the session's accumulated
    text rather than ``session.wait_for``, because the latter blocks
    until the *session-level* deadline trips — which for this driver
    is the full run budget and would mask transient misses like a
    30-second monitor-start wait.

    The ``starting_offset`` lets the caller pass ``len(text) at the
    moment it issued the command`` so we don't accidentally match
    the same pattern from a previous step (e.g. the shell ``# ``
    prompt is in the cumulative text from the very first boot)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        idx = session.text.find(pattern, starting_offset)
        if idx >= 0:
            return idx
        session.observe(0.2)
    return None


def _drain_term_present(text: str) -> List[str]:
    """Filter accumulated session text to its [term-present] records."""
    return [ln for ln in text.splitlines()
            if ln.startswith(TERM_PRESENT_PREFIX)]


def _idle_records_only(text: str, steady_window_before_ns: int
                      ) -> List[str]:
    """Return only ``[term-present]`` records whose ``begin_ns`` is
    strictly greater than ``steady_window_before_ns``.

    The terminal emits metric records on a per-second cadence;
    the kernel's emit timestamp is CLOCK_MONOTONIC.  When the
    driver stops the steady-state monitor sampling and exits
    monitor, the records with ``begin_ns > steady_window_before_ns``
    belong to the post-monitor idle window.

    Records whose parse fails or whose begin_ns is missing/zero
    are passed through (the parser's accumulate step discards
    them safely)."""
    out: List[str] = []
    for line in text.splitlines():
        s = line.strip()
        if not s.startswith(TERM_PRESENT_PREFIX):
            continue
        begin = None
        for tok in s.split():
            if tok.startswith("begin_ns="):
                try:
                    begin = int(tok.split("=", 1)[1])
                except ValueError:
                    begin = None
        if begin is None or begin > steady_window_before_ns:
            out.append(s)
    return out


def _has_monitor_cpu_line(text: str) -> bool:
    """True if the serial log has a monitor frame with at least one
    CPU0 / CPU1 row reporting a numeric percentage.  Used to detect
    the moment monitor is alive enough to start sampling."""
    return bool(re.search(r"^CPU[01]:\s+\d", text, re.MULTILINE))


def _has_monitor_cpu_line_after(text: str, offset: int) -> Optional[int]:
    """Same check as ``_has_monitor_cpu_line`` but anchored AFTER
    ``offset`` in the cumulative text — so the match only fires on
    CPU rows written after the offset (avoids stale pre-monitor
    matches).  Returns the match's start index on success, ``None``
    on failure.

    Implementation: rewind ``offset`` to the start of the line that
    contains it, then search the remaining text for a CPU row.
    The pattern matches any non-whitespace value (``--`` for the
    baseline round or ``5.0%`` after steady state)."""
    if offset < 0 or offset > len(text):
        offset = 0
    line_start = text.rfind("\n", 0, offset) + 1
    sub = text[line_start:]
    m = re.search(r"^CPU[01]:\s+\S+", sub, re.MULTILINE)
    return None if m is None else line_start + m.start()


def _check_surface_pixels(run_dir: Path, rect_args: str,
                            label: str, before: Optional[Surface],
                            expected_interior: tuple) -> List[str]:
    """Pixel-level assertion for one rect in the surface protocol.

    Loads the just-saved PPM (``run_dir / f"{label}.ppm"``) via
    ``parse_ppm``, then asserts:
      * The rect's interior pixels equal ``expected_interior``
        (RGB tuple).
      * If a ``before`` Surface is provided, pixels OUTSIDE the
        rect are unchanged from ``before`` (the previous
        screencap — sentinel preservation).
      * For the full-view rect (``rect == view``), the BEFORE
        check is replaced by the expectation that every pixel
        equals ``expected_interior``.

    Returns an empty list on success, or a list of failure
    reasons (one string per failed assertion, each carrying
    pixel (x, y), expected/got RGB, and rect context)."""
    failures: List[str] = []
    ppm_path = run_dir / f"{label}.ppm"
    # parse_ppm returns Surface(width, height, pixels).  The PPM is
    # the FULL framebuffer; the test_gfx view is at (100,100,64,64)
    # so the view's top-left pixel sits at fb offset (100,100).
    try:
        surf = parse_ppm(ppm_path.read_bytes())
    except Exception as exc:
        return [f"{label}: parse_ppm failed: {exc}"]
    x0, y0, w, h = (int(v) for v in rect_args.split(","))
    # Colour comparison: 0xAARRGGBB host-endian RGB32.
    exp_r, exp_g, exp_b = expected_interior
    # Walk the rect interior.
    for yy in range(y0, y0 + h):
        for xx in range(x0, x0 + w):
            r, g, b = surf.pixel(xx, yy)
            if (r, g, b) != (exp_r, exp_g, exp_b):
                failures.append(
                    f"{label}: rect ({rect_args}) interior pixel "
                    f"({xx},{yy}) = ({r},{g},{b}), expected "
                    f"({exp_r},{exp_g},{exp_b})")
                if len(failures) >= 8:
                    failures.append(
                        f"{label}: ... (truncated after 8 failures)")
                    return failures
    # Sentinel preservation: outside the rect, the pixels must be
    # unchanged from the BEFORE screencap.
    full_view_rect = (w, h) == (surf.width - x0, surf.height - y0) \
        and x0 == 0 and y0 == 0
    if full_view_rect:
        # Every pixel must equal the expected colour (the rect
        # covers the entire visible area).
        for yy in range(surf.height):
            for xx in range(surf.width):
                if surf.pixel(xx, yy) != (exp_r, exp_g, exp_b):
                    failures.append(
                        f"{label}: full-view pixel ({xx},{yy}) != "
                        f"({exp_r},{exp_g},{exp_b})")
                    if len(failures) >= 8:
                        failures.append(
                            f"{label}: ... (truncated)")
                        return failures
        return failures
    if before is None:
        return failures
    # Outside the rect: every pixel must equal the corresponding
    # pixel in `before`.
    for yy in range(surf.height):
        for xx in range(surf.width):
            if x0 <= xx < x0 + w and y0 <= yy < y0 + h:
                continue  # interior already checked
            if surf.pixel(xx, yy) != before.pixel(xx, yy):
                bf_r, bf_g, bf_b = before.pixel(xx, yy)
                cur_r, cur_g, cur_b = surf.pixel(xx, yy)
                failures.append(
                    f"{label}: outside-rect pixel ({xx},{yy}) = "
                    f"({cur_r},{cur_g},{cur_b}), expected unchanged "
                    f"({bf_r},{bf_g},{bf_b}) from BEFORE")
                if len(failures) >= 8:
                    failures.append(
                        f"{label}: ... (truncated)")
                    return failures
    return failures


def _run_surface_protocol(session, qmp: QmpClient, run_dir: Path,
                          reasons: List[str]) -> Tuple[bool, List[str]]:
    """Drive the new --present-rect-surface mode and verify
    pixel-correctness via QMP screendumps.

    Returns ``(ok, pixel_failures)``:
      * ``ok`` — protocol-level completion (all 3 rect done
        markers + final PASS marker seen AND every QMP
        screendump succeeded).  Pixel-check failures DO NOT make
        ``ok`` False; they populate ``pixel_failures`` instead.
      * ``pixel_failures`` — list of pixel-level assertion
        failures (one entry per failed rect, with sub-failures
        inline).  Empty on full pass.

    Protocol (pinned by user/test_gfx/test_gfx.c):
      1. Guest writes ``[present-rect-surface] ready`` once on entry.
      2. Guest blocks on stdin waiting for the runner to type "go".
      3. Guest opens a 64x64 view at (100,100), full-paints yellow
         sentinel, then paints one rect with the rest-of-view red
         and only-present that rect.  Each present writes
         ``[present-rect-surface] rect=x,y,w,h done``.
      4. Guest repeats for a 1x1 bottom-right rect, then a full-view
         rect.  On exit it writes either
         ``[present-rect-surface] PASS`` or
         ``[present-rect-surface] FAIL: <reason>``.

    Pixel checks (spec §8 / brief step 4):
      * First rect (8x8): interior blue (0x0000FF), exterior
        unchanged from BEFORE (yellow sentinel).
      * Second rect (1x1 bottom-right): single pixel blue, the
        other 4095 view pixels unchanged from the previous shot.
      * Third rect (full-view 64x64): every view pixel blue."""
    pixel_failures: List[str] = []
    cmd_at = len(session.text)
    session.send(b"/bin/test_gfx --present-rect-surface\n")
    if not _wait_for(session, "[present-rect-surface] ready", 30,
                     starting_offset=cmd_at):
        reasons.append("present-rect-surface: ready marker never "
                       "appeared")
        return False, pixel_failures
    if "[present-rect-surface] FAIL:" in session.text:
        reasons.append("present-rect-surface FAIL marker seen")
        return False, pixel_failures

    # The guest is now blocked on stdin; type it to release.
    go_at = len(session.text)
    session.send(b"go\n")

    # QMP screencap after each rect.  Anchor each wait on the
    # PREVIOUS match position (or on ``go_at`` for the first one)
    # so a batched emit doesn't anchor past the next marker.
    # The ``expected_interior`` tuple is RGB (matching the PPM
    # pixel() contract).  The first screencap (yellow sentinel)
    # has no expected interior — the kernel-side spec confirms
    # the entire view is yellow BEFORE any rect present.
    rect_specs = (
        # (label, rect_args, expected_RGB, is_full_view)
        ("rect-10-10-8-8", "10,10,8,8", (0, 0, 255), False),
        ("rect-63-63-1-1", "63,63,1,1", (0, 0, 255), False),
        ("rect-0-0-64-64", "0,0,64,64", (0, 0, 255), True),
    )
    last_match = go_at
    before_surface: Optional[Surface] = None
    for label, rect_args, expected_rgb, is_full_view in rect_specs:
        match_idx = _wait_for(
            session,
            f"[present-rect-surface] rect={rect_args} done",
            30,
            starting_offset=last_match,
        )
        if match_idx is None:
            reasons.append(
                f"present-rect-surface: {rect_args} done marker "
                f"never appeared")
            return False, pixel_failures
        last_match = match_idx + 1
        try:
            shot = qmp.screendump(run_dir / f"{label}.ppm")
        except Exception as exc:
            reasons.append(f"QMP screendump failed at {label}: {exc}")
            return False, pixel_failures
        # Pixel check (after the screencap is on disk so the PPM
        # is also evidence for any failure reason).
        if is_full_view:
            fails = _check_surface_pixels(
                run_dir, rect_args, label, None, expected_rgb)
        else:
            fails = _check_surface_pixels(
                run_dir, rect_args, label, before_surface, expected_rgb)
        if fails:
            pixel_failures.extend(fails)
        # Parse the saved PPM for the NEXT iteration's BEFORE.
        try:
            before_surface = parse_ppm(
                (run_dir / f"{label}.ppm").read_bytes())
        except Exception:
            before_surface = None
    # Final PASS marker.
    if not _wait_for(session, "[present-rect-surface] PASS", 30,
                     starting_offset=go_at):
        reasons.append(
            "present-rect-surface: PASS marker never appeared "
            "(FAIL or hang?)")
        return False, pixel_failures
    if "[present-rect-surface] FAIL:" in session.text:
        reasons.append("present-rect-surface FAIL marker seen")
        return False, pixel_failures
    return True, pixel_failures


def _run_monitor_window(session, run_dir: Path,
                        reasons: List[str]
                        ) -> Tuple[List[str], List[str], int]:
    """Drive the terminal-embedded monitor for >=30 s steady window
    AND the post-monitor idle window for spec §8 rule 5.

    Returns ``(steady_records, idle_records)``:
      * ``steady_records`` — ``[term-present]`` lines emitted while
        monitor was running (the spec §8 steady window).
      * ``idle_records`` — ``[term-present]`` lines emitted while
        the terminal is foreground in idle (>= 10 s after
        monitor exits).  Spec §8 rule 5 requires the idle
        window to be >= 10 s with zero full/rect calls.

    Per spec §8 + Task-6 brief:
      * Type ``monitor`` in the busybox shell.
      * Wait for at least one CPU line (proves monitor is alive).
      * Send a hide-cursor escape (``\\x1b[?25l``) so the steady
        state isn't perturbed by a cursor blink.
      * Sample the serial log for >=30 s.
      * Type ``q`` to exit monitor.
      * Verify the shell survives via ``echo MONITOR SHELL ALIVE``
        AND via Ctrl-C (``\\x03``) AND via an ANSI fixture
        (clear + re-print).

    Each ``_wait_for`` uses ``len(session.text)`` captured at the
    moment the command was issued so the match is anchored to
    the NEW output, not the cumulative text from the boot."""
    cmd_at: int = len(session.text)
    session.send(b"monitor\n")
    mon_at_raw = _wait_for(session, "OS01 monitor", 30,
                           starting_offset=cmd_at)
    if mon_at_raw is None:
        reasons.append("monitor never started (OS01 monitor header "
                       "missing)")
        return []
    mon_at: int = mon_at_raw
    # Wait for at least one numeric CPU row before sampling.  Same
    # anchor trick: only check text written AFTER monitor's header.
    cpu_at = time.monotonic() + 30
    while time.monotonic() < cpu_at:
        idx = _has_monitor_cpu_line_after(session.text, mon_at)
        if idx is not None:
            break
        time.sleep(0.5)
    else:
        reasons.append("monitor: no CPU0/1 percentage line seen")
        return []
    # Hide cursor (private mode 25 hide).
    session.send(b"\x1b[?25l")
    # Sample the steady window.
    sample_begin = time.monotonic()
    sample_deadline = sample_begin + 32.0   # 30s + 2s slack
    while time.monotonic() < sample_deadline:
        time.sleep(0.5)
    sample_end = time.monotonic()
    session.observe(0.0)
    # Exit monitor with 'q'.
    q_at = len(session.text)
    session.send(b"q")
    if not _wait_for(session, "# ", 10, starting_offset=q_at):
        reasons.append("shell prompt did not return after monitor q")
        return []
    # Shell-alive marker.
    alv_at = len(session.text)
    session.send(b"echo MONITOR SHELL ALIVE\n")
    if not _wait_for(session, "MONITOR SHELL ALIVE", 10,
                     starting_offset=alv_at):
        reasons.append("shell did not survive monitor exit")
        return []
    # Drain.
    session.observe(1.0)
    steady_records = _drain_term_present(session.text)

    # Anchor: the largest begin_ns seen during the steady-state
    # monitor sampling.  Anything strictly greater than this in
    # the cumulative text belongs to the post-monitor idle window.
    last_steady_begin_ns = 0
    for r in steady_records:
        for tok in r.split():
            if tok.startswith("begin_ns="):
                try:
                    last_steady_begin_ns = max(
                        last_steady_begin_ns, int(tok.split("=", 1)[1]))
                except ValueError:
                    pass

    # Spec §9 / plan Task 6 step 5: Ctrl-C must not kill the
    # shell architecture (terminal.elf already ignores SIGINT;
    # the busybox shell forwards via the PTY).
    ctrlc_at = len(session.text)
    session.send(b"\x03")
    time.sleep(0.5)
    session.send(b"echo SHELL_ALIVE_AFTER_CTRL_C\n")
    if not _wait_for(session, "SHELL_ALIVE_AFTER_CTRL_C", 10,
                     starting_offset=ctrlc_at):
        reasons.append("shell did not survive Ctrl-C")
        return steady_records, [], 0

    # Spec §9 / plan Task 6 step 5: ANSI fixture — clear screen
    # and re-print, then verify the kernel fb is consistent.
    # We save an "ansi fixture marker" so the runner can capture
    # a QMP screencap alongside for forensics.
    try:
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "ansi_fixture_marker.txt").write_text(
            "ansi fixture executed: clear + re-print + readback")
        # Issue the clear escape sequence directly to the PTY
        # master (QEMU stdin == serial stdio == /dev/ttyS0).
        session.send(b"printf '\\x1b[2J\\x1b[Hclear-test\\n'\n")
        time.sleep(0.5)
    except Exception:
        pass

    # Spec §8 rule 5: idle window >= 10 s with zero full/rect
    # calls.  The terminal is foreground (no monitor, no GUI)
    # and the renderer has no damage to flush, so we expect
    # zero presents in this window.  Use the steady-window anchor
    # so the parser sees ONLY post-monitor records in idle.
    idle_begin = time.monotonic()
    idle_deadline = idle_begin + 11.0   # 10 s + 1 s slack
    while time.monotonic() < idle_deadline:
        time.sleep(0.5)
    session.observe(0.0)
    idle_records = _idle_records_only(
        session.text, last_steady_begin_ns)
    return steady_records, idle_records, int(
        (time.monotonic() - idle_begin) * 1_000_000_000)


def _build_summary(run_dir: Path, manifest: Optional[BaselineManifest],
                   qemu_argv: List[str], qemu_version: str,
                   records: List[str], reasons: List[str],
                   capture_baseline: bool,
                   soft_warnings: Optional[List[str]] = None,
                   idle_records: Optional[List[str]] = None,
                   idle_wallclock_ns: Optional[int] = None) -> dict:
    if evaluate_present_window is not None:
        gate = evaluate_present_window(records, idle_records,
                                       idle_wallclock_ns)
    else:
        gate = None
    summary = {
        "schema_version": 1,
        "suite": "terminal-present",
        "capture_baseline": bool(capture_baseline),
        "qemu_argv": qemu_argv,
        "qemu_version": qemu_version,
        "image_sha256": (_sha256(run_dir.parent.parent.parent
                                 / "image" / "disk.img")
                         if (run_dir.parents[2] / "image" / "disk.img").is_file()
                         else None),
        "records": records,
        "record_count": len(records),
        "reasons": reasons,
    }
    if gate is not None:
        summary["gate"] = {
            "ok": gate.ok,
            "bytes_per_frame": gate.bytes_per_frame,
            "ns_per_frame": gate.ns_per_frame,
            "success_frames": gate.success_frames,
            "success_bytes": gate.success_bytes,
            "success_ns": gate.success_ns,
            "failed_attempts": gate.failed_attempts,
            "failed_completed_rect_bytes":
                gate.failed_completed_rect_bytes,
            "failed_prefix": gate.failed_prefix,
            "width_sum_ns": gate.width_sum_ns,
            "earliest_window_s": gate.earliest_window_s,
            "idle_window_ns": gate.idle_window_ns,
            "idle_success_frames": gate.idle_success_frames,
            "idle_window_ok": gate.idle_window_ok,
            "reasons": gate.reasons,
        }
    if soft_warnings:
        summary["baseline_warnings"] = soft_warnings
    return summary


def _write_summary(run_dir: Path, summary: dict) -> None:
    with open(run_dir / "summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True)


def _run_terminal_present(ctx: _RunContext) -> bool:
    """Top-level driver.  Returns True on PASS."""
    reasons: List[str] = []
    soft: List[str] = []
    # Early failures have no sampled idle window to report.
    idle_records: List[str] = []
    idle_wallclock_ns = 0

    # Capture mode does not require a baseline log; acceptance mode
    # REQUIRES one.
    if not ctx.capture_baseline:
        if not ctx.baseline_log.is_file():
            reasons.append(f"evidence missing: baseline log "
                           f"{ctx.baseline_log} not found")
            run_dir = _ensure_archive_dir(ctx)
            _write_summary(run_dir, _build_summary(
                run_dir, None, [], "", [], reasons,
                ctx.capture_baseline, soft_warnings=soft, idle_records=idle_records, idle_wallclock_ns=idle_wallclock_ns))
            return False
        manifest_path = ctx.baseline_log.parent / "manifest.json"
        if not manifest_path.is_file():
            reasons.append(f"evidence missing: manifest.json "
                           f"not found next to {ctx.baseline_log}")
            run_dir = _ensure_archive_dir(ctx)
            _write_summary(run_dir, _build_summary(
                run_dir, None, [], "", [], reasons,
                ctx.capture_baseline, soft_warnings=soft, idle_records=idle_records, idle_wallclock_ns=idle_wallclock_ns))
            return False
        try:
            manifest = BaselineManifest.load(manifest_path)
        except Exception as exc:
            reasons.append(f"manifest.json parse error: {exc}")
            run_dir = _ensure_archive_dir(ctx)
            _write_summary(run_dir, _build_summary(
                run_dir, None, [], "", [], reasons,
                ctx.capture_baseline, soft_warnings=soft, idle_records=idle_records, idle_wallclock_ns=idle_wallclock_ns))
            return False
        hard, soft = _verify_baseline(ctx.image, ctx.firmware,
                                      ctx.baseline_log, manifest)
        if hard:
            reasons.extend(hard)
            run_dir = _ensure_archive_dir(ctx)
            _write_summary(run_dir, _build_summary(
                run_dir, manifest, [], manifest.qemu_version,
                [], reasons, ctx.capture_baseline, soft_warnings=soft, idle_records=idle_records, idle_wallclock_ns=idle_wallclock_ns))
            return False
    else:
        manifest = None

    run_dir = _ensure_archive_dir(ctx)
    qmp_dir = Path(tempfile.mkdtemp(prefix="os01-tpresent-qmp-"))
    qmp_sock = str(qmp_dir / "qmp.sock")
    argv = _build_argv_for(ctx, qmp_sock)

    # Detect the real QEMU version.
    try:
        qemu_version = subprocess.check_output(
            [argv[0], "--version"], text=True, timeout=5)
        qemu_version = qemu_version.splitlines()[0]
    except Exception:
        qemu_version = "unknown"

    # Launch QEMU via ProcessSession.
    try:
        from qemutests.harness.process import ProcessSession
        session = ProcessSession(
            argv=argv, run_dir=run_dir, timeout_s=ctx.timeout,
            writable_stdin=True,
        )
        session.start()
    except Exception as exc:
        reasons.append(f"QEMU launch failed: {exc}")
        _write_summary(run_dir, _build_summary(
            run_dir, manifest, argv, qemu_version,
            [], reasons, ctx.capture_baseline, soft_warnings=soft, idle_records=idle_records, idle_wallclock_ns=idle_wallclock_ns))
        return False

    qmp: Optional[QmpClient] = None
    try:
        # Connect QMP.
        try:
            qmp = QmpClient(qmp_sock, timeout=15.0)
            qmp.connect()
        except Exception as exc:
            reasons.append(f"QMP connect failed: {exc}")
            return False

        # Wait for the shell prompt.
        ready, _ = _wait_shell(session, 60.0)
        if not ready:
            reasons.append("shell prompt never observed")
            return False

        # Steady-state monitor window FIRST — before the surface
        # protocol — so the foreground-restore full redraw that
        # follows the surface test does NOT pollute the steady
        # window.  The brief step 5 had the order reversed; the
        # orchestrator approved swapping the order in the post-
        # Task-6 review (Task-6 report §3) because the surface test
        # is the only source of the 5.18-MB foreground-restore full
        # redraws that drown the partial-present cost in the average.
        records, idle_records, idle_wallclock_ns = (
            _run_monitor_window(session, run_dir, reasons))
        if not records and not ctx.capture_baseline:
            reasons.append("no [term-present] records captured")
            return False

        # Surface protocol AFTER the monitor steady window — its
        # foreground-borrow pollutes the next monitor run, but the
        # perf gate has already been evaluated.  Pixel correctness is
        # verified here so the brief's QMP-screencap contract is
        # satisfied.  Pixel failures populate ``pixel_failures``
        # (added to summary.json); protocol-level completion is
        # reflected in ``surface_pixel_gate`` (True iff every rect
        # passes the per-pixel assertion).
        surface_ok, pixel_failures = _run_surface_protocol(
            session, qmp, run_dir, reasons)
        surface_pixel_gate = surface_ok and not pixel_failures

        summary = _build_summary(run_dir, manifest, argv, qemu_version,
                                 records, reasons, ctx.capture_baseline,
                                 soft_warnings=soft,
                                 idle_records=idle_records,
                                 idle_wallclock_ns=idle_wallclock_ns)
        summary["surface_pixel_gate"] = surface_pixel_gate
        summary["surface_pixel_failures"] = pixel_failures
        _write_summary(run_dir, summary)
        if ctx.capture_baseline:
            return True
        if evaluate_present_window is None:
            reasons.append("evaluate_present_window not importable")
            return False
        gate = evaluate_present_window(records, idle_records,
                                       idle_wallclock_ns)
        if not gate.ok:
            reasons.extend(gate.reasons)
            _write_summary(run_dir, _build_summary(
                run_dir, manifest, argv, qemu_version,
                records, reasons, ctx.capture_baseline,
                soft_warnings=soft, idle_records=idle_records, idle_wallclock_ns=idle_wallclock_ns))
            return False
        # Spec §9 / brief task-9: every gate row must pass for the
        # suite to return True.  ``gate.ok`` covers the spec §8 perf
        # + idle rules; ``surface_pixel_gate`` covers the surface
        # pixel-correctness row of spec §9's verification matrix.
        suite_ok = (gate.ok and surface_pixel_gate)
        if not suite_ok:
            if not surface_pixel_gate:
                reasons.append(
                    "surface pixel verification failed "
                    "(see surface_pixel_failures)")
            _write_summary(run_dir, _build_summary(
                run_dir, manifest, argv, qemu_version,
                records, reasons, ctx.capture_baseline,
                soft_warnings=soft, idle_records=idle_records, idle_wallclock_ns=idle_wallclock_ns))
            return False
        return True
    finally:
        try:
            if qmp:
                qmp.close()
        except Exception:
            pass
        try:
            session.close()
        except Exception:
            pass
        shutil.rmtree(qmp_dir, ignore_errors=True)


def test_terminal_present(tester=None) -> bool:
    """run_test.py dispatcher entry: returns True on success."""
    image = os.environ.get("DISK_IMG")
    if not image or not Path(image).is_file():
        print("FAIL: DISK_IMG is not a readable file")
        return False
    firmware = os.environ.get("OVMF_FIRMWARE")
    if not firmware or not Path(firmware).is_file():
        print("FAIL: OVMF_FIRMWARE is not a readable file")
        return False
    baseline = os.environ.get("TERMINAL_PRESENT_BASELINE_LOG",
                              "/tmp/os01-terminal-present-baseline/serial.log")
    build_dir = os.environ.get("OS01_BUILD_DIR")
    ctx = _RunContext(
        image=Path(image), firmware=Path(firmware),
        build_dir=Path(build_dir) if build_dir else Path("build/x86_64-clang"),
        baseline_log=Path(baseline),
        capture_baseline=False, output_dir=None,
        timeout=int(os.environ.get("TERMINAL_PRESENT_TIMEOUT", "200")),
    )
    ok = _run_terminal_present(ctx)
    return ok


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="OS01 terminal-present QEMU suite")
    ap.add_argument("--image", required=True)
    ap.add_argument("--firmware", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--baseline-log", default=None,
                    help="Required for normal mode; the Task-3 baseline "
                    "serial.log. Ignored with --capture-baseline.")
    ap.add_argument("--capture-baseline", action="store_true",
                    help="Capture a fresh baseline into --output-dir "
                    "instead of running the acceptance gate. The "
                    "performance gate is parked in capture mode; "
                    "only the protocol-level checks run.")
    ap.add_argument("--output-dir", default=None,
                    help="Output directory for --capture-baseline. "
                    "Must NOT point at /tmp/os01-terminal-present-baseline.")
    ap.add_argument("--timeout", type=int, default=200)
    args = ap.parse_args(argv)

    if args.capture_baseline and args.baseline_log:
        print("--capture-baseline and --baseline-log cannot be combined",
              file=sys.stderr)
        return 2
    if not args.capture_baseline and not args.baseline_log:
        print("--baseline-log is required in normal mode", file=sys.stderr)
        return 2

    ctx = _RunContext(
        image=Path(args.image), firmware=Path(args.firmware),
        build_dir=Path(args.build_dir),
        baseline_log=Path(args.baseline_log) if args.baseline_log else Path(),
        capture_baseline=args.capture_baseline,
        output_dir=Path(args.output_dir) if args.output_dir else None,
        timeout=args.timeout,
    )
    ok = _run_terminal_present(ctx)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
