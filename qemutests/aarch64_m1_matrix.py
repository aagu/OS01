#!/usr/bin/env python3
"""Run the AArch64 M1 RAM/CPU matrix against normal and self-test images."""

import argparse
import hashlib
import json
import os
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

try:
    from aarch64_m1_evidence import m1_failure_evidence_ok
except ImportError:
    from qemutests.aarch64_m1_evidence import m1_failure_evidence_ok

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
SUITE = "aarch64-m1"


MATRIX = ((256, 1), (512, 1), (512, 2), (512, 4),
          (2048, 1), (2048, 2), (2048, 4), (4096, 1))


# Direct-runner exit codes (plan global constraint): 0=PASS,
# 1=FAIL/TIMEOUT, 2=configuration/environment ERROR, 130=Ctrl-C.

def _normalize_rc(rc: int) -> int:
    """Map a child process return code onto the 0/1/2 runner contract."""
    if rc == 0:
        return 0
    if rc == 2:
        return 2
    # Any other nonzero (including a signal-killed child) is a failure.
    return 1


def _aggregate_exit_codes(codes: list[int]) -> int:
    """Fold per-case exit codes into the runner's own exit code.

    0 when every case passed; 2 when any case hit a configuration /
    environment ERROR; otherwise 1 (the FAIL/TIMEOUT slot)."""
    if all(code == 0 for code in codes):
        return 0
    if any(code == 2 for code in codes):
        return 2
    return 1


def _sha256_path(path) -> str | None:
    if path is None:
        return None
    try:
        with open(path, "rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()
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
                        status, runner_exit_code, errors, cpu=1, firmware=None,
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
            argv=list(argv), cpu_count=int(cpu), memory_mib=512,
            tool_versions={},
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


def run_matrix(args: argparse.Namespace, *, build_dir=None,
               profile="default") -> int:
    script = Path(__file__).with_name("aarch64_uefi_smp.py")
    codes: list[int] = []
    for label, image, selftest in (
        ("normal", args.normal_image, False),
        ("selftest", args.selftest_image, True),
    ):
        for ram_mib, cpus in MATRIX:
            log_dir = Path(args.log_dir) / f"{label}-ram-{ram_mib}-cpus-{cpus}"
            log_dir.mkdir(parents=True, exist_ok=True)
            command = [
                sys.executable, str(script), "--firmware", args.firmware,
                "--image", image, "--qemu", args.qemu, "--log-dir", str(log_dir),
                "--cpus", str(cpus), "--repeat", "1", "--timeout", str(args.timeout),
                "--ram-mib", str(ram_mib), "--diagnostic-dtb", "auto",
                "--expect-m1",
            ]
            if build_dir:
                command += ["--build-dir", str(build_dir), "--profile", profile]
            if selftest:
                command.extend(("--expect-selftest", "--expect-gic", "--expect-clk"))
            print(f"M1 matrix: {label}, RAM={ram_mib} MiB, CPUs={cpus}")
            codes.append(
                _normalize_rc(subprocess.run(command, check=False).returncode))
    return _aggregate_exit_codes(codes)


def run_sparse_variant(args: argparse.Namespace, *, build_dir=None,
                       profile="default") -> int:
    script = Path(__file__).with_name("aarch64_uefi_smp.py")
    log_dir = Path(args.log_dir) / "sparse-ram-512-cpus-1"
    log_dir.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable, str(script), "--firmware", args.firmware,
        "--image", args.image, "--qemu", args.qemu, "--log-dir", str(log_dir),
        "--cpus", "1", "--repeat", "1", "--timeout", str(args.timeout),
        "--ram-mib", "512", "--diagnostic-dtb", "auto",
        "--expect-m1", "--m1-variant", "sparse", "--expect-selftest",
        "--expect-gic", "--expect-clk",
    ]
    if build_dir:
        command += ["--build-dir", str(build_dir), "--profile", profile]
    return _normalize_rc(subprocess.run(command, check=False).returncode)


def run_expected_failure(args: argparse.Namespace, *, session_factory=None,
                         build_dir=None, profile="default") -> int:
    """Run one intentional failure and verify its signature while QEMU lives.

    The expected-fatal adapter: evidence is ``m1_failure_evidence_ok`` over
    the captured log, and success additionally requires QEMU to *stay
    alive* for the 2-second grace period after the evidence — the runner
    then owns the stop.  A QEMU that exits early FAILs.

    Returns the plan's direct-runner exit code: 0 (PASS), 1 (FAIL /
    TIMEOUT) or 2 (configuration / environment ERROR).  The archived
    ``status`` always matches the returned code."""
    if session_factory is None:
        session_factory = ProcessSession
    started = time.monotonic()
    try:
        from aarch64_uefi_smp import generate_diagnostic_dtb, qemu_command
    except ImportError:
        from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb, qemu_command

    cpus = 2 if args.variant == "ap-bad-root" else 1
    ram_mib = 512

    # ``--diagnostic-dtb`` as an explicit PATH skips generation (tests);
    # otherwise the suite materializes its own DTB (unchanged behaviour).
    diagnostic = getattr(args, "diagnostic_dtb", None)
    if not diagnostic or diagnostic == "auto":
        case_root = Path(args.log_dir) / f"{args.variant}-ram-{ram_mib}-cpus-{cpus}"
        case_root.mkdir(parents=True, exist_ok=True)
        diagnostic = generate_diagnostic_dtb(args.qemu, str(case_root), cpus, ram_mib)

    archive = None
    if build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir) / f"{args.variant}-ram-{ram_mib}-cpus-{cpus}"
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"

    command_args = argparse.Namespace(
        qemu=args.qemu, firmware=args.firmware, image=args.image, ram_mib=ram_mib)
    command = qemu_command(command_args, cpus, diagnostic)
    metadata = {"command": command, "variant": args.variant, "cpus": cpus,
                "ram_mib": ram_mib, "image": str(Path(args.image).resolve()),
                "firmware": str(Path(args.firmware).resolve()),
                "diagnostic_dtb": diagnostic, "timeout_seconds": args.timeout}

    session = None
    evidence_at: float | None = None
    stayed_alive = False
    exited_early = False
    spawn_error = None
    try:
        if session_factory is None:
            raise RuntimeError("ProcessSession is unavailable")
        session = session_factory(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        session.wait_for(lambda text: m1_failure_evidence_ok(
            text, args.variant, cpus, stayed_alive=True))
        evidence_at = None if session.timed_out else time.monotonic()
        # The evidence must stay valid for a 2-second grace period during
        # which QEMU must not exit (stayed_alive).  observe returns early
        # if the child exits, so a self-exit shows up as a set returncode.
        session.observe(2.0)
        alive_after_window = session.returncode is None
        session.stop()
        session.close()
        stayed_alive = alive_after_window and session.stopped_by_runner
        exited_early = not stayed_alive
    except Exception as error:  # noqa: BLE001
        spawn_error = error
        if session is not None:
            try:
                session.close()
            except Exception:
                pass

    text = session.text if session is not None else ""
    metadata.update({"stayed_alive_after_evidence": stayed_alive,
                     "exited_before_acceptance": exited_early,
                     "returncode_after_harness_stop":
                         (session.returncode if session is not None else None)})

    if spawn_error is not None:
        print(json.dumps({"event": "spawn-error", "variant": args.variant,
                          "error": str(spawn_error)}))
        metadata.update({"result": "FAIL"})
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        _write_suite_report(
            archive, argv=command, profile=profile, session=session,
            started_monotonic=started, status="ERROR", runner_exit_code=2,
            errors=[f"spawn error: {spawn_error}"], cpu=cpus,
            firmware=args.firmware, image=args.image)
        return 2

    accepted = (not exited_early and stayed_alive
                and m1_failure_evidence_ok(text, args.variant, cpus,
                                           stayed_alive=True))
    metadata.update({"result": "PASS" if accepted else "FAIL"})
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    errors = []
    if not accepted:
        errors.append("expected-failure evidence not sustained while alive")
    _write_suite_report(
        archive, argv=command, profile=profile, session=session,
        started_monotonic=started, status="PASS" if accepted else "FAIL",
        runner_exit_code=0 if accepted else 1, errors=errors, cpu=cpus,
        firmware=args.firmware, image=args.image,
        extra={"variant": args.variant, "stayed_alive_after_evidence": stayed_alive,
               "exited_before_acceptance": exited_early})
    print(json.dumps({"event": "m1-negative-case", "variant": args.variant,
                      "cpus": cpus,
                      "result": "PASS" if accepted else "FAIL",
                      "log": str(run_dir / "stdout.log")}))
    return 0 if accepted else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--normal-image")
    parser.add_argument("--selftest-image")
    parser.add_argument("--variant", choices=("sparse", "arena-exhaust",
                                               "table-exhaust", "ap-bad-root"))
    parser.add_argument("--image", help="image for --variant failure/sparse run")
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive, supplied by Make")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                             "default"),
                        help="profile name recorded in the report")
    parser.add_argument("--diagnostic-dtb", default=None,
                        help="explicit PATH to a prebuilt DTB (skips generation)")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.variant:
        if not args.image:
            parser.error("--image is required with --variant")
    elif not args.normal_image or not args.selftest_image:
        parser.error("--normal-image and --selftest-image are required for the default matrix")
    elif args.image:
        parser.error("--image is only valid with --variant")
    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    if args.variant == "sparse":
        code = run_sparse_variant(args, build_dir=args.build_dir,
                                  profile=args.profile)
    elif args.variant:
        code = run_expected_failure(args, build_dir=args.build_dir,
                                    profile=args.profile)
    else:
        code = run_matrix(args, build_dir=args.build_dir,
                          profile=args.profile)
    return code


if __name__ == "__main__":
    sys.exit(main())
