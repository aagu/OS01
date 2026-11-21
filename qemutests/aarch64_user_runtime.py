#!/usr/bin/env python3
"""AArch64 formal EL0 user-runtime acceptance runner.

Boots the dedicated ``user-runtime-<case>`` selftest variant under a
120 s absolute deadline using ``ProcessSession`` and ``RunArchive``,
consuming only explicit paths passed by ``make``.

Timeout, fatal/panic lines, premature QEMU exit, or missing/duplicate
required evidence all fail.
"""

import argparse
import hashlib
import json
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

try:  # noqa: E402
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport, parse_v1
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]
    parse_v1 = None  # type: ignore[assignment]


SUITE = "aarch64-user-runtime"
EM_AARCH64 = 183
BOOT_DEADLINE_SECONDS = 120.0
DRAIN_SECONDS = 1.0
MEMORY_MB = 512

ANY_FATAL_RE = re.compile(
    r"(\bFATAL\b|\bPANIC\b|\bOops\b|context fatal|context reject|"
    r"kernel panic|\[user-runtime\] FAIL\b)",
    re.I,
)
IRQ_SAMPLES_RE = re.compile(
    r"\[user-runtime\] irq-samples eoi_a=(\d+) eoi_b=(\d+) "
    r"switch_resume_a=(\d+) switch_resume_b=(\d+)\b"
)
ASID_SAMPLES_RE = re.compile(
    r"\[user-runtime\] asid-samples asid_a=(\d+) asid_b=(\d+) "
    r"clean_switches=(\d+) full_flushes=(\d+) reused=(\d+)\b"
)
SMP_SAMPLES_RE = re.compile(
    r"\[user-runtime\] smp-samples child_cpu=(\d+) "
    r"cpu0_eoi=(\d+) cpu1_eoi=(\d+) cpu0_sw=(\d+) cpu1_sw=(\d+) "
    r"cow_last_free=(\d+) waitpid_blocked=(\d+) waitpid_exit=(\d+) "
    r"leaked_roots=(\d+) leaked_pages=(\d+)\b"
)
SMP_EXPLICIT_PIN_RE = re.compile(
    r"\[user-runtime\] explicit_pin pid=(\d+) cpu=(\d+) mask=(\d+)\b"
)
MIGRATION_EVENT_RE = re.compile(
    r"\[user-runtime\] migration-event seq=(\d+) kind=([a-z0-9_]+)(.*)"
)
PASS_SUMMARY_RE = {
    "irq": re.compile(r"\[user-runtime\] PASS case=irq cpu_mask=1\b"),
    "asid": re.compile(r"\[user-runtime\] PASS case=asid cpu_mask=1\b"),
    "smp": re.compile(r"\[user-runtime\] PASS case=smp cpu_mask=3\b"),
    "migration": re.compile(r"\[user-runtime\] PASS case=migration cpu_mask=3\b"),
}
SELFTEST_DONE_RE = re.compile(r"^\[selftest\] done \(failed=(\d+)\)$")

REQUIRED_SYMBOLS = ("test_aarch64_user_runtime_run",)
FORBIDDEN_PRODUCT_PREFIXES = (
    "aarch64_user_runtime_test_",
    "test_aarch64_user_runtime_",
    "test_aarch64_user_migration_",
    "aarch64_user_mig_",
    "aarch64_sched_tick_mig_",
    "aarch64_asid_test_",
)


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def audit_elf(elf_path, llvm_nm, case="irq"):
    """Verify AArch64 ELF header and required/forbidden test symbols."""
    errors = []
    path = Path(elf_path)
    if not path.is_file():
        return [f"missing ELF: {path}"]
    hdr = path.read_bytes()[:64]
    if len(hdr) < 20 or hdr[:4] != b"\x7fELF" or hdr[4] != 2 or hdr[5] != 1:
        return [f"not an ELF64 little-endian file: {path}"]
    e_machine = int.from_bytes(hdr[18:20], "little")
    if e_machine != EM_AARCH64:
        return [f"unexpected e_machine={e_machine}, expected {EM_AARCH64}"]
    proc = subprocess.run(
        [llvm_nm, str(path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        return [f"llvm-nm failed on {path}: {proc.stderr.strip()}"]
    syms = set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            syms.add(parts[-1])
    if case in ("product", "fp"):
        for sym in sorted(syms):
            if any(sym.startswith(pfx) for pfx in FORBIDDEN_PRODUCT_PREFIXES):
                errors.append(f"forbidden test symbol {sym} in normal product ELF {path}")
    else:
        for sym in REQUIRED_SYMBOLS:
            if sym not in syms:
                errors.append(f"missing required symbol {sym} in {path}")
    return errors


def _parse_kv(rest):
    out = {}
    for tok in rest.strip().split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def _as_int(val, default=None):
    if val is None:
        return default
    try:
        return int(val, 0)
    except ValueError:
        return default


def _validate_migration_events(lines):
    errors = []
    events = []
    pass_line_idx = None
    final_line_idx = None

    for idx, line in enumerate(lines):
        if PASS_SUMMARY_RE["migration"].search(line):
            pass_line_idx = idx
        m = MIGRATION_EVENT_RE.search(line)
        if not m:
            continue
        seq = int(m.group(1))
        kind = m.group(2)
        kv = _parse_kv(m.group(3))
        events.append((idx, seq, kind, kv))
        if kind == "final":
            final_line_idx = idx

    if not events:
        return ["missing [user-runtime] migration-event records"]

    for expected_seq, (_idx, seq, _kind, _kv) in enumerate(events, start=1):
        if seq != expected_seq:
            errors.append(f"migration-event seq mismatch: expected {expected_seq}, got {seq}")
            break

    if pass_line_idx is not None and final_line_idx is not None and pass_line_idx < final_line_idx:
        errors.append("PASS summary appeared before final baseline check")

    by_kind = {}
    for pos, (_idx, seq, kind, kv) in enumerate(events):
        by_kind.setdefault(kind, []).append((pos, seq, kv))

    for req_one in ("baseline", "final", "spawn", "parent_check", "cow_check",
                    "old_load", "remap", "new_load", "permission_abort",
                    "idle_zero", "wfi_enter", "idle_take",
                    "copy_ok", "copy_denied", "exec_commit", "exec_el0"):
        cnt = len(by_kind.get(req_one, []))
        if cnt != 1:
            errors.append(f"expected exactly 1 {req_one} event, got {cnt}")

    if by_kind.get("full_flush"):
        errors.append(f"unexpected full_flush in migration window ({len(by_kind['full_flush'])})")

    if errors:
        return errors

    base = by_kind["baseline"][0][2]
    fin = by_kind["final"][0][2]
    for f in ("roots", "pages", "asids"):
        if _as_int(base.get(f)) != _as_int(fin.get(f)):
            errors.append(f"baseline/final mismatch on {f}: {base.get(f)} vs {fin.get(f)}")

    sp = by_kind["spawn"][0][2]
    forks = by_kind.get("fork", [])
    if len(forks) != 2:
        errors.append(f"expected 2 fork events, got {len(forks)}")
        return errors

    sp_pid = _as_int(sp.get("pid"))
    f0 = forks[0][2]
    f1 = forks[1][2]
    main_pid = _as_int(f0.get("pid"))
    child2_pid = _as_int(f1.get("pid"))
    if len({sp_pid, main_pid, child2_pid}) != 3:
        errors.append("spawn and fork PIDs must be distinct")
    if (_as_int(sp.get("cpu")), _as_int(sp.get("mask"))) != (1, 3):
        errors.append(f"unexpected spawn placement: {sp}")
    if (_as_int(f0.get("cpu")), _as_int(f0.get("mask"))) != (0, 3):
        errors.append(f"unexpected fork[0] placement: {f0}")
    if (_as_int(f1.get("cpu")), _as_int(f1.get("mask"))) != (1, 3):
        errors.append(f"unexpected fork[1] placement: {f1}")

    move_order = [
        "irq_save", "hw_detach", "root_unpublish", "tail",
        "commit", "pick", "hw_install", "irq_resume", "el0_after",
    ]
    move_expect = {
        1: (main_pid, 0, 1),
        2: (main_pid, 1, 0),
        3: (child2_pid, 1, 0),
    }
    move_pos = {}
    for mid, (exp_pid, exp_src, exp_dst) in move_expect.items():
        positions = []
        root0 = None
        asid0 = None
        for ev_name in move_order:
            matches = [
                (pos, kv) for pos, _s, k, kv in
                [(p, s, kind, d) for p, s, kind, d in
                 [(i, s, k, d) for i, (_x, s, k, d) in enumerate(events)]]
                if k == ev_name and _as_int(kv.get("move_id")) == mid
            ]
            if len(matches) != 1:
                errors.append(f"move {mid} expected 1 {ev_name}, got {len(matches)}")
                return errors
            pos, kv = matches[0]
            if _as_int(kv.get("pid")) != exp_pid:
                errors.append(f"move {mid} {ev_name} wrong pid: {kv.get('pid')}")
            asid_val = _as_int(kv.get("asid"), 0)
            root_val = kv.get("root")
            if asid_val <= 0:
                errors.append(f"move {mid} {ev_name} requires nonzero asid, got {asid_val}")
            if root0 is None:
                root0, asid0 = root_val, asid_val
            elif root_val != root0 or asid_val != asid0:
                errors.append(f"move {mid} {ev_name} root/asid changed")
            if ev_name == "tail" and _as_int(kv.get("on_cpu")) != 0:
                errors.append(f"move {mid} tail must have on_cpu=0")
            if ev_name == "pick" and _as_int(kv.get("on_cpu")) != 1:
                errors.append(f"move {mid} pick must have on_cpu=1")
            if ev_name == "commit":
                if (_as_int(kv.get("src")), _as_int(kv.get("dst"))) != (exp_src, exp_dst):
                    errors.append(f"move {mid} wrong direction src={kv.get('src')} dst={kv.get('dst')}")
            if ev_name == "irq_resume" and _as_int(kv.get("match")) != 1:
                errors.append(f"move {mid} irq_resume frame/TLS match != 1")
            positions.append(pos)
        if positions != sorted(positions):
            errors.append(f"move {mid} events out of order: {positions}")
        move_pos[mid] = dict(zip(move_order, positions))

    # Move 1 idle window checks
    iz_pos, _, iz = by_kind["idle_zero"][0]
    wfi_pos, _, wfi = by_kind["wfi_enter"][0]
    take_pos, _, take = by_kind["idle_take"][0]
    ticks = by_kind.get("idle_tick", [])
    checks = by_kind.get("idle_check", [])
    if len(ticks) != 8 or len(checks) != 7:
        errors.append(f"expected 8 idle_tick and 7 idle_check, got {len(ticks)}/{len(checks)}")
    if not (move_pos[1]["tail"] < iz_pos < wfi_pos < ticks[0][0]):
        errors.append("idle_zero/wfi_enter must sit between move 1 tail and first idle_tick")
    if not (ticks[-1][0] < take_pos < move_pos[1]["commit"] < move_pos[1]["pick"]):
        errors.append("idle_take must sit after idle_tick 8 and before move 1 commit/pick")
    if (_as_int(iz.get("cpu")), _as_int(iz.get("rq_nr")),
        _as_int(iz.get("pending")), _as_int(iz.get("cadence"))) != (1, 0, 0, 0):
        errors.append(f"unexpected idle_zero fields: {iz}")
    for i, (_p, _s, tk) in enumerate(ticks, start=1):
        if (_as_int(tk.get("cpu")), _as_int(tk.get("intid")), _as_int(tk.get("idx"))) != (1, 30, i):
            errors.append(f"unexpected idle_tick {i}: {tk}")
    for i, (_p, _s, ck) in enumerate(checks, start=1):
        if (_as_int(ck.get("cpu")), _as_int(ck.get("idx")), _as_int(ck.get("wfi"))) != (1, i, 1):
            errors.append(f"unexpected idle_check {i}: {ck}")
        for zero_k in ("sgi", "other_irq", "ap_work", "direct_sched"):
            if _as_int(ck.get(zero_k), -1) != 0:
                errors.append(f"nonzero {zero_k} in idle_check {i}")
    for zero_k in ("sgi", "other_irq", "ap_work", "direct_sched"):
        if _as_int(take.get(zero_k), -1) != 0:
            errors.append(f"nonzero {zero_k} in idle_take")

    # Move 2 & 3 bsp_tick_schedule
    bsp_scheds = by_kind.get("bsp_tick_schedule", [])
    for mid in (2, 3):
        m_bsp = [(p, d) for p, _s, d in bsp_scheds if _as_int(d.get("move_id")) == mid]
        if len(m_bsp) != 1:
            errors.append(f"expected 1 bsp_tick_schedule for move {mid}, got {len(m_bsp)}")
        else:
            bp, bd = m_bsp[0]
            if not (move_pos[mid]["tail"] < bp < move_pos[mid]["commit"]):
                errors.append(f"move {mid} bsp_tick_schedule out of order")
            if _as_int(bd.get("bsp_req"), 0) < 1 or any(
                _as_int(bd.get(k), -1) != 0 for k in ("sgi", "ap_work", "direct_sched")
            ):
                errors.append(f"invalid bsp_tick_schedule fields for move {mid}: {bd}")

    # VM/TLBI checks
    old_p, _, old_ev = by_kind["old_load"][0]
    rem_p, _, rem_ev = by_kind["remap"][0]
    new_p, _, new_ev = by_kind["new_load"][0]
    abt_p, _, abt_ev = by_kind["permission_abort"][0]
    par_p, _, par_ev = by_kind["parent_check"][0]
    cow_p, _, cow_ev = by_kind["cow_check"][0]
    if not (cow_p < old_p < move_pos[1]["irq_save"] and move_pos[1]["el0_after"] < rem_p < move_pos[2]["irq_save"] < move_pos[2]["el0_after"] < new_p < abt_p < par_p):
        errors.append("old_load/remap/new_load/permission_abort/parent_check ordering invalid")
    if old_ev.get("pa") == new_ev.get("pa"):
        errors.append("old_load and new_load PA must differ")
    if _as_int(par_ev.get("pa"), 0) != _as_int(old_ev.get("pa"), -1):
        errors.append(f"parent_check PA {par_ev.get('pa')} != old_load PA {old_ev.get('pa')}")
    if _as_int(rem_ev.get("pa"), 0) != _as_int(new_ev.get("pa"), -1):
        errors.append(f"remap PA {rem_ev.get('pa')} != new_load PA {new_ev.get('pa')}")
    if (_as_int(old_ev.get("data"), -1) != 0x1111 or
        _as_int(par_ev.get("data"), -1) != 0x1111 or
        _as_int(par_ev.get("cow_val"), -1) != 0x1111 or
        _as_int(cow_ev.get("cow_val"), -1) != 0x2222 or
        _as_int(rem_ev.get("data"), -1) != 0x3333 or
        _as_int(new_ev.get("data"), -1) != 0x3333):
        errors.append("unexpected data/cow_val pattern in VM/COW checks")
    if _as_int(old_ev.get("asid"), 0) <= 0 or old_ev.get("asid") != new_ev.get("asid"):
        errors.append("main child ASID must stay nonzero and constant")
    expected_el0 = {
        1: (0xCAFE0001, 0xBEEF1234),
        2: (0xCAFE0002, 0xBEEF1234),
        3: (0xCAFE0003, 0xBEEF5678),
    }
    for _p, _s, ev in by_kind.get("el0_after", []):
        mid = _as_int(ev.get("move_id"), 0)
        if mid in expected_el0 and "x13" in ev and "tpidr" in ev:
            exp_x13, exp_tp = expected_el0[mid]
            if _as_int(ev.get("x13"), -1) != exp_x13 or _as_int(ev.get("tpidr"), -1) != exp_tp:
                errors.append(f"move {mid} el0_after sentinel mismatch: {ev}")
    for _p, _s, ev in by_kind.get("hw_detach", []):
        if "ttbr0" in ev and _as_int(ev.get("ttbr0"), 0) == _as_int(ev.get("root"), -1):
            errors.append(f"hw_detach ttbr0 still equals user root: {ev}")
    if (_as_int(abt_ev.get("ec")), _as_int(abt_ev.get("fsc")),
        _as_int(abt_ev.get("far")), _as_int(abt_ev.get("wnr"))) != (0x24, 0x0f, 0x40000000, 1):
        errors.append(f"unexpected permission_abort ESR/FAR fields: {abt_ev}")

    # Edit tx ordering
    expected_ops = {1: ("munmap", "edit_leaf"), 2: ("mmap", "edit_vma"),
                    3: ("fault", "edit_leaf"), 4: ("mprotect", "edit_leaf")}
    for tx_id, (op_name, leaf_kind) in expected_ops.items():
        eb = [(p, d) for p, _s, d in by_kind.get("edit_begin", []) if _as_int(d.get("tx")) == tx_id]
        td = [(p, d) for p, _s, d in by_kind.get("tlbi_done", []) if _as_int(d.get("tx")) == tx_id]
        el = [(p, d) for p, _s, d in by_kind.get(leaf_kind, []) if _as_int(d.get("tx")) == tx_id]
        if len(eb) != 1 or len(td) != 1 or len(el) != 1:
            errors.append(f"tx {tx_id} ({op_name}) missing begin/tlbi_done/{leaf_kind}")
        elif not (eb[0][0] < td[0][0] < el[0][0]):
            errors.append(f"tx {tx_id} ({op_name}) out of order")
        elif op_name == "mmap" and _as_int(el[0][1].get("leaf_absent")) != 1:
            errors.append("mmap edit_vma must have leaf_absent=1")

    for _p, _s, ttag in by_kind.get("tlbi_tag", []):
        if _as_int(ttag.get("tx")) not in (1, 2, 3, 4, 5):
            errors.append(f"unexpected unrelated same-tag TLBI: {ttag}")

    # Copy, exec, wait_status
    cok_p, _, cok = by_kind["copy_ok"][0]
    cden_p, _, cden = by_kind["copy_denied"][0]
    ec_p, _, ec = by_kind["exec_commit"][0]
    ee_p, _, ee = by_kind["exec_el0"][0]
    if not (move_pos[3]["el0_after"] < cok_p < cden_p < ec_p < ee_p):
        errors.append("copy/exec ordering invalid")
    if _as_int(cok.get("ret"), 0) <= 0 or _as_int(cden.get("ret"), 0) != -14:
        errors.append(f"unexpected copy return values: ok={cok.get('ret')} denied={cden.get('ret')}")
    if ec.get("old_root") == ec.get("new_root") or _as_int(ec.get("old_mask")) != 3 or _as_int(ec.get("new_mask")) != 3:
        errors.append(f"invalid exec_commit: {ec}")
    if ee.get("root") != ec.get("new_root") or _as_int(ee.get("mask")) != 3:
        errors.append(f"invalid exec_el0: {ee}")

    waits = by_kind.get("wait_status", [])
    if len(waits) != 2:
        errors.append(f"expected 2 wait_status events, got {len(waits)}")
    else:
        wmap = {_as_int(d.get("child_pid")): (_as_int(d.get("returned_pid")), _as_int(d.get("status"))) for _p, _s, d in waits}
        if wmap.get(main_pid) != (main_pid, -14):
            errors.append(f"unexpected main child wait_status: {wmap.get(main_pid)}")
        if wmap.get(child2_pid) != (child2_pid, 42 << 8):
            errors.append(f"unexpected second child wait_status: {wmap.get(child2_pid)}")

    return errors


def parse_user_runtime_evidence(text, case="irq"):
    """Validate serial output for user-runtime acceptance.

    Returns ``(ok, errors)``.
    """
    errors = []
    if case not in PASS_SUMMARY_RE:
        return False, [f"unsupported case: {case}"]

    lines = [line.rstrip("\r") for line in text.splitlines()]
    for line in lines:
        if ANY_FATAL_RE.search(line):
            errors.append(f"fatal/fail line observed: {line.strip()}")

    if parse_v1 is not None:
        v1 = parse_v1(text, suite="kernel-selftest")
        errors.extend(v1.errors)
        if v1.failed or v1.skipped:
            errors.append(f"selftest v1 reported failed={v1.failed} skipped={v1.skipped}")
        case_id = f"aarch64_user_runtime_{case}"
        if case_id not in v1.terminal:
            errors.append(f"missing [TEST] terminal record for {case_id}")

    sample_matches = []
    asid_matches = []
    smp_matches = []
    pass_matches = []
    done_matches = []
    for line in lines:
        m = IRQ_SAMPLES_RE.search(line)
        if m:
            sample_matches.append(tuple(int(g) for g in m.groups()))
        am = ASID_SAMPLES_RE.search(line)
        if am:
            asid_matches.append(tuple(int(g) for g in am.groups()))
        sm = SMP_SAMPLES_RE.search(line)
        if sm:
            smp_matches.append(tuple(int(g) for g in sm.groups()))
        if PASS_SUMMARY_RE[case].search(line):
            pass_matches.append(line)
        dm = SELFTEST_DONE_RE.match(line.strip())
        if dm:
            done_matches.append(int(dm.group(1)))

    if case == "irq":
        if len(sample_matches) == 0:
            errors.append("missing [user-runtime] irq-samples line")
        elif len(sample_matches) > 1:
            errors.append(f"duplicate [user-runtime] irq-samples line ({len(sample_matches)})")
        else:
            eoi_a, eoi_b, sw_a, sw_b = sample_matches[0]
            if min(eoi_a, eoi_b, sw_a, sw_b) < 1:
                errors.append(
                    f"insufficient irq samples: eoi_a={eoi_a} eoi_b={eoi_b} "
                    f"sw_a={sw_a} sw_b={sw_b}"
                )
    elif case == "asid":
        if len(asid_matches) == 0:
            errors.append("missing [user-runtime] asid-samples line")
        elif len(asid_matches) > 1:
            errors.append(f"duplicate [user-runtime] asid-samples line ({len(asid_matches)})")
        else:
            asid_a, asid_b, clean_sw, full_fl, reused = asid_matches[0]
            if asid_a < 1 or asid_b < 1 or asid_a == asid_b:
                errors.append(f"invalid ASIDs: asid_a={asid_a} asid_b={asid_b}")
            if clean_sw < 2 or full_fl != 0:
                errors.append(f"unexpected flush stats: clean={clean_sw} full={full_fl}")
            if reused < 1:
                errors.append(f"ASID reuse not observed: reused={reused}")
    elif case == "smp":
        pin_matches = []
        for line in lines:
            pm = SMP_EXPLICIT_PIN_RE.search(line)
            if pm:
                pin_matches.append(tuple(int(g) for g in pm.groups()))
        if len(pin_matches) != 2:
            errors.append(f"expected 2 explicit_pin records, got {len(pin_matches)}")
        else:
            seen_pins = {(cpu, mask) for _pid, cpu, mask in pin_matches}
            if seen_pins != {(0, 1), (1, 2)}:
                errors.append(f"unexpected explicit_pin cpu/mask pairs: {pin_matches}")
        if len(smp_matches) == 0:
            errors.append("missing [user-runtime] smp-samples line")
        elif len(smp_matches) > 1:
            errors.append(f"duplicate [user-runtime] smp-samples line ({len(smp_matches)})")
        else:
            (child_cpu, cpu0_eoi, cpu1_eoi, cpu0_sw, cpu1_sw,
             cow_last_free, waitpid_blocked, waitpid_exit,
             leaked_roots, leaked_pages) = smp_matches[0]
            if child_cpu != 1:
                errors.append(f"expected child_cpu=1, got {child_cpu}")
            if min(cpu0_eoi, cpu1_eoi, cpu0_sw, cpu1_sw) < 1:
                errors.append(
                    f"insufficient per-CPU samples: cpu0_eoi={cpu0_eoi} "
                    f"cpu1_eoi={cpu1_eoi} cpu0_sw={cpu0_sw} cpu1_sw={cpu1_sw}"
                )
            if cow_last_free != 1:
                errors.append(f"expected cow_last_free=1, got {cow_last_free}")
            if waitpid_blocked != 1:
                errors.append(f"expected waitpid_blocked=1, got {waitpid_blocked}")
            if waitpid_exit == 0:
                errors.append(f"expected nonzero waitpid_exit, got {waitpid_exit}")
            if leaked_roots != 0 or leaked_pages != 0:
                errors.append(
                    f"reclaim leak: leaked_roots={leaked_roots} leaked_pages={leaked_pages}"
                )
    elif case == "migration":
        errors.extend(_validate_migration_events(lines))

    if len(pass_matches) == 0:
        errors.append(f"missing [user-runtime] PASS case={case} summary")
    elif len(pass_matches) > 1:
        errors.append(
            f"duplicate [user-runtime] PASS case={case} summary ({len(pass_matches)})"
        )

    if len(done_matches) == 0:
        errors.append("missing [selftest] done line")
    elif len(done_matches) > 1:
        errors.append(f"duplicate [selftest] done line ({len(done_matches)})")
    elif done_matches[0] != 0:
        errors.append(f"[selftest] done reported failed={done_matches[0]}")

    return len(errors) == 0, errors


def parse_product_boot_evidence(text, check_shell_io=False, test_fp_pass=False):
    """Validate serial output for normal product boot."""
    errors = []
    lines = [line.rstrip("\r") for line in text.splitlines()]
    for line in lines:
        if ANY_FATAL_RE.search(line):
            errors.append(f"fatal/fail line observed: {line.strip()}")

    if not any("init-created pid=1" in line for line in lines):
        errors.append("missing PID 1 creation marker (init-created pid=1)")
    if not any("init: phase SYSINIT" in line or "inittab" in line for line in lines):
        errors.append("missing inittab marker (init: phase SYSINIT)")
    # BusyBox ash (2026-11-21: /bin/sh became the busybox multicall copy)
    # prints its banner once per interactive process: "BusyBox v1.36.1
    # (<build timestamp>) built-in shell (ash)" — match the timestamp-free
    # suffix.  The prompt is "# " because OS01 userland runs as uid 0 and
    # FEATURE_EDITING_FANCY_PROMPT is off (ash.c: geteuid()==0 → PS1="# ").
    ash_banner_count = sum(1 for line in lines if "built-in shell (ash)" in line)
    if ash_banner_count < 1:
        errors.append("missing busybox ash banner")
    prompt_count = sum(1 for line in lines if "# " in line)
    if prompt_count < 1:
        errors.append("missing initial '# ' prompt")

    if check_shell_io:
        if test_fp_pass:
            if not any(line.strip() == "test_fp: PASS" for line in lines):
                errors.append(
                    "missing standalone 'test_fp: PASS' output line from /bin/test_fp"
                )
        elif not any(line.strip() == "hi" for line in lines):
            errors.append("missing standalone 'hi' output line from echo hi")
        if ash_banner_count < 2:
            errors.append(f"expected respawned busybox ash banner after exit, got {ash_banner_count}")
        if prompt_count < 2:
            errors.append(f"expected second '# ' prompt after exit, got {prompt_count}")

    return len(errors) == 0, errors


def qemu_command(qemu_bin, cpus, image, firmware, dtb):
    return [
        qemu_bin,
        "-M", "virt,gic-version=2,acpi=off",
        "-cpu", "cortex-a53",
        "-smp", str(cpus),
        "-m", str(MEMORY_MB),
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"if=none,file={image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        "-device", "virtio-rng-device",
        "-dtb", str(dtb),
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot",
    ]


def _git_revision():
    try:
        rev = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=str(_ROOT), text=True, stderr=subprocess.DEVNULL,
        ).strip()
        dirty = bool(subprocess.check_output(
            ["git", "status", "--porcelain"],
            cwd=str(_ROOT), text=True, stderr=subprocess.DEVNULL,
        ).strip())
        return rev or "unknown", dirty
    except Exception:
        return "unknown", False


def _write_report(archive, *, argv, profile, cpu, variant, firmware, image,
                  started, status, runner_exit_code, session, errors, extra=None):
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
            declared_ids=[f"aarch64_user_runtime_{variant}"],
            observed_ids=None,
            argv=list(argv),
            cpu_count=int(cpu),
            memory_mib=MEMORY_MB,
            tool_versions={},
            firmware_path=str(firmware),
            firmware_sha256_before=None,
            firmware_sha256_after=_sha256(firmware) if Path(firmware).is_file() else None,
            image_path=str(image),
            image_sha256_before=None,
            image_sha256_after=_sha256(image) if Path(image).is_file() else None,
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
    except Exception as exc:
        print(f"[{SUITE}] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


def _run_product_cpu(args, cpus, archive, run_dir, budget, drain, dtb_factory):
    sub_dir = run_dir / f"cpu-{cpus}"
    sub_dir.mkdir(parents=True, exist_ok=True)
    dtb = (dtb_factory or generate_diagnostic_dtb)(args.qemu, str(sub_dir), cpus)
    command = qemu_command(args.qemu, cpus, args.image, args.firmware, dtb)
    if ProcessSession is None:
        return False, ["spawn error: ProcessSession is unavailable"], None, False, False, command

    session = None
    child_exited_before_runner_stop = False
    check_shell_io = (cpus == 2) or (args.case == "fp")
    try:
        session = ProcessSession(
            argv=command,
            run_dir=sub_dir,
            timeout_s=budget - drain,
            writable_stdin=check_shell_io,
        )
        session.start()
        session.wait_for(
            lambda text: parse_product_boot_evidence(text, check_shell_io=False)[0]
        )
        if check_shell_io and not session.timed_out:
            if args.case == "fp":
                # FP/SIMD runtime smoke: the product disk carries
                # /bin/test_fp; run it from the busybox ash and require
                # its standalone PASS line before exiting the shell.
                session.send(b"/bin/test_fp\n")
                session.wait_for(
                    lambda text: any(
                        l.strip() == "test_fp: PASS" for l in text.splitlines()
                    )
                )
            else:
                session.send(b"echo hi\n")
                session.wait_for(
                    lambda text: any(l.strip() == "hi" for l in text.splitlines())
                )
            session.send(b"exit\n")
            # Respawn evidence: init's RESPAWN loop restarts terminal, which
            # execs a fresh busybox ash printing the banner again.  wait_for
            # predicates see only the post-cursor text slice (the first
            # banner is already behind the cursor), so one more banner in
            # the slice is the respawn; the final parse still requires two
            # banners plus two prompts over the whole transcript.
            session.wait_for(
                lambda text: text.count("built-in shell (ash)") >= 1
            )
        session.observe(drain)
        child_exited_before_runner_stop = session.returncode is not None
        session.stop()
        child_exited_before_runner_stop |= not session.stopped_by_runner
        session.close()
    except Exception as error:
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        return False, [f"spawn error: {error}"], session, False, False, command

    text = session.text
    timed_out = session.timed_out
    accepted, why = parse_product_boot_evidence(
        text, check_shell_io=check_shell_io, test_fp_pass=(args.case == "fp")
    )
    result = accepted and not timed_out and not child_exited_before_runner_stop
    errors = []
    if timed_out:
        errors.append(f"cpu={cpus}: 120s boot deadline expired")
    if child_exited_before_runner_stop:
        errors.append(f"cpu={cpus}: QEMU exited before runner completed observation drain")
    errors.extend(f"cpu={cpus}: {e}" for e in why)
    return result, errors, session, timed_out, child_exited_before_runner_stop, command


def run_boot(args, deadline_s=None, dtb_factory=None):
    started = time.monotonic()
    cpus = 2 if args.case in ("smp", "migration") else 1
    variant = "product" if args.case == "product" else f"user-runtime-{args.case}"
    archive = None
    if args.build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(args.build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir) / f"case-{args.case}"
    run_dir.mkdir(parents=True, exist_ok=True)

    budget = float(deadline_s if deadline_s is not None else BOOT_DEADLINE_SECONDS)
    drain = min(DRAIN_SECONDS, budget / 4.0)

    elf_errors = audit_elf(args.elf, args.llvm_nm, case=args.case)
    if elf_errors:
        _write_report(
            archive, argv=[], profile=args.profile, cpu=cpus, variant=variant,
            firmware=args.firmware, image=args.image, started=started,
            status="FAIL", runner_exit_code=1, session=None, errors=elf_errors,
        )
        for err in elf_errors:
            print(f"FAIL: {err}", file=sys.stderr)
        return 1

    if args.case in ("product", "fp"):
        all_ok = True
        all_errors = []
        last_cmd = []
        last_session = None
        for cpu_n in (1, 2, 4, 8):
            ok, errs, sess, timed_out, _, cmd = _run_product_cpu(
                args, cpu_n, archive, run_dir, budget, drain, dtb_factory
            )
            last_cmd = cmd
            last_session = sess
            if not ok:
                all_ok = False
                all_errors.extend(errs)
                break
        _write_report(
            archive, argv=last_cmd, profile=args.profile, cpu=8, variant=variant,
            firmware=args.firmware, image=args.image, started=started,
            status="PASS" if all_ok else "FAIL",
            runner_exit_code=0 if all_ok else 1, session=last_session,
            errors=all_errors, extra={"case": args.case},
        )
        if all_ok:
            print(f"PASS: user-runtime case={args.case} cpus=1,2,4,8 ({run_dir})")
            return 0
        for err in all_errors:
            print(f"FAIL: {err}", file=sys.stderr)
        return 1

    dtb = (dtb_factory or generate_diagnostic_dtb)(args.qemu, str(run_dir), cpus)
    command = qemu_command(args.qemu, cpus, args.image, args.firmware, dtb)
    metadata = {
        "case": args.case,
        "command": command,
        "cpus": cpus,
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
        _write_report(
            archive, argv=command, profile=args.profile, cpu=cpus, variant=variant,
            firmware=args.firmware, image=args.image, started=started,
            status="ERROR", runner_exit_code=2, session=None, errors=errors,
        )
        return 2

    session = None
    child_exited_before_runner_stop = False
    try:
        session = ProcessSession(
            argv=command, run_dir=run_dir, timeout_s=budget - drain
        )
        session.start()
        session.wait_for(
            lambda text: parse_user_runtime_evidence(text, args.case)[0]
        )
        session.observe(drain)
        child_exited_before_runner_stop = session.returncode is not None
        session.stop()
        child_exited_before_runner_stop |= not session.stopped_by_runner
        session.close()
    except Exception as error:
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        errors = [f"spawn error: {error}"]
        _write_report(
            archive, argv=command, profile=args.profile, cpu=cpus, variant=variant,
            firmware=args.firmware, image=args.image, started=started,
            status="ERROR", runner_exit_code=2, session=session, errors=errors,
        )
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    text = session.text
    timed_out = session.timed_out
    accepted, why = parse_user_runtime_evidence(text, args.case)
    result = accepted and not timed_out and not child_exited_before_runner_stop

    errors = []
    if timed_out:
        errors.append("absolute 120s boot deadline expired before evidence completed")
    if child_exited_before_runner_stop:
        errors.append("QEMU exited before runner completed observation drain")
    errors.extend(why)

    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": session.returncode,
        "result": "PASS" if result else "FAIL",
        "errors": errors,
    })
    (run_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    _write_report(
        archive, argv=command, profile=args.profile, cpu=cpus, variant=variant,
        firmware=args.firmware, image=args.image, started=started,
        status="PASS" if result else "FAIL",
        runner_exit_code=0 if result else 1, session=session, errors=errors,
        extra={"timeout": timed_out, "case": args.case},
    )

    if result:
        print(f"PASS: user-runtime case={args.case} ({run_dir})")
        return 0
    for err in errors:
        print(f"FAIL: {err}", file=sys.stderr)
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", default="irq", choices=("irq", "asid", "smp", "migration", "product", "fp"))
    parser.add_argument("--image", required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--llvm-nm", default="llvm-nm")
    parser.add_argument("--build-dir", default="")
    parser.add_argument("--profile", default="aarch64-clang")
    parser.add_argument("--log-dir", default="test-results/aarch64-user-runtime")
    args = parser.parse_args(argv)
    return run_boot(args)


if __name__ == "__main__":
    sys.exit(main())
