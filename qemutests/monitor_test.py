#!/usr/bin/env python3
"""OS01 terminal-monitor QEMU suite (monitor plan Task 9).

Registered as ``make test-qemu SUITE=monitor`` — the same dispatch shape as
the ``resolution`` suite: the ``run_test.py`` dispatcher entry
``test_monitor(tester)`` (and the standalone ``main()``) run the 1/2/8-core
matrix, each core count booting its own QEMU (serial stdio) and driving the
guest helper ``/bin/test_monitor`` plus the real ``/bin/monitor``.

Design notes
------------
* The 512-byte PTY pipe means any command whose output we do not drain can
  block the guest writer — every wait pumps the ProcessSession.
* There is NO ``sched_setaffinity`` syscall in this kernel, so the dual-core
  "one CPU saturated" case only reports a window in which the spin process's
  observed CPU ownership stayed constant.  A migration during the sampling
  window is retried a bounded number of times, never claimed as pinning.
* Pure parsing/evaluation helpers (``parse_frames``, ``parse_guest_result``,
  ``evaluate_transcript``) are import-friendly so ``test_monitor_runner.py``
  can exercise the gates without launching QEMU.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

# Invoked as a script by mk/components/run.mk (via run_test.py) — put the
# repo root on sys.path so ``import qemutests.harness.*`` resolves.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

# ── tuning ────────────────────────────────────────────────────────────
DEFAULT_TIMEOUT = 240            # per-core-count global session budget (s)
SPIN_SECS = 70                   # background spin lifetime (covers retries)
SAMPLES_2CORE = 8                # --samples for the dual-core window
SAMPLES_8CORE = 6
SAMPLES_1CORE = 4
DUAL_TOLERANCE_10000 = 1500      # |total - 5000| <= 1500 (±15 points)
SPIN_MIN_PERCENT_10000 = 8000    # the spin process must own >= 80 % of a CPU
MIN_VALID_SAMPLES = 5            # drop the first (baseline) round
DUAL_MAX_ATTEMPTS = 3

# ── parsing ───────────────────────────────────────────────────────────


def _pct_to_10000(tok: str):
    """'55.0%' -> 5500; '--' or junk -> None."""
    m = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)%\s*", tok)
    if not m:
        return None
    return int(round(float(m.group(1)) * 100))


class Frame:
    """One parsed monitor dashboard frame."""

    __slots__ = ("total_10000", "cpus", "procs", "temp_unsupported",
                 "ncpu_rows")

    def __init__(self):
        self.total_10000 = None          # None => invalid ("--")
        self.cpus = {}                   # cpu_id -> value_10000 or None
        self.procs = {}                  # name -> value_10000 or None
        self.temp_unsupported = False
        self.ncpu_rows = 0


_CPU_RE = re.compile(r"^CPU(\d+):\s+(\S+)")
_PROC_RE = re.compile(r"^\s*(\d+)\s+(\S+)\s+(\S)\s+(\S+)\s*$")


def parse_frames(text: str) -> list[Frame]:
    """Split the plain-text monitor output into frames.

    Frames start at every ``OS01 monitor`` line.  Anything before the first
    such line is ignored."""
    frames: list[Frame] = []
    cur = None
    for line in text.splitlines():
        if line.strip() == "OS01 monitor":
            cur = Frame()
            frames.append(cur)
            continue
        if cur is None:
            continue
        if line.startswith("CPU total:"):
            cur.total_10000 = _pct_to_10000(line.split(":", 1)[1])
        elif line.startswith("Temp:"):
            if "unsupported" in line:
                cur.temp_unsupported = True
        elif _CPU_RE.match(line):
            m = _CPU_RE.match(line)
            cid = int(m.group(1))
            cur.cpus[cid] = _pct_to_10000(m.group(2))
            cur.ncpu_rows += 1
        else:
            m = _PROC_RE.match(line)
            if m and not line.startswith("  PID"):
                cur.procs[m.group(4)] = _pct_to_10000(m.group(2))
    return frames


_GUEST_RESULT_RE = re.compile(
    r"\[MONITOR TEST\] RESULT: mode=(\w+) pass=(\d+) fail=(\d+)")


def parse_guest_result(text: str, mode: str):
    """Return (pass, fail) for the last RESULT line of `mode`, or None."""
    result = None
    for m in _GUEST_RESULT_RE.finditer(text):
        if m.group(1) == mode:
            result = (int(m.group(2)), int(m.group(3)))
    return result


def count_epoch_lines(text: str) -> int:
    """Number of spin CPU-ownership epoch lines seen so far."""
    return len(re.findall(r"\[MONITOR SPIN\][^\n]*epoch=", text))


def parse_spin_epochs(text: str) -> list[int]:
    return [int(m.group(1)) for m in
            re.finditer(r"\[MONITOR SPIN\][^\n]*epoch=(\d+)", text)]


def valid_total_samples(frames: list[Frame]) -> list[int]:
    """total_10000 values, dropping the leading baseline ('--') round."""
    out = []
    for f in frames:
        if f.total_10000 is not None:
            out.append(f.total_10000)
    return out


# ── gates ─────────────────────────────────────────────────────────────


def _guest_gate(text: str, reasons: list[str]) -> None:
    for mode in ("snapshot", "churn", "pty"):
        res = parse_guest_result(text, mode)
        if res is None:
            reasons.append(f"guest {mode}: no RESULT line (did not run)")
        elif res[1] != 0:
            reasons.append(f"guest {mode}: {res[1]} check(s) failed")


def _thermal_gate(text: str, frames: list[Frame], reasons: list[str]) -> None:
    if not frames:
        reasons.append("thermal: no monitor frame produced")
        return
    if not any(f.temp_unsupported for f in frames):
        reasons.append("thermal: QEMU must report 'Temp: unsupported'")


def _marker_gate(text: str, reasons: list[str]) -> None:
    if "MONITOR SHELL ALIVE" not in text:
        reasons.append("shell did not survive (marker missing)")


def evaluate_single_core(text: str) -> list[str]:
    reasons: list[str] = []
    _guest_gate(text, reasons)
    frames = parse_frames(text)
    _thermal_gate(text, frames, reasons)
    if len(frames) < 2:
        reasons.append(f"1-core: only {len(frames)} monitor frame(s)")
    elif not any(f.cpus.get(0) is not None for f in frames):
        reasons.append("1-core: CPU0 never reported a valid percentage")
    _marker_gate(text, reasons)
    return reasons


def evaluate_eight_core(text: str) -> list[str]:
    reasons: list[str] = []
    _guest_gate(text, reasons)
    frames = parse_frames(text)
    _thermal_gate(text, frames, reasons)
    # every one of the 8 CPUs must show a valid increment (not "--") in the
    # steady-state frames.
    good = [f for f in frames if f.ncpu_rows >= 8
            and all(f.cpus.get(i) is not None for i in range(8))]
    if not good:
        rows = frames[0].ncpu_rows if frames else 0
        reasons.append(
            "8-core: no frame reported valid increments for all 8 CPUs "
            f"(max rows seen: {max((f.ncpu_rows for f in frames), default=0)})")
    _marker_gate(text, reasons)
    return reasons


def evaluate_dual_core(text: str) -> list[str]:
    reasons: list[str] = []
    _guest_gate(text, reasons)
    frames = parse_frames(text)
    _thermal_gate(text, frames, reasons)

    # The real terminal->PTY shell must survive q and Ctrl-C.
    for marker in ("MONITOR HEADLESS Q OK", "MONITOR HEADLESS C OK"):
        if marker not in text:
            reasons.append(f"headless shell: '{marker}' missing "
                           "(shell did not survive the monitor exit)")

    # The spin's own ~100 % process row proves it is the saturated load.
    spin_seen = None
    for f in frames:
        v = f.procs.get("test_monitor")
        if v is not None:
            spin_seen = v if spin_seen is None else max(spin_seen, v)
    if spin_seen is None:
        reasons.append("dual-core: spin process (test_monitor) not in table")
    elif spin_seen < SPIN_MIN_PERCENT_10000:
        reasons.append(
            f"dual-core: spin process only {spin_seen/100:.1f}% "
            f"(< {SPIN_MIN_PERCENT_10000/100:.0f}%)")

    totals = valid_total_samples(frames)
    if len(totals) < MIN_VALID_SAMPLES:
        reasons.append(
            f"dual-core: only {len(totals)} valid sample(s) "
            f"(need >= {MIN_VALID_SAMPLES})")
    else:
        lo = min(totals)
        hi = max(totals)
        worst = max(abs(t - 5000) for t in totals)
        if worst > DUAL_TOLERANCE_10000:
            reasons.append(
                "dual-core: overall CPU out of tolerance "
                f"(worst |total-50%| = {worst/100:.1f} points, "
                f"range {lo/100:.1f}%-{hi/100:.1f}%)")
    _marker_gate(text, reasons)
    return reasons


_EVALUATORS = {1: evaluate_single_core, 2: evaluate_dual_core,
               8: evaluate_eight_core}


def evaluate_transcript(cpus: int, text: str) -> list[str]:
    """Return a list of failure reasons (empty means PASS)."""
    ev = _EVALUATORS.get(cpus)
    if ev is None:
        return [f"unsupported core count {cpus}"]
    if not text or not text.strip():
        return [f"{cpus}-core: empty transcript"]
    return ev(text)


# ── QEMU driver ───────────────────────────────────────────────────────


def _qemu_argv(cpus: int, image: str, firmware: str) -> list[str]:
    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    return [
        qemu, "-M", "q35",
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"file={image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-m", "512", "-smp", str(cpus),
        "-serial", "stdio",
        "-display", "none", "-no-reboot", "-no-shutdown", "-snapshot",
    ]


class MonitorCaseRunner:
    """Drives one QEMU boot for a single core count over serial stdio."""

    def __init__(self, cpus, image, firmware, log, session_factory=None,
                 run_dir=None, timeout=DEFAULT_TIMEOUT):
        self.cpus = cpus
        self.image = image
        self.firmware = firmware
        self.log = log
        self.timeout = timeout
        self.session = None
        if session_factory is None:
            from qemutests.harness.process import ProcessSession
            session_factory = ProcessSession
        self._factory = session_factory
        self._run_dir = run_dir or Path(f"/tmp/os01-monitor-{os.getpid()}")

    # -- low level ----------------------------------------------------
    def _send(self, line: str) -> None:
        self.session.send((line + "\n").encode("utf-8"))

    def _send_raw(self, data: str) -> None:
        """Send raw bytes with no trailing newline (raw-mode keys)."""
        self.session.send(data.encode("utf-8"))

    def _wait(self, pred, what):
        sl = self.session.wait_for(pred)
        if not sl:
            raise TimeoutError(
                f"{self.cpus}-core: timed out waiting for {what}")
        return sl

    def _wait_prompt(self):
        self._wait(lambda s: "# " in s, "shell prompt")

    # -- collection ---------------------------------------------------
    def collect(self) -> str:
        self.session = self._factory(
            argv=_qemu_argv(self.cpus, self.image, self.firmware),
            run_dir=self._run_dir, timeout_s=self.timeout,
            writable_stdin=True,
        )
        self.session.start()
        try:
            self._run_guest_helpers()
            if self.cpus == 2:
                self._run_headless_shell()
                self._run_dual_core()
            else:
                self._run_plain_matrix()
            # Prove the shell survived all of the above.
            self._send("echo MONITOR SHELL ALIVE")
            self._wait(lambda s: "MONITOR SHELL ALIVE" in s, "alive marker")
        finally:
            text = self.session.text
            self.session.close()
        return text

    def _run_headless_shell(self):
        """Drive the REAL terminal->PTY shell (started by inittab) and prove
        it survives quitting the monitor with 'q' and interrupting it with
        Ctrl-C, then echo a unique marker through the same shell."""
        for key, marker in (("q", "MONITOR HEADLESS Q OK"),
                            ("\x03", "MONITOR HEADLESS C OK")):
            self._send("/bin/monitor")
            self._wait(lambda s: "\x1b[?25l" in s, "monitor enter screen")
            self._send_raw(key)
            # The cursor-restore sequence appears as the monitor's single
            # exit path finishes; the shell then regains the prompt.
            self._wait(lambda s: "\x1b[?25h" in s, "monitor cursor restore")
            self._send(f"echo {marker}")
            self._wait(lambda s, m=marker: m in s, f"headless marker {marker}")

    def _run_guest_helpers(self):
        self._wait_prompt()
        for mode in ("snapshot", "churn 9", "pty"):
            self._send(f"/bin/test_monitor {mode}")
            self._wait(
                lambda s, m=mode.split()[0]: (
                    f"[MONITOR TEST] RESULT: mode={m}" in s),
                f"test_monitor {mode} RESULT")

    def _run_plain_matrix(self):
        self._send(f"/bin/monitor --samples {SAMPLES_8CORE if self.cpus == 8 else SAMPLES_1CORE}")
        self._wait(
            lambda s: len(parse_frames(s)) >= self._frame_target(),
            f"{self.cpus}-core monitor frames")

    def _frame_target(self):
        return SAMPLES_1CORE if self.cpus == 1 else SAMPLES_8CORE

    def _spin_stable(self, settle_s=2.4, quiet_rounds=3):
        """Wait until the spin's CPU ownership has been quiet for a while.

        The scheduler places the task on the BSP and then rebalances it once
        or twice in the first ~2 s, so we first absorb that burst, then
        require several consecutive quiet observation rounds (no new epoch
        line) before declaring the window stable."""
        waited = 0.0
        while waited < settle_s:
            self.session.observe(0.6)
            waited += 0.6
        quiet = 0
        for _ in range(12):
            sl = self.session.observe(0.6)
            if "epoch=" in sl:
                quiet = 0
            else:
                quiet += 1
                if quiet >= quiet_rounds:
                    return
        return

    def _run_dual_core(self, attempts=DUAL_MAX_ATTEMPTS):
        self._send(f"/bin/test_monitor spin {SPIN_SECS} &")
        self._wait(lambda s: "[MONITOR SPIN]" in s and "start" in s,
                   "spin start")

        # There is no sched_setaffinity syscall: the spin only reports the
        # CPU the scheduler happened to place it on.  The 2-CPU load
        # balancer moves it every few seconds, so a >=5 s window with NO
        # ownership change is often unavailable.  We retry a bounded number
        # of times; if none is stable we still evaluate the (migration
        # invariant) overall-CPU tolerance but explicitly do NOT claim
        # strict pinning.
        stable = False
        for attempt in range(attempts):
            self._spin_stable()
            self._send(f"/bin/monitor --samples {SAMPLES_2CORE}")
            sl = self._wait(
                lambda s: len(parse_frames(s)) >= SAMPLES_2CORE,
                "dual-core monitor frames")
            if count_epoch_lines(sl) == 0:
                stable = True
                self.log(f"  [monitor] dual-core: observed a stable-ownership "
                         f"window (attempt {attempt + 1})")
                break
            self.log(f"  [monitor] dual-core: spin migrated during window; "
                     f"retrying (attempt {attempt + 1})")
        if not stable:
            self.log("  [monitor] dual-core: NO stable-ownership window "
                     "observed (spin migrated every attempt); reporting the "
                     "observed overall CPU only — strict CPU pinning is NOT "
                     "covered (needs a future kernel-side restricted spin).")


# ── suite entry points ────────────────────────────────────────────────


CORE_MATRIX = (1, 2, 8)


def run_matrix(image, firmware, log=print, build_dir=None,
               session_factory=None, timeout=DEFAULT_TIMEOUT,
               cpus=None):
    """Run the matrix; returns (ok, per-core reasons dict)."""
    cpus = list(cpus if cpus is not None else CORE_MATRIX)
    reasons: dict[int, list[str]] = {}
    archive = None
    if build_dir:
        try:
            from qemutests.harness.result import RunArchive
            archive = RunArchive.create(build_dir=Path(build_dir),
                                        suite="monitor")
        except Exception as exc:  # pragma: no cover - archive is best effort
            log(f"  [monitor] warning: no archive: {exc}")

    ok = True
    for n in cpus:
        log(f"  [monitor] === {n}-core case ===")
        run_dir = (archive.run_dir / f"cpu{n}") if archive else None
        runner = MonitorCaseRunner(n, image, firmware, log,
                                   session_factory=session_factory,
                                   run_dir=run_dir, timeout=timeout)
        try:
            text = runner.collect()
        except Exception as exc:
            reasons[n] = [f"{n}-core: run error: {exc}"]
            ok = False
            log(f"  [monitor] {n}-core FAIL: {exc}")
            continue
        rs = evaluate_transcript(n, text)
        reasons[n] = rs
        if rs:
            ok = False
            for r in rs:
                log(f"  [monitor] {n}-core FAIL: {r}")
        else:
            log(f"  [monitor] {n}-core PASS")

    # The matrix must actually have run every registered core count.
    for n in CORE_MATRIX:
        if n not in reasons:
            reasons[n] = [f"{n}-core: matrix entry missing"]
            ok = False

    if archive is not None:
        _write_report(archive, image, firmware, int(cpus[-1]), ok, reasons)
    return ok, reasons


def _write_report(archive, image, firmware, cpu_count, ok, reasons):
    try:
        from qemutests.harness.result import RunReport
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision="unknown",
            git_dirty=False,
            profile=os.environ.get("OS01_PROFILE", "default"),
            suite="monitor",
            request=None,
            declared_ids=None,
            observed_ids=None,
            argv=[],
            cpu_count=cpu_count,
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
    except Exception as exc:  # pragma: no cover
        print(f"  [monitor] warning: failed to write RunReport: {exc}")


def _env_path(name: str):
    v = os.environ.get(name)
    return Path(v) if v else None


def test_monitor(tester=None) -> bool:
    """run_test.py dispatcher entry: returns True on success."""
    log = print
    image = _env_path("DISK_IMG") or (Path(tester.disk_img) if tester else None)
    firmware = _env_path("OVMF_FIRMWARE")
    if not image or not Path(image).is_file():
        log("FAIL: DISK_IMG is not a readable file")
        return False
    if not firmware or not firmware.is_file():
        log("FAIL: OVMF_FIRMWARE is not a readable file")
        return False
    build_dir = os.environ.get("OS01_BUILD_DIR")
    log(f"monitor suite: image={image} firmware={firmware} "
        f"cpus={CORE_MATRIX}")
    ok, _ = run_matrix(image, firmware, log=log, build_dir=build_dir)
    return ok


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="OS01 monitor QEMU matrix")
    ap.add_argument("--image", required=True)
    ap.add_argument("--firmware", required=True)
    ap.add_argument("--build-dir", default=None)
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    ap.add_argument("--cpus", nargs="+", type=int, default=list(CORE_MATRIX))
    args = ap.parse_args(argv)
    ok, reasons = run_matrix(Path(args.image), Path(args.firmware),
                             build_dir=args.build_dir, timeout=args.timeout,
                             cpus=args.cpus)
    if not ok:
        for n in sorted(reasons):
            for r in reasons[n]:
                print(f"FAIL: {r}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
