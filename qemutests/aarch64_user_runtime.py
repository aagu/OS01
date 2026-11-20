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
PASS_SUMMARY_RE = {
    "irq": re.compile(r"\[user-runtime\] PASS case=irq cpu_mask=1\b"),
    "asid": re.compile(r"\[user-runtime\] PASS case=asid cpu_mask=1\b"),
    "smp": re.compile(r"\[user-runtime\] PASS case=smp cpu_mask=3\b"),
}
SELFTEST_DONE_RE = re.compile(r"^\[selftest\] done \(failed=(\d+)\)$")

REQUIRED_SYMBOLS = ("test_aarch64_user_runtime_run",)
FORBIDDEN_PRODUCT_PREFIXES = ("aarch64_user_runtime_test_", "test_aarch64_user_runtime_")


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
    if case == "product":
        for sym in sorted(syms):
            if any(sym.startswith(pfx) for pfx in FORBIDDEN_PRODUCT_PREFIXES):
                errors.append(f"forbidden test symbol {sym} in normal product ELF {path}")
    else:
        for sym in REQUIRED_SYMBOLS:
            if sym not in syms:
                errors.append(f"missing required symbol {sym} in {path}")
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


def parse_product_boot_evidence(text, check_shell_io=False):
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
    banner_count = sum(1 for line in lines if "OS01 Shell" in line)
    if banner_count < 1:
        errors.append("missing OS01 Shell banner")
    prompt_count = sum(1 for line in lines if "$ " in line)
    if prompt_count < 1:
        errors.append("missing initial '$ ' prompt")

    if check_shell_io:
        if not any(line.strip() == "hi" for line in lines):
            errors.append("missing standalone 'hi' output line from echo hi")
        if banner_count < 2:
            errors.append(f"expected respawned OS01 Shell banner after exit, got {banner_count}")
        if prompt_count < 2:
            errors.append(f"expected second '$ ' prompt after exit, got {prompt_count}")

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
    check_shell_io = (cpus == 2)
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
            session.send(b"echo hi\n")
            session.wait_for(
                lambda text: any(l.strip() == "hi" for l in text.splitlines())
            )
            session.send(b"exit\n")
            session.wait_for(
                lambda text: "OS01 Shell" in text and "$ " in text
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
    accepted, why = parse_product_boot_evidence(text, check_shell_io=check_shell_io)
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
    cpus = 2 if args.case == "smp" else 1
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

    if args.case == "product":
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
            print(f"PASS: user-runtime case=product cpus=1,2,4,8 ({run_dir})")
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
    parser.add_argument("--case", default="irq", choices=("irq", "asid", "smp", "product"))
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
