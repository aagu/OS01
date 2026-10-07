#!/usr/bin/env python3
"""Run systest repeatedly from the real terminal/ash path (not systest init).

An isolated *private* copy of the normal image keeps filesystem tests from
modifying the normal image.  A shell ``printf`` completion marker is split
from its numeric argument so the serial echo of the typed command cannot
satisfy the completion check, and each stage's marker is matched **only
after that command's output cursor** — a marker echoed earlier in the
transcript can never satisfy a later stage.

The common process/output/archive layer is ``ProcessSession`` (spec §5.3)
and ``RunArchive`` (spec §7.2): every QEMU run is archived under
``build/<profile>/logs/tests/systest-repeat/<UTC>-<id>/`` with
``stdout.log``, ``stderr.log`` and an atomic ``result.json``.  This is a
**legacy/repeat** suite — it drives the *normal* image through the shell
and publishes no guest v1 cases — so the report uses the explicit
``count_unit="suite"`` adapter with ``declared_ids``/``observed_ids`` left
``None``; no guest v1 records are fabricated.

Exit codes (spec §6.2): 0=PASS, 1=FAIL/TIMEOUT, 2=config/env ERROR,
130=Ctrl-C (SIGINT).

Run directly::

    python3 qemutests/x86_64_systest_repeat.py \
        --disk build/x86_64-clang/image/disk.img \
        --firmware build/x86_64-clang/firmware/OVMF.fd \
        --qemu qemu-system-x86_64 --smp 4 --memory 512M --timeout 180 \
        --build-dir build/x86_64-clang --profile x86_64-clang
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, List, Optional

# Running this file as a script (``python3 qemutests/x86_64_systest_repeat.py``)
# puts the *qemutests* directory on ``sys.path[0]`` — not the repo root — so
# ``import qemutests.*`` would fail.  Bootstrap the repo root explicitly so
# the runner works both as a script and as ``python3 -m qemutests....``.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

try:  # noqa: E402 — path bootstrap must precede this import
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

try:  # noqa: E402
    from qemutests.harness.result import RunArchive, RunReport
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]


# Suite id for the archive path and the RunReport.  This suite publishes no
# guest v1 protocol, so every report is counted by ``suite``.
SUITE = "systest-repeat"

# Post-completion observation window (spec §5.3): a kernel corruption that
# arrives right after the last stage must still fail the run.
OBSERVE_SECONDS = 1.0

# (shell command, expected number of complete passing systest runs).
#   ``systest``                 -> 1
#   ``for i in 1..3; ...``      -> 1  (ash has no ``..`` range; it runs once)
#   ``for i in 1 2 3; ...``     -> 3
COMMANDS = (
    (b"systest", 1),
    (b"for i in 1..3; do systest; done", 1),
    (b"for i in 1 2 3; do systest; done", 3),
)

# Boot is complete once the normal inittab has launched the ash shell.
BOOT_MARKERS = ("built-in shell (ash)", "# ")

# Kernel corruption the suite must reject outright.
CORRUPTION_MARKERS = ("VFS: find_mount: CORRUPT", "PF-KRN:")

_RESULT_RE = re.compile(r"\[SYS TEST\] RESULT: (\d+) passed, (\d+) failed")


# ────────────────────────────────────────────────────────────────────
# Pure contract helpers (the fixture surface)
# ────────────────────────────────────────────────────────────────────


def completion_marker(stage: int) -> str:
    """The concrete completion marker emitted for ``stage``."""
    return f"__REPEAT_DONE_{stage}__"


def command_line(command: bytes, stage: int) -> bytes:
    """The exact bytes to send for a stage.

    The command is followed by a ``printf`` that emits the completion
    marker.  The stage **number is passed as a separate printf argument**,
    so the serial echo of this line shows the template
    ``__REPEAT_DONE_%d__`` — the echo can never satisfy the completion
    check, only the real marker output can.
    """
    return (
        command
        + b"; printf '\\n__REPEAT_DONE_%d__\\n' "
        + str(stage).encode()
        + b"\n"
    )


def completion_reached(window: str, stage: int) -> bool:
    """True iff ``stage``'s completion marker is in this command's window.

    ``window`` is the text after the command's cursor (``text[cursor:]`` as
    handed to the predicate by ``ProcessSession.wait_for``), never the whole
    transcript — so a marker echoed earlier cannot satisfy a later stage.
    """
    return completion_marker(stage) in window


def boot_reached(window: str) -> bool:
    """True iff every boot marker has been observed."""
    return all(marker in window for marker in BOOT_MARKERS)


def corruption_in(text: str) -> Optional[str]:
    """Return the first kernel-corruption marker found in ``text``."""
    for marker in CORRUPTION_MARKERS:
        if marker in text:
            return marker
    return None


def stage_result_errors(window: str, expected: int) -> List[str]:
    """Validate one stage's window against its strict run count.

    ``expected`` complete passing suites must be present; every result must
    report ``passed > 0`` and ``failed == 0`` (a run that executed nothing,
    or that failed, does not count).
    """
    results = _RESULT_RE.findall(window)
    errors: List[str] = []
    if len(results) != expected:
        errors.append(
            f"expected {expected} complete systest run(s), got {len(results)}"
        )
    for passed, failed in results:
        if int(passed) == 0 or int(failed) != 0:
            errors.append(
                f"stage reported passed={passed} failed={failed} "
                f"(want passed>0 and failed=0)"
            )
    return errors


# ────────────────────────────────────────────────────────────────────
# QEMU argv / report helpers
# ────────────────────────────────────────────────────────────────────


def qemu_argv(qemu: str, firmware: str, image: str,
              smp: int, memory: str) -> List[str]:
    """Assemble the explicit QEMU argv for the normal-image boot.

    Mirrors the historical recipe (q35 + OVMF pflash + ahci root disk +
    virtio-rng for the AAGU STRONG-only AT_RANDOM source + ``-serial stdio``
    + ``-no-reboot``).  The disk is the caller's *private copy*.
    """
    return [
        qemu,
        "-M", "q35",
        "-m", str(memory),
        "-smp", str(smp),
        # virtio-rng-pci: gives OVMF a UEFI GetRNG source; without it AAGU-5
        # STRONG-only AT_RANDOM fails closed and exec/init never comes up.
        "-device", "virtio-rng-pci",
        "-drive", f"if=pflash,format=raw,readonly=on,file={firmware}",
        "-drive", f"file={image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
    ]


def _sha256_path(path) -> Optional[str]:
    """Return the hex SHA-256 of ``path``, or None if it is unreadable."""
    if path is None:
        return None
    try:
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except (OSError, TypeError):
        return None


def _memory_mib(memory: str) -> int:
    """Best-effort MiB integer from a QEMU memory string (``512M`` -> 512)."""
    text = str(memory).strip().upper()
    try:
        if text.endswith("G"):
            return int(float(text[:-1]) * 1024)
        if text.endswith("M"):
            return int(float(text[:-1]))
        if text.endswith("K"):
            return max(0, int(float(text[:-1]) / 1024))
        return int(float(text))
    except ValueError:
        return 0


def _git_revision():
    """Best-effort (revision, dirty) for the RunReport."""
    rev, dirty = "unknown", False
    try:
        r = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=_ROOT,
            capture_output=True, text=True, timeout=5)
        if r.returncode == 0 and r.stdout.strip():
            rev = r.stdout.strip()
        d = subprocess.run(
            ["git", "status", "--porcelain"], cwd=_ROOT,
            capture_output=True, text=True, timeout=5)
        if d.returncode == 0:
            dirty = bool(d.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return rev, dirty


def _write_report(
    archive,
    *,
    argv: List[str],
    profile: str,
    smp: int,
    memory: str,
    disk: str,
    private_disk: Optional[Path],
    firmware: str,
    image_sha256_before: Optional[str],
    started_monotonic: float,
    session,
    status: str,
    runner_exit_code: int,
    errors: List[str],
    stages: Optional[List[dict]] = None,
) -> None:
    """Persist the suite-unit RunReport; best-effort (never raises)."""
    if archive is None or RunReport is None:
        return
    try:
        rev, dirty = _git_revision()
        outcomes = [{
            "errors": list(errors),
            "stages": list(stages or []),
            "private_image": (str(private_disk) if private_disk else None),
        }]
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision=rev,
            git_dirty=dirty,
            profile=profile,
            suite=SUITE,
            request=None,
            # Legacy/repeat suite: no guest v1 cases exist, so the report
            # counts by suite and fabricates no case ids.
            declared_ids=None,
            observed_ids=None,
            argv=list(argv),
            cpu_count=int(smp),
            memory_mib=_memory_mib(memory),
            tool_versions={},
            firmware_path=firmware or None,
            firmware_sha256_before=None,
            firmware_sha256_after=_sha256_path(firmware),
            image_path=disk or None,
            image_sha256_before=image_sha256_before,
            image_sha256_after=_sha256_path(private_disk),
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=max(0.0, time.monotonic() - started_monotonic),
            runner_exit_code=runner_exit_code,
            child_exit_code=(session.returncode if session is not None else None),
            stopped_by_runner=bool(
                session is not None and session.stopped_by_runner),
            status=status,
            count_unit="suite",
            outcomes=outcomes,
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:  # noqa: BLE001 — evidence must not mask the run
        print(f"[{SUITE}] warning: failed to write RunReport: {exc}",
              file=sys.stderr)


# ────────────────────────────────────────────────────────────────────
# Runner
# ────────────────────────────────────────────────────────────────────


def run_repeat(
    *,
    disk: str,
    firmware: str,
    qemu: str = "qemu-system-x86_64",
    memory: str = "512M",
    smp: int = 4,
    timeout_s: float = 180.0,
    build_dir: Optional[str] = None,
    profile: str = "default",
    session_factory: Optional[Callable] = None,
    run_dir: Optional[Path] = None,
    observe_s: float = OBSERVE_SECONDS,
) -> int:
    """Boot the private image copy and drive systest through ash.

    Returns one of 0 (PASS), 1 (FAIL/TIMEOUT) or 2 (ERROR).  The
    KeyboardInterrupt path (Ctrl-C) returns 130.
    """
    if session_factory is None:
        session_factory = ProcessSession

    started_monotonic = time.monotonic()
    archive = None
    if build_dir and RunArchive is not None:
        archive = RunArchive.create(build_dir=Path(build_dir), suite=SUITE)
        run_dir = archive.run_dir
    elif run_dir is None:
        run_dir = Path(tempfile.mkdtemp(prefix="os01-repeat-log-"))
    run_dir.mkdir(parents=True, exist_ok=True)

    # The *input* hash (the normal image Make built) — recorded before the run.
    image_sha256_before = _sha256_path(disk)

    # Isolated private copy: filesystem tests must not touch the normal image.
    image_tmpdir = Path(tempfile.mkdtemp(prefix="os01-repeat-img-"))
    private_disk = image_tmpdir / "disk.img"
    shutil.copyfile(disk, private_disk)

    argv = qemu_argv(qemu, firmware, str(private_disk), smp, memory)

    session = None
    errors: List[str] = []
    stages: List[dict] = []
    timed_out = False
    early_exit = False
    try:
        session = session_factory(
            argv=argv, run_dir=run_dir, timeout_s=float(timeout_s),
            writable_stdin=True,
        )
        session.start()

        # 1. Wait for the normal ash prompt.
        session.wait_for(boot_reached)
        if session.timed_out:
            errors.append("timed out waiting for the ash shell prompt")

        # 2. Run each stage, matching its marker only in its own window.
        for stage, (command, expected) in enumerate(COMMANDS):
            if errors:
                break
            corruption = corruption_in(session.text)
            if corruption:
                errors.append(f"kernel corruption detected: {corruption}")
                break
            # Consume the cursor *before* sending: the marker must be found
            # only in the output this command produces.
            session.send(command_line(command, stage))
            window = session.wait_for(
                lambda candidate, stage=stage: completion_reached(candidate, stage))
            if session.timed_out:
                stages.append({
                    "stage": stage,
                    "command": command.decode("utf-8", "replace"),
                    "expected": expected,
                    "timed_out": True,
                    "results": [],
                })
                break
            corruption = corruption_in(window)
            if corruption:
                errors.append(f"kernel corruption detected: {corruption}")
                break
            errors.extend(stage_result_errors(window, expected))
            stages.append({
                "stage": stage,
                "command": command.decode("utf-8", "replace"),
                "expected": expected,
                "results": _RESULT_RE.findall(window),
            })

        # 3. Post-completion observation window (late corruption).
        tail = session.observe(observe_s)
        corruption = corruption_in(tail)
        if corruption:
            errors.append(
                f"kernel corruption in observation window: {corruption}")

        timed_out = bool(session.timed_out)
        # A QEMU that exited on its own (returncode set, no runner-owned
        # stop) died before completing — that is a FAIL, not a deadline.
        early_exit = bool(
            timed_out and not session.stopped_by_runner
            and session.returncode is not None)

        session.stop()
        session.close()
    except KeyboardInterrupt:
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        _write_report(
            archive, argv=argv, profile=profile, smp=smp, memory=memory,
            disk=disk, private_disk=private_disk, firmware=firmware,
            image_sha256_before=image_sha256_before,
            started_monotonic=started_monotonic, session=session,
            status="ERROR", runner_exit_code=130,
            errors=["interrupted (Ctrl-C)"], stages=stages)
        shutil.rmtree(image_tmpdir, ignore_errors=True)
        return 130
    except Exception as exc:  # noqa: BLE001 — launch/OS error is ERROR
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        print(f"ERROR: {SUITE} could not run: {exc}", file=sys.stderr)
        _write_report(
            archive, argv=argv, profile=profile, smp=smp, memory=memory,
            disk=disk, private_disk=private_disk, firmware=firmware,
            image_sha256_before=image_sha256_before,
            started_monotonic=started_monotonic, session=session,
            status="ERROR", runner_exit_code=2,
            errors=[f"runner error: {exc}"], stages=stages)
        shutil.rmtree(image_tmpdir, ignore_errors=True)
        return 2

    ok = not errors and not timed_out and not early_exit
    if ok:
        status = "PASS"
    elif early_exit:
        status = "FAIL"
    elif timed_out:
        status = "TIMEOUT"
    else:
        status = "FAIL"

    report_errors = list(errors)
    if early_exit:
        report_errors.append("QEMU exited before completing all stages")
    elif timed_out:
        report_errors.append("timed out before completing all stages")
    for error in report_errors:
        print(f"ERROR: {SUITE}: {error}", file=sys.stderr)

    _write_report(
        archive, argv=argv, profile=profile, smp=smp, memory=memory,
        disk=disk, private_disk=private_disk, firmware=firmware,
        image_sha256_before=image_sha256_before,
        started_monotonic=started_monotonic, session=session,
        status=status, runner_exit_code=(0 if ok else 1),
        errors=report_errors, stages=stages)

    shutil.rmtree(image_tmpdir, ignore_errors=True)

    if ok:
        print(f"SMP={smp}, all {len(COMMANDS)} stages passed "
              f"(1/1/3 systest runs)")
        return 0
    print(f"ERROR: {SUITE} failed ({status})", file=sys.stderr)
    return 1


def _validate_paths(disk: str, firmware: str) -> Optional[str]:
    """Return an error string if the disk/firmware inputs are unusable."""
    if not disk or not os.path.isfile(disk):
        return f"disk image is not a readable file: {disk!r}"
    if not firmware or not os.path.isfile(firmware):
        return f"firmware is not a readable file: {firmware!r}"
    return None


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run systest repeatedly from the real terminal/ash path.")
    parser.add_argument("--disk", required=True,
                        help="normal-image disk copy to boot")
    parser.add_argument("--firmware", required=True,
                        help="path to the OVMF firmware file")
    parser.add_argument("--qemu", default="qemu-system-x86_64",
                        help="QEMU binary to invoke")
    parser.add_argument("--smp", type=int, default=4,
                        help="effective QEMU CPU count (-smp)")
    parser.add_argument("--memory", default="512M",
                        help="QEMU memory size (e.g. 512M)")
    parser.add_argument("--timeout", type=float, default=180.0,
                        help="single monotonic budget in seconds")
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive (optional)")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                             "default"),
                        help="profile name recorded in the report")
    args = parser.parse_args(argv)

    bad = _validate_paths(args.disk, args.firmware)
    if bad is not None:
        print(f"ERROR: {SUITE}: {bad}", file=sys.stderr)
        return 2
    if args.smp < 1:
        print(f"ERROR: {SUITE}: --smp must be >= 1, got {args.smp}",
              file=sys.stderr)
        return 2

    return run_repeat(
        disk=args.disk,
        firmware=args.firmware,
        qemu=args.qemu,
        memory=args.memory,
        smp=args.smp,
        timeout_s=args.timeout,
        build_dir=args.build_dir,
        profile=args.profile,
    )


if __name__ == "__main__":
    sys.exit(main())
