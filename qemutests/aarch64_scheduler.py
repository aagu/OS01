#!/usr/bin/env python3
"""AArch64 scheduler acceptance: idle smoke (Phase 3) + MODE=sched (Phase 5).

Two live modes share this file:

* ``--idle-smoke`` (Phase 3, unchanged contract): boot an already-built
  NORMAL image at each requested CPU count, retain the serial log, and
  require published CPU0 idle readiness followed by real ticks.  It never
  rebuilds and never consumes a selftest variant.
* ``--case positive|sp-top|sp-low|pc-low|pc-data|all`` (Phase 5, spec
  §10.2): drive the dedicated ``sched`` / ``sched-fault-<value>`` images
  built by ``MODE=sched`` prep, parse the protocol-v1 trace STRICTLY
  (complete 14-ID manifest, order, statuses, counts, every frozen
  ``[sched-evidence]`` field and predicate, per-AP observations), and —
  for the fault variants — require the pre-switch rejection to fire with
  the exact frozen marker and matching production fatal reason, with no
  victim entry and no scheduler completion afterwards.

Every boot has an ABSOLUTE 120 s deadline (the one-second post-completion
drain happens inside that budget); continuous ticks never extend it, and
a timeout or a crash is always a failure.  The runner only consumes the
explicit per-variant image/firmware/ELF paths Make passes — it never
rebuilds a variant and never guesses a path.
"""

import argparse
import hashlib
import json
import os
import re
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb  # noqa: E402
from qemutests.aarch64_uefi_smp import SCHEDULER_LATE_IDS  # noqa: E402

try:  # noqa: E402 — path bootstrap must precede this import
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport, parse_v1
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]
    parse_v1 = None  # type: ignore[assignment]


# ── Phase 3 idle smoke (contract frozen; unchanged) ────────────────

READY = "[sched] idle-ready cpu=0 scheduler_ok=1 irq=on"
FATAL = re.compile(r"(\[.*FATAL|\bPANIC\b|\bOops\b|Exception level .* fatal|context fatal)", re.I)
TICK = re.compile(r"^\[tick\]\s+(\d+)\s*$", re.M)


def qemu_command(qemu, cpus, image, firmware, dtb):
    cmd = [qemu, "-M", "virt,gic-version=2,acpi=off", "-cpu", "cortex-a53",
           "-smp", str(cpus), "-m", "512",
           "-drive", f"if=pflash,format=raw,file={firmware}",
           "-drive", f"if=none,file={image},format=raw,readonly=on,id=disk",
           "-device", "virtio-blk-device,drive=disk", "-serial", "stdio",
           "-display", "none", "-no-reboot", "-no-shutdown", "-dtb", dtb]
    return cmd


def run_one(args, cpus, log_dir):
    case_dir = log_dir / f"cpus-{cpus}"
    case_dir.mkdir(parents=True, exist_ok=True)
    log_path = case_dir / "serial.log"
    dtb = generate_diagnostic_dtb(args.qemu, str(case_dir), cpus)
    command = qemu_command(args.qemu, cpus, args.image, args.firmware, dtb)
    started = time.monotonic()
    with log_path.open("wb") as stream:
        proc = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        try:
            while time.monotonic() - started < args.timeout:
                if proc.poll() is not None:
                    break
                stream.flush()
                text = log_path.read_text(errors="replace")
                if FATAL.search(text):
                    raise RuntimeError(f"fatal/crash marker in {log_path}")
                marker_pos = text.find(READY)
                if marker_pos >= 0:
                    after = text[marker_pos + len(READY):]
                    ticks = [int(v) for v in TICK.findall(after)]
                    if len(ticks) >= 3 and all(a < b for a, b in zip(ticks, ticks[1:])):
                        return 0, log_path, command
                time.sleep(0.2)
            else:
                raise TimeoutError(f"idle-ready + 3 increasing ticks not seen in {args.timeout}s")
            stream.flush()
            text = log_path.read_text(errors="replace")
            if FATAL.search(text):
                raise RuntimeError(f"fatal/crash marker in {log_path}")
            raise RuntimeError(f"QEMU exited before scheduler smoke completed (rc={proc.returncode})")
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


# ── Phase 5: the frozen strict manifest ────────────────────────────

# Early IDs in registration order (kernel/selftest/selftest.c, aarch64).
EARLY_IDS = ("slab_alloc_free", "slab_many_sizes", "slab_16_caches",
             "aarch64_pt_vmm", "spinlock_basic")
# The nine late IDs come from the shared frozen tuple in the P4 gate
# module — one source of truth for the series.
LATE_IDS = tuple(SCHEDULER_LATE_IDS)
EXPECTED_IDS = tuple(EARLY_IDS) + LATE_IDS
EXPECTED_TOTAL = len(EXPECTED_IDS)          # 14

SCHED_FAULT_CASES = ("sp-top", "sp-low", "pc-low", "pc-data")
SCHED_CASES = SCHED_FAULT_CASES + ("positive", "all")
FAULT_SYMBOL = "aarch64_sched_fault_inject"

# The production fatal reason each corruption must produce (the
# pre-switch validator in kernel/arch/aarch64/cpu/task_arch.c).
FAULT_REASONS = {
    "sp-top": "saved sp outside usable stack",
    "sp-low": "saved sp outside usable stack",
    "pc-low": "continuation outside executable kernel text",
    "pc-data": "continuation outside executable kernel text",
}

# Frozen evidence field predicates per case: name -> (min, max) on the
# decimal value; min == max for exact equality, None for unbounded.
_INF = (0, None)


def _rng(lo, hi):
    return (lo, hi)


EVIDENCE_FIELDS = {
    "aarch64_sched_yield": {
        "a_yields": _rng(10000, None), "b_yields": _rng(10000, None),
        "switches": _rng(1, None)},
    "aarch64_sched_regs": {
        "reg_fail": _rng(0, 0), "stack_fail": _rng(0, 0),
        "sp_fail": _rng(0, 0)},
    "aarch64_sched_entry": {
        "entries": _rng(1, 1), "arg_ok": _rng(1, 1),
        "pre_c_sp_ok": _rng(1, 1), "irq_enabled": _rng(1, 1),
        "current_ok": _rng(1, 1)},
    "aarch64_sched_irq": {
        "on_switch": _rng(1, 1), "off_switch": _rng(1, 1),
        "on_noswitch": _rng(1, 1), "off_noswitch": _rng(1, 1)},
    "aarch64_sched_blocker": {
        "already": _rng(1, 1), "explicit": _rng(1, 1),
        "pre_switch": _rng(1, 1), "scan": _rng(1, 1),
        "duplicate_enqueue": _rng(0, 0)},
    "aarch64_sched_reclaim": {
        "created": _rng(1000, None), "stack_live": _rng(0, 0),
        "thread_live": _rng(0, 0), "fpu_live": _rng(0, 0),
        "next_stack": _rng(1, 1), "list_delta": _rng(0, 0),
        "rq_delta": _rng(0, 0)},
    "aarch64_sched_idle": {
        "idle_in_tree": _rng(0, 0), "idle_observed": _rng(1, 1),
        "wake_ticks": _rng(0, 10)},
    "aarch64_sched_timer": {
        "tick_delta": _rng(3, None), "irq_frame_ok": _rng(1, 1),
        "nested_schedule": _rng(0, 0), "irq_switches": _rng(0, 0)},
}
AP_SUMMARY_FIELDS = {"cpu0_only": _rng(1, 1)}
AP_OBSERVATION_FIELDS = {
    "scheduler_ok": _rng(0, 0), "nr_running": _rng(0, 0),
    "work_ok": _rng(1, 1)}

TEST_LINE = re.compile(r"^\[TEST\] (START|SELECT|BEGIN|PASS|FAIL|SKIP|END)\b(.*)$")
EVIDENCE_LINE = re.compile(r"^\[sched-evidence\] id=(\S+)(?:\s+(.*))?$")
FAULT_LINE = re.compile(
    r"^\[sched-fault\] case=(\S+) phase=pre-switch target=(\d+) "
    r"switches_to_target=(\d+)$")
CONTEXT_REJECT_PREFIX = "[aarch64] context reject code="
ANY_FATAL_LINE = re.compile(
    r"(\bFATAL\b|\bPANIC\b|\bOops\b|context fatal|context reject|"
    r"\[aarch64\] .* fatal)", re.I)
COORDINATOR_ERROR = re.compile(r"^\[selftest\] ERROR:", re.M)


def _parse_kv(rest):
    """Parse ``k=v`` tokens; duplicate keys are an error (reported)."""
    kv = {}
    dup = []
    for tok in rest.split():
        if "=" not in tok:
            return None, [f"non key=value token {tok!r}"]
        k, _, v = tok.partition("=")
        if k in kv:
            dup.append(k)
        kv[k] = v
    if dup:
        return None, [f"duplicate field(s) {sorted(set(dup))}"]
    return kv, []


def _check_fields(where, kv, spec, errors):
    for name, (lo, hi) in spec.items():
        if name not in kv:
            errors.append(f"{where}: missing field {name}")
            continue
        raw = kv[name]
        if not raw.isdigit():
            errors.append(f"{where}: field {name}={raw!r} is not decimal")
            continue
        value = int(raw)
        if value < lo or (hi is not None and value > hi):
            bound = f">={lo}" if hi is None else f"in [{lo},{hi}]"
            errors.append(f"{where}: field {name}={value} violates {bound}")
    for name in kv:
        if name not in spec:
            errors.append(f"{where}: unknown field {name}")


def parse_sched_evidence(text, cpus):
    """Strict §10.2 positive evidence gate.

    Returns ``(ok, errors)``.  A trace is accepted only when the complete
    14-ID manifest appears exactly once per case as PASS, in manifest
    order, with no FAIL/SKIP/fatal/coordinator error, every frozen
    evidence field present exactly once with its predicate satisfied, and
    the per-AP observation count matching ``cpus``."""
    errors = []
    text = text.replace("\r", "")

    # 1. Protocol shape via the shared parser (allowed_ids membership).
    pr = parse_v1(text, suite="kernel-selftest",
                  allowed_ids=set(EXPECTED_IDS))
    errors.extend(pr.errors)
    if pr.failed or pr.skipped:
        errors.append(f"failed={pr.failed} skipped={pr.skipped} must both be 0")

    # 2. Exact set equality to the frozen manifest — NOT membership.  A
    # self-consistent reduced manifest fails here even with valid totals.
    manifest = set(EXPECTED_IDS)
    for label, observed in (("selected", pr.selected),
                            ("started", pr.started),
                            ("terminal", set(pr.terminal))):
        if observed != manifest:
            missing = sorted(manifest - observed)
            extra = sorted(observed - manifest)
            errors.append(f"{label} set != manifest (missing={missing} "
                          f"extra={extra})")

    # 3. Ordered anchored [TEST] event stream.
    events = []
    for line in text.splitlines():
        m = TEST_LINE.match(line)
        if m:
            events.append((m.group(1), m.group(2).strip().split()))

    starts = [e for e in events if e[0] == "START"]
    ends = [e for e in events if e[0] == "END"]
    if len(starts) != 1:
        errors.append(f"expected exactly one START, saw {len(starts)}")
    if len(ends) != 1:
        errors.append(f"expected exactly one END, saw {len(ends)}")

    selects = [tokens[0] for kw, tokens in events
               if kw == "SELECT" and tokens]
    bare = [kw for kw, tokens in events if not tokens]
    if bare:
        errors.append(f"truncated [TEST] record(s) without arguments: {bare}")
    if selects != list(EXPECTED_IDS):
        errors.append("SELECT order/set does not match the frozen manifest "
                      f"exactly: {selects}")

    # BEGIN/terminal pairs strictly in manifest order, nothing between.
    run_events = [(kw, tokens) for kw, tokens in events
                  if kw in ("BEGIN", "PASS", "FAIL", "SKIP")]
    expected_pairs = []
    for cid in EXPECTED_IDS:
        expected_pairs.append(("BEGIN", cid))
        expected_pairs.append(("PASS", cid))
    flattened = [(kw, tokens[0] if tokens else "") for kw, tokens in run_events]
    if flattened != expected_pairs:
        errors.append("BEGIN/terminal pairs are not exactly one "
                      "BEGIN+PASS per case in manifest order")

    # 4. Any fatal / coordinator error anywhere (including after END).
    fatal_hits = [ln for ln in text.splitlines() if ANY_FATAL_LINE.search(ln)]
    if fatal_hits:
        errors.append(f"fatal/diagnostic failure line(s): {fatal_hits[:3]}")
    if COORDINATOR_ERROR.search(text):
        errors.append("coordinator ERROR diagnostic present")

    # 5. Per-case evidence records: exact order, exact fields, correct
    #    placement (after the previous case's terminal, before own).
    records = []                       # (case_id, kv, line_index)
    for idx, line in enumerate(text.splitlines()):
        m = EVIDENCE_LINE.match(line)
        if m:
            records.append((m.group(1), m.group(2) or "", idx))
    sched_ids = set(LATE_IDS)
    for cid, _, _ in records:
        if cid not in sched_ids:
            errors.append(f"unknown evidence record id={cid}")

    line_count = len(text.splitlines())
    # Position of each case's terminal record, for placement checks.
    terminal_pos = {}
    for idx, line in enumerate(text.splitlines()):
        m = TEST_LINE.match(line)
        if m and m.group(1) in ("PASS", "FAIL", "SKIP") and m.group(2).strip():
            cid = m.group(2).split()[0]
            terminal_pos.setdefault(cid, idx)

    prev_terminal = -1
    for cid in LATE_IDS:
        own = [r for r in records if r[0] == cid]
        own.sort(key=lambda r: r[2])
        terminal = terminal_pos.get(cid)
        if terminal is None:
            continue                     # already reported by the protocol check
        if cid == "aarch64_sched_ap":
            expected_lines = cpus - 1 + 1
            if len(own) != expected_lines:
                errors.append(
                    f"aarch64_sched_ap: expected {cpus - 1} AP observation(s) "
                    f"+ 1 summary at cpus={cpus}, saw {len(own)} record(s)")
                continue
            # First cpus-1 are per-AP observations, last is the summary.
            ap_ids = set()
            for _, rest, _ in own[:-1]:
                kv, errs = _parse_kv(rest)
                if kv is None:
                    errors.extend(f"ap observation: {e}" for e in errs)
                    continue
                if "cpu" not in kv or not kv["cpu"].isdigit():
                    errors.append("ap observation missing decimal cpu field")
                    continue
                ap_ids.add(int(kv["cpu"]))
                kv.pop("cpu")
                _check_fields("ap observation", kv, AP_OBSERVATION_FIELDS,
                              errors)
            if ap_ids != set(range(1, cpus)):
                errors.append(
                    f"ap observation cpu ids {sorted(ap_ids)} != "
                    f"{sorted(set(range(1, cpus)))}")
            kv, errs = _parse_kv(own[-1][1])
            if kv is None:
                errors.extend(f"ap summary: {e}" for e in errs)
            else:
                if "cpu_count" not in kv or not kv["cpu_count"].isdigit():
                    errors.append("ap summary missing decimal cpu_count")
                elif int(kv["cpu_count"]) != cpus:
                    errors.append(f"ap summary cpu_count={kv['cpu_count']} "
                                  f"!= -smp {cpus}")
                else:
                    kv.pop("cpu_count")
                    _check_fields("ap summary", kv, AP_SUMMARY_FIELDS, errors)
        else:
            if len(own) != 1:
                errors.append(
                    f"{cid}: expected exactly 1 evidence record, saw {len(own)}")
                continue
            kv, errs = _parse_kv(own[0][1])
            if kv is None:
                errors.extend(f"{cid}: {e}" for e in errs)
            else:
                _check_fields(cid, kv, EVIDENCE_FIELDS[cid], errors)
        if own:
            if own[0][2] < prev_terminal:
                errors.append(f"{cid}: evidence record out of case order")
            if own[-1][2] > terminal:
                errors.append(f"{cid}: evidence record after its terminal")
            prev_terminal = terminal

    # 6. Every sched case must have had its record (missing = failure).
    for cid in LATE_IDS:
        if not [r for r in records if r[0] == cid]:
            errors.append(f"{cid}: no [sched-evidence] record")

    # 7. No [TEST] events after END (a second protocol, late records...).
    if ends:
        end_idx = None
        for idx, line in enumerate(text.splitlines()):
            if TEST_LINE.match(line) and line.startswith("[TEST] END"):
                end_idx = idx
                break
        if end_idx is not None:
            for idx, line in enumerate(text.splitlines()):
                if idx > end_idx and TEST_LINE.match(line):
                    errors.append(f"[TEST] record after END: {line!r}")
                    break

    return (not errors), errors


# ── Phase 5: fault-variant evidence ────────────────────────────────

def parse_sched_fault(text, case, cpus):
    """Strict §10.2 negative-context gate for one fault ``case``.

    The expected outcome IS the pre-switch rejection: the exact frozen
    marker, then the matching production fatal reason, then NOTHING —
    no victim entry, no scheduler completion, no further protocol or
    evidence records.  Returns ``(ok, errors)``."""
    errors = []
    text = text.replace("\r", "")
    lines = text.splitlines()

    faults = [(m.group(1), m.group(2), m.group(3), idx)
              for idx, line in enumerate(lines)
              for m in [FAULT_LINE.match(line)] if m]
    if not faults:
        return False, ["no [sched-fault] pre-switch record observed "
                       "(timeout or wrong variant)"]
    if len(faults) != 1:
        return False, [f"expected exactly one fault record, saw {len(faults)}"]
    fcase, target, switches, fault_idx = faults[0]
    if fcase != case:
        errors.append(f"fault record case={fcase} != expected {case}")
    if not target.isdigit():
        errors.append(f"fault record target={target!r} is not decimal")
    if switches != "0":
        errors.append(f"switches_to_target={switches} != 0: the victim was "
                      "committed before rejection")

    reason = FAULT_REASONS[case]
    expected_fatal = f"{CONTEXT_REJECT_PREFIX}6 reason={reason} "
    reject_idx = [idx for idx, line in enumerate(lines)
                  if line.startswith(CONTEXT_REJECT_PREFIX)]
    matching = [idx for idx in reject_idx if lines[idx].startswith(expected_fatal)]
    if len(matching) != 1:
        seen = [lines[idx][len(CONTEXT_REJECT_PREFIX):] for idx in reject_idx]
        errors.append(f"expected exactly one 'context reject code=6 "
                      f"reason={reason}' fatal, saw {len(matching)} "
                      f"(all rejects: {seen})")
    else:
        if matching[0] < fault_idx:
            errors.append("the fatal fired before the [sched-fault] record")
        fatal_idx = matching[0]
        # Nothing may follow the expected fatal: no protocol records, no
        # evidence records, no second fatal, no marker duplication.
        for idx in range(fatal_idx + 1, len(lines)):
            line = lines[idx]
            if not line.strip():
                continue
            if (TEST_LINE.match(line) or EVIDENCE_LINE.match(line)
                    or FAULT_LINE.match(line) or ANY_FATAL_LINE.search(line)
                    or line.startswith("[sched]")):
                errors.append(f"record after the expected fatal: {line!r}")
                break

    # The injection fires at the FIRST cross-task switch, so no scheduler
    # case can have completed and no late case can have started.
    for line in lines:
        m = TEST_LINE.match(line)
        if m and m.group(1) in ("BEGIN", "PASS", "FAIL", "SKIP"):
            cid = m.group(2).split()[0] if m.group(2).strip() else ""
            if cid in LATE_IDS:
                errors.append(f"late case {cid} reached {m.group(1)} — the "
                              "scheduler ran past the injection point")
                break
        if m and m.group(1) == "END":
            errors.append("protocol END observed — the scheduler completed")
            break
    if any(EVIDENCE_LINE.match(line) for line in lines):
        errors.append("[sched-evidence] record observed — a scheduler case "
                      "completed before or after the injection")
    return (not errors), errors


# ── Phase 5: symbol audit (nm, checked subprocess) ─────────────────

def audit_fault_symbol(llvm_nm, elf, expect_present, symbol=FAULT_SYMBOL):
    """Exact-symbol nm audit on the real ELF path.

    Uses ``subprocess.run([nm, elf], check=True, ...)`` so an nm failure
    is an error, never a silent no-match.  Returns (ok, error_or_None,
    sha256)."""
    path = Path(elf)
    if not path.is_file():
        return False, f"ELF not found: {elf} (build the variant first; " \
                      "never audit a deleted/stale artifact)", None
    try:
        with path.open("rb") as stream:
            sha = hashlib.file_digest(stream, "sha256").hexdigest()
    except OSError as exc:
        return False, f"cannot read {elf}: {exc}", None
    try:
        proc = subprocess.run([llvm_nm, str(path)], check=True,
                              capture_output=True, text=True)
    except FileNotFoundError:
        return False, f"nm tool not found: {llvm_nm}", sha
    except subprocess.CalledProcessError as exc:
        return False, f"nm failed on {elf}: rc={exc.returncode} " \
                      f"{exc.stderr[-200:]}", sha
    names = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == symbol:
            names.add(parts[-1])
    present = bool(names)
    if present != expect_present:
        return False, (f"{elf}: symbol {symbol} "
                       f"{'present' if present else 'absent'} "
                       f"(expected {'present' if expect_present else 'absent'})"), sha
    return True, None, sha


# ── Phase 5: the live sched runner ─────────────────────────────────

SUITE = "aarch64-sched"
# Absolute per-boot budget (spec §10.2): evidence deadline + the 1 s
# post-completion drain both fit inside BOOT_DEADLINE_SECONDS.
BOOT_DEADLINE_SECONDS = 120.0
DRAIN_SECONDS = 1.0

# The positive matrix: every case at 1/2/4/8 CPUs, with the stress
# trio (regs/blocker/reclaim — all inside the suite) repeated 3x at
# 1 and 2 CPUs per spec §10.2 (repeat the whole positive suite).
POSITIVE_MATRIX = [(1, 1), (1, 2), (1, 3), (2, 1), (2, 2), (2, 3), (4, 1), (8, 1)]
FAULT_MATRIX = [(1, 1), (2, 1)]


def _sched_qemu_command(qemu, cpus, image, firmware, dtb):
    return [qemu, "-M", "virt,gic-version=2,acpi=off", "-cpu", "cortex-a53",
            "-smp", str(cpus), "-m", "512",
            "-drive", f"if=pflash,format=raw,file={firmware}",
            "-drive", f"if=none,file={image},format=raw,readonly=on,id=disk",
            "-device", "virtio-blk-device,drive=disk", "-serial", "stdio",
            "-display", "none", "-no-reboot", "-no-shutdown", "-dtb", dtb]


def _git_revision():
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


def _sha256(path):
    try:
        with open(path, "rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
    except (OSError, TypeError):
        return None


def _write_report(archive, *, argv, profile, cpu, variant, firmware, image,
                  started, status, runner_exit_code, session, errors,
                  extra=None):
    if archive is None or RunReport is None:
        return
    try:
        rev, dirty = _git_revision()
        outcomes = [{"variant": variant, "errors": list(errors)}]
        if extra:
            outcomes[0].update(extra)
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision=rev,
            git_dirty=dirty,
            profile=profile,
            suite=SUITE,
            request=None,
            declared_ids=list(EXPECTED_IDS),
            observed_ids=None,
            argv=list(argv),
            cpu_count=int(cpu),
            memory_mib=512,
            tool_versions={},
            firmware_path=str(firmware),
            firmware_sha256_before=None,
            firmware_sha256_after=_sha256(firmware),
            image_path=str(image),
            image_sha256_before=None,
            image_sha256_after=_sha256(image),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started),
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None else None),
            stopped_by_runner=bool(session is not None and session.stopped_by_runner),
            status=status,
            count_unit="case",
            outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # evidence must never mask the run result
        print(f"[{SUITE}] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


def run_sched_boot(args, variant, cpus, repetition, paths, session_factory=None):
    """One QEMU boot of one variant.  Returns the plan exit code
    (0 PASS / 1 FAIL / 2 ERROR)."""
    started = time.monotonic()
    is_fault = variant != "positive"
    archive = None
    if args.build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir) / f"{variant}-cpus-{cpus}-run-{repetition}"
    run_dir.mkdir(parents=True, exist_ok=True)

    dtb = generate_diagnostic_dtb(args.qemu, str(run_dir), cpus)
    command = _sched_qemu_command(args.qemu, cpus, paths["image"],
                                  paths["firmware"], dtb)
    metadata = {
        "command": command, "variant": variant, "cpus": cpus,
        "run": repetition,
        "firmware": str(Path(paths["firmware"]).resolve()),
        "firmware_sha256": _sha256(paths["firmware"]),
        "image": str(Path(paths["image"]).resolve()),
        "image_sha256": _sha256(paths["image"]),
        "elf": str(Path(paths["elf"]).resolve()),
        "elf_sha256": _sha256(paths["elf"]),
        "deadline_s": BOOT_DEADLINE_SECONDS,
    }
    (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print(json.dumps({"event": "spawn-error", "variant": variant,
                          "cpus": cpus, "run": repetition,
                          "error": "ProcessSession is unavailable"}))
        _write_report(archive, argv=command, profile=args.profile, cpu=cpus,
                      variant=variant, firmware=paths["firmware"],
                      image=paths["image"], started=started, status="ERROR",
                      runner_exit_code=2, session=None,
                      errors=["spawn error: ProcessSession is unavailable"])
        return 2

    session = None
    child_exited_before_runner_stop = False
    try:
        # Absolute deadline: the evidence window ends 1 s before the boot
        # budget so the drain stays inside BOOT_DEADLINE_SECONDS; ticks
        # arriving after evidence never extend anything.
        session = session_factory(
            argv=command, run_dir=run_dir,
            timeout_s=BOOT_DEADLINE_SECONDS - DRAIN_SECONDS)
        session.start()
        if is_fault:
            predicate = lambda text: parse_sched_fault(text, variant, cpus)[0]
        else:
            predicate = lambda text: parse_sched_evidence(text, cpus)[0]
        session.wait_for(predicate)
        session.observe(DRAIN_SECONDS)
        # A complete trace is only evidence if QEMU remains alive through
        # the full drain. observe() returns early on natural child exit;
        # stop() also leaves stopped_by_runner false in that case.
        child_exited_before_runner_stop = session.returncode is not None
        session.stop()
        child_exited_before_runner_stop |= not session.stopped_by_runner
        session.close()
    except Exception as error:  # launch / OS error → ERROR, never PASS
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        print(json.dumps({"event": "spawn-error", "variant": variant,
                          "cpus": cpus, "run": repetition, "error": str(error)}))
        _write_report(archive, argv=command, profile=args.profile, cpu=cpus,
                      variant=variant, firmware=paths["firmware"],
                      image=paths["image"], started=started, status="ERROR",
                      runner_exit_code=2, session=session,
                      errors=[f"spawn error: {error}"])
        return 2

    text = session.text
    timed_out = session.timed_out
    if is_fault:
        accepted, why = parse_sched_fault(text, variant, cpus)
    else:
        accepted, why = parse_sched_evidence(text, cpus)
    result = accepted and not timed_out and not child_exited_before_runner_stop

    errors = []
    if timed_out:
        errors.append("absolute boot deadline exceeded before evidence")
    if child_exited_before_runner_stop:
        errors.append("QEMU exited before the runner completed the observation drain")
    errors.extend(why)
    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": session.returncode,
        "result": "PASS" if result else "FAIL",
        "errors": errors,
    })
    (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    _write_report(archive, argv=command, profile=args.profile, cpu=cpus,
                  variant=variant, firmware=paths["firmware"],
                  image=paths["image"], started=started,
                  status="PASS" if result else "FAIL",
                  runner_exit_code=0 if result else 1, session=session,
                  errors=errors,
                  extra={"timeout": timed_out, "is_fault": is_fault})
    print(json.dumps({
        "event": "sched-case", "variant": variant, "cpus": cpus,
        "run": repetition, "result": "PASS" if result else "FAIL",
        "timeout": timed_out,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
        "errors": errors[:5],
    }))
    return 0 if result else 1


def run_sched_mode(args):
    """The MODE=sched live entry: matrix + symbol audit + summary."""
    variants = {}
    for spec in args.variant_spec:
        variants[spec["variant"]] = spec
    wanted = (list(SCHED_FAULT_CASES) + ["positive"]) if args.case == "all" \
        else [args.case]
    missing = [v for v in wanted if v not in variants]
    if missing:
        print(f"[{SUITE}] ERROR: missing --variant spec(s) for {missing}",
              file=sys.stderr)
        return 2
    for v in wanted:
        spec = variants[v]
        for key in ("image", "firmware", "elf"):
            if not Path(spec[key]).is_file():
                print(f"[{SUITE}] ERROR: {v} {key} not found: {spec[key]} "
                      "(MODE=sched prep builds it; the runner never rebuilds)",
                      file=sys.stderr)
                return 2

    # Symbol audit FIRST (build-level property; fails fast):
    # the positive ELF must be clean, each fault ELF must carry the
    # injection symbol.  `all` additionally audits the ordinary normal
    # and ordinary selftest ELFs — the whole-mode gate guarantees they
    # were built by then.
    audit_failures = []
    audit_records = []
    audits = [(variants[v]["elf"], v != "positive", v) for v in wanted]
    if args.case == "all":
        build_root = Path(args.build_dir) if args.build_dir else None
        if build_root:
            audits.append((build_root / "kernel" / "kernel.elf", False,
                           "ordinary-normal"))
            audits.append((build_root / "kernel" / "selftest" / "kernel.elf",
                           False, "ordinary-selftest"))
    for elf, expect, label in audits:
        ok, err, sha = audit_fault_symbol(args.llvm_nm, elf, expect)
        audit_records.append({"elf": str(elf), "label": label,
                              "expect_injection_symbol": expect,
                              "sha256": sha, "ok": ok, "error": err})
        if not ok:
            audit_failures.append(err)
    print(json.dumps({"event": "symbol-audit", "records": audit_records,
                      "ok": not audit_failures}))
    if audit_failures:
        for err in audit_failures:
            print(f"[{SUITE}] symbol audit FAIL: {err}", file=sys.stderr)
        return 1

    # Matrix: positive full suite 3x at 1/2 + once at 4/8; each fault at
    # 1/2.  Every entry runs even after an earlier failure; a crash of
    # the runner itself records the remaining entries as not-run.
    matrix = []
    if args.case in ("positive", "all"):
        matrix += [("positive", c, r) for c, r in POSITIVE_MATRIX]
    if args.case in SCHED_FAULT_CASES or args.case == "all":
        faults = SCHED_FAULT_CASES if args.case == "all" else [args.case]
        matrix += [(f, c, r) for f in faults for c, r in FAULT_MATRIX]

    outcomes = []
    for variant, cpus, repetition in matrix:
        try:
            code = run_sched_boot(args, variant, cpus, repetition,
                                  variants[variant])
        except Exception as error:  # keep the matrix explicit, never skip
            code = 2
            print(json.dumps({"event": "runner-error", "variant": variant,
                              "cpus": cpus, "run": repetition,
                              "error": str(error)}), file=sys.stderr)
        outcomes.append({"variant": variant, "cpus": cpus,
                         "run": repetition,
                         "status": {0: "PASS", 1: "FAIL", 2: "ERROR"}[code]})

    print(json.dumps({"event": "matrix-summary", "case": args.case,
                      "outcomes": outcomes}))
    if any(o["status"] != "PASS" for o in outcomes):
        return 1
    return 0


# ── Self-test (pure; no QEMU) ──────────────────────────────────────

def _selftest_fixture(cpus=2):
    evidence = {
        "aarch64_sched_yield": "a_yields=10000 b_yields=10000 switches=99",
        "aarch64_sched_regs": "reg_fail=0 stack_fail=0 sp_fail=0",
        "aarch64_sched_entry":
            "entries=1 arg_ok=1 pre_c_sp_ok=1 irq_enabled=1 current_ok=1",
        "aarch64_sched_irq":
            "on_switch=1 off_switch=1 on_noswitch=1 off_noswitch=1",
        "aarch64_sched_blocker":
            "already=1 explicit=1 pre_switch=1 scan=1 duplicate_enqueue=0",
        "aarch64_sched_reclaim":
            "created=1000 stack_live=0 thread_live=0 fpu_live=0 "
            "next_stack=1 list_delta=0 rq_delta=0",
        "aarch64_sched_idle": "idle_in_tree=0 idle_observed=1 wake_ticks=3",
        "aarch64_sched_timer":
            "tick_delta=3 irq_frame_ok=1 nested_schedule=0 irq_switches=0",
    }
    lines = ["[TEST] START v=1 suite=kernel-selftest expected=14"]
    lines += [f"[TEST] SELECT {cid} required=1" for cid in EXPECTED_IDS]
    for cid in EXPECTED_IDS:
        if cid.startswith("aarch64_sched_"):
            if cid == "aarch64_sched_ap":
                for cpu in range(1, cpus):
                    lines.append("[sched-evidence] id=aarch64_sched_ap "
                                 f"cpu={cpu} scheduler_ok=0 nr_running=0 "
                                 "work_ok=1")
                lines.append("[sched-evidence] id=aarch64_sched_ap "
                             f"cpu_count={cpus} cpu0_only=1")
            else:
                lines.append(f"[sched-evidence] id={cid} {evidence[cid]}")
        lines.append(f"[TEST] BEGIN {cid}")
        lines.append(f"[TEST] PASS {cid}")
    lines.append("[TEST] END suite=kernel-selftest total=14 passed=14 "
                 "failed=0 skipped=0")
    return "\n".join(lines) + "\n"


def self_test():
    base = _selftest_fixture(cpus=2)
    ok, errors = parse_sched_evidence(base, cpus=2)
    assert ok, errors
    ok, errors = parse_sched_evidence(_selftest_fixture(cpus=1), cpus=1)
    assert ok, errors
    ok, errors = parse_sched_evidence(_selftest_fixture(cpus=8), cpus=8)
    assert ok, errors

    def bad(text, cpus=2):
        ok, errors = parse_sched_evidence(text, cpus=cpus)
        assert not ok, errors or "expected rejection"

    # missing case (drop regs entirely, self-consistent 13)
    pruned = base.replace(
        "[TEST] SELECT aarch64_sched_regs required=1\n", "") \
        .replace("[sched-evidence] id=aarch64_sched_regs reg_fail=0 "
                 "stack_fail=0 sp_fail=0\n", "") \
        .replace("[TEST] BEGIN aarch64_sched_regs\n", "") \
        .replace("[TEST] PASS aarch64_sched_regs\n", "") \
        .replace("expected=14", "expected=13") \
        .replace("total=14 passed=14", "total=13 passed=13")
    bad(pruned)
    # duplicate terminal
    bad(base + "[TEST] PASS aarch64_sched_yield\n")
    # FAIL and SKIP records
    bad(base.replace("[TEST] PASS aarch64_sched_irq\n",
                     "[TEST] FAIL aarch64_sched_irq reason=rc_-1\n")
        .replace("total=14 passed=14", "total=14 passed=13")
        .replace("failed=0 skipped=0", "failed=1 skipped=0"))
    bad(base.replace("[TEST] PASS aarch64_sched_timer\n",
                     "[TEST] SKIP aarch64_sched_timer reason=x\n")
        .replace("total=14 passed=14", "total=14 passed=13")
        .replace("failed=0 skipped=0", "failed=0 skipped=1"))
    # crash: no END
    bad(base.replace("[TEST] END suite=kernel-selftest total=14 "
                     "passed=14 failed=0 skipped=0\n", ""))
    # timeout: nothing at all
    bad("")
    # swapped case blocks (regs <-> entry)
    ra = ("[sched-evidence] id=aarch64_sched_regs reg_fail=0 stack_fail=0 "
          "sp_fail=0\n[TEST] BEGIN aarch64_sched_regs\n"
          "[TEST] PASS aarch64_sched_regs\n")
    ea = ("[sched-evidence] id=aarch64_sched_entry entries=1 arg_ok=1 "
          "pre_c_sp_ok=1 irq_enabled=1 current_ok=1\n"
          "[TEST] BEGIN aarch64_sched_entry\n"
          "[TEST] PASS aarch64_sched_entry\n")
    bad(base.replace(ra, "@@R@@").replace(ea, ra).replace("@@R@@", ea))
    # unknown late record + coordinator error + post-END fatal
    bad(base.replace("[TEST] BEGIN aarch64_sched_ap\n",
                     "[sched-evidence] id=aarch64_sched_ghost x=1\n"
                     "[TEST] BEGIN aarch64_sched_ap\n"))
    bad(base.replace("[TEST] END",
                     "[selftest] ERROR: duplicate case id 'x'\n[TEST] END"))
    bad(base + "[aarch64] context fatal code=1 sp=0x1 base=0x2 top=0x3\n")
    # evidence predicate violations
    for old, new in (("a_yields=10000", "a_yields=9999"),
                     ("switches=99", "switches=0"),
                     ("created=1000", "created=1"),
                     ("next_stack=1", "next_stack=0"),
                     ("wake_ticks=3", "wake_ticks=11"),
                     ("tick_delta=3", "tick_delta=2"),
                     ("cpu0_only=1", "cpu0_only=0"),
                     ("work_ok=1", "work_ok=0")):
        bad(base.replace(old, new), cpus=2)
    # missing whole record / missing field / duplicate field
    bad(base.replace("[sched-evidence] id=aarch64_sched_timer tick_delta=3 "
                     "irq_frame_ok=1 nested_schedule=0 irq_switches=0\n", ""))
    bad(base.replace("sp_fail=0\n", "\n"))
    bad(base.replace("switches=99", "switches=99 switches=1"))
    # AP count mismatch at cpus=2
    bad(base.replace("cpu=1 scheduler_ok=0 nr_running=0 work_ok=1\n", ""))
    bad(base.replace("cpu_count=2", "cpu_count=4"))
    bad(base.replace("cpu_count=2",
                     "cpu=1 scheduler_ok=0 nr_running=0 work_ok=1\n"
                     "cpu_count=2"))

    # Fault evidence.
    def fault_log(case, switches="0", tail=""):
        reason = FAULT_REASONS[case]
        text = (f"[TEST] START v=1 suite=kernel-selftest expected=14\n"
                "[sched] idle-ready cpu=0 scheduler_ok=1 irq=on\n"
                f"[sched-fault] case={case} phase=pre-switch target=3 "
                f"switches_to_target={switches}\n"
                f"[aarch64] context reject code=6 reason={reason} "
                "sp=0x0 pc=0x0 range=0x0..0x0\n")
        return text + tail

    for case in SCHED_FAULT_CASES:
        ok, errors = parse_sched_fault(fault_log(case), case, 2)
        assert ok, (case, errors)
    ok, errors = parse_sched_fault(fault_log("sp-top"), "sp-top", 1)
    assert ok, errors
    ok, errors = parse_sched_fault("UEFI: booting OS01\n", "sp-top", 2)
    assert not ok
    ok, errors = parse_sched_fault(
        fault_log("sp-low", switches="1"), "sp-low", 2)
    assert not ok
    ok, errors = parse_sched_fault(
        fault_log("pc-low").replace(FAULT_REASONS["pc-low"], "valid"),
        "pc-low", 2)
    assert not ok
    ok, errors = parse_sched_fault(
        fault_log("pc-data", tail="[TEST] END suite=kernel-selftest "
                    "total=14 passed=14 failed=0 skipped=0\n"),
        "pc-data", 2)
    assert not ok
    ok, errors = parse_sched_fault(
        fault_log("sp-top", tail="[sched-evidence] id=aarch64_sched_entry "
                                 "entries=1 arg_ok=1 pre_c_sp_ok=1 "
                                 "irq_enabled=1 current_ok=1\n"),
        "sp-top", 2)
    assert not ok
    ok, errors = parse_sched_fault(fault_log("sp-top") + fault_log("sp-top"),
                                   "sp-top", 2)
    assert not ok
    print("aarch64_scheduler: self-test passed")


# ── CLI ────────────────────────────────────────────────────────────

def _extract_variant_specs(argv):
    """Pull the ``--variant NAME --image P --firmware P --elf P`` triplets
    out of ``argv`` (fixed schema, repeatable).  Returns (specs, rest):
    the rest is what argparse should see."""
    specs, rest = [], []
    i = 0
    while i < len(argv):
        tok = argv[i]
        if tok == "--variant":
            if i + 1 >= len(argv):
                raise SystemExit("--variant requires a name argument")
            spec = {"variant": argv[i + 1]}
            i += 2
            for key in ("image", "firmware", "elf"):
                if i + 1 < len(argv) and argv[i] == f"--{key}":
                    spec[key] = argv[i + 1]
                    i += 2
                else:
                    raise SystemExit(
                        f"--variant {spec['variant']} must be followed by "
                        f"--{key} <path>")
            specs.append(spec)
        else:
            rest.append(tok)
            i += 1
    return specs, rest


def main():
    variant_specs, argv_rest = _extract_variant_specs(
        [a for a in sys.argv[1:] if a != "--self-test"]
        if "--self-test" not in sys.argv[1:] else sys.argv[1:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    # Phase 3 idle smoke (preserved contract).
    parser.add_argument("--idle-smoke", action="store_true")
    parser.add_argument("--image")
    parser.add_argument("--firmware")
    parser.add_argument("--cpus", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--qemu", default=os.environ.get("QEMU_BIN",
                                                         "qemu-system-aarch64"))
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--log-dir")
    # Phase 5 MODE=sched surface (the --variant triplets were already
    # extracted above; these declarations only keep --help honest).
    parser.add_argument("--case", choices=SCHED_CASES)
    parser.add_argument("--variant", action="append", default=[],
                        metavar="NAME",
                        help="repeatable: --variant NAME --image P "
                             "--firmware P --elf P (fixed schema)")
    parser.add_argument("--llvm-nm", default=os.environ.get("AARCH64_NM",
                                                            "llvm-nm"))
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                            "default"))
    args = parser.parse_args(argv_rest)

    if args.self_test:
        self_test()
        return 0

    args.variant_spec = variant_specs
    for spec in variant_specs:
        if spec["variant"] not in SCHED_FAULT_CASES + ("positive",):
            parser.error(f"--variant name must be one of "
                         f"{SCHED_FAULT_CASES + ('positive',)}, got "
                         f"{spec['variant']!r}")

    if args.idle_smoke and args.case:
        parser.error("--idle-smoke and --case are mutually exclusive")
    if not args.idle_smoke and not args.case:
        parser.error("select a mode: --idle-smoke or --case <name>")
    if args.case and not args.log_dir and not args.build_dir:
        parser.error("--case requires --log-dir and/or --build-dir")

    if args.idle_smoke:
        if not args.image or not args.firmware:
            parser.error("--idle-smoke requires --image and --firmware")
        if not Path(args.image).is_file() or not Path(args.firmware).is_file():
            parser.error("--image and --firmware must name existing files")
        if args.timeout != 120:
            parser.error("Phase 3 idle smoke uses a fixed 120-second deadline")
        if any(c not in (1, 2, 4, 8) for c in args.cpus):
            parser.error("--cpus values must be selected from 1 2 4 8")
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        log_dir = Path(args.log_dir) if args.log_dir else \
            _ROOT / "test-results" / "aarch64-scheduler" / stamp
        log_dir.mkdir(parents=True, exist_ok=True)
        failed = False
        for cpus in args.cpus:
            try:
                _, log_path, command = run_one(args, cpus, log_dir)
                print(f"[aarch64-scheduler] cpus={cpus} status=PASS log={log_path}")
                print("[aarch64-scheduler] command=" + " ".join(command))
            except Exception as exc:  # evidence collection should continue across counts
                failed = True
                print(f"[aarch64-scheduler] cpus={cpus} status=FAIL error={exc}", file=sys.stderr)
                print(f"[aarch64-scheduler] log={log_dir / f'cpus-{cpus}' / 'serial.log'}", file=sys.stderr)
        return 1 if failed else 0

    return run_sched_mode(args)


if __name__ == "__main__":
    raise SystemExit(main())
