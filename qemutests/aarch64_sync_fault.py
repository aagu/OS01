#!/usr/bin/env python3
"""Acceptance harness for the aarch64 EL1h sync fault probe (spec §5).

Drives the dedicated `KERNEL_VARIANT=sync-fault` kernel/image in QEMU
``virt,gic-version=2`` with one ``cortex-a53`` and verifies the fatal
diagnostic line emitted by ``aarch64_el1_sync_fatal`` (Task 1) reaches
the serial port exactly once, in the spec format, in the right order,
and is not preceded by a ``precondition FAIL`` or followed by a
``returned``/tick/normal-completion marker (Task 2 probe).

The parser is the contract surface — it is intentionally host-pure and
used both by ``--self-test`` and the live QEMU driver. Timeouts never
count as success: if the fatal line never appears, the harness exits
non-zero even when the QEMU process terminated cleanly.
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

# Running this file as a script puts ``qemutests/`` on ``sys.path[0]`` —
# not the repo root — so bootstrap the repo root explicitly (Ruling 7).
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

# Suite id for the archive path and the RunReport.
SUITE = "aarch64-sync-fault"

# Post-fatal drain before the runner stops QEMU (spec §5.2/§5.3).
POST_FATAL_DRAIN = 0.5


# A typical live QEMU run starts with a small amount of UEFI banner text
# before the kernel probe path runs. The valid synthetic log mirrors that
# shape so the parser is exercised against the same window it sees live.
#
# Spec §5.1 pins the controlled-probe diagnostic values: EC=0x25
# (data abort, same EL — the ldr from AARCH64_PT_SELFTEST_VA) and
# FAR=0xffff800000000000 (= AARCH64_PT_SELFTEST_VA). Any other EC or
# FAR in the line means the kernel reported something other than the
# expected data abort from the probe — that is "weak evidence" (spec
# §5.3) and must reject.
valid_log = """\
UEFI: booting OS01
UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872
UEFI-A64: pmm alloc smoke OK
UEFI-A64: pt map smoke OK
[aarch64-sync-test] armed
[aarch64-sync] FATAL mpidr=0x0000000000000000 ec=0x25 esr=0x0000000096000245 elr=0xffff800008000abc spsr=0x0000000060000000 far=0xffff800000000000
"""

# EC=0x25 + FAR=0xffff800000000000 is the spec contract. A second
# positive fixture exercising different MPIDR/ELR/SPSR values proves
# the parser is not anchored on a single line.
valid_log_alt_regs = """\
[aarch64-sync-test] armed
[aarch64-sync] FATAL mpidr=0x0000000080000000 ec=0x25 esr=0x0000000096000007 elr=0xffff000040093ce4 spsr=0x00000000600003c5 far=0xffff800000000000
"""


def self_test() -> None:
    # Positive fixtures
    assert sync_fault_evidence(valid_log), "valid synthetic log must pass"
    assert sync_fault_evidence(valid_log.replace("\n", "\n\r")), \
        "valid log with PL011's LF+CR must pass"
    assert sync_fault_evidence(valid_log_alt_regs), \
        "valid log with different MPIDR/ELR/SPSR (real QEMU values) must pass"

    # Negative — markers must be exactly once each, in order
    assert not sync_fault_evidence(""), "empty log must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "[aarch64-sync-test] armed\n", "")), "missing armed must fail"
    assert not sync_fault_evidence(valid_log + "[aarch64-sync-test] armed\n"), \
        "duplicate armed must fail"
    # FATAL line fields
    assert not sync_fault_evidence(valid_log.replace(
        "[aarch64-sync] FATAL ", "[aarch64-sync] FATAL  ")), \
        "duplicate space inside FATAL header must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "[aarch64-sync] FATAL ", "[SYNC] FATAL ")), \
        "missing `[aarch64-sync]` prefix must fail"

    # FAR: required to be exactly 0xffff800000000000 — the spec §5.1
    # contract for the controlled probe. Any other value means the
    # kernel reported a fault at the wrong VA or with the wrong width.
    assert not sync_fault_evidence(valid_log.replace(
        "far=0xffff800000000000", "far=n/a")), \
        "far=n/a for data abort (FnV clear) must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "far=0xffff800000000000", "far=0x0000000000000000")), \
        "FAR=0x0 (probe did not target AARCH64_PT_SELFTEST_VA) must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "far=0xffff800000000000", "far=0xdeadbeefdeadbeef")), \
        "wrong FAR value must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "far=0xffff800000000000", "far=0xffff80000000000")), \
        "15-digit FAR must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "far=0xffff800000000000", "far=0xFFFF800000000000")), \
        "uppercase FAR must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "far=0xffff800000000000", "far=0xffff8000_00000000")), \
        "FAR with non-hex char must fail"

    # EC field: spec §5.1 pins EC=0x25 for the data abort from the
    # controlled probe. Any other EC means the kernel reported a
    # different exception class than expected.
    assert not sync_fault_evidence(valid_log.replace("ec=0x25", "ec=0x24")), \
        "wrong `ec=` field (lower-EL data abort) must fail"
    assert not sync_fault_evidence(valid_log.replace("ec=0x25", "ec=0x21")), \
        "wrong `ec=` field (same-EL instruction abort) must fail"
    assert not sync_fault_evidence(valid_log.replace("ec=0x25", "ec=0x3c")), \
        "wrong `ec=` field (BRK) must fail"
    # ESR EC bits disagree with `ec=0x25` — the regex already pins
    # `ec=0x25` but a buggy kernel could emit ESR with different EC
    # bits. The parser's runtime check catches that.
    assert not sync_fault_evidence(valid_log.replace(
        "esr=0x0000000096000245", "esr=0x0000000090000024")), \
        "ESR EC bits inconsistent with ec=0x25 must fail"

    # Width and case
    assert not sync_fault_evidence(valid_log.replace(
        "mpidr=0x0000000000000000", "mpidr=0x000000000000000")), \
        "15-digit MPIDR must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "esr=0x0000000096000245", "esr=0x000000009600024")), \
        "15-digit ESR must fail"
    assert not sync_fault_evidence(valid_log.replace(
        "elr=0xffff800008000abc", "elr=0xFFFF800008000ABC")), \
        "uppercase hex must fail"
    # ESR EC bits must encode the same value as `ec=`. The parser does
    # not allow zero-extended 1-digit EC.
    assert not sync_fault_evidence(valid_log.replace("ec=0x25", "ec=0x5")), \
        "1-digit EC must fail"

    # Probe-side rejections
    assert not sync_fault_evidence("[aarch64-sync-test] precondition FAIL\n"), \
        "bare precondition FAIL must fail"
    # Duplicate FATAL — even a malformed second FATAL counts as a
    # duplicate (spec §5.3 "exactly one FATAL"). The full spec regex
    # already rejects malformed FATALs, but a 2nd `[aarch64-sync] FATAL`
    # line (any shape) must also fail.
    assert not sync_fault_evidence(
        valid_log + "[aarch64-sync] FATAL malformed\n"), \
        "second FATAL (malformed) must fail"
    assert not sync_fault_evidence(
        valid_log + "[aarch64-sync] FATAL\n"), \
        "second FATAL (truncated after marker) must fail"
    assert not sync_fault_evidence(
        valid_log + "[aarch64-sync] FATAL mpidr=0x0000000000000000\n"), \
        "second FATAL (incomplete fields) must fail"
    assert not sync_fault_evidence(
        valid_log + "[aarch64-sync-test] precondition FAIL\n"), \
        "precondition FAIL appended must fail"
    assert not sync_fault_evidence(valid_log + "[aarch64-sync-test] returned\n"), \
        "returned marker appended must fail"

    # Post-diagnostic absence checks
    assert not sync_fault_evidence(valid_log + "[tick] 1\n"), \
        "tick after FATAL must fail"
    assert not sync_fault_evidence(valid_log + "[selftest] done\n"), \
        "selftest completion after FATAL must fail"
    assert not sync_fault_evidence(
        valid_log + "[smp] cpu=0 online mpidr=0x0\n"), \
        "SMP bring-up after FATAL must fail"
    assert not sync_fault_evidence(
        valid_log + "OS01 aarch64 phase1 boot ok\n"), \
        "phase1 boot ok after FATAL must fail (probe did not halt)"

    # Out-of-order: FATAL before armed
    reordered = (valid_log.replace(
        "[aarch64-sync-test] armed\n[aarch64-sync] FATAL ",
        "[aarch64-sync] FATAL ", 1)
        + "\n[aarch64-sync-test] armed\n")
    assert not sync_fault_evidence(reordered), "armed after FATAL must fail"


_FATAL_LINE_RE = re.compile(
    r"^\[aarch64-sync\] FATAL "
    r"mpidr=0x([0-9a-f]{16}) ec=0x25 "   # spec §5.1: data abort, same EL
    r"esr=0x([0-9a-f]{16}) elr=0x([0-9a-f]{16}) "
    r"spsr=0x([0-9a-f]{16}) "
    r"far=0xffff800000000000$",            # spec §5.1: AARCH64_PT_SELFTEST_VA
    re.MULTILINE,
)
# Any `[aarch64-sync] FATAL ...` line, regardless of whether it matches the
# full spec format. spec §5.3 requires "exactly one FATAL"; a malformed
# second FATAL still counts (the kernel printing a corrupt second record is
# itself a defect), so we anchor on the prefix instead of the full regex.
_ANY_FATAL_PREFIX_RE = re.compile(r"^\[aarch64-sync\] FATAL(?:[ \t]|$)", re.MULTILINE)
_ARMED_RE = re.compile(r"^\[aarch64-sync-test\] armed$", re.MULTILINE)
_RETURNED_RE = re.compile(r"\[aarch64-sync-test\] returned")
_PRECONDITION_FAIL_RE = re.compile(r"\[aarch64-sync-test\] precondition FAIL")
_TICK_RE = re.compile(r"^\[tick\] \d+$", re.MULTILINE)
_SELFTEST_RE = re.compile(r"^\[selftest\]", re.MULTILINE)
_SMP_RE = re.compile(r"^\[smp\]", re.MULTILINE)
_PHASE1_OK_RE = re.compile(r"^OS01 aarch64 phase1 boot ok$", re.MULTILINE)


def sync_fault_evidence(text: str) -> bool:
    """Return True iff ``text`` contains a complete EL1h sync fatal
    diagnostic produced by ``aarch64_el1_sync_fatal``, with the probe
    pre-conditions satisfied and no post-diagnostic continuation.

    This is the contract surface used by both the harness and the
    self-test: it must be host-pure and reject every weak-evidence case
    listed in spec §5.3.
    """
    # PL011 emits LF then CR after each line; normalize so the regex
    # anchors are identical to the live and saved-log cases.
    text = text.replace("\r", "")

    # Probe-side rejections: any precondition FAIL or `returned` line
    # invalidates the run outright (the probe must either succeed and
    # never reach `returned`, or stop at precondition).
    if _PRECONDITION_FAIL_RE.search(text):
        return False
    if _RETURNED_RE.search(text):
        return False

    # Post-diagnostic absence: the kernel halts on the fatal path, so
    # any later tick / selftest / SMP / phase1-boot markers mean the
    # probe did not stop the CPU as required.
    if _TICK_RE.search(text):
        return False
    if _SELFTEST_RE.search(text):
        return False
    if _SMP_RE.search(text):
        return False
    if _PHASE1_OK_RE.search(text):
        return False

    # Exactly one armed marker, exactly one FATAL line. spec §5.3 demands
    # "exactly one FATAL" — a malformed second FATAL still counts as a
    # duplicate, because a kernel printing a corrupt extra record is itself
    # a defect. Anchor on the prefix, not the full spec regex, so a line
    # like `[aarch64-sync] FATAL malformed` cannot slip past.
    armed_matches = list(_ARMED_RE.finditer(text))
    if len(armed_matches) != 1:
        return False
    any_fatal_matches = list(_ANY_FATAL_PREFIX_RE.finditer(text))
    if len(any_fatal_matches) != 1:
        return False
    fatal_matches = list(_FATAL_LINE_RE.finditer(text))
    if len(fatal_matches) != 1:
        return False

    # `armed` must precede the FATAL line.
    if armed_matches[0].start() >= fatal_matches[0].start():
        return False

    # ESR EC bits (bits 26..31) must agree with `ec=0x25` — the parser
    # already locked `ec=` and FAR to the spec values in the regex, but
    # a kernel bug could still emit an ESR whose EC bits disagree.
    # Catching that here keeps the diagnostic consistent.
    mpidr_hex, esr_hex, elr_hex, spsr_hex = fatal_matches[0].groups()
    esr_int = int(esr_hex, 16)
    esr_ec = (esr_int >> 26) & 0x3F
    if esr_ec != 0x25:
        return False
    return True


def qemu_command(args: argparse.Namespace) -> list[str]:
    return [
        args.qemu, "-M", "virt,gic-version=2", "-cpu", "cortex-a53",
        "-smp", "1", "-m", "512",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware}",
        "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        "-serial", "stdio", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]


def file_sha256(path: str) -> str:
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _sha256_path(path) -> str | None:
    if path is None:
        return None
    try:
        return file_sha256(str(path))
    except (OSError, TypeError):
        return None


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


def _write_suite_report(archive, *, argv, profile, session, started_monotonic,
                        status, runner_exit_code, errors, firmware=None,
                        image=None, extra=None) -> None:
    """Persist a suite-unit RunReport; the suite has no guest v1 cases."""
    if archive is None or RunReport is None:
        return
    try:
        rev, dirty = _git_revision()
        outcomes = [{"errors": list(errors)}]
        if extra:
            outcomes[0].update(extra)
        report = RunReport(
            schema_version=1, run_id=archive.run_dir.name,
            git_revision=rev, git_dirty=dirty, profile=profile, suite=SUITE,
            request=None, declared_ids=None, observed_ids=None,
            argv=list(argv), cpu_count=1, memory_mib=512, tool_versions={},
            firmware_path=firmware or None,
            # Only the post-run hash is recorded: a ``*_before`` computed at
            # report time would not be a real mutation check (same convention
            # as ``run_kernel_selftest.py`` / ``run_hosttests.py``).
            firmware_sha256_before=None,
            firmware_sha256_after=_sha256_path(firmware),
            image_path=image or None,
            image_sha256_before=None,
            image_sha256_after=_sha256_path(image),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started_monotonic),
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None else None),
            stopped_by_runner=bool(session is not None and session.stopped_by_runner),
            status=status, count_unit="suite", outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # noqa: BLE001
        print(f"[{SUITE}] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


def run_case(args: argparse.Namespace, *, session_factory=None, build_dir=None,
             profile="default") -> int:
    """Run the sync-fault case once through ``ProcessSession`` +
    ``RunArchive``. The evidence decision is still ``sync_fault_evidence``
    (suite owned); the process boundary and per-run archive are common.

    Success requires the runner to actively stop QEMU after the ordered
    fatal evidence (spec §5.2): a QEMU that self-exits is weak evidence
    and FAILs, even though the fatal line is present.

    Returns the plan's direct-runner exit code: 0 (PASS), 1 (FAIL /
    TIMEOUT) or 2 (configuration / environment ERROR).  The archived
    ``status`` always matches the returned code.
    """
    if session_factory is None:
        session_factory = ProcessSession
    started = time.monotonic()

    archive = None
    if build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"

    command = qemu_command(args)
    metadata = {
        "command": command,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    session = None
    fatal_observed = False
    timed_out = False
    stopped_by_runner = False
    spawn_error = None
    try:
        if session_factory is None:
            raise RuntimeError("ProcessSession is unavailable")
        session = session_factory(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        # Wait for the suite-owned fatal evidence. If it never arrives the
        # deadline trips (timed_out) and the run is not a success.
        session.wait_for(lambda text: sync_fault_evidence(text))
        fatal_observed = not session.timed_out
        # Short drain so trailing kernel noise is captured, but do not
        # wait for QEMU to exit on its own: the runner stops it (spec §5.2).
        session.observe(POST_FATAL_DRAIN)
        session.stop()
        session.close()
    except Exception as error:  # noqa: BLE001 — launch/OS error is ERROR
        spawn_error = error
        if session is not None:
            try:
                session.close()
            except Exception:
                pass

    text = session.text if session is not None else ""
    timed_out = bool(session is not None and session.timed_out)
    stopped_by_runner = bool(session is not None and session.stopped_by_runner)

    if spawn_error is not None:
        print(json.dumps({"event": "spawn-error", "error": str(spawn_error)}))
        _write_suite_report(
            archive, argv=command, profile=profile, session=session,
            started_monotonic=started, status="ERROR", runner_exit_code=2,
            errors=[f"spawn error: {spawn_error}"],
            firmware=args.firmware, image=args.image)
        return 2

    accepted = sync_fault_evidence(text)
    # spec §5.2: success requires the harness to actively terminate QEMU
    # after observing the FATAL. A QEMU that self-exits before the
    # harness gets a chance to terminate it (e.g. it shut down cleanly
    # on its own) is weak evidence — the diagnostic may have been
    # printed by something other than the kernel under test.
    #
    # ``stopped_by_runner`` is ProcessSession's record of the runner
    # issuing terminate/kill: it is True only when QEMU was still alive
    # when the runner stopped it. If it is False, QEMU had already exited
    # and the harness did NOT get to terminate it — fail unless the fatal
    # was never observed (in which case it is a different failure mode).
    qemu_self_exited = (
        fatal_observed
        and not stopped_by_runner
        and not timed_out
    )
    result = accepted and not timed_out and not qemu_self_exited
    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "harness_terminated": stopped_by_runner,
        "fatal_observed": fatal_observed,
        "qemu_self_exited": qemu_self_exited,
        "result": "PASS" if result else "FAIL",
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    errors = []
    if timed_out:
        errors.append("timeout before sync-fault evidence")
    elif not accepted:
        errors.append("sync-fault evidence not satisfied")
    if qemu_self_exited:
        errors.append("QEMU self-exited after the fatal evidence "
                      "(no runner-owned stop)")
    _write_suite_report(
        archive, argv=command, profile=profile, session=session,
        started_monotonic=started, status="PASS" if result else "FAIL",
        runner_exit_code=0 if result else 1, errors=errors,
        firmware=args.firmware, image=args.image,
        extra={"timeout": timed_out, "fatal_observed": fatal_observed,
               "qemu_self_exited": qemu_self_exited})

    print(json.dumps({
        "event": "case", "result": "PASS" if result else "FAIL",
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "fatal_observed": fatal_observed,
        "qemu_self_exited": qemu_self_exited,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
    }))
    return 0 if result else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--qemu")
    parser.add_argument("--log-dir")
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive, supplied by Make")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                             "default"),
                        help="profile name recorded in the report")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("aarch64_sync_fault: self-test passed")
        return 0
    if not all((args.firmware, args.image, args.qemu, args.log_dir)):
        parser.error("--firmware, --image, --qemu, and --log-dir are required outside --self-test")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return run_case(args, build_dir=args.build_dir, profile=args.profile)


if __name__ == "__main__":
    sys.exit(main())
