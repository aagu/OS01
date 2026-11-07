#!/usr/bin/env python3
"""aarch64 per-CPU tick evidence runner (per-CPU tick accounting, Task 3).

The runner boots the dedicated ``sched-tick`` selftest variant at every
SMP size of the frozen matrix (1/2/4/8, two repetitions each, 512 MiB,
one ABSOLUTE 120 s budget per boot) and accepts the boot ONLY when the
whole per-CPU evidence batch is present and consistent:

    [sched-tick] START cpus=N accepted_mask=M
    [sched-tick] case=<ID> cpu=<C> <field>=<value> ...
    [sched-tick] END failed=0

The seven mandatory late IDs are frozen: ``tick-ready``, ``tick-idle``,
``tick-busy``, ``tick-yield``, ``tick-deadline``, ``tick-global`` and
``tick-remote``.  Every ID carries exactly one row per CPU 0..N-1 (the
remote case is explicitly NA on a single-CPU boot).

Strictness rules (all of them are failures, never warnings):

  * the kernel-selftest v1 protocol must carry the early cases AND the
    seven late IDs, all PASS — the shared ``parse_v1`` parser is reused,
    no second, laxer PASS parser is written here;
  * the evidence batch must be published BEFORE the matching
    ``selftest_record_late`` records and the ``[sched-tick] END`` line
    must appear exactly once, AFTER the selftest ``[TEST] END``;
  * ``accepted_mask`` must equal ``(1 << N) - 1`` — the number of
    ONLINE CPUs is never accepted as a substitute for the admission
    mask;
  * a missing field, a negative value, an unknown field, a duplicate
    CPU row, an out-of-range CPU and a missing/duplicate/unknown case
    are all rejected;
  * the cross-field relations are checked exactly (``idle == samples``,
    ``busy == samples``, ``runtime == samples``, ``vruntime ==
    samples``, ``local_samples == busy + idle``, ``local_samples >= 1``
    on every CPU, on the BSP ``local_samples == global_ticks ==
    jiffies_delta`` — every BSP tick IS a global tick — and an AP's
    ``global_ticks == 0``);
  * the artifacts are audited by CONTENT, not by file name: the ELF
    must be an AArch64 ELF that carries the dedicated selftest symbols,
    the image/firmware/ELF must come from the SAME variant directory of
    the SAME profile build tree, and the dedicated symbols must not
    leak into the ordinary product ELFs.

A fake QEMU that keeps printing ticks can never extend the boot
budget: the deadline is absolute and the drain is inside it.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb  # noqa: E402

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


SUITE = "aarch64-sched-tick"

# ── the frozen protocol ───────────────────────────────────────────────

#: The seven mandatory late IDs, in registration order.
TICK_IDS = (
    "tick-ready",
    "tick-idle",
    "tick-busy",
    "tick-yield",
    "tick-deadline",
    "tick-global",
    "tick-remote",
)

#: The early cases of the kernel-selftest v1 protocol on aarch64.
EARLY_IDS = (
    "slab_alloc_free",
    "slab_many_sizes",
    "slab_16_caches",
    "aarch64_pt_vmm",
    "spinlock_basic",
)

#: The complete manifest the tick variant must publish.
EXPECTED_IDS = tuple(EARLY_IDS) + TICK_IDS
EXPECTED_TOTAL = len(EXPECTED_IDS)          # 12

START_RE = re.compile(r"^\[sched-tick\] START cpus=(\d+) accepted_mask=(\d+)$")
CASE_RE = re.compile(r"^\[sched-tick\] case=([a-z0-9-]+) cpu=(\d+)(?:\s+(.*))?$")
END_RE = re.compile(r"^\[sched-tick\] END failed=(\d+)$")
TEST_LINE_RE = re.compile(r"^\[TEST\] (START|SELECT|BEGIN|PASS|FAIL|SKIP|END)\b(.*)$")
ANY_FATAL_RE = re.compile(
    r"(\bFATAL\b|\bPANIC\b|\bOops\b|context fatal|context reject|"
    r"kernel panic|\[sched-tick\] FATAL)", re.I)

#: The dedicated selftest symbols the tick variant MUST carry.
TICK_SYMBOLS = (
    "test_aarch64_sched_tick_run",
    "aarch64_sched_tick_observe",
    "aarch64_sched_tick_irq_probe",
)

#: ELF e_machine value of AArch64.
EM_AARCH64 = 183


def _bounds(lo, hi):
    return (lo, hi)


#: Frozen field schema per case id: name -> (min, max).  ``min == max``
#: is an exact equality, ``None`` as the maximum is "unbounded above".
#: The cross-field relations are checked separately (they cannot be
#: expressed as a per-field range).
TICK_FIELD_SCHEMA = {
    "tick-ready": {
        "started": _bounds(1, 1),
        "valid": _bounds(1, 1),
        "hz": _bounds(100, 100),
    },
    "tick-idle": {
        "samples": _bounds(3, None),
        "idle": _bounds(0, None),
        "busy": _bounds(0, 0),
        "runtime": _bounds(0, 0),
        "vruntime": _bounds(0, 0),
    },
    "tick-busy": {
        "samples": _bounds(7, None),
        "busy": _bounds(0, None),
        "idle": _bounds(0, 0),
        "runtime": _bounds(0, None),
        "vruntime": _bounds(0, None),
    },
    "tick-yield": {
        "calls": _bounds(16, 16),
        "samples": _bounds(0, 0),
        "runtime": _bounds(0, 0),
        "vruntime": _bounds(0, 0),
        "self_switches": _bounds(0, 0),
    },
    "tick-deadline": {
        "need": _bounds(1, 1),
        "irq_switches": _bounds(0, 0),
        "nested": _bounds(0, 0),
        "regs_ok": _bounds(1, 1),
    },
    # The BSP additionally publishes the jiffies delta of the same
    # IRQ-masked window; an AP must not (and must not carry a global
    # tick count of its own).  The window must contain at least ONE
    # local sample — an empty window (0 == 0 + 0) proves nothing.
    "tick-global": {
        "local_samples": _bounds(1, None),
        "busy": _bounds(0, None),
        "idle": _bounds(0, None),
        "global_ticks": _bounds(0, None),
    },
    "tick-remote": {
        "sgi_ok": _bounds(1, 1),
        "tlb_ok": _bounds(1, 1),
        "wfi_enter": _bounds(1, None),
        "timer_live": _bounds(1, 1),
        "na": _bounds(0, 0),
    },
}

#: The BSP's global row carries the jiffies delta on top.
GLOBAL_CPU0_EXTRA = {"jiffies_delta": _bounds(1, None)}

#: The single-CPU boot: the remote case is explicitly NOT APPLICABLE.
REMOTE_NA_SCHEMA = {
    "sgi_ok": _bounds(0, 0),
    "tlb_ok": _bounds(0, 0),
    "wfi_enter": _bounds(0, 0),
    "timer_live": _bounds(0, 0),
    "na": _bounds(1, 1),
}

#: The frozen matrix: SMP 1/2/4/8, two repetitions each.
MATRIX = [(cpus, rep) for cpus in (1, 2, 4, 8) for rep in (1, 2)]

#: One boot: absolute budget (the 1 s drain lives inside it).
BOOT_DEADLINE_SECONDS = 120.0
DRAIN_SECONDS = 1.0
MEMORY_MIB = 512


# ── the strict evidence parser ───────────────────────────────────────

def _parse_kv(rest):
    """Parse ``k=v`` tokens.  A non ``k=v`` token, a repeated key or an
    empty value is an error (the caller reports it)."""
    kv = {}
    errors = []
    for tok in rest.split():
        if "=" not in tok:
            errors.append(f"non key=value token {tok!r}")
            continue
        key, _, value = tok.partition("=")
        if not key:
            errors.append(f"empty field name in {tok!r}")
            continue
        if key in kv:
            errors.append(f"duplicate field {key!r}")
            continue
        if not value:
            errors.append(f"field {key!r} has an empty value")
            continue
        kv[key] = value
    return kv, errors


def _check_fields(where, kv, schema, errors):
    """Range-check every frozen field and reject unknown ones."""
    values = {}
    for name, (lo, hi) in schema.items():
        if name not in kv:
            errors.append(f"{where}: missing field {name}")
            continue
        raw = kv[name]
        if raw.startswith("-"):
            errors.append(f"{where}: field {name}={raw!r} is negative")
            continue
        if not raw.isdigit():
            errors.append(f"{where}: field {name}={raw!r} is not decimal")
            continue
        value = int(raw)
        if value < lo or (hi is not None and value > hi):
            bound = f">={lo}" if hi is None else f"in [{lo},{hi}]"
            errors.append(f"{where}: field {name}={value} violates {bound}")
            continue
        values[name] = value
    for name in kv:
        if name not in schema:
            errors.append(f"{where}: unknown field {name}")
    return values


def _check_relations(case, cpu, values, errors):
    """The cross-field relations of one row."""
    where = f"case {case} cpu={cpu}"

    def need(*names):
        return all(name in values for name in names)

    if case == "tick-idle":
        if need("samples", "idle") and values["idle"] != values["samples"]:
            errors.append(f"{where}: idle={values['idle']} != "
                          f"samples={values['samples']}")
        if need("busy") and values["busy"] != 0:
            errors.append(f"{where}: busy={values['busy']} != 0")
        if need("runtime") and values["runtime"] != 0:
            errors.append(f"{where}: runtime={values['runtime']} != 0")
        if need("vruntime") and values["vruntime"] != 0:
            errors.append(f"{where}: vruntime={values['vruntime']} != 0")
    elif case == "tick-busy":
        if need("samples", "busy") and values["busy"] != values["samples"]:
            errors.append(f"{where}: busy={values['busy']} != "
                          f"samples={values['samples']}")
        if need("idle") and values["idle"] != 0:
            errors.append(f"{where}: idle={values['idle']} != 0")
        if need("samples", "runtime") and \
                values["runtime"] != values["samples"]:
            errors.append(f"{where}: runtime={values['runtime']} != "
                          f"samples={values['samples']}")
        if need("samples", "vruntime") and \
                values["vruntime"] != values["samples"]:
            errors.append(f"{where}: vruntime={values['vruntime']} != "
                          f"samples={values['samples']}")
    elif case == "tick-global":
        if need("local_samples", "busy", "idle") and \
                values["local_samples"] != values["busy"] + values["idle"]:
            errors.append(f"{where}: local_samples={values['local_samples']} "
                          f"!= busy+idle={values['busy'] + values['idle']}")
        if cpu == 0:
            # Every BSP tick IS a global tick: the local samples of the
            # window, the global tick count and the jiffies delta are
            # read in the SAME interrupt-masked window and must be
            # equal.  A BSP that published a local sample without
            # advancing the global time base (or the other way round)
            # would break the tie between the two time bases.
            if need("local_samples", "global_ticks") and \
                    values["local_samples"] != values["global_ticks"]:
                errors.append(f"{where}: local_samples="
                              f"{values['local_samples']} != "
                              f"global_ticks={values['global_ticks']}")
            if need("global_ticks", "jiffies_delta") and \
                    values["jiffies_delta"] != values["global_ticks"]:
                errors.append(f"{where}: jiffies_delta="
                              f"{values['jiffies_delta']} != "
                              f"global_ticks={values['global_ticks']}")
        elif need("global_ticks") and values["global_ticks"] != 0:
            errors.append(f"{where}: global_ticks={values['global_ticks']} "
                          "!= 0 (an AP never advances the global time base)")


def parse_sched_tick_evidence(text, cpus, manifest=None):
    """Strict per-CPU tick evidence gate.

    Returns ``(ok, errors)``.  ``cpus`` is the QEMU ``-smp`` count of
    this boot; ``manifest`` overrides the expected selftest manifest
    (used by the unit tests).
    """
    errors = []
    text = text.replace("\r", "")
    lines = text.splitlines()
    manifest = tuple(manifest or EXPECTED_IDS)

    # 1. The kernel-selftest v1 protocol: the early cases AND the seven
    #    mandatory late IDs, all PASS.  The shared parser is reused.
    if parse_v1 is None:
        return False, ["parse_v1 is unavailable"]
    pr = parse_v1(text, suite="kernel-selftest", allowed_ids=set(manifest))
    errors.extend(pr.errors)
    if pr.failed or pr.skipped:
        errors.append(f"failed={pr.failed} skipped={pr.skipped} must both be 0")
    for label, observed in (("selected", pr.selected),
                            ("started", pr.started),
                            ("terminal", set(pr.terminal))):
        if observed != set(manifest):
            errors.append(
                f"{label} set != manifest (missing="
                f"{sorted(set(manifest) - observed)} extra="
                f"{sorted(observed - set(manifest))})")
    for cid in TICK_IDS:
        if cid not in pr.terminal:
            errors.append(f"mandatory late case {cid} never reached a "
                          "terminal record")

    # 2. Any fatal / panic line anywhere is a failure.
    fatal_hits = [ln for ln in lines if ANY_FATAL_RE.search(ln)]
    if fatal_hits:
        errors.append(f"fatal/diagnostic failure line(s): {fatal_hits[:3]}")

    # 3. The [sched-tick] envelope.
    starts = [(idx, m) for idx, line in enumerate(lines)
              for m in [START_RE.match(line)] if m]
    ends = [(idx, m) for idx, line in enumerate(lines)
            for m in [END_RE.match(line)] if m]
    if len(starts) != 1:
        errors.append(f"expected exactly one [sched-tick] START, saw "
                      f"{len(starts)}")
    if len(ends) != 1:
        errors.append(f"expected exactly one [sched-tick] END, saw "
                      f"{len(ends)}")

    idx_start = None
    if len(starts) == 1:
        idx_start, start_match = starts[0]
        declared_cpus = int(start_match.group(1))
        accepted_mask = int(start_match.group(2))
        if declared_cpus != cpus:
            errors.append(f"[sched-tick] START cpus={declared_cpus} != "
                          f"-smp {cpus}")
        if accepted_mask != (1 << cpus) - 1:
            errors.append(f"[sched-tick] START accepted_mask={accepted_mask} "
                          f"!= (1<<{cpus})-1 (the online CPU count is never "
                          "accepted as the admission mask)")

    # 4. The case rows: exactly one per (ID, CPU).
    rows = {}          # (case, cpu) -> (idx, kv)
    order = []
    for idx, line in enumerate(lines):
        m = CASE_RE.match(line)
        if not m:
            continue
        case, cpu_text, rest = m.group(1), m.group(2), m.group(3) or ""
        if case not in TICK_IDS:
            errors.append(f"unknown [sched-tick] case id {case!r}")
            continue
        if not cpu_text.isdigit():
            errors.append(f"case {case}: cpu={cpu_text!r} is not decimal")
            continue
        cpu = int(cpu_text)
        if cpu >= cpus:
            errors.append(f"case {case}: cpu={cpu} is out of range for "
                          f"-smp {cpus}")
            continue
        key = (case, cpu)
        if key in rows:
            errors.append(f"duplicate case row {case} cpu={cpu}")
            continue
        kv, kv_errors = _parse_kv(rest)
        for err in kv_errors:
            errors.append(f"case {case} cpu={cpu}: {err}")
        rows[key] = (idx, kv)
        order.append(key)

    # 5. Every ID must have exactly one row per CPU 0..N-1.
    for case in TICK_IDS:
        present = sorted(cpu for (cid, cpu) in rows if cid == case)
        expected_cpus = list(range(cpus))
        if present != expected_cpus:
            errors.append(f"case {case}: CPU rows {present} != "
                          f"{expected_cpus} (exactly one row per CPU)")

    # 6. The per-row field schema and the cross-field relations.
    for case in TICK_IDS:
        for cpu in range(cpus):
            key = (case, cpu)
            if key not in rows:
                continue
            idx, kv = rows[key]
            where = f"case {case} cpu={cpu}"
            schema = dict(TICK_FIELD_SCHEMA[case])
            if case == "tick-global" and cpu == 0:
                schema.update(GLOBAL_CPU0_EXTRA)
            if case == "tick-remote" and cpus == 1:
                schema = dict(REMOTE_NA_SCHEMA)
            values = _check_fields(where, kv, schema, errors)
            _check_relations(case, cpu, values, errors)

    # 7. Placement: every evidence row is published BEFORE the matching
    #    selftest_record_late() records (the BEGIN line), and the END
    #    line comes after the selftest END.
    begin_idx = {}
    terminal_idx = {}
    end_idx = None
    for idx, line in enumerate(lines):
        m = TEST_LINE_RE.match(line)
        if not m:
            continue
        cid = m.group(2).split()[0] if m.group(2).strip() else ""
        if m.group(1) == "BEGIN":
            begin_idx.setdefault(cid, idx)
        elif m.group(1) in ("PASS", "FAIL", "SKIP"):
            terminal_idx.setdefault(cid, idx)
        elif m.group(1) == "END" and end_idx is None:
            end_idx = idx
    for case in TICK_IDS:
        for cpu in range(cpus):
            key = (case, cpu)
            if key not in rows:
                continue
            idx = rows[key][0]
            begin = begin_idx.get(case)
            if begin is not None and idx > begin:
                errors.append(f"case {case} cpu={cpu}: the evidence row was "
                              "published AFTER its selftest_record_late() "
                              "record")
            if idx_start is not None and idx < idx_start:
                errors.append(f"case {case} cpu={cpu}: the evidence row "
                              "precedes the [sched-tick] START line")
    if len(ends) == 1:
        idx_end, end_match = ends[0]
        if int(end_match.group(1)) != 0:
            errors.append(f"[sched-tick] END failed={end_match.group(1)} != 0")
        if end_idx is None:
            errors.append("the selftest [TEST] END line is missing")
        elif idx_end < end_idx:
            errors.append("[sched-tick] END appears BEFORE the selftest END")
        if idx_start is not None and idx_end < idx_start:
            errors.append("[sched-tick] END appears before the START line")

    return (not errors), errors


# ── the artifact audit (content, not file name) ─────────────────────

def _elf_machine(path):
    """Read the ELF header's e_machine (None when the file is not an
    ELF64 little-endian object)."""
    try:
        with open(path, "rb") as stream:
            head = stream.read(20)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return None
    if head[4] != 2:      # EI_CLASS: ELFCLASS64
        return None
    return int.from_bytes(head[18:20], "little")


def _nm_symbols(llvm_nm, elf):
    """The symbol names of one ELF.  Returns (set, error)."""
    try:
        proc = subprocess.run([llvm_nm, str(elf)], check=True,
                              capture_output=True, text=True)
    except FileNotFoundError:
        return set(), f"nm tool not found: {llvm_nm}"
    except subprocess.CalledProcessError as exc:
        return set(), (f"nm failed on {elf}: rc={exc.returncode} "
                       f"{exc.stderr[-200:]}")
    names = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            names.add(parts[-1])
    return names, None


def audit_artifacts(llvm_nm, build_dir, profile, image, firmware, elf):
    """Audit the artifacts by CONTENT and provenance.

    Returns ``(ok, errors, records)``.  The checks are:

      * the ELF exists and is an AArch64 ELF64 object;
      * the ELF carries every dedicated selftest symbol;
      * the ELF lives in ``<build>/kernel/<variant>/`` and the image and
        firmware in ``<build>/image/<variant>/`` — the SAME variant of
        the SAME profile tree;
      * the build directory's name is the requested profile;
      * the dedicated symbols do NOT leak into the ordinary product
        ELFs (when they exist).
    """
    errors = []
    records = []
    elf_path = Path(elf)
    image_path = Path(image)
    firmware_path = Path(firmware)
    build = Path(build_dir) if build_dir else None

    for label, path in (("image", image_path), ("firmware", firmware_path),
                        ("elf", elf_path)):
        if not path.is_file():
            errors.append(f"{label} not found: {path}")
    if errors:
        records.append({"elf": str(elf_path), "ok": False, "errors": errors})
        return False, errors, records

    machine = _elf_machine(elf_path)
    if machine != EM_AARCH64:
        errors.append(f"{elf_path}: ELF machine={machine} != AArch64 "
                      f"({EM_AARCH64})")
    names, nm_error = _nm_symbols(llvm_nm, elf_path)
    if nm_error:
        errors.append(nm_error)
    else:
        for symbol in TICK_SYMBOLS:
            if symbol not in names:
                errors.append(f"{elf_path}: the dedicated selftest symbol "
                              f"{symbol} is absent (wrong variant?)")

    variant = None
    if build is not None:
        build = build.resolve()
        if build.name != profile:
            errors.append(f"build directory {build} does not match the "
                          f"profile {profile!r}")
        kernel_dir = elf_path.resolve().parent.parent
        if kernel_dir.parent != build or kernel_dir.name != "kernel":
            errors.append(f"{elf_path}: the ELF must live in "
                          "<build>/kernel/<variant>/")
        else:
            variant = elf_path.resolve().parent.name
            image_dir = image_path.resolve().parent
            firmware_dir = firmware_path.resolve().parent
            if image_dir != build / "image" / variant:
                errors.append(f"{image_path}: the image must live in "
                              f"<build>/image/{variant}/")
            if firmware_dir != image_dir and firmware_dir != build / "firmware":
                errors.append(f"{firmware_path}: the firmware must live "
                              "under firmware/ or beside the image")
    records.append({"elf": str(elf_path), "variant": variant,
                    "profile": profile, "machine": machine,
                    "symbols": sorted(set(TICK_SYMBOLS) & names),
                    "ok": not errors, "errors": list(errors)})

    # The dedicated symbols must not leak into the ordinary product.
    if build is not None:
        for relative in (("kernel", "kernel.elf"),
                         ("kernel", "selftest", "kernel.elf")):
            ordinary = build.joinpath(*relative)
            if not ordinary.is_file():
                continue
            ordinary_names, ordinary_error = _nm_symbols(llvm_nm, ordinary)
            if ordinary_error:
                errors.append(ordinary_error)
                continue
            leaked = sorted(set(TICK_SYMBOLS) & ordinary_names)
            if leaked:
                errors.append(f"{ordinary}: the dedicated selftest symbol(s) "
                              f"{leaked} leaked into an ordinary product ELF")
            records.append({"elf": str(ordinary), "leaked": leaked,
                            "ok": not leaked})
    return (not errors), errors, records


# ── the live runner ──────────────────────────────────────────────────

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


def qemu_command(qemu, cpus, image, firmware, dtb):
    """The frozen QEMU command: virt/gicv2, 512 MiB, the diagnostic
    DTB, the serial console on stdout."""
    return [qemu, "-M", "virt,gic-version=2,acpi=off", "-cpu", "cortex-a53",
            "-smp", str(cpus), "-m", str(MEMORY_MIB),
            "-drive", f"if=pflash,format=raw,file={firmware}",
            "-drive", f"if=none,file={image},format=raw,readonly=on,id=disk",
            "-device", "virtio-blk-device,drive=disk", "-serial", "stdio",
            "-display", "none", "-no-reboot", "-no-shutdown", "-dtb", dtb]


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
            memory_mib=MEMORY_MIB,
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
    except Exception as exc:  # the evidence must never mask the result
        print(f"[{SUITE}] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


def run_boot(args, cpus, repetition, deadline_s=None, dtb_factory=None):
    """One QEMU boot of the tick variant.

    Returns the plan exit code (0 PASS / 1 FAIL / 2 ERROR).
    ``dtb_factory`` overrides the diagnostic-DTB generator (the unit
    tests inject a fake one; the live runner never does)."""
    started = time.monotonic()
    archive = None
    if args.build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir) / f"cpus-{cpus}-run-{repetition}"
    run_dir.mkdir(parents=True, exist_ok=True)

    budget = float(deadline_s if deadline_s is not None
                   else BOOT_DEADLINE_SECONDS)
    drain = min(DRAIN_SECONDS, budget / 4.0)

    dtb = (dtb_factory or generate_diagnostic_dtb)(args.qemu, str(run_dir),
                                                    cpus)
    command = qemu_command(args.qemu, cpus, args.image, args.firmware, dtb)
    metadata = {
        "command": command,
        "cpus": cpus,
        "run": repetition,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": _sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": _sha256(args.image),
        "elf": str(Path(args.elf).resolve()),
        "elf_sha256": _sha256(args.elf),
        "deadline_s": budget,
    }
    (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")

    if ProcessSession is None:
        errors = ["spawn error: ProcessSession is unavailable"]
        print(json.dumps({"event": "spawn-error", "cpus": cpus,
                          "run": repetition, "error": errors[0]}))
        _write_report(archive, argv=command, profile=args.profile, cpu=cpus,
                      variant="sched-tick", firmware=args.firmware,
                      image=args.image, started=started, status="ERROR",
                      runner_exit_code=2, session=None, errors=errors)
        return 2

    session = None
    child_exited_before_runner_stop = False
    try:
        # ABSOLUTE deadline: the evidence window ends before the budget
        # so the drain stays inside it.  A child that keeps printing
        # ticks can never extend the budget.
        session = ProcessSession(argv=command, run_dir=run_dir,
                                 timeout_s=budget - drain)
        session.start()
        session.wait_for(lambda text: parse_sched_tick_evidence(text, cpus)[0])
        session.observe(drain)
        child_exited_before_runner_stop = session.returncode is not None
        session.stop()
        child_exited_before_runner_stop |= not session.stopped_by_runner
        session.close()
    except Exception as error:  # launch / OS error -> ERROR, never PASS
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        print(json.dumps({"event": "spawn-error", "cpus": cpus,
                          "run": repetition, "error": str(error)}))
        _write_report(archive, argv=command, profile=args.profile, cpu=cpus,
                      variant="sched-tick", firmware=args.firmware,
                      image=args.image, started=started, status="ERROR",
                      runner_exit_code=2, session=session,
                      errors=[f"spawn error: {error}"])
        return 2

    text = session.text
    timed_out = session.timed_out
    accepted, why = parse_sched_tick_evidence(text, cpus)
    result = accepted and not timed_out and not child_exited_before_runner_stop

    errors = []
    if timed_out:
        errors.append("the absolute boot deadline expired before the "
                      "evidence was complete")
    if child_exited_before_runner_stop:
        errors.append("QEMU exited before the runner completed the "
                      "observation drain")
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
                  variant="sched-tick", firmware=args.firmware,
                  image=args.image, started=started,
                  status="PASS" if result else "FAIL",
                  runner_exit_code=0 if result else 1, session=session,
                  errors=errors,
                  extra={"timeout": timed_out,
                         "child_exited_early": child_exited_before_runner_stop})
    print(json.dumps({
        "event": "sched-tick-case", "cpus": cpus, "run": repetition,
        "result": "PASS" if result else "FAIL", "timeout": timed_out,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
        "errors": errors[:5],
    }))
    return 0 if result else 1


def run_matrix(args, deadline_s=None, dtb_factory=None):
    """The frozen matrix: every entry runs, even after a failure."""
    outcomes = []
    for cpus, repetition in MATRIX:
        try:
            code = run_boot(args, cpus, repetition, deadline_s=deadline_s,
                            dtb_factory=dtb_factory)
        except Exception as error:
            code = 2
            print(json.dumps({"event": "runner-error", "cpus": cpus,
                              "run": repetition, "error": str(error)}),
                  file=sys.stderr)
        outcomes.append({"cpus": cpus, "run": repetition,
                         "status": {0: "PASS", 1: "FAIL", 2: "ERROR"}[code]})
    print(json.dumps({"event": "matrix-summary", "outcomes": outcomes}))
    return 0 if all(o["status"] == "PASS" for o in outcomes) else 1


# ── the pure self-test (no QEMU) ─────────────────────────────────────

def _fixture(cpus=2, **overrides):
    """A complete, valid evidence trace for ``cpus`` CPUs."""
    lines = ["[TEST] START v=1 suite=kernel-selftest "
             f"expected={len(EXPECTED_IDS)}"]
    lines += [f"[TEST] SELECT {cid} required=1" for cid in EXPECTED_IDS]
    lines.append(f"[sched-tick] START cpus={cpus} "
                 f"accepted_mask={(1 << cpus) - 1}")
    for case in TICK_IDS:
        for cpu in range(cpus):
            if case == "tick-ready":
                fields = "started=1 valid=1 hz=100"
            elif case == "tick-idle":
                fields = "samples=3 idle=3 busy=0 runtime=0 vruntime=0"
            elif case == "tick-busy":
                fields = "samples=7 busy=7 idle=0 runtime=7 vruntime=7"
            elif case == "tick-yield":
                fields = ("calls=16 samples=0 runtime=0 vruntime=0 "
                          "self_switches=0")
            elif case == "tick-deadline":
                fields = "need=1 irq_switches=0 nested=0 regs_ok=1"
            elif case == "tick-global":
                # Only the BSP advances the global time base, and every
                # BSP tick IS a global tick: on the BSP the local
                # samples, the global ticks and the jiffies delta of
                # the window are equal.
                fields = ("local_samples=10 busy=7 idle=3 global_ticks="
                          + ("10" if cpu == 0 else "0"))
                if cpu == 0:
                    fields += " jiffies_delta=10"
            else:
                fields = ("sgi_ok=1 tlb_ok=1 wfi_enter=3 timer_live=1 "
                          "na=0")
                if cpus == 1:
                    fields = ("sgi_ok=0 tlb_ok=0 wfi_enter=0 timer_live=0 "
                              "na=1")
            lines.append(f"[sched-tick] case={case} cpu={cpu} {fields}")
    for cid in EXPECTED_IDS:
        lines.append(f"[TEST] BEGIN {cid}")
        lines.append(f"[TEST] PASS {cid}")
    lines.append(f"[TEST] END suite=kernel-selftest total={len(EXPECTED_IDS)} "
                 f"passed={len(EXPECTED_IDS)} failed=0 skipped=0")
    lines.append("[sched-tick] END failed=0")
    text = "\n".join(lines) + "\n"
    for old, new in overrides.items():
        text = text.replace(old, new)
    return text


def self_test():
    for cpus in (1, 2, 4, 8):
        ok, errors = parse_sched_tick_evidence(_fixture(cpus), cpus)
        assert ok, (cpus, errors)

    def bad(text, cpus=2):
        ok, errors = parse_sched_tick_evidence(text, cpus=cpus)
        assert not ok, errors or "expected rejection"

    base = _fixture(2)
    # A dropped AP row.
    bad(base.replace("[sched-tick] case=tick-busy cpu=1 samples=7 busy=7 "
                     "idle=0 runtime=7 vruntime=7\n", ""))
    # A duplicated CPU row.
    bad(base.replace(
        "[sched-tick] case=tick-busy cpu=1 samples=7 busy=7 idle=0 "
        "runtime=7 vruntime=7\n",
        "[sched-tick] case=tick-busy cpu=1 samples=7 busy=7 idle=0 "
        "runtime=7 vruntime=7\n"
        "[sched-tick] case=tick-busy cpu=1 samples=8 busy=8 idle=0 "
        "runtime=8 vruntime=8\n"))
    # An out-of-range CPU.
    bad(base.replace("case=tick-busy cpu=1", "case=tick-busy cpu=7"))
    # An AP that claims a global tick.
    bad(base.replace("case=tick-global cpu=1 local_samples=10 busy=7 idle=3 "
                     "global_ticks=0",
                     "case=tick-global cpu=1 local_samples=10 busy=7 idle=3 "
                     "global_ticks=1"))
    # An EMPTY global window: 0 == 0 + 0 must not pass.
    bad(base.replace("case=tick-global cpu=1 local_samples=10 busy=7 idle=3 "
                     "global_ticks=0",
                     "case=tick-global cpu=1 local_samples=0 busy=0 idle=0 "
                     "global_ticks=0"))
    # A BSP whose local samples do not match its global ticks.
    bad(base.replace("case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
                     "global_ticks=10 jiffies_delta=10",
                     "case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
                     "global_ticks=4 jiffies_delta=4"))
    # A BSP whose jiffies delta does not match its global ticks.
    bad(base.replace("case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
                     "global_ticks=10 jiffies_delta=10",
                     "case=tick-global cpu=0 local_samples=10 busy=7 idle=3 "
                     "global_ticks=10 jiffies_delta=9"))
    # busy and runtime disagree.
    bad(base.replace("case=tick-busy cpu=0 samples=7 busy=7 idle=0 "
                     "runtime=7 vruntime=7",
                     "case=tick-busy cpu=0 samples=7 busy=7 idle=0 "
                     "runtime=8 vruntime=7"))
    # A switch inside the deadline IRQ window.
    bad(base.replace("case=tick-deadline cpu=0 need=1 irq_switches=0",
                     "case=tick-deadline cpu=0 need=1 irq_switches=1"))
    # A missing END, an unknown ID and a duplicated case.
    bad(base.replace("[sched-tick] END failed=0\n", ""))
    bad(base.replace("[sched-tick] case=tick-ready cpu=0",
                     "[sched-tick] case=tick-ghost cpu=0"))
    bad(base + "[sched-tick] case=tick-ready cpu=0 started=1 valid=1 hz=100\n")
    # The wrong SMP count / a wrong admission mask.
    bad(base.replace("accepted_mask=3", "accepted_mask=1"))
    bad(base.replace("accepted_mask=3", "accepted_mask=7"))
    bad(base.replace("[sched-tick] START cpus=2", "[sched-tick] START cpus=4"))
    # A missing field, an unknown field and a negative value.
    bad(base.replace(" started=1 valid=1 hz=100", " valid=1 hz=100"))
    bad(base.replace(" started=1 valid=1 hz=100",
                     " started=1 valid=1 hz=100 invented=1"))
    bad(base.replace(" started=1 valid=1 hz=100", " started=1 valid=1 hz=-100"))
    # The evidence published after its own record_late().
    swapped = base.replace(
        "[sched-tick] case=tick-ready cpu=0 started=1 valid=1 hz=100\n", "")
    swapped = swapped.replace(
        "[TEST] PASS tick-ready\n",
        "[TEST] PASS tick-ready\n"
        "[sched-tick] case=tick-ready cpu=0 started=1 valid=1 hz=100\n")
    bad(swapped)
    # The END before the selftest END.
    bad(base.replace("[TEST] END suite=kernel-selftest",
                     "[sched-tick] END failed=0\n[TEST] END suite=kernel-selftest")
        .replace("[sched-tick] END failed=0\n[sched-tick] END failed=0\n",
                 "[sched-tick] END failed=0\n"))
    # A fatal line and a failed late case.
    bad(base + "[aarch64] context fatal code=1 sp=0x1 base=0x2 top=0x3\n")
    bad(base.replace("[TEST] PASS tick-busy", "[TEST] FAIL tick-busy reason=x")
        .replace(f"passed={len(EXPECTED_IDS)} failed=0",
                 f"passed={len(EXPECTED_IDS) - 1} failed=1"))
    # Nothing at all.
    bad("")
    print("aarch64_sched_tick: self-test passed")


# ── CLI ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--image")
    parser.add_argument("--firmware")
    parser.add_argument("--elf")
    parser.add_argument("--qemu", default=os.environ.get("QEMU_BIN",
                                                         "qemu-system-aarch64"))
    parser.add_argument("--llvm-nm", default=os.environ.get("AARCH64_NM",
                                                            "llvm-nm"))
    parser.add_argument("--build-dir", default=None)
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                            "default"))
    parser.add_argument("--log-dir")
    parser.add_argument("--cpus", type=int, nargs="+", default=None,
                        help="override the frozen matrix (testing only)")
    parser.add_argument("--timeout", type=float, default=BOOT_DEADLINE_SECONDS,
                        help="the absolute per-boot budget in seconds")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0

    for name in ("image", "firmware", "elf"):
        if not getattr(args, name):
            parser.error(f"--{name} is required")
    for name in ("image", "firmware", "elf"):
        if not Path(getattr(args, name)).is_file():
            parser.error(f"--{name} must name an existing file: "
                         f"{getattr(args, name)}")
    if not args.log_dir and not args.build_dir:
        parser.error("--log-dir and/or --build-dir is required")
    if args.timeout <= 0 or args.timeout > BOOT_DEADLINE_SECONDS:
        parser.error(f"--timeout must be in (0, {BOOT_DEADLINE_SECONDS}]")

    ok, errors, records = audit_artifacts(
        args.llvm_nm, args.build_dir, args.profile, args.image, args.firmware,
        args.elf)
    print(json.dumps({"event": "artifact-audit", "records": records,
                      "ok": ok}))
    if not ok:
        for err in errors:
            print(f"[{SUITE}] artifact audit FAIL: {err}", file=sys.stderr)
        return 1

    if args.cpus:
        global MATRIX
        MATRIX = [(c, rep) for c in args.cpus for rep in (1, 2)]
    return run_matrix(args, deadline_s=args.timeout)


if __name__ == "__main__":
    sys.exit(main())
