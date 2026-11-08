#!/usr/bin/env python3
"""BOOT-2 x86 early-IRQ/time fault-matrix acceptance runner.

Task 4 of the BOOT-2 plan (spec §7.2) needs *isolated* evidence for the
BSP early-IRQ/time boot-path faults:

  * ``force-rtc``      — bypasses the CPUID nominal TSC answer; the
    real RTC window must produce 256 handler samples and paired returns.
  * ``rtc-timeout``    — the RTC route stays masked; the calibration
    must end bounded and degrade to the PIT/jiffies tick.
  * ``bsp-lapic-fail`` — only the BSP handoff fails; the PIT fallback
    and later ticks must work.
  * ``unsafe-open``    — the controlled window is opened before the
    context exists; the state machine must fail closed with exactly one
    ``[boot-2] FATAL stage=context``.

``normal`` boots an unmodified kernel at 1/2/4/8 CPUs and requires the
production stage markers (``[boot-2] context-ready`` /
``[boot-2] runtime-open source=pit``), the BOOT-1 complete line, PID 1
and a shell prompt.

The normal image runs read-only (``-snapshot``); every fault case runs
its own isolated ``boot-lifecycle-<case>`` kernel/image variant.  The
runner owns the QEMU stop — a QEMU that exited on its own is weak
evidence — and judges from the guest serial stream only: no QMP
cross-channel ordering inference is ever made.

The decision logic is pure (:func:`check_evidence`) so the unit fixtures
can drive it with synthetic logs; the QEMU plumbing is thin and
injectable (``session_factory``).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:  # noqa: E402
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]


# ── Suite identity ──────────────────────────────────────────────
SUITE = "boot-irq-time"

CASE_NORMAL = "normal"
CASE_FORCE_RTC = "force-rtc"
CASE_RTC_TIMEOUT = "rtc-timeout"
CASE_BSP_LAPIC_FAIL = "bsp-lapic-fail"
CASE_UNSAFE_OPEN = "unsafe-open"
CASES = (CASE_NORMAL, CASE_FORCE_RTC, CASE_RTC_TIMEOUT,
         CASE_BSP_LAPIC_FAIL, CASE_UNSAFE_OPEN)
FATAL_CASES = (CASE_UNSAFE_OPEN,)

# The isolated kernel/image variant slug.  MUST match the slug derived
# by mk/project.mk (IMAGE_VARIANT) and mk/profiles/x86_64-clang.mk
# (KERNEL_VARIANT) from the X86_SMP_BOOT_TEST_* macros.
VARIANT_SLUG_FMT = "boot-irq-time-%s"

# Post-evidence observation window.  Fatal cases must show at least one
# second of quiet (spec §8.2: "fatal 阶段观察 1s"); normal cases get a
# short window too, bounded by the runner's own deadline.
OBSERVE_SECONDS = 1.0

# ── Guest markers (kernel production + boot-2-test fixture) ──────
# Production stage markers (BOOT-2 §6).
_CTX_RE = re.compile(r"^\[boot-2\] context-ready$", re.MULTILINE)
_OPEN_RE = re.compile(r"^\[boot-2\] runtime-open source=pit$", re.MULTILINE)
# BOOT-1 completion + userspace continuation.
_BOOT1_COMPLETE_RE = re.compile(
    r"^\[boot-1\] complete discovered=(\d+) ready=(\d+)$", re.MULTILINE)
_PID1_RE = re.compile(r"^init: running as PID 1$", re.MULTILINE)
_SHELL_RE = re.compile(r"(?m)^# ?$")

# Fixture evidence (boot-irq-time variants only).
_VARIANT_RE = re.compile(
    r"^\[boot-2-test\] variant=" + VARIANT_SLUG_FMT % r"(\S+)" + r"$",
    re.MULTILINE)
_RTC_RE = re.compile(
    r"^\[boot-2-test\] rtc result=(ok|timeout) pf=(\d+) returns=(\d+) "
    r"if=0 jiffies_delta=(\d+) unexpected=(\d+)$", re.MULTILINE)
_BOOTSTRAP_RE = re.compile(
    r"^\[boot-2-test\] bootstrap source=pit count=(\d+) returns=(\d+) "
    r"jiffies_delta=(\d+)$", re.MULTILINE)
_TICK_RE = re.compile(
    r"^\[boot-2-test\] tick source=(pit|lapic) count=(\d+) returns=(\d+) "
    r"jiffies_delta=(\d+)$", re.MULTILINE)
_DEVICES_BEGIN_RE = re.compile(
    r"^\[boot-2-test\] devices-begin$", re.MULTILINE)
_FATAL2_RE = re.compile(
    r"^\[boot-2\] FATAL stage=(context|runtime|tick)$", re.MULTILINE)

# Any [boot-1] fault-marker shape is wrong-image evidence here too.
_ANY_BOOT1_FATAL_RE = re.compile(r"^\[boot-1\] FATAL ", re.MULTILINE)

# A panic line that is NOT the expected BOOT-1 fatal consequence.
# kpanic renders "kernel panic: <msg>"; the BOOT-1 fatal path is the
# only panic a fault case may produce.
# A panic line that is NOT the expected BOOT-1 fatal consequence is
# judged on the WHOLE line (kpanic renders "kernel panic - not syncing:
# BOOT-1: AP boot failed ..."), which is the only allowed panic.
_PANIC_LINE_RE = re.compile(r"^.*kernel panic.*$", re.MULTILINE | re.IGNORECASE)
_BOOT1_PANIC_RE = re.compile(
    r"kernel panic.*(?:BOOT-1: AP boot failed|"
    r"BOOT-2: boot IRQ/time protocol violation)")
_RTC_TIMEOUT_ROOTFS_PANIC = ("[kernel panic] root filesystem unavailable")
_RTC_TIMEOUT_RE = re.compile(
    r"(?:^" + re.escape(_RTC_TIMEOUT_ROOTFS_PANIC) + r"$|"
    r"^\[boot-3\] FATAL stage=fs component=root rc=-19 phase=-1 cpu=-1$)",
    re.MULTILINE)


def _unexpected_panics(text: str) -> list[str]:
    """Panic lines that are NOT the expected BOOT-1 fatal consequence."""
    return [m.group(0) for m in _PANIC_LINE_RE.finditer(text)
            if not _BOOT1_PANIC_RE.search(m.group(0))]


def check_evidence(text: str, case: str, cpus: int) -> tuple[bool, list[str]]:
    """Pure parser: judge one case's serial stream (spec §7.2)."""
    if case not in CASES:
        raise ValueError(f"unknown case {case!r}")
    if cpus <= 0:
        return False, ["cpus must be positive"]

    errors: list[str] = []
    text = text.replace("\r", "")

    unexpected = _unexpected_panics(text)
    if unexpected and case == CASE_RTC_TIMEOUT:
        unexpected = [p for p in unexpected
                      if not _RTC_TIMEOUT_RE.match(p)]
    if unexpected:
        errors.append(f"unrelated panic: {unexpected[0]!r}")

    if case == CASE_NORMAL:
        if _ANY_BOOT1_FATAL_RE.search(text) or _FATAL2_RE.search(text):
            errors.append("fatal marker in a normal run")
        if _VARIANT_RE.search(text) or _DEVICES_BEGIN_RE.search(text):
            errors.append("fault-marker evidence in a normal run")
        if not _CTX_RE.search(text):
            errors.append("missing [boot-2] context-ready")
        if not _OPEN_RE.search(text):
            errors.append("missing [boot-2] runtime-open source=pit")
        completes = list(_BOOT1_COMPLETE_RE.finditer(text))
        if len(completes) != 1:
            errors.append(f"[boot-1] complete lines: {len(completes)} (want 1)")
        else:
            discovered, ready = (int(completes[0].group(1)),
                                 int(completes[0].group(2)))
            if discovered != cpus or ready != cpus:
                errors.append(
                    f"complete counts discovered={discovered} ready={ready}"
                    f" != {cpus}/{cpus}")
        if not _PID1_RE.search(text):
            errors.append("PID 1 startup marker missing")
        if not _SHELL_RE.search(text):
            errors.append("shell prompt missing")
        return (not errors), errors

    # ── Fixture cases ────────────────────────────────────────────
    variants = list(_VARIANT_RE.finditer(text))
    if len(variants) != 1:
        errors.append(f"variant identity lines: {len(variants)} (want 1)")
    elif variants[0].group(1) != case:
        errors.append(f"wrong variant identity: {variants[0].group(1)!r}")

    devices = list(_DEVICES_BEGIN_RE.finditer(text))
    bootstraps = list(_BOOTSTRAP_RE.finditer(text))
    ticks = list(_TICK_RE.finditer(text))
    rtcs = list(_RTC_RE.finditer(text))
    fatals = list(_FATAL2_RE.finditer(text))
    completes = list(_BOOT1_COMPLETE_RE.finditer(text))

    if case == CASE_UNSAFE_OPEN:
        # unique context FATAL, sti-前终止: no runtime-open, no BOOT-1
        # complete, no PID1, no devices-begin, no fixture records.
        if len(fatals) != 1:
            errors.append(f"FATAL lines: {len(fatals)} (want 1)")
        elif fatals[0].group(1) != "context":
            errors.append(f"FATAL stage={fatals[0].group(1)!r} != context")
        if _OPEN_RE.search(text):
            errors.append("runtime-open after the unsafe open")
        if _CTX_RE.search(text):
            errors.append("context-ready after the unsafe open")
        if completes or _PID1_RE.search(text) or _SHELL_RE.search(text):
            errors.append("continuation evidence after the unsafe open")
        if devices or bootstraps or ticks or rtcs:
            errors.append("fixture records after the unsafe open")
        if len(fatals) == 1:
            tail = text[fatals[0].end():]
            if _PID1_RE.search(tail) or _CTX_RE.search(tail):
                errors.append("continuation after the FATAL line")
        return (not errors), errors

    # Non-fatal fixture cases boot fully; the per-case evidence rules:
    if _FATAL2_RE.search(text):
        errors.append("FATAL marker in a non-fatal case")

    # ordering: context-ready < runtime-open < bootstrap < devices-begin
    # < tick; and the fixture evidence completeness per case.
    def pos(match_list):
        return match_list[0].start() if match_list else -1

    expected_order = [(_CTX_RE.search(text), "context"),
                      (_OPEN_RE.search(text), "open"),
                      (bootstraps and bootstraps[0], "bootstrap"),
                      (devices and devices[0], "devices")]
    if case != CASE_RTC_TIMEOUT:
        expected_order.append((ticks and ticks[0], "tick"))
    positions = [m.start() for m, _ in expected_order if m]
    if len(positions) != len(expected_order) or positions != sorted(positions):
        errors.append("stage/fixture ordering violated")

    if case in (CASE_FORCE_RTC, CASE_RTC_TIMEOUT):
        if len(rtcs) != 1:
            errors.append(f"rtc records: {len(rtcs)} (want 1)")
        else:
            result, pf, rets, delta, unexpected = rtcs[0].groups()
            if case == CASE_FORCE_RTC:
                if result != "ok" or int(pf) != 256 or int(rets) < 256:
                    errors.append(
                        f"force-rtc rtc record: result={result} pf={pf} "
                        f"returns={rets} (want ok/256/>=256)")
            else:
                if result != "timeout" or int(pf) != 0:
                    errors.append(
                        f"rtc-timeout record: result={result} pf={pf} "
                        "(want timeout/0)")
            if int(delta) != 0:
                errors.append(f"rtc window jiffies_delta={delta} != 0")
            if int(unexpected) != 0:
                errors.append(f"unexpected overlapping tokens: {unexpected}")
    elif rtcs:
        # force-rtc/rtc-timeout REQUIRE the record above; other cases
        # may legitimately run the RTC transaction (CPUID can fail on
        # any variant) — the record must simply be well-formed.
        if len(rtcs) > 1:
            errors.append(f"rtc records: {len(rtcs)} (want at most 1)")
        else:
            if int(rtcs[0].group(4)) != 0:
                errors.append(f"rtc window jiffies_delta="
                              f"{rtcs[0].group(4)} != 0")
            if int(rtcs[0].group(5)) != 0:
                errors.append(f"unexpected overlapping tokens: "
                              f"{rtcs[0].group(5)}")

    # Shared requirements: the stage markers and the bootstrap record.
    if len(bootstraps) != 1:
        errors.append(f"bootstrap records: {len(bootstraps)} (want 1)")
    else:
        c, r, d = (int(bootstraps[0].group(1)),
                   int(bootstraps[0].group(2)),
                   int(bootstraps[0].group(3)))
        if c < 3 or r < 3 or d < 1:
            errors.append(
                f"bootstrap record count/returns/jiffies={c}/{r}/{d} "
                "(all must be >= 3/3/1)")
    if len(devices) != 1:
        errors.append(f"devices-begin lines: {len(devices)} (want 1)")

    if case != CASE_RTC_TIMEOUT:
        if len(ticks) != 1:
            errors.append(f"tick records: {len(ticks)} (want 1)")
        else:
            src, c, r, d = ticks[0].groups()
            if case == CASE_BSP_LAPIC_FAIL and src != "pit":
                errors.append("bsp-lapic-fail must tick via the PIT")
            if int(c) < 3 or int(r) < 3 or int(d) < 1:
                errors.append(
                    f"tick record source={src} count/returns/jiffies="
                    f"{c}/{r}/{d} (all must be >= 3/3/1)")
        if len(completes) != 1:
            errors.append(f"[boot-1] complete lines: {len(completes)} (want 1)")
        if not _PID1_RE.search(text):
            errors.append("PID 1 startup marker missing")
        if not _SHELL_RE.search(text):
            errors.append("shell prompt missing")
    else:
        # rtc-timeout: bounded timeout with the clocksource inactive.
        # The degraded chain ends here BY SPEC (§7.2): ahci's deadline
        # waits need a clocksource, so without one there is no block
        # device and no rootfs — the [kernel panic] rootfs line is the
        # EXPECTED terminal state (before the BSP handoff, hence no tick
        # record and no userspace).  PID1 must NOT appear.
        if ticks:
            errors.append("rtc-timeout must have no tick record (the "
                          "rootfs terminal precedes the handoff)")
        if len(completes) != 0 or _PID1_RE.search(text) \
                or _SHELL_RE.search(text):
            errors.append("rtc-timeout must NOT reach userspace "
                          "(clocksource inactive ⇒ no storage)")
        if not _RTC_TIMEOUT_RE.search(text):
            errors.append("rtc-timeout missing the expected rootfs-"
                          "unavailable terminal line")

    return (not errors), errors


def qemu_command(args: argparse.Namespace) -> list[str]:
    """The QEMU command for one case.  Normal runs the normal image
    read-only (-snapshot); fault cases run their isolated image."""
    cmd = [
        args.qemu, "-M", "q35",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware}",
        "-drive", f"file={args.image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        # Entropy: every other suite passes a virtio-rng device; without
        # it the CSPRNG pool never seeds and the guest stalls before PID 1.
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-m", "512", "-smp", str(args.cpus),
        "-serial", "stdio", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]
    if args.case == CASE_NORMAL:
        cmd.append("-snapshot")
    return cmd


def file_sha256(path: str) -> str | None:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


_ELF_AUDIT_STRINGS = {
    "normal": None,
    "force-rtc": b"[boot-2-test] variant=boot-irq-time-force-rtc",
    "rtc-timeout": b"[boot-2-test] variant=boot-irq-time-rtc-timeout",
    "bsp-lapic-fail": b"[boot-2-test] variant=boot-irq-time-bsp-lapic-fail",
    "unsafe-open": b"[boot-2-test] variant=boot-irq-time-unsafe-open",
}


def _elf_identity_ok(case: str, elf_path: str) -> tuple[bool, str]:
    """Audit the variant ELF identity: a fault ELF must carry its own
    identity string; the normal ELF must carry none of the fault
    markers or macro selectors."""
    try:
        blob = Path(elf_path).read_bytes()
    except OSError as exc:
        return False, f"cannot read kernel ELF {elf_path}: {exc}"
    wanted = _ELF_AUDIT_STRINGS[case]
    if wanted is not None:
        if wanted not in blob:
            return False, (f"kernel ELF {elf_path} lacks the fault identity "
                           f"{wanted.decode()!r}")
        return True, ""
    for marker in tuple(_ELF_AUDIT_STRINGS[c] for c in CASES[1:]) + \
            (b"[boot-1] variant=boot-lifecycle-delay-online",
             b"X86_SMP_BOOT_TEST_DELAY_ONLINE",
             b"X86_SMP_BOOT_TEST_NO_ONLINE",
             b"X86_SMP_BOOT_TEST_NO_SCHEDULER"):
        if marker in blob:
            return False, (f"normal kernel ELF {elf_path} carries fault "
                           f"marker {marker.decode()!r}")
    return True, ""


def _git_revision() -> tuple[str, bool]:
    rev, dirty = "unknown", False
    try:
        r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=_ROOT,
                           capture_output=True, text=True, timeout=5)
        if r.returncode == 0 and r.stdout.strip():
            rev = r.stdout.strip()
        d = subprocess.run(["git", "status", "--porcelain"], cwd=_ROOT,
                           capture_output=True, text=True, timeout=5)
        if d.returncode == 0:
            dirty = bool(d.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return rev, dirty


def _write_suite_report(archive, command, args, session, started_monotonic,
                        status, runner_exit_code, errors) -> bool:
    """Persist a suite-unit RunReport; no guest v1 protocol cases."""
    if archive is None or RunReport is None:
        return True
    try:
        rev, dirty = _git_revision()
        report = RunReport(
            schema_version=1, run_id=archive.run_dir.name,
            git_revision=rev, git_dirty=dirty, profile=args.profile,
            suite=SUITE, request=None, declared_ids=None, observed_ids=None,
            argv=list(command), cpu_count=args.cpus, memory_mib=512,
            tool_versions={},
            firmware_path=str(Path(args.firmware).resolve()),
            firmware_sha256_before=None,
            firmware_sha256_after=file_sha256(args.firmware),
            image_path=str(Path(args.image).resolve()),
            image_sha256_before=None,
            image_sha256_after=file_sha256(args.image),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started_monotonic),
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None
                             else None),
            stopped_by_runner=bool(session is not None
                                   and session.stopped_by_runner),
            status=status, count_unit="suite", outcomes=[{"errors": errors}],
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
        return True
    except Exception as exc:  # noqa: BLE001
        print(f"[{SUITE}] ERROR: failed to write RunReport: {exc}",
              file=sys.stderr)
        return False


def run_case(args, *, session_factory=None) -> int:
    """Run one case and judge it.

    Returns 0 (PASS), 1 (FAIL) or 2 (configuration / environment ERROR).
    The runner owns the QEMU stop; the archived status always matches
    the returned code.
    """
    started = time.monotonic()
    if args.case not in CASES:
        print(f"[{SUITE}] ERROR: unknown case {args.case!r}")
        return 2
    if args.cpus <= 0:
        print(f"[{SUITE}] ERROR: invalid cpus {args.cpus}")
        return 2

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print(f"[{SUITE}] ERROR: ProcessSession is unavailable")
        return 2

    # ELF identity audit: the variant ELF must prove the build isolation
    # before any QEMU time is spent.
    elf_ok, elf_err = _elf_identity_ok(args.case, args.kernel_elf)
    if not elf_ok:
        print(f"[{SUITE}] ERROR: {elf_err}")
        return 2

    archive = None
    if getattr(args, "build_dir", None) and RunArchive is not None:
        archive = RunArchive.create(Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"

    command = qemu_command(args)
    metadata = {
        "case": args.case,
        "cpus": args.cpus,
        "command": command,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
        "kernel_elf": str(Path(args.kernel_elf).resolve()),
        "elf_identity_audited": True,
        "observe_seconds": OBSERVE_SECONDS,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    session = None
    errors: list[str] = []
    spawn_error = None
    try:
        session = session_factory(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        deadline = session.deadline
        # wait_for consumes everything when its predicate matches and
        # only sees text after the cursor; predicates evaluate against
        # the WHOLE serial stream via `seen`, never the host's read
        # boundaries.
        seen = ""

        def matches(matcher):
            prefix = seen
            def check(suffix):
                return matcher.search(prefix + suffix) is not None
            return check

        if args.case in FATAL_CASES:
            # The FATAL line is the ordering boundary.
            session.wait_for(matches(_FATAL2_RE))
            if session.timed_out:
                errors.append("timeout before the FATAL marker")
            elif deadline - time.monotonic() < OBSERVE_SECONDS:
                errors.append("timeout: insufficient fatal observation budget")
            else:
                session.observe(OBSERVE_SECONDS)
        else:
            # normal / delay-online: the complete marker, then PID 1 and
            # the shell prompt prove userspace continued.
            if args.case == CASE_RTC_TIMEOUT:
                # Terminal state: the rootfs-unavailable panic (no
                # userspace by spec — see check_evidence).
                session.wait_for(matches(_RTC_TIMEOUT_RE))
                if session.timed_out:
                    errors.append("timeout before the rtc-timeout terminal")
                elif deadline - time.monotonic() < OBSERVE_SECONDS:
                    errors.append("timeout: insufficient observation budget")
                else:
                    session.observe(OBSERVE_SECONDS)
            else:
                wait_re = (_BOOTSTRAP_RE if args.case != CASE_NORMAL
                           else _BOOT1_COMPLETE_RE)
                session.wait_for(matches(wait_re))
                if session.timed_out:
                    errors.append("timeout before the primary evidence "
                                  "marker")
                else:
                    seen = session.text
                    session.wait_for(matches(_SHELL_RE))
                    if session.timed_out:
                        errors.append("timeout before the shell prompt")
                    elif deadline - time.monotonic() < OBSERVE_SECONDS:
                        errors.append("timeout: insufficient observation "
                                      "budget")
                    else:
                        session.observe(OBSERVE_SECONDS)
        if time.monotonic() >= deadline:
            errors.append("overall deadline expired")
    except Exception as exc:  # noqa: BLE001 — launch/OS error is ERROR
        spawn_error = exc
    finally:
        # The runner owns the stop: a QEMU that exited on its own is
        # weak evidence.
        if session is not None:
            try:
                session.stop()
            except Exception:  # noqa: BLE001
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001
                pass

    text = session.text if session is not None else ""
    timed_out = bool(session is not None and session.timed_out)
    stopped_by_runner = bool(session is not None and session.stopped_by_runner)

    if spawn_error is not None:
        print(json.dumps({"event": "spawn-error", "error": str(spawn_error)}))
        _write_suite_report(archive, command, args, session, started,
                            "ERROR", 2, [f"spawn error: {spawn_error}"])
        return 2

    accepted, parser_errors = check_evidence(text, args.case, args.cpus)
    errors.extend(parser_errors)
    # Weak evidence: the guest stopped producing output and QEMU exited
    # on its own — the runner did not observe the quiet window it owns.
    qemu_self_exited = (accepted and session is not None
                        and not stopped_by_runner and not timed_out
                        and session.returncode is not None)
    result = accepted and not errors and not timed_out and not qemu_self_exited

    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "harness_terminated": stopped_by_runner,
        "evidence_accepted": accepted,
        "qemu_self_exited": qemu_self_exited,
        "errors": errors,
        "result": "PASS" if result else "FAIL",
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    report_written = _write_suite_report(
        archive, command, args, session, started,
        "PASS" if result else "FAIL", 0 if result else 1, errors)

    if not report_written:
        metadata["result"] = "ERROR"
        metadata["archive_error"] = "RunReport write failed"
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        return 2

    print(json.dumps({
        "event": "case", "case": args.case, "cpus": args.cpus,
        "result": "PASS" if result else "FAIL",
        "timeout": timed_out,
        "evidence_accepted": accepted,
        "qemu_self_exited": qemu_self_exited,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
    }))
    return 0 if result else 1


def _install_sigterm_handler() -> None:
    try:
        signal.signal(signal.SIGTERM, _sigterm_handler)
    except (ValueError, OSError, AttributeError):  # pragma: no cover
        pass


def _sigterm_handler(signum, frame):  # noqa: ANN001, ARG001
    raise SystemExit(143)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="OS01 BOOT-2 x86 early-IRQ/time fault-matrix acceptance")
    parser.add_argument("--case", choices=list(CASES), required=True)
    parser.add_argument("--cpus", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--qemu")
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--kernel-elf")
    parser.add_argument("--log-dir")
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive, supplied by Make")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                            "x86_64-clang"))
    args = parser.parse_args()
    _install_sigterm_handler()
    if not all((args.qemu, args.firmware, args.image, args.kernel_elf,
                args.log_dir)):
        parser.error("--qemu, --firmware, --image, --kernel-elf and "
                     "--log-dir are required")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.cpus <= 0:
        parser.error("--cpus must be positive")
    return run_case(args, session_factory=ProcessSession)


if __name__ == "__main__":
    sys.exit(main())
