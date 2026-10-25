#!/usr/bin/env python3
"""Acceptance harness for the isolated x86 destructive dispatch probes.

Task 5 of the intr-dispatch phase-2 plan (spec §9.3) needs *isolated*
evidence for the two fatal entries that cannot be exercised through the
ordinary boot or selftest path:

  * ``bp``  — a kernel-mode ``int3`` (``#BP``) that reaches the original
    fatal handler, and
  * ``nmi`` — a real hardware NMI delivered over QMP ``inject-nmi``.

Both cases run against a dedicated ``intr-x86-fault-<case>`` kernel and
image variant; the normal image and the normal kernel never contain the
probe.  The runner judges the run from the *guest serial stream* only:
the QMP response has no defined order relative to the serial bytes, so
the complete fatal diagnostic in the serial stream is the ordering
boundary (spec §9.3).

Judgement rules (spec §9.3):

  * exactly one ``armed`` marker, exactly one complete fatal diagnostic,
    armed before the fatal;
  * the ``#BP`` saved RIP must equal the resume label resolved from the
    *variant ELF* symbol table — never a hardcoded address, and never
    the ``int3``'s own address;
  * after the complete diagnostic a bounded observation window
    (>= 1 s) must show no new complete heartbeat, no continuation
    marker, no second fatal and no panic;
  * a timeout is never a pass, a silent guest is never a pass, and a
    normal completion is never a pass;
  * the runner must own the QEMU stop — a QEMU that exited on its own is
    weak evidence.

The decision logic is pure (:func:`fault_evidence`) so the unit fixtures
can drive it with synthetic logs; the QEMU/QMP plumbing is thin and
injectable (``session_factory`` / ``qmp_factory`` /
``symbol_resolver``).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

# Running this file as a script puts ``qemutests/`` on ``sys.path[0]`` —
# not the repo root — so bootstrap the repo root explicitly.
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
SUITE = "intr-x86"

CASE_BP = "bp"
CASE_NMI = "nmi"
CASES = (CASE_BP, CASE_NMI)

# The isolated kernel/image variant slug.  MUST match the slug built by
# mk/profiles/x86_64-clang.mk (``intr-x86-fault-$(X86_INTR_FAULT_TEST)``)
# and mk/project.mk's IMAGE_VARIANT composition; the build-contract
# bucket pins the two spellings together.
VARIANT_SLUG_FMT = "intr-x86-fault-%s"

# The ELF symbol that names the resume label placed IMMEDIATELY AFTER
# the probe's ``int3``.  The saved RIP of the ``#BP`` must equal it.
RESUME_SYMBOL = "x86_intr_fault_bp_resume"

# Post-diagnostic observation window (spec §9.3: at least one second).
# The window is bounded by the runner's own finite deadline.
OBSERVE_SECONDS = 1.5

# QMP socket connect/command timeout (seconds).  The injection is a
# one-shot command; a command that has not answered within this budget
# is a FAILURE, never a pass.
QMP_TIMEOUT_S = 15.0


# ── Guest markers (kernel/arch/x86_64/intr/fault_probe.c) ───────
_ARMED_TEXT = {
    CASE_BP: "[x86-intr-fault] bp armed",
    CASE_NMI: "[x86-intr-fault] nmi armed",
}
_RETURNED_TEXT = {
    CASE_BP: "[x86-intr-fault] bp returned",
    CASE_NMI: "[x86-intr-fault] nmi returned",
}

# The armed marker must be a COMPLETE line: a partially written marker
# means the probe never finished arming.
_ARMED_RE = {
    case: re.compile(r"^" + re.escape(text) + r"$", re.MULTILINE)
    for case, text in _ARMED_TEXT.items()
}
_RETURNED_RE = re.compile(
    "|".join(re.escape(text) for text in _RETURNED_TEXT.values()))
_PRECONDITION_RE = re.compile(r"\[x86-intr-fault\] precondition FAIL")

# A heartbeat is only "complete" when the whole line is there.  A
# heartbeat interrupted by the injected NMI is truncated in the serial
# stream and must NOT be mistaken for a post-diagnostic continuation.
_HEARTBEAT_RE = re.compile(
    r"^\[x86-intr-fault\] nmi heartbeat \d+$", re.MULTILINE)

# The probe prints its isolated KERNEL_VARIANT_NAME after random_init().
_VARIANT_RE = {
    case: re.compile(
        r"^\[x86-intr-fault\] variant=" + VARIANT_SLUG_FMT % case + r"$",
        re.MULTILINE)
    for case in CASES
}

# The boot stage the probe ran at.  The probe prints this marker only
# after its own precondition check passed, and the precondition
# (`num_cpus == 1`) can only hold AFTER x86_64_boot_percpu() published
# num_cpus.  Requiring the marker therefore CHECKS the placement at
# run time instead of asserting it in prose.
_STAGE_RE = re.compile(
    r"^\[x86-intr-fault\] stage=post-percpu$", re.MULTILINE)

# Boot markers that can only be produced AFTER the probe point (the
# probe runs after x86_64_boot_percpu() and before the FS/TTY bring-up,
# the AP bring-up and the user-space init).  Seeing one of them means
# the guest continued past the probe.
_CONTINUE_RE = re.compile(
    r"OS01 Init v1\.0|"
    r"tty: console TTY created|"
    r"devfs: /dev/null|"
    r"subsys: percpu|"
    r"\[smp\]|"
    r"\[selftest\]")

# The kernel panic marker (both the bracketed and the capitalised
# forms) — a panic anywhere in the stream is a failure.
_PANIC_RE = re.compile(r"kernel panic", re.IGNORECASE)


# ── Fatal diagnostics (kernel/arch/x86_64/intr/trap.c) ───────────
#
# The CURRENT logger line format, reproduced exactly:
#
#   log_err("do_int3(3),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n", ...)
#   log_err("do_nmi(2),ERROR_CODE:%#018lx,RSP:%#018lx,RIP:%#018lx\n", ...)
#
# ``%#018lx`` is the kernel's own printf rendering of the fields: the
# archived production output shows the zero error code as
# ``0x0000000000000000`` (the ``0x`` prefix IS present, 16 hex digits
# after it) and the non-zero RSP/RIP the same way. The parser also
# accepts the historical bare-zero spelling; the field VALUE is what
# the evidence checks, while width and complete addresses stay strict.
_ZERO_FIELD = r"(?:0{18}|0x0{16})"
_ADDR_FIELD = r"0x[0-9a-f]{16}"

_FATAL_RE = {
    CASE_BP: re.compile(
        r"do_int3\(3\),ERROR_CODE:(" + _ZERO_FIELD + r"),"
        r"RSP:(" + _ADDR_FIELD + r"),"
        r"RIP:(" + _ADDR_FIELD + r")\n"),
    CASE_NMI: re.compile(
        r"do_nmi\(2\),ERROR_CODE:(" + _ZERO_FIELD + r"),"
        r"RSP:(" + _ADDR_FIELD + r"),"
        r"RIP:(" + _ADDR_FIELD + r")\n"),
}

# Recognize production exception starts even when their fields are truncated.
# No line anchor: an NMI may interrupt a heartbeat's serial output.
_FATAL_PREFIX_RE = re.compile(
    r"do_(?:divide_error|debug|nmi|int3|overflow|bounds|undefined_opcode|"
    r"dev_not_available|double_fault|coprocessor_segment_overrun|invalid_TSS|"
    r"segment_not_present|stack_segment_fault|general_protection|page_fault|"
    r"x87_FPU_error|alignment_check|machine_check|SIMD_exception|"
    r"virtualization_exception)\(")


def fault_evidence(text: str, *, case: str,
                    resume_addr: int | None = None) -> bool:
    """Return True iff ``text`` proves the destructive dispatch ran.

    ``text`` is the guest serial stream of one case.  ``case`` selects
    the fatal record shape (``bp`` → ``do_int3(3)``, ``nmi`` →
    ``do_nmi(2)``).  ``resume_addr`` is the ELF-resolved address of the
    resume label; it is REQUIRED for the ``#BP`` case (the saved RIP
    must equal it) and ignored for the NMI case.

    The function is host-pure: it is the single success gate used by
    both the unit fixtures and the live QEMU driver.
    """
    if case not in _FATAL_RE:
        raise ValueError(f"unknown case {case!r}")

    # The kernel's serial output is LF-only, but normalise CR anyway so
    # the regex anchors behave the same for a saved log.
    text = text.replace("\r", "")

    # Probe-side rejections: a precondition failure or a `returned`
    # marker means the probe did not behave as the evidence requires.
    if _PRECONDITION_RE.search(text):
        return False
    if _RETURNED_RE.search(text):
        return False
    if _PANIC_RE.search(text):
        return False

    # Exactly one armed marker ...
    armed = list(_ARMED_RE[case].finditer(text))
    if len(armed) != 1:
        return False

    # ... and exactly one complete fatal diagnostic.  A malformed second
    # fatal still counts as a duplicate.
    fatals = list(_FATAL_RE[case].finditer(text))
    if len(fatals) != 1:
        return False
    if len(list(_FATAL_PREFIX_RE.finditer(text))) != 1:
        return False

    # The armed marker must precede the fatal.
    if armed[0].start() >= fatals[0].start():
        return False

    # The guest must be the ISOLATED variant — a normal kernel never
    # prints this marker and never carries the probe.
    variants = list(_VARIANT_RE[case].finditer(text))
    if len(variants) != 1:
        return False
    if variants[0].start() > armed[0].start():
        return False

    stages = list(_STAGE_RE.finditer(text))
    if len(stages) != 1 or not (
            variants[0].end() < stages[0].start() < armed[0].start()):
        return False

    # Field semantics: both entries carry an explicit zero error code.
    error_code, _rsp, rip = fatals[0].groups()
    if int(error_code, 16) != 0:
        return False

    # The #BP saved RIP must be the resume label that follows the int3
    # — resolved from the variant ELF, never hardcoded, and never the
    # int3's own address.
    if case == CASE_BP:
        if resume_addr is None:
            return False
        if int(rip, 16) != resume_addr:
            return False

    # Nothing may run between the armed marker and the fatal either:
    # the fatal is supposed to be the immediate consequence.
    between = text[armed[0].end():fatals[0].start()]
    if _CONTINUE_RE.search(between):
        return False

    # Nothing may follow the complete diagnostic inside the observation
    # window: no new complete heartbeat, no continuation marker, no
    # second fatal, no panic.
    tail = text[fatals[0].end():]
    if _HEARTBEAT_RE.search(tail):
        return False
    if _CONTINUE_RE.search(tail):
        return False
    if _PANIC_RE.search(tail):
        return False
    if _FATAL_PREFIX_RE.search(tail):
        return False
    return True


# ── ELF symbol resolution ───────────────────────────────────────

_NM_LINE_RE = re.compile(r"^([0-9a-fA-F]+)\s+([A-Za-z?])\s+(\S+)$")


def parse_nm_symbols(text: str) -> dict[str, int]:
    """Parse ``llvm-nm`` output into ``{name: address}``.

    Undefined symbols (``U``/``u``) are skipped: an undefined name has
    no address and must never satisfy the resume-label check.
    """
    symbols: dict[str, int] = {}
    for line in text.splitlines():
        m = _NM_LINE_RE.match(line.strip())
        if not m:
            continue
        addr = int(m.group(1), 16)
        typ = m.group(2)
        name = m.group(3)
        if typ in ("U", "u"):
            continue
        symbols.setdefault(name, addr)
    return symbols


def resolve_symbol(nm_text: str, name: str) -> int | None:
    """Return the address of ``name`` in ``nm_text`` (None if absent)."""
    return parse_nm_symbols(nm_text).get(name)


def _make_nm_resolver(nm: str):
    """Build the default symbol resolver: run ``nm <elf>`` and parse it."""
    def resolve(elf_path: str, symbol: str) -> int | None:
        proc = subprocess.run([nm, elf_path], capture_output=True, text=True,
                              timeout=120)
        if proc.returncode != 0:
            raise RuntimeError(
                f"{nm} failed on {elf_path}: {proc.stderr.strip()}")
        return resolve_symbol(proc.stdout, symbol)
    return resolve


# ── QMP ─────────────────────────────────────────────────────────


class QmpClient:
    """Minimal QMP client over a private unix socket.

    Mirrors the client used by the terminal-present suite: connect,
    read the greeting, send ``qmp_capabilities`` (the handshake), then
    issue commands.  Only the ``inject-nmi`` command is needed here.
    """

    def __init__(self, path: str, timeout: float = QMP_TIMEOUT_S):
        self.path = path
        self.timeout = timeout
        self.sock = None
        self._buffer = b""

    def connect(self, *, deadline: float | None = None) -> None:
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        err = None
        while time.monotonic() < deadline:
            attempt = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                attempt.settimeout(self._remaining(deadline))
                attempt.connect(self.path)
                self.sock = attempt
                break
            except OSError as exc:
                attempt.close()
                err = exc
                time.sleep(min(0.1, max(0, deadline - time.monotonic())))
        if self.sock is None:
            raise RuntimeError(f"QMP connect failed: {err}")
        try:
            greeting = self._read_message(deadline)
            if "QMP" not in greeting:
                raise RuntimeError("QMP greeting missing")
            self.execute("qmp_capabilities", deadline=deadline)
        except BaseException:
            self.close()
            raise

    def _remaining(self, deadline: float) -> float:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("QMP total deadline expired")
        return remaining

    def _read_message(self, deadline: float) -> dict:
        if self.sock is None:
            raise RuntimeError("QMP not connected")
        while True:
            self.sock.settimeout(self._remaining(deadline))
            if b"\n" not in self._buffer:
                data = self.sock.recv(4096)
                if not data:
                    raise RuntimeError("QMP connection closed")
                self._buffer += data
                if len(self._buffer) > 1024 * 1024:
                    raise RuntimeError("QMP message too large")
                continue
            line, self._buffer = self._buffer.split(b"\n", 1)
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def execute(self, cmd: str, args: dict | None = None, *,
                deadline: float | None = None) -> dict:
        if self.sock is None:
            raise RuntimeError("QMP not connected")
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        request: dict = {"execute": cmd}
        if args is not None:
            request["arguments"] = args
        self.sock.settimeout(self._remaining(deadline))
        self.sock.sendall((json.dumps(request) + "\n").encode())
        msg = self._read_message(deadline)
        if "error" in msg:
            raise RuntimeError(f"QMP error {cmd}: {msg['error']}")
        if "return" not in msg:
            raise RuntimeError(f"QMP response missing return: {msg}")
        return msg["return"]

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None


# ── QEMU command line ───────────────────────────────────────────


def qemu_command(args, qmp_socket: str) -> list[str]:
    """The single-CPU QEMU command line used by both cases.

    ``-snapshot`` keeps the isolated variant image read-only, ``-smp 1``
    gives the single-CPU boot spec §9.3 asks for, and ``-qmp`` opens the
    PRIVATE unix socket the NMI case injects through.
    """
    return [
        args.qemu,
        "-M", "q35",
        "-m", "512",
        "-smp", "1",
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.firmware}",
        "-drive", f"file={args.image},format=raw,if=none,id=disk",
        "-device", "ahci,id=ahci",
        "-device", "ide-hd,drive=disk,bus=ahci.0",
        "-object", "rng-random,filename=/dev/urandom,id=rng0",
        "-device", "virtio-rng-pci,rng=rng0",
        "-snapshot",
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-no-shutdown",
        "-qmp", f"unix:{qmp_socket},server,nowait",
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
                        image=None, extra=None) -> bool:
    """Persist a suite-unit RunReport; the suite has no guest v1 cases."""
    if archive is None or RunReport is None:
        return True
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
        return True
    except Exception as exc:  # noqa: BLE001
        print(f"[{SUITE}] ERROR: failed to write RunReport: {exc}",
              file=sys.stderr)
        return False


# ── The case driver ─────────────────────────────────────────────


def run_case(args, *, session_factory=None, qmp_factory=None,
             symbol_resolver=None, build_dir=None, profile="default") -> int:
    """Run one destructive case and judge it.

    Returns 0 (PASS), 1 (FAIL) or 2 (configuration / environment ERROR).
    The archived ``status`` always matches the returned code.
    """
    started = time.monotonic()
    case = getattr(args, "case", None)
    if case not in CASES:
        print(f"[{SUITE}] ERROR: unknown case {case!r}")
        return 2

    if session_factory is None:
        session_factory = ProcessSession
    if session_factory is None:
        print("[intr-x86] ERROR: ProcessSession is unavailable")
        return 2

    archive = None
    if build_dir and RunArchive is not None:
        archive = RunArchive.create(Path(build_dir), SUITE)
        run_dir = archive.run_dir
    else:
        run_dir = Path(args.log_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = run_dir / "metadata.json"

    # The resume label comes from the VARIANT ELF — never from a
    # hardcoded address of an older build.
    resume_addr = None
    if case == CASE_BP:
        resolver = symbol_resolver or _make_nm_resolver(args.nm)
        try:
            resume_addr = resolver(args.elf, RESUME_SYMBOL)
        except Exception as exc:  # noqa: BLE001
            _write_suite_report(
                archive, argv=[], profile=profile, session=None,
                started_monotonic=started, status="ERROR", runner_exit_code=2,
                errors=[f"resume symbol resolution failed: {exc}"],
                firmware=args.firmware, image=args.image)
            print(f"[{SUITE}] ERROR: resume symbol resolution failed: {exc}")
            return 2
        if resume_addr is None:
            _write_suite_report(
                archive, argv=[], profile=profile, session=None,
                started_monotonic=started, status="ERROR", runner_exit_code=2,
                errors=[f"resume symbol {RESUME_SYMBOL} not found in "
                        f"{args.elf}"],
                firmware=args.firmware, image=args.image)
            print(f"[{SUITE}] ERROR: resume symbol {RESUME_SYMBOL} not found "
                  f"in {args.elf}")
            return 2

    # A private QMP socket.  The AF_UNIX path is length-limited, so the
    # default lives in a short temporary directory.
    socket_dir = None
    qmp_socket = getattr(args, "qmp_socket", None)
    if not qmp_socket:
        socket_dir = tempfile.mkdtemp(prefix="os01-qmp-")
        qmp_socket = str(Path(socket_dir) / "qmp.sock")

    command = qemu_command(args, qmp_socket)
    metadata = {
        "case": case,
        "command": command,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
        "elf": str(Path(args.elf).resolve()),
        "resume_symbol": RESUME_SYMBOL if case == CASE_BP else None,
        "resume_addr": (f"0x{resume_addr:016x}"
                        if resume_addr is not None else None),
        "qmp_socket": qmp_socket,
        "observe_seconds": OBSERVE_SECONDS,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    session = None
    errors: list[str] = []
    spawn_error = None
    qmp_error = None
    try:
        session = session_factory(argv=command, run_dir=run_dir,
                                  timeout_s=args.timeout)
        session.start()
        deadline = session.deadline
        # The session's wait_for() consumes EVERYTHING it has received
        # when its predicate matches, and its predicate only sees the
        # text AFTER the cursor.  A diagnostic that arrives in the same
        # read as the armed marker would therefore be swallowed by the
        # first wait, and a diagnostic split across two reads would be
        # split by the cursor.  ``seen`` carries the already-consumed
        # text so every predicate can be evaluated against the WHOLE
        # stream — the guest serial order, never the host's read
        # boundaries.
        seen = ""

        def matches(matcher):
            prefix = seen
            def check(suffix):
                return matcher.search(prefix + suffix) is not None
            return check

        # 1. The probe must arm before anything else happens.
        session.wait_for(matches(_ARMED_RE[case]))
        if session.timed_out:
            errors.append("timeout before the probe armed")
        else:
            seen = session.text
            # 2. The NMI case injects a REAL hardware NMI over QMP once
            #    the guest has armed.  The QMP response has no defined
            #    order relative to the serial bytes — the serial stream
            #    is the only ordering authority.
            if case == CASE_NMI:
                client = (qmp_factory or QmpClient)(qmp_socket)
                try:
                    client.connect(deadline=deadline)
                    client.execute("inject-nmi", deadline=deadline)
                except Exception as exc:  # noqa: BLE001
                    qmp_error = str(exc)
                finally:
                    try:
                        client.close()
                    except Exception:  # noqa: BLE001
                        pass
                if qmp_error is not None:
                    errors.append(f"QMP inject-nmi failed: {qmp_error}")
            # 3. The complete fatal diagnostic is the ordering boundary.
            if not errors:
                session.wait_for(matches(_FATAL_RE[case]))
                if session.timed_out:
                    errors.append("timeout before the fatal diagnostic")
                else:
                    # 4. The bounded post-diagnostic observation window.
                    if deadline - time.monotonic() < OBSERVE_SECONDS:
                        errors.append("timeout: insufficient fatal observation budget")
                    else:
                        session.observe(OBSERVE_SECONDS)
            # Include QMP, serial waiting and observation; cleanup has its
            # own bounded reap allowance and cannot turn timeout into PASS.
            if time.monotonic() >= deadline:
                errors.append("overall deadline expired")
    except Exception as exc:  # noqa: BLE001 — launch/OS error is ERROR
        spawn_error = exc
    finally:
        # The runner owns the stop: a QEMU that exited on its own is weak
        # evidence (the diagnostic may not have come from the kernel
        # under test).
        if session is not None:
            try:
                session.stop()
            except Exception:  # noqa: BLE001
                pass
            try:
                session.close()
            except Exception:  # noqa: BLE001
                pass
        if socket_dir is not None:
            shutil.rmtree(socket_dir, ignore_errors=True)

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

    accepted = fault_evidence(text, case=case, resume_addr=resume_addr)
    # Weak evidence: the guest never reached the fatal path and the
    # runner had to stop a QEMU that had already exited on its own.
    qemu_self_exited = accepted and not stopped_by_runner and not timed_out
    result = accepted and not errors and not timed_out and not qemu_self_exited

    metadata.update({
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "harness_terminated": stopped_by_runner,
        "evidence_accepted": accepted,
        "qemu_self_exited": qemu_self_exited,
        "result": "PASS" if result else "FAIL",
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    if qemu_self_exited:
        errors.append("QEMU self-exited after the fatal evidence "
                      "(no runner-owned stop)")
    if not accepted and not errors:
        errors.append("dispatch evidence not satisfied")
    report_written = _write_suite_report(
        archive, argv=command, profile=profile, session=session,
        started_monotonic=started, status="PASS" if result else "FAIL",
        runner_exit_code=0 if result else 1, errors=errors,
        firmware=args.firmware, image=args.image,
        extra={"case": case, "timeout": timed_out,
               "evidence_accepted": accepted,
               "qemu_self_exited": qemu_self_exited,
               "resume_addr": (f"0x{resume_addr:016x}"
                               if resume_addr is not None else None)})

    if not report_written:
        metadata["result"] = "ERROR"
        metadata["archive_error"] = "RunReport write failed"
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        return 2

    print(json.dumps({
        "event": "case", "case": case, "result": "PASS" if result else "FAIL",
        "timeout": timed_out,
        "returncode": (session.returncode if session is not None else None),
        "evidence_accepted": accepted,
        "qemu_self_exited": qemu_self_exited,
        "stdout": str(run_dir / "stdout.log"),
        "stderr": str(run_dir / "stderr.log"),
    }))
    return 0 if result else 1


def _sigterm_handler(signum, frame):  # noqa: ANN001, ARG001
    """Turn SIGTERM into SystemExit so the runner's ``finally`` block
    still reaps QEMU and removes the private QMP socket.

    A SIGKILL cannot be caught by any process — that limitation is
    documented in the task report — but SIGTERM is what a CI timeout
    and an interactive interrupt send, and it must not orphan a QEMU.
    """
    raise SystemExit(143)


def _install_sigterm_handler() -> None:
    """Install the SIGTERM handler (best effort)."""
    try:
        signal.signal(signal.SIGTERM, _sigterm_handler)
    except (ValueError, OSError, AttributeError):  # pragma: no cover
        # Not the main thread, or the platform has no SIGTERM.
        pass


def main() -> int:
    parser = argparse.ArgumentParser(
        description="OS01 isolated x86 #BP / NMI dispatch acceptance")
    parser.add_argument("--case", choices=list(CASES), required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--qemu")
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--elf")
    parser.add_argument("--nm", default="llvm-nm")
    parser.add_argument("--log-dir")
    parser.add_argument("--qmp-socket", default=None)
    parser.add_argument("--build-dir", default=None,
                        help="build dir for the run archive, supplied by Make")
    parser.add_argument("--profile", default=os.environ.get("OS01_PROFILE",
                                                             "default"),
                        help="profile name recorded in the report")
    args = parser.parse_args()
    _install_sigterm_handler()
    if not all((args.qemu, args.firmware, args.image, args.elf, args.log_dir)):
        parser.error("--qemu, --firmware, --image, --elf and --log-dir are "
                     "required")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return run_case(args, build_dir=args.build_dir, profile=args.profile)


if __name__ == "__main__":
    sys.exit(main())
