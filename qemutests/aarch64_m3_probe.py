#!/usr/bin/env python3
"""Acceptance harness for the aarch64 M3 shootdown probe (spec §7.3).

Drives the PRODUCTION (non-selftest) kernel image in QEMU
``virt,gic-version=2`` with two ``cortex-a53`` CPUs (`-smp 2` — the
probe target is only CPU1) and verifies the serial log shows the
spec'd probe sequence:

    M3-SHOOTDOWN-PROBE: START
    M3-SHOOTDOWN-PROBE: OK

exactly once each, in that order, with NO ``M3-SHOOTDOWN-PROBE: FAIL``
line anywhere. The probe halts the kernel on any failure, so a FAIL
line (or a missing OK) is fatal for acceptance. A
``M3-SHOOTDOWN-PROBE: SKIP (single-CPU boot)`` line under `-smp 2`
means the DTB topology lied about the CPU count and is also a failure.

The parser is the contract surface — it is intentionally host-pure and
used both by ``--self-test`` and the live QEMU driver. Timeouts never
count as success: if the OK line never appears, the harness exits
non-zero even when the QEMU process terminated cleanly (the kernel
halts forever after a probe FAIL, so the harness must terminate QEMU
itself — mirroring qemutests/aarch64_sync_fault.py).
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
SUITE = "aarch64-m3-probe"

# Post-OK drain before the runner stops QEMU.
POST_OK_DRAIN = 0.5


# A typical live QEMU run starts with a small amount of UEFI banner
# text before the kernel probe path runs. The synthetic fixture mirrors
# that shape so the parser is exercised against the same window it
# sees live.
valid_log = """\
UEFI: booting OS01
UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872
M3-SHOOTDOWN-PROBE: START
M3-SHOOTDOWN-PROBE: OK
"""

valid_log_crlf = valid_log.replace("\n", "\r\n")

# Failure shapes the parser must reject.
fail_ap_not_ready = """\
M3-SHOOTDOWN-PROBE: START
M3-SHOOTDOWN-PROBE: FAIL ap-not-ready
"""
fail_requires_one_ap = """\
M3-SHOOTDOWN-PROBE: FAIL requires-at-least-one-AP
"""
skip_single_cpu = """\
M3-SHOOTDOWN-PROBE: SKIP (single-CPU boot)
"""
ok_before_start = """\
M3-SHOOTDOWN-PROBE: OK
M3-SHOOTDOWN-PROBE: START
"""
duplicated_ok = valid_log + "M3-SHOOTDOWN-PROBE: OK\n"
missing_ok = "M3-SHOOTDOWN-PROBE: START\n"

_START_PREFIX = "M3-SHOOTDOWN-PROBE: START"
_OK_PREFIX = "M3-SHOOTDOWN-PROBE: OK"
_FAIL_PREFIX = "M3-SHOOTDOWN-PROBE: FAIL"
_SKIP_PREFIX = "M3-SHOOTDOWN-PROBE: SKIP"

# Line-anchored so e.g. a hypothetical "...: OKK" cannot slip through.
_START_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: START\r?$", re.MULTILINE)
_OK_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: OK\r?$", re.MULTILINE)
_FAIL_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: FAIL.*$", re.MULTILINE)
_SKIP_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: SKIP.*$", re.MULTILINE)


def probe_evidence(text: str) -> bool:
    """Contract parser: exactly one START, exactly one OK, OK strictly
    after START, and no FAIL / SKIP line anywhere."""
    # The PL011 serial path emits LF+CR ("\n\r") line endings; strip
    # stray CRs so the line-anchored regexes see plain "\n".
    text = text.replace("\r\n", "\n").replace("\n\r", "\n")
    start_matches = list(_START_RE.finditer(text))
    ok_matches = list(_OK_RE.finditer(text))
    if len(start_matches) != 1 or len(ok_matches) != 1:
        return False
    if start_matches[0].start() >= ok_matches[0].start():
        return False
    if _FAIL_RE.search(text) or _SKIP_RE.search(text):
        return False
    return True


def self_test() -> None:
    # Positive
    assert probe_evidence(valid_log), "valid synthetic log must pass"
    assert probe_evidence(valid_log_crlf), \
        "valid log with CRLF line endings must pass"
    assert probe_evidence(valid_log.replace("\n", "\n\r")), \
        "valid log with PL011's LF+CR line endings must pass"

    # Negative
    assert not probe_evidence(""), "empty log must fail"
    assert not probe_evidence(fail_ap_not_ready), \
        "FAIL ap-not-ready must fail"
    assert not probe_evidence(fail_requires_one_ap), \
        "FAIL requires-at-least-one-AP must fail"
    assert not probe_evidence(skip_single_cpu), \
        "single-CPU SKIP under -smp 2 must fail"
    assert not probe_evidence(ok_before_start), \
        "OK before START must fail"
    assert not probe_evidence(duplicated_ok), \
        "duplicated OK must fail"
    assert not probe_evidence(missing_ok), \
        "missing OK must fail"
    assert not probe_evidence(valid_log.replace(
        "M3-SHOOTDOWN-PROBE: OK", "M3-SHOOTDOWN-PROBE: OKK")), \
        "non-line-anchored OK match must fail"


def qemu_command(args: argparse.Namespace, diagnostic_dtb: str | None) -> list[str]:
    # -smp 2 per spec §7.3: the probe targets exactly CPU1. With a
    # diagnostic DTB, `acpi=off` makes the kernel read /cpus from the
    # blob, so the DTB must be generated for the same CPU count.
    command = [
        args.qemu, "-M", "virt,gic-version=2" + (",acpi=off" if diagnostic_dtb else ""),
        "-cpu", "cortex-a53", "-smp", "2", "-m", "512",
        "-drive", f"if=pflash,format=raw,file={args.firmware}",
        "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        "-serial", "stdio", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]
    if diagnostic_dtb:
        command.extend(["-dtb", diagnostic_dtb])
    return command


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
            argv=list(argv), cpu_count=2, memory_mib=512, tool_versions={},
            firmware_path=firmware or None,
            firmware_sha256_before=_sha256_path(firmware),
            firmware_sha256_after=_sha256_path(firmware),
            image_path=image or None,
            image_sha256_before=_sha256_path(image),
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


def generate_diagnostic_dtb(qemu: str, log_dir: str, cpus: int,
                            ram_mib: int = 512) -> str:
    """Materialize a packed QEMU-generated virt DTB (same mechanism as
    qemutests/aarch64_uefi_smp.py): some prebuilt UEFI firmwares do not
    expose the device tree through the EFI configuration table, and the
    kernel then dies at `[dtb] FATAL: UEFI handoff has no DTB`."""
    case_dir = os.path.join(log_dir, f"dtb-ram-{ram_mib}-cpus-{cpus}")
    Path(case_dir).mkdir(parents=True, exist_ok=True)
    sparse = os.path.join(case_dir, "qemu-virt.dtb.sparse")
    packed = os.path.join(case_dir, "qemu-virt.dtb")
    completed = subprocess.run(
        [qemu, "-M", "virt,gic-version=2", "-cpu", "cortex-a53", "-smp", str(cpus),
         "-machine", f"dumpdtb={sparse}", "-display", "none", "-m", str(ram_mib)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    )
    if completed.returncode != 0 or not os.path.exists(sparse):
        raise RuntimeError(
            f"failed to generate diagnostic DTB via '{qemu} -machine dumpdtb='"
        )
    subprocess.run(["dtc", "-I", "dtb", "-O", "dtb", "-o", packed, sparse],
                   check=True)
    os.unlink(sparse)
    return packed


def run_case(args: argparse.Namespace, *, session_factory=None, build_dir=None,
             profile="default") -> bool:
    """Run the production-image probe case once through ``ProcessSession``
    + ``RunArchive``. The evidence decision is still ``probe_evidence``
    (suite owned); a timeout is a failure."""
    if session_factory is None:
        session_factory = ProcessSession
    started = time.monotonic()

    diagnostic_dtb = None
    if getattr(args, "diagnostic_dtb", None):
        diagnostic_dtb = generate_diagnostic_dtb(
            args.qemu, args.log_dir, 2)

    archive = None
    if build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"
    command = qemu_command(args, diagnostic_dtb)
    metadata = {
        "command": command,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
        "diagnostic_dtb": diagnostic_dtb,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    session = None
    ok_observed = False
    timed_out = False
    spawn_error = None
    try:
        if session_factory is None:
            raise RuntimeError("ProcessSession is unavailable")
        session = session_factory(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        session.wait_for(lambda text: probe_evidence(text))
        ok_observed = not session.timed_out
        # Short drain so trailing kernel noise is captured; the kernel
        # keeps halting forever after OK, so QEMU never exits on its own
        # and the runner stops it.
        session.observe(POST_OK_DRAIN)
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
            started_monotonic=started, status="ERROR", runner_exit_code=1,
            errors=[f"spawn error: {spawn_error}"],
            firmware=args.firmware, image=args.image)
        return False

    accepted = probe_evidence(text)
    result = accepted and not timed_out
    metadata.update({
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "harness_terminated": stopped_by_runner,
        "ok_observed": ok_observed,
        "result": "PASS" if result else "FAIL",
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    errors = []
    if timed_out:
        errors.append("timeout before probe evidence")
    elif not accepted:
        errors.append("probe evidence not satisfied")
    _write_suite_report(
        archive, argv=command, profile=profile, session=session,
        started_monotonic=started, status="PASS" if result else "FAIL",
        runner_exit_code=0 if result else 1, errors=errors,
        firmware=args.firmware, image=args.image,
        extra={"timeout": timed_out, "ok_observed": ok_observed})

    print(json.dumps({
        "event": "case", "result": "PASS" if result else "FAIL",
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "ok_observed": ok_observed,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
    }))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--qemu")
    parser.add_argument("--log-dir")
    parser.add_argument("--diagnostic-dtb", default=None)
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive, supplied by Make")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                             "default"),
                        help="profile name recorded in the report")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("aarch64_m3_probe: self-test passed")
        return 0
    if not all((args.firmware, args.image, args.qemu, args.log_dir)):
        parser.error("--firmware, --image, --qemu, and --log-dir are required outside --self-test")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return 0 if run_case(args, build_dir=args.build_dir,
                         profile=args.profile) else 1


if __name__ == "__main__":
    sys.exit(main())
