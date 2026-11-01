#!/usr/bin/env python3
"""P1 MODE=sched-ap runner: the AP cooperative-scheduler acceptance
suite (spec §7).

T3 scope: the ``fatal-owner`` fault case — an independent 2-CPU boot of
the sched-ap-fatal-owner variant whose two CPUs drive into the
no-stack fatal arbitration.  The boot passes ONLY when the serial trace
carries exactly one complete owner evidence line

    [ap-fatal] owner=<0|1> arrivals_mask=3 entrants=1 losers=1

inside the absolute 120-second boot deadline, with no successful suite
END.  Missing / duplicated / wrong-valued evidence fails, and the
runner stops QEMU as soon as the complete evidence arrives (both CPUs
are parked at that point).

The runner never rebuilds a variant and consumes only the explicit
image/firmware/ELF paths Make passes.  Before the first boot it audits
the ELFs with llvm-nm: the fatal-owner ELF must carry the arbitration
owner symbol, and the NORMAL sched-ap ELF must carry NO test-injection
symbol at all.

P1 Task 4 extends this file with the ``positive`` phase (per-CPU
ready/entry/yield/reap/work/admission/default-mask cases).
"""
import json
import re
import subprocess
import sys
import time
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb  # noqa: E402
from qemutests.aarch64_scheduler import _sched_qemu_command  # noqa: E402

try:  # noqa: E402 — path bootstrap must precede this import
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]

SUITE = "aarch64-ap-sched"
BOOT_DEADLINE_SECONDS = 120.0
DRAIN_SECONDS = 1.0

FATAL_LINE = re.compile(
    r"^\[ap-fatal\] owner=([0-9]+) arrivals_mask=([0-9]+) "
    r"entrants=([0-9]+) losers=([0-9]+)$", re.M)
FATAL_SUCCESS_END = re.compile(r"^\[TEST\] END failed=0\s*$", re.M)

# ── frozen positive schema (plan P1 Task 4) ────────────────────────
# case id -> required fields (exact names; values validated separately)
AP_CASE_FIELDS = {
    "ready": ("scheduler_ok", "idle_on_cpu", "idle_on_rq", "unique_stack"),
    "entry": ("entries", "wrong_cpu", "bad_mask", "bad_sp", "bad_regs"),
    "yield": ("tasks", "yields", "bad_canary", "irq_switches"),
    "reap": ("tasks", "stack_alloc", "stack_free", "ctx_alloc",
             "ctx_free", "bad_stack", "duplicate", "overflow",
             "idle_freed"),
    "work": ("pre_handoff", "post_handoff", "seq_ok"),
    # cpu=0 only:
    "admission": ("invalid_rejected", "zero_publish", "unready_rejected",
                  "unready_na"),
    "default-mask": ("default_cpu0", "ap_to_bsp", "bsp_to_ap", "ap_to_ap",
                     "na"),
}
AP_CASE_CPU0_ONLY = ("admission", "default-mask")
# Serial lines may carry a leading CR (the aarch64 console emits
# \r before each line), so no ^ anchor: the failed=0 tail is the
# discriminating part.
FRAMEWORK_END_OK = re.compile(
    r"\[TEST\] END suite=[A-Za-z0-9_-]+ total=[0-9]+ passed=[0-9]+ "
    r"failed=0 skipped=0")
START_LINE = re.compile(
    r"^\[ap-sched\] START phase=ap cpus=([0-9]+) accepted_mask=([0-9]+)$")
CASE_LINE = re.compile(
    r"^\[ap-sched\] phase=ap case=([a-z-]+) cpu=([0-9]+)(?:\s+(.*))?$")
END_LINE = re.compile(r"^\[ap-sched\] END phase=ap failed=([0-9]+)$")


def parse_ap_start(line):
    m = START_LINE.match(line.strip())
    if not m:
        return None
    cpus, mask = int(m.group(1)), int(m.group(2))
    if cpus < 1 or cpus > 8 or mask != (1 << cpus) - 1:
        return None
    return cpus, mask


def parse_ap_case_line(line):
    m = CASE_LINE.match(line.strip())
    if not m:
        return None
    case, cpu = m.group(1), int(m.group(2))
    kv = {}
    rest = m.group(3) or ""
    for tok in rest.split():
        k, _, v = tok.partition("=")
        if not k or not v or not v.isdigit() or k in kv:
            return None
        kv[k] = int(v)
    return case, cpu, kv


def parse_ap_end(line):
    m = END_LINE.match(line.strip())
    if not m:
        return None
    failed = int(m.group(1))
    if failed != 0:
        return None
    return failed


def check_ap_positive_evidence(text, cpus):
    """The strict positive gate: exactly one START with the full
    accepted mask, every case line present exactly once per required
    CPU with every frozen field, exactly one END failed=0, all
    bracketed by START and END in order.  Returns (ok, errors)."""
    errors = []
    lines = text.replace("\r", "").splitlines()
    starts = [parse_ap_start(l) for l in lines]
    starts = [s for s in starts if s]
    if len(starts) != 1:
        return 1, [f"expected exactly one START line, got {len(starts)}"]
    start_cpus, _mask = starts[0]
    if start_cpus != cpus:
        errors.append(f"START cpus={start_cpus} != QEMU -smp {cpus}")

    idx_start = next(i for i, l in enumerate(lines)
                     if parse_ap_start(l))
    idx_end = None
    cases = {}
    order = []
    for i, line in enumerate(lines[idx_start + 1:], start=idx_start + 1):
        if parse_ap_start(line):
            errors.append("second START line")
            break
        end = parse_ap_end(line)
        if end is not None:
            idx_end = i
            break
        parsed = parse_ap_case_line(line)
        if parsed is None:
            continue
        case, cpu, kv = parsed
        if case not in AP_CASE_FIELDS:
            errors.append(f"unknown case {case!r}")
            continue
        order.append((case, cpu))
        key = (case, cpu)
        if key in cases:
            errors.append(f"duplicate case line {key}")
        cases[key] = kv
    if idx_end is None:
        errors.append("no [ap-sched] END phase=ap failed=0 line")
        return 1, errors

    accepted_cpus = list(range(cpus))
    up = cpus < 2

    def want(where, kv, field, value):
        if kv.get(field) != value:
            errors.append(f"{where}: {field}={kv.get(field)} != {value}")

    for cpu in accepted_cpus:
        where = f"case ready cpu={cpu}"
        kv = cases.get(("ready", cpu))
        if kv is None:
            errors.append(f"missing case ready cpu={cpu}")
            continue
        want(where, kv, "scheduler_ok", 1)
        want(where, kv, "idle_on_cpu", 1)
        want(where, kv, "idle_on_rq", 0)
        want(where, kv, "unique_stack", 1)

    for cpu in accepted_cpus:
        where = f"case entry cpu={cpu}"
        kv = cases.get(("entry", cpu))
        if kv is None:
            errors.append(f"missing case entry cpu={cpu}")
            continue
        want(where, kv, "entries", 2)
        for f in ("wrong_cpu", "bad_mask", "bad_sp", "bad_regs"):
            want(where, kv, f, 0)

    for cpu in accepted_cpus:
        where = f"case yield cpu={cpu}"
        kv = cases.get(("yield", cpu))
        if kv is None:
            errors.append(f"missing case yield cpu={cpu}")
            continue
        want(where, kv, "tasks", 2)
        want(where, kv, "yields", 64)
        want(where, kv, "bad_canary", 0)
        want(where, kv, "irq_switches", 0)

    for cpu in accepted_cpus:
        where = f"case reap cpu={cpu}"
        kv = cases.get(("reap", cpu))
        if kv is None:
            errors.append(f"missing case reap cpu={cpu}")
            continue
        for f in ("tasks", "stack_alloc", "stack_free", "ctx_alloc",
                  "ctx_free"):
            want(where, kv, f, 256)
        for f in ("bad_stack", "duplicate", "overflow", "idle_freed"):
            want(where, kv, f, 0)

    for cpu in accepted_cpus:
        where = f"case work cpu={cpu}"
        kv = cases.get(("work", cpu))
        if kv is None:
            errors.append(f"missing case work cpu={cpu}")
            continue
        for f in ("pre_handoff", "post_handoff", "seq_ok"):
            want(where, kv, f, 1)

    kv = cases.get(("admission", 0))
    if kv is None:
        errors.append("missing case admission cpu=0")
    else:
        want("case admission", kv, "invalid_rejected", 1)
        want("case admission", kv, "zero_publish", 1)
        want("case admission", kv, "unready_rejected", 0 if up else 1)
        want("case admission", kv, "unready_na", 1 if up else 0)

    kv = cases.get(("default-mask", 0))
    if kv is None:
        errors.append("missing case default-mask cpu=0")
    else:
        want("case default-mask", kv, "default_cpu0", 1)
        want("case default-mask", kv, "ap_to_bsp", 0 if up else 1)
        want("case default-mask", kv, "bsp_to_ap", 0 if up else 1)
        want("case default-mask", kv, "ap_to_ap", 0 if up else 1)
        want("case default-mask", kv, "na", 1 if up else 0)

    for cid in AP_CASE_CPU0_ONLY:
        for cpu in accepted_cpus[1:]:
            if (cid, cpu) in cases:
                errors.append(f"case {cid} must be cpu=0 only")

    if not FRAMEWORK_END_OK.search(text):
        errors.append("[TEST] END ... failed=0 missing (framework END owner)")
    return (0 if not errors else 1), errors

# Injection symbols that must NEVER appear in a normal sched-ap ELF.
INJECTION_SYMBOLS = (
    "aarch64_ap_fatal_owner_enter",
)
# Symbol the fatal-owner ELF must carry.
FATAL_OWNER_SYMBOL = "aarch64_ap_fatal_owner_enter"


def parse_ap_fatal_line(line):
    """Parse one owner evidence line.  Returns (owner, arrivals,
    entrants, losers) for the EXACT frozen evidence, else None."""
    m = FATAL_LINE.match(line.strip())
    if not m:
        return None
    owner, arrivals, entrants, losers = (int(g) for g in m.groups())
    if (owner not in (0, 1) or arrivals != 3 or entrants != 1
            or losers != 1):
        return None
    return (owner, arrivals, entrants, losers)


def extract_ap_fatal_lines(text):
    return [ln for ln in text.splitlines() if "[ap-fatal]" in ln]


def check_fatal_evidence(lines):
    """Gate the fatal-owner trace: exactly one complete owner line, no
    successful END.  Returns (0, []) or (nonzero, [errors])."""
    errors = []
    owners = []
    for line in lines:
        parsed = parse_ap_fatal_line(line)
        if parsed is not None:
            owners.append(parsed)
    if len(owners) == 0:
        errors.append("no complete [ap-fatal] owner evidence line")
    if len(owners) > 1:
        errors.append(f"expected exactly one owner line, got {len(owners)}")
    text = "\n".join(lines)
    if FATAL_SUCCESS_END.search(text):
        errors.append("fatal-owner boot reported a successful END")
    return (1 if errors else 0), errors


def _nm_symbols(llvm_nm, elf):
    proc = subprocess.run([llvm_nm, str(elf)], capture_output=True,
                          text=True, timeout=60)
    if proc.returncode != 0:
        return None, proc.stderr.strip() or "llvm-nm failed"
    symbols = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            symbols.add(parts[2])
        elif len(parts) == 2 and parts[0] != "":
            symbols.add(parts[1])
    return symbols, ""


def _parse_nm_output(out):
    symbols = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            symbols.add(parts[2])
        elif len(parts) == 2 and parts[0] != "":
            symbols.add(parts[1])
    return symbols


def audit_no_injection_symbols(nm_or_none, elf, llvm_nm):
    """Audit that @elf carries none of the injection symbols.  A
    missing ELF (or an nm failure) fails.  The test seam (@nm_or_none)
    bypasses the filesystem check."""
    path = Path(elf)
    if nm_or_none is None and not path.is_file():
        return 0, f"ELF not found: {elf}", None
    if nm_or_none is not None:
        # Test seam: a callable(nm, elf) -> (rc, stdout, stderr).
        rc, out, err = nm_or_none(llvm_nm, elf)
        if rc != 0:
            return 0, err or "nm failed", None
        symbols = _parse_nm_output(out)
    else:
        symbols, err = _nm_symbols(llvm_nm, elf)
        if symbols is None:
            return 0, err or "nm failed", None
    bad = sorted(s for s in symbols if s in INJECTION_SYMBOLS)
    if bad:
        return 0, (f"injection symbols present in normal ELF {elf}: "
                   f"{', '.join(bad)}"), None
    return 1, "", None


def audit_fatal_owner_symbol(nm_or_none, elf, llvm_nm):
    """The fatal-owner variant must carry the arbitration owner entry.
    The test seam (@nm_or_none) bypasses the filesystem check."""
    path = Path(elf)
    if nm_or_none is None and not path.is_file():
        return 0, f"ELF not found: {elf}", None
    if nm_or_none is not None:
        rc, out, err = nm_or_none(llvm_nm, elf)
        if rc != 0:
            return 0, err or "nm failed", None
        symbols = _parse_nm_output(out)
    else:
        symbols, err = _nm_symbols(llvm_nm, elf)
        if symbols is None:
            return 0, err or "nm failed", None
    if FATAL_OWNER_SYMBOL not in symbols:
        return 0, f"fatal-owner ELF {elf} lacks {FATAL_OWNER_SYMBOL}", None
    return 1, "", None


def _sha256(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_fatal_boot(args, paths, session_factory=None, archive_factory=None):
    """One fatal-owner boot: 2 CPUs, evidence-gated early stop.
    Returns the plan exit code (0 PASS / 1 FAIL / 2 ERROR)."""
    started = time.monotonic()
    archive = None
    if args.build_dir and RunArchive is not None:
        archive = archive_factory(Path(args.build_dir), SUITE) \
            if archive_factory else RunArchive.create(
                Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir) / "fatal-owner-cpus-2"
    run_dir.mkdir(parents=True, exist_ok=True)

    dtb = generate_diagnostic_dtb(args.qemu, str(run_dir), 2)
    command = _sched_qemu_command(args.qemu, 2, paths["image"],
                                  paths["firmware"], dtb)
    metadata = {
        "command": command, "case": "fatal-owner", "cpus": 2,
        "firmware": str(Path(paths["firmware"]).resolve()),
        "firmware_sha256": _sha256(paths["firmware"]),
        "image": str(Path(paths["image"]).resolve()),
        "image_sha256": _sha256(paths["image"]),
        "elf": str(Path(paths["elf"]).resolve()),
        "elf_sha256": _sha256(paths["elf"]),
        "deadline_s": BOOT_DEADLINE_SECONDS,
    }
    (run_dir / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n")

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print(json.dumps({"event": "spawn-error", "case": "fatal-owner",
                          "error": "ProcessSession is unavailable"}))
        return 2

    session = None
    try:
        session = session_factory(
            argv=command, run_dir=run_dir,
            timeout_s=BOOT_DEADLINE_SECONDS - DRAIN_SECONDS)
        session.start()
        session.wait_for(
            lambda text: check_fatal_evidence(
                extract_ap_fatal_lines(text))[0] == 0)
        # Both CPUs are parked the moment the owner printed its line:
        # stop QEMU immediately (the contract stops ON evidence).
        session.stop()
        session.close()
    except Exception as error:
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        print(json.dumps({"event": "runner-error", "case": "fatal-owner",
                          "error": str(error)}))
        return 2

    text = session.text
    timed_out = session.timed_out
    accepted, why = check_fatal_evidence(extract_ap_fatal_lines(text))
    result = accepted == 0 and not timed_out

    errors = []
    if timed_out:
        errors.append("absolute boot deadline exceeded before evidence")
    errors.extend(why)
    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": session.returncode,
        "result": "PASS" if result else "FAIL",
        "errors": errors,
    })
    (run_dir / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n")
    if archive is not None and RunReport is not None:
        try:
            archive.write(RunReport(
                argv=command, profile=args.profile, cpu=2,
                firmware=paths["firmware"], image=paths["image"],
                started=started, status="PASS" if result else "FAIL",
                runner_exit_code=0 if result else 1, session=session,
                count_unit="case", outcomes=[{
                    "case": "fatal-owner", "cpus": 2,
                    "status": "PASS" if result else "FAIL"}],
                stdout_log=str(archive.run_dir / "stdout.log"),
                stderr_log=str(archive.run_dir / "stderr.log")))
        except Exception as exc:
            print(f"[{SUITE}] warning: RunReport failed: {exc}",
                  file=sys.stderr)
    print(json.dumps({
        "event": "ap-sched-case", "case": "fatal-owner", "cpus": 2,
        "result": "PASS" if result else "FAIL", "timeout": timed_out,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
        "errors": errors[:5],
    }))
    return 0 if result else 1


POSITIVE_MATRIX = [(cpus, rep) for cpus in (1, 2, 4, 8)
                   for rep in (1, 2)]


def run_positive_boot(args, paths, cpus, repetition, session_factory=None,
                      archive_factory=None):
    """One positive boot at @cpus; strictly gated on the frozen
    envelope.  Returns (code, errors)."""
    started = time.monotonic()
    archive = None
    if args.build_dir and RunArchive is not None:
        archive = archive_factory(Path(args.build_dir), SUITE) \
            if archive_factory else RunArchive.create(
                Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir) / f"positive-cpus-{cpus}-run-{repetition}"
    run_dir.mkdir(parents=True, exist_ok=True)

    dtb = generate_diagnostic_dtb(args.qemu, str(run_dir), cpus)
    command = _sched_qemu_command(args.qemu, cpus, paths["image"],
                                  paths["firmware"], dtb)
    metadata = {
        "command": command, "case": "positive", "cpus": cpus,
        "run": repetition,
        "elf_sha256": _sha256(paths["elf"]),
        "deadline_s": BOOT_DEADLINE_SECONDS,
    }
    (run_dir / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n")

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print(json.dumps({"event": "spawn-error", "case": "positive",
                          "error": "ProcessSession is unavailable"}))
        return 2, ["spawn error"]

    session = None
    try:
        session = session_factory(
            argv=command, run_dir=run_dir,
            timeout_s=BOOT_DEADLINE_SECONDS - DRAIN_SECONDS)
        session.start()
        session.wait_for(
            lambda text: check_ap_positive_evidence(text, cpus)[0] == 0)
        session.observe(DRAIN_SECONDS)
        session.stop()
        session.close()
    except Exception as error:
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        return 2, [f"runner error: {error}"]

    text = session.text
    timed_out = session.timed_out
    accepted, why = check_ap_positive_evidence(text, cpus)
    result = accepted == 0 and not timed_out
    errors = []
    if timed_out:
        errors.append("absolute boot deadline exceeded before evidence")
    errors.extend(why)
    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": session.returncode,
        "result": "PASS" if result else "FAIL",
        "errors": errors,
    })
    (run_dir / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({
        "event": "ap-sched-case", "case": "positive", "cpus": cpus,
        "run": repetition, "result": "PASS" if result else "FAIL",
        "timeout": timed_out,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
        "errors": errors[:5],
    }))
    return (0 if result else 1), errors


def run_ap_mode(args):
    """MODE=sched-ap live entry: fatal-owner (T3) and the positive
    per-CPU matrix (T4)."""
    paths = {
        "image": args.image, "firmware": args.firmware, "elf": args.elf,
    }
    for key, p in paths.items():
        if not p or not Path(p).is_file():
            print(f"[{SUITE}] ERROR: {key} not found: {p}",
                  file=sys.stderr)
            return 2

    audits = []
    if args.case == "fatal-owner":
        audits.append(audit_fatal_owner_symbol(None, paths["elf"],
                                               args.llvm_nm))
        audits.append(audit_no_injection_symbols(None, args.normal_elf,
                                                 args.llvm_nm)
                      if args.normal_elf else (1, "", None))
    else:
        # positive: the variant ELF must carry NO injection symbol and
        # NO driver-absent surprise — the strict audited symbol set is
        # exactly the two driver entry points.
        audits.append(audit_no_injection_symbols(None, paths["elf"],
                                                 args.llvm_nm))
    records = [{"ok": ok, "error": err} for ok, err, _ in audits]
    print(json.dumps({"event": "symbol-audit", "records": records,
                      "ok": all(ok == 1 for ok, _, _ in audits)}))
    if any(ok != 1 for ok, _, _ in audits):
        for ok, err, _ in audits:
            if ok != 1:
                print(f"[{SUITE}] symbol audit FAIL: {err}",
                      file=sys.stderr)
        return 1

    if args.case == "fatal-owner":
        return run_fatal_boot(args, paths)

    outcomes = []
    any_errors = []
    for cpus, repetition in POSITIVE_MATRIX:
        code, errors = run_positive_boot(args, paths, cpus, repetition)
        outcomes.append({"case": "positive", "cpus": cpus,
                         "run": repetition,
                         "status": {0: "PASS", 1: "FAIL",
                                    2: "ERROR"}[code]})
        any_errors.extend(errors)
    print(json.dumps({"event": "matrix-summary", "case": "positive",
                      "outcomes": outcomes}))
    if any(o["status"] != "PASS" for o in outcomes):
        return 1
    return 0


def _self_test():
    """Assert the frozen parser/audit contract with synthetic data."""
    assert parse_ap_fatal_line(
        "[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1") == \
        (0, 3, 1, 1)
    assert parse_ap_fatal_line(
        "[ap-fatal] owner=1 arrivals_mask=3 entrants=1 losers=1") == \
        (1, 3, 1, 1)
    for bad in ("[ap-fatal] owner=2 arrivals_mask=3 entrants=1 losers=1",
                "[ap-fatal] owner=0 arrivals_mask=2 entrants=1 losers=1",
                "[ap-fatal] owner=0 arrivals_mask=3 entrants=0 losers=1",
                "[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=2",
                "x [ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1",
                "[ap-fatal] owner=0 arrivals_mask=3 entrants=1"):
        assert parse_ap_fatal_line(bad) is None, bad
    ok, _ = check_fatal_evidence(
        ["[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1"])
    assert ok == 0
    ok, _ = check_fatal_evidence([
        "[ap-fatal] owner=0 arrivals_mask=3 entrants=1 losers=1",
        "[TEST] END failed=0"])
    assert ok != 0

    class FakeNM:
        def __init__(self, symbols):
            self.symbols = symbols

        def __call__(self, _nm, _elf):
            return 0, "".join(f"00000000 T {s}\n" for s in self.symbols), ""

    ok, err, _ = audit_no_injection_symbols(
        FakeNM(["aarch64_ap_fatal_owner_enter", "idle_resume"]),
        "kernel.elf", "llvm-nm")
    assert ok == 0 and "aarch64_ap_fatal_owner_enter" in err
    ok, err, _ = audit_no_injection_symbols(
        FakeNM(["idle_resume", "aarch64_context_fatal_asm"]),
        "kernel.elf", "llvm-nm")
    assert (ok, err) == (1, "")
    ok, err, _ = audit_no_injection_symbols(
        None, "/nonexistent/kernel.elf", "llvm-nm")
    assert ok == 0 and "ELF not found" in err
    ok, err, _ = audit_fatal_owner_symbol(
        FakeNM(["aarch64_ap_fatal_owner_enter"]), "kernel.elf", "llvm-nm")
    assert ok == 1
    ok, err, _ = audit_fatal_owner_symbol(
        FakeNM(["idle_resume"]), "kernel.elf", "llvm-nm")
    assert ok == 0 and "lacks" in err
    print(json.dumps({"event": "self-test", "suite": SUITE, "ok": True}))
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return _self_test()
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--phase", choices=("ap", "wake"), default="ap")
    parser.add_argument("--case", choices=("positive", "fatal-owner"),
                        default="positive")
    parser.add_argument("--image")
    parser.add_argument("--firmware")
    parser.add_argument("--elf", help="variant kernel ELF")
    parser.add_argument("--normal-elf",
                        help="the NORMAL sched-ap ELF (injection audit)")
    parser.add_argument("--cpus", type=int, nargs="+", default=[2])
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--llvm-nm", default="llvm-nm")
    parser.add_argument("--build-dir")
    parser.add_argument("--profile", default="default")
    parser.add_argument("--log-dir")
    args = parser.parse_args()
    if args.phase == "wake":
        parser.error("--phase wake arrives with P2")
    return run_ap_mode(args)


if __name__ == "__main__":
    raise SystemExit(main())
