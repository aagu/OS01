#!/usr/bin/env python3
"""OS01 test runner — launches QEMU with serial pipe, feeds commands, checks output.

Task 5 of the lightweight test-framework plan (spec §5.3): the
QEMU process boundary now delegates to ``ProcessSession`` so a
single deadline, monotonic-timeout, and process-group kill covers
every suite.  The existing ``TestRunner`` interface is preserved —
``start_qemu`` / ``send_line`` / ``read_until`` / ``cleanup`` are
the only public surface the suite functions consume.

Two transports remain available:

  * ``serial_stdio=True``  → ``-serial stdio`` and a writable
    stdin.  Used by ``test_gfx`` so the runner can type shell
    commands into BusyBox.  Serial output is captured by the
    ProcessSession's stdout pipe (``process.text``) and surfaced
    via ``read_until``.

  * ``serial_stdio=False`` (default) → ``-serial file:<path>`` and
    a closed stdin.  Used by ``test_boot``, ``test_systest``,
    ``test_inittab_phase``, ``test_network``, ``test_resolution``
    (and the driver-matrix delegate).  The runner polls the
    serial log file directly; ProcessSession still owns the QEMU
    process lifecycle, log files, and timeout.

``SMP`` (effective QEMU CPU count) is recorded on the TestRunner
and used in the ``-smp`` argv token.  When both ``SMP`` and the
legacy ``QEMU_SMP`` are set they must agree — a mismatch raises
``SystemExit`` before ``start_qemu`` is allowed to spawn QEMU.

``ProcessSession`` is overridable via the ``session_factory``
constructor kwarg.  Every fixture in
``qemutests/test_run_test_harness.py`` and
``qemutests/test_gfx_runner.py`` installs a ``FakeProcessSession``
factory to bypass the real subprocess.Popen boundary.
"""

import sys
import os
import hashlib
import subprocess
import re
import time
import argparse
import tempfile
import http.server
import socketserver
import threading
import traceback
from pathlib import Path

# mk/components/run.mk invokes this runner as a *script*
# (``python3 qemutests/run_test.py $(SUITE)``), which puts ``sys.path[0]``
# on the *qemutests* directory — not the repo root — so ``import
# qemutests.*`` below would fail and be swallowed by the ``except
# ImportError`` guards (leaving ``ProcessSession = None`` until a QEMU
# launch raises "ProcessSession is unavailable").  Bootstrap the repo root
# explicitly so the runner works both as a script and as
# ``python3 -m qemutests.run_test``.  Keeping ``qemutests/`` on the path
# also preserves the bare ``from test_resolution_switcher import ...`` at
# main().  Idempotent: the root is added at most once.
_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
DISK_IMG = os.environ.get("DISK_IMG", "disk.img")
TIMEOUT = int(os.environ.get("TEST_TIMEOUT", "60"))

# The x86 UEFI firmware is profile-private (build/<profile>/firmware/OVMF.fd)
# and must be passed explicitly by the caller. There is deliberately NO
# fallback to the source-tree boot/uefi/OVMF.fd. The check runs at module
# load — before any QEMU process can start.
OVMF_FIRMWARE = os.environ.get("OVMF_FIRMWARE")
if not OVMF_FIRMWARE or not os.path.isfile(OVMF_FIRMWARE):
    raise SystemExit("OVMF_FIRMWARE must name a readable firmware file")

# Number of guest CPUs passed to QEMU -smp (default: single CPU, the
# historical behavior). Validated at module load — before any QEMU
# process can start.
QEMU_SMP = os.environ.get("QEMU_SMP", "1")
if not QEMU_SMP.isdigit() or int(QEMU_SMP) < 1:
    raise SystemExit(
        f"QEMU_SMP must be a positive integer, got {QEMU_SMP!r}")

# ProcessSession — spec §5.3 of the test-framework plan.  Tasks 3
# established the frozen interface; the import here is the only
# connection between the legacy runner and the new harness layer.
# Import guarded so the legacy _FakePopen fixtures (test_gfx_runner.py)
# can monkey-patch ``run_test.subprocess.Popen`` without needing the
# harness module available.
try:
    from qemutests.harness.process import ProcessSession
except ImportError:  # pragma: no cover
    ProcessSession = None  # type: ignore[assignment]

# RunArchive + RunReport (spec §7.2).  The harness owns the evidence
# directory under ``build/<profile>/logs/tests/<suite>/<UTC>-<uuid>/``
# and writes ``result.json`` after every QEMU run.  Imported with the
# same guard as ProcessSession so legacy test_gfx_runner fixtures can
# monkey-patch ``run_test.subprocess.Popen`` without needing the
# harness submodule installed.
try:
    from qemutests.harness.result import RunArchive, RunReport, parse_v1
except ImportError:  # pragma: no cover
    RunArchive = None  # type: ignore[assignment]
    RunReport = None  # type: ignore[assignment]
    parse_v1 = None  # type: ignore[assignment]


def _sha256_path(path):
    """Return the hex SHA-256 of ``path``, or None if it is unreadable."""
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fp:
            for chunk in iter(lambda: fp.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except (OSError, TypeError):
        return None


class TestRunner:
    def __init__(self, disk_img, timeout=TIMEOUT,
                 extra_qemu_args=None, snapshot=False,
                 smp=None, qemu_smp=None,
                 session_factory=None, suite="phase-0"):
        self.disk_img = disk_img
        self.timeout = timeout
        self.extra_qemu_args = list(extra_qemu_args) if extra_qemu_args else []
        self.snapshot = snapshot
        self.suite = suite
        # SMP / QEMU_SMP conflict check (spec §7.1).  All three sources
        # (kwarg smp, kwarg qemu_smp, QEMU_SMP env var) must agree
        # before any QEMU spawn.  ``smp`` is the preferred name going
        # forward.
        env_qemu_smp = os.environ.get("QEMU_SMP")
        if (smp is not None and qemu_smp is not None
                and str(smp) != str(qemu_smp)):
            raise SystemExit(
                f"ERROR: SMP={smp} conflicts with legacy QEMU_SMP={qemu_smp}"
            )
        if (smp is not None and env_qemu_smp
                and str(smp) != env_qemu_smp):
            raise SystemExit(
                f"ERROR: SMP={smp} conflicts with env QEMU_SMP={env_qemu_smp}"
            )
        if (qemu_smp is not None and env_qemu_smp
                and str(qemu_smp) != env_qemu_smp):
            raise SystemExit(
                f"ERROR: QEMU_SMP={qemu_smp} conflicts with "
                f"env QEMU_SMP={env_qemu_smp}"
            )
        self.smp = smp
        self.qemu_smp = qemu_smp
        self._effective_smp = (
            str(smp) if smp is not None
            else (str(qemu_smp) if qemu_smp is not None else QEMU_SMP)
        )
        # Session factory: tests install a stand-in (see FakeProcessSession
        # in qemutests/test_run_test_harness.py).  Default: the real
        # ProcessSession.
        if session_factory is None and ProcessSession is not None:
            self._session_factory = ProcessSession
        else:
            self._session_factory = session_factory
        # State.
        self.process = None        # ProcessSession (or FakeProcessSession)
        self.proc = None           # legacy alias for `process`
        self.serial_path = None
        self._serial_stdout_fp = None
        self._run_dir = None
        self._is_serial_stdio = False
        # RunArchive (spec §7.2): populated by start_qemu() when the
        # OS01_BUILD_DIR env var is set.  ``run_archive = None`` when
        # the runner is invoked outside a build context (the legacy
        # path used by host-only unit tests).
        self.run_archive = None
        # RunReport evidence (spec §7.2): the input image is hashed
        # before launch and again after the run so a mid-run mutation
        # (QEMU writing the disk) is detectable; the wall-clock duration
        # is measured from launch.
        self._started_monotonic = None
        self._image_sha_before = None

    def start_qemu(self, network=False, serial_stdio=False,
                   extra_qemu_args=None, snapshot=False, run_dir=None):
        """Launch QEMU.

        With ``serial_stdio=False`` (default) the historical
        file-serial contract is preserved: serial output is captured
        to a temp file via ``-serial file:<path>`` and stdin is
        ``DEVNULL`` (the suite never types into QEMU).

        With ``serial_stdio=True`` the runner uses ``-serial stdio``
        so its own stdin/stdout ARE the serial port: the runner can
        type shell commands at the BusyBox prompt.  Serial output is
        also captured by the ProcessSession's stdout pipe (``text``).

        ``extra_qemu_args`` is a list of argv tokens appended to the
        default QEMU invocation (e.g., second -netdev, alternate
        -machine, or extra -device).  ``snapshot=True`` appends
        ``-snapshot`` so writes to disk are discarded on shutdown.

        The QEMU process boundary is owned by a ProcessSession
        (spec §5.3) — argv-only, monotonic deadline, persistent
        log files, process-group cleanup.  The factory used to
        construct the session is ``self._session_factory``; tests
        install a FakeProcessSession to bypass real subprocess.Popen.
        """
        # Merge instance-level extra args with per-call overrides.
        all_extra = list(self.extra_qemu_args)
        if extra_qemu_args:
            all_extra += list(extra_qemu_args)
        if snapshot or self.snapshot:
            if "-snapshot" not in all_extra:
                all_extra.append("-snapshot")

        # RunReport evidence (spec §7.2): snapshot the input image hash
        # and the wall-clock start time now, before QEMU can write the
        # disk, so _write_run_report records a real before/after pair and
        # a real duration.
        self._started_monotonic = time.monotonic()
        self._image_sha_before = _sha256_path(self.disk_img)

        # Allocate run_dir for the ProcessSession.
        # RunArchive integration (spec §7.2): when OS01_BUILD_DIR is
        # set, the runner creates an archive under
        # ``build/<profile>/logs/tests/<suite>/<UTC>-<uuid>/`` so the
        # suite's evidence is preserved at a stable, predictable
        # path.  When the env var is absent (host-only unit tests
        # that don't care about evidence), fall back to the legacy
        # mkdtemp in /tmp.  The factory can still override run_dir
        # explicitly via the ``run_dir`` kwarg.
        if run_dir is None:
            build_dir = os.environ.get("OS01_BUILD_DIR")
            if build_dir and RunArchive is not None:
                archive = RunArchive.create(
                    build_dir=Path(build_dir), suite=self.suite,
                )
                self.run_archive = archive
                run_dir = archive.run_dir
            else:
                run_dir = Path(tempfile.mkdtemp(prefix="os01-qemu-"))
        self._run_dir = run_dir

        # For file-serial mode, allocate the serial log path.
        if not serial_stdio:
            serial_log = tempfile.NamedTemporaryFile(
                prefix="os01_serial_", suffix=".log", delete=False)
            self.serial_path = serial_log.name
            serial_log.close()
            serial_arg = f"file:{self.serial_path}"
        else:
            serial_arg = "stdio"
            # ProcessSession owns stdout.log; expose it as serial_path
            # for legacy callers / fixture staging.
            self.serial_path = str(run_dir / "stdout.log")
        self._is_serial_stdio = bool(serial_stdio)

        args = [
            QEMU,
            "-M", "q35",
            "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_FIRMWARE}",
            "-drive", f"file={self.disk_img},format=raw,if=none,id=disk",
            "-device", "ahci,id=ahci",
            "-device", "ide-hd,drive=disk,bus=ahci.0",
            # Issue AAGU-2 §1: provide real entropy at boot so the
            # kernel's CSPRNG can seed itself in QEMU (no RDRAND/
            # RDSEED on default CPU). OVMF exposes this via
            # EFI_RNG_PROTOCOL, which the UEFI bootloader fetches
            # into boot_context.boot_entropy.
            "-object", "rng-random,filename=/dev/urandom,id=rng0",
            "-device", "virtio-rng-pci,rng=rng0",
            "-m", "512",
            "-smp", self._effective_smp,
            "-serial", serial_arg,
            "-display", "none",
            "-no-reboot",
            "-no-shutdown",
        ]
        # -machine pc drops the on-board SATA function so the
        # ``no-ahci`` case can boot virtio-blk-pci as the root disk.
        # The matrix harness detects ``-machine pc`` in
        # extra_qemu_args and rebuilds the disk spec; for legacy
        # callers the default q35 + ahci is preserved.
        has_pc = any(a == "pc" for a in all_extra)
        if has_pc:
            # Strip -machine pc out of extras; we'll insert it next.
            cleaned = []
            skip = 0
            for i, a in enumerate(all_extra):
                if skip:
                    skip -= 1
                    continue
                if a == "-machine" and i + 1 < len(all_extra) and \
                        all_extra[i+1] == "pc":
                    skip = 1
                    continue
                cleaned.append(a)
            # Replace the q35 with pc, drop the default ahci/ide-hd
            # pair (we'll re-add virtio-blk-pci at the end).
            args = [a for a in args if a != "q35" and
                    a != "-device" and
                    not (a.startswith("ahci") or
                         a.startswith("ide-hd") or
                         a.startswith("virtio-rng"))]
            # Drop the pflash + first -drive to rebuild cleanly.
            args = [
                QEMU,
                "-M", "pc",
                "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_FIRMWARE}",
                "-drive", f"file={self.disk_img},format=raw,if=none,id=disk",
                "-device", "virtio-blk-pci,drive=disk",
                "-m", "512",
                "-smp", self._effective_smp,
                "-serial", serial_arg,
                "-display", "none",
                "-no-reboot",
                "-no-shutdown",
            ]
            all_extra = cleaned
        if network:
            nic = os.environ.get("NETWORK_NIC", "e1000")
            nic_dev = "virtio-net-pci,netdev=net0,disable-modern=on" if nic == "virtio" else "e1000,netdev=net0"
            args += ["-netdev", "user,id=net0,dhcpstart=10.0.2.20",
                     "-device", nic_dev]
        # Append any caller-supplied tokens LAST (so the matrix harness
        # can override machine, add NICs, etc.).
        if all_extra:
            args += all_extra

        # Construct ProcessSession via the injected factory (or directly).
        if self._session_factory is None:
            raise RuntimeError(
                "ProcessSession is unavailable; cannot launch QEMU"
            )
        self.process = self._session_factory(
            argv=args,
            run_dir=run_dir,
            timeout_s=self.timeout,
            writable_stdin=bool(serial_stdio),
        )
        self.process.start()
        # Legacy alias: tests that imported `_FakePopen.last_instance`
        # can read ``tester.proc.args`` against the session's argv.
        self.proc = self.process

    def _read_available(self):
        """Read the serial log file (file-serial mode).  ProcessSession's
        ``text`` is the serial-stdio source.

        For FakeProcessSession-backed tests (no real QEMU writes to
        the serial file), the file is empty; the runner falls back
        to ``process.text`` so the file-serial polling path still
        sees the staged chunks.
        """
        if self._is_serial_stdio:
            return self.process.text.encode("utf-8", errors="replace") if self.process else b""
        if not self.serial_path:
            return b""
        try:
            with open(self.serial_path, 'rb') as f:
                data = f.read()
        except (OSError, IOError):
            data = b""
        # Empty file + fake session with staged text → use the fake's
        # text as the source.
        if not data and self.process is not None:
            text = getattr(self.process, "text", "") or ""
            if text:
                return text.encode("utf-8", errors="replace")
        return data

    def read_until(self, pattern, timeout=None):
        """Read serial output until pattern matches. Returns the match or None.

        In ``serial_stdio=True`` mode the source is the ProcessSession's
        stdout pipe (``text``).  In ``serial_stdio=False`` mode the
        source is the file QEMU writes to (``serial_path``) — the
        existing file-polling loop is preserved for backwards
        compatibility with file-serial suites.
        """
        if self.process is None:
            return None
        if self._is_serial_stdio:
            if isinstance(pattern, str):
                result = self.process.wait_for(lambda s: pattern in s)
                if not result:
                    return None
                return result
            # Compiled regex: return a ``re.Match`` object so the
            # legacy ``m.group(0)`` callers keep working.  We match
            # against the accumulated text; on no match we return
            # None (predicate-fail semantics).
            text = self.process.text
            m = pattern.search(text)
            if m:
                return m
            # Wait for the predicate to match.
            matched_slice = self.process.wait_for(
                lambda s: bool(pattern.search(s))
            )
            if not matched_slice:
                return None
            return pattern.search(self.process.text)
        # File-serial mode: legacy file-polling loop.
        if timeout is None:
            timeout = self.timeout
        deadline = time.time() + timeout
        buf = ""
        last_size = 0

        while time.time() < deadline:
            data = self._read_available()
            if len(data) > last_size:
                # New data available
                text = data[last_size:].decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()
                buf += text
                last_size = len(data)

                if isinstance(pattern, str):
                    if pattern in buf:
                        return buf
                else:
                    m = pattern.search(buf)
                    if m:
                        return m

            if self.process._proc and self.process._proc.poll() is not None:
                # QEMU exited — read any remaining output
                data = self._read_available()
                if len(data) > last_size:
                    text = data[last_size:].decode('utf-8', errors='replace')
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    buf += text
                break

            time.sleep(0.5)

        # Timeout: dump what we have
        print(f"\n[TEST] TIMEOUT waiting for pattern: {pattern}")
        print(f"[TEST] Last output: {buf[-500:]}")
        # If this was a systest run, surface the last test that completed so a
        # hang is attributable to a specific test name.
        last = [l for l in buf.splitlines()
                if '[PASS]' in l or '[FAIL]' in l or 'SYS TEST] RESULT' in l]
        if last:
            print(f"[TEST] Last completed test: {last[-1]}")
        return None

    def send(self, text):
        """Send a raw string to the QEMU stdin pipe (serial-stdio mode).

        Raises if QEMU was started in file-serial mode (no writable
        stdin).  The bytes are flushed before returning so a
        subsequent wait_for_prompt() can rely on QEMU having
        received them.
        """
        if self.process is None:
            raise RuntimeError("QEMU not started")
        if not self._is_serial_stdio:
            raise RuntimeError(
                "send() requires serial_stdio=True (stdin pipe is closed in file-serial mode)")
        self.process.send(text.encode("utf-8"))

    def send_line(self, text):
        """Send a command line (text + ``\\n``) and flush.

        Used by the gfx suite to invoke ``/bin/test_gfx`` after the
        shell prompt is observed.  Other suites never call this.
        """
        self.send(text + "\n")

    def wait_for_prompt(self, timeout=None):
        """Wait for the shell prompt."""
        return self.read_until("# ", timeout=timeout)

    def cleanup(self):
        if self.process is not None:
            try:
                self.process.close()
            except Exception:
                pass
            self.process = None
            self.proc = None
        if self._serial_stdout_fp is not None:
            try:
                self._serial_stdout_fp.close()
            except OSError:
                pass
            self._serial_stdout_fp = None
        if self.serial_path and os.path.exists(self.serial_path):
            try:
                os.unlink(self.serial_path)
            except OSError:
                pass
            self.serial_path = None


def _panic_in(text):
    """True if ``text`` contains a kernel-panic marker.

    The kernel emits a bracketed ``[kernel panic]`` form AND a
    ``Kernel panic: <reason>`` / ``Kernel panic - <reason>`` form
    (the capitalised heading comes straight from ``panic()``).  Match
    case-insensitively so a late fault in the 1-second observation
    window is never missed regardless of which form it takes.  This is
    the single shared detector used by every suite's observe-window
    check (spec §5.3).
    """
    return "kernel panic" in text.lower()


def test_boot(tester):
    """Phase 0 test: verify kernel boots, boot-log markers appear in
    order, and the shell runs."""
    tester.start_qemu()

    # Wait for evidence of boot — init banner. read_until() returns the
    # whole buffer, so the ordered boot-marker assertion below runs on
    # everything printed up to (and including) the banner.
    booted = tester.read_until("OS01 Init v1.0", timeout=25)
    if not booted:
        print("FAIL: Kernel did not boot")
        return False

    # Boot-log markers, in serial-output order.
    #
    # Post-Task 6: BSP per-CPU registration runs BEFORE the
    # filesystem/TTY phase at every SMP count, so the "percpu: N
    # CPU(s) registered" marker always precedes TTY creation, which
    # precedes the /dev/null probe, which precedes the init banner.
    # Both single- and multi-CPU branches enforce this ordering —
    # BSP percpu runs even at SMP=1 (num_cpus=1 after the BSP loop).
    effective_smp = int(tester._effective_smp)
    if effective_smp > 1:
        markers = (
            rf"percpu: {effective_smp} CPU\(s\) registered"
            r".*tty: console TTY created"
            r".*devfs: /dev/null read=0 write=4"
            r".*OS01 Init v1\.0"
        )
    else:
        markers = (
            rf"percpu: {effective_smp} CPU\(s\) registered"
            r".*tty: console TTY created"
            r".*devfs: /dev/null read=0 write=4"
            r".*OS01 Init v1\.0"
        )
    if not re.search(markers, booted, re.DOTALL):
        print("FAIL: boot-log markers missing or out of order "
              f"(QEMU_SMP={tester._effective_smp})")
        return False

    # Wait for shell prompt
    prompt = tester.read_until("# ", timeout=15)
    if not prompt:
        print("FAIL: No shell prompt")
        return False

    # 1-second observation window (spec §5.3): a kernel panic that
    # arrives within observe(1) after the prompt is treated as a
    # late fault — the run FAILs even though boot succeeded.  This
    # closes the "kernel crashes 100 ms after the prompt" hole.
    if tester.process is not None:
        try:
            tail = tester.process.observe(1.0)
        except Exception:
            tail = ""
        if _panic_in(tail):
            print("FAIL: kernel panic in 1-second observation window")
            return False

    print("PASS: Kernel booted, boot markers verified, shell prompt appeared")
    return True


def test_systest(tester):
    """Run systest; validate protocol v1 as the only success gate.

    The guest publishes a protocol-v1 trace (``[TEST] START/SELECT/
    BEGIN/terminal/END``) from the ``tests[]`` table in
    ``user/systest.c``.  ``parse_v1`` is the *only* success gate: it
    enforces exact SELECT/BEGIN/terminal set equality, count arithmetic,
    at least one PASS, and — when ``SYSTEST_CASE`` selects a single case
    — that the declared *and* observed IDs equal that case.  The legacy
    ``[SYS TEST] RESULT`` line is retained purely as the COW handshake's
    completion signal; it no longer decides pass/fail.

    ``SYSTEST_CASE`` (written by ``mk/components/run.mk`` alongside the
    private ``inittab.systest.case``) names the requested case; an
    unset/empty value runs the full table.
    """
    requested_case = os.environ.get("SYSTEST_CASE") or None
    tester.start_qemu(serial_stdio=True)
    deadline = time.monotonic() + 60
    # Each region's child signals only after fork, immediately before read.
    # Exact markers prevent replaying input for a previously handled region.
    result_pattern = r"(\[SYS TEST\] RESULT:|'/bin/terminal|BusyBox v)"
    m = None
    for region in range(4):
        pattern = re.compile(rf"\[COW TTY READY {region}\]|{result_pattern}")
        m = tester.read_until(pattern, timeout=max(0, deadline - time.monotonic()))
        if not m:
            print("FAIL: systest did not complete")
            return False
        if "COW TTY READY" not in m.group(0):
            break
        tester.send_line("COW!")
    if m and "COW TTY READY" in m.group(0):
        m = tester.read_until(re.compile(result_pattern),
                              timeout=max(0, deadline - time.monotonic()))
    if not m:
        print("FAIL: systest did not complete")
        return False
    matched = m.group(0)
    if "'/bin/terminal" in matched or "BusyBox v" in matched:
        print("FAIL: disk.img is not a systest build (booted /bin/terminal "
              "instead of /bin/systest). Rebuild with: make OS01_SYSTEST=1 test-qemu SUITE=systest")
        return False

    # RESULT line matched — drain whatever remains (in particular the
    # ``[TEST] END`` record that follows it) so parse_v1 sees the
    # complete trace.
    time.sleep(2)
    output = tester._read_available().decode('utf-8', errors='replace')

    # 1-second observation window (spec §5.3): a kernel panic that
    # arrives within observe(1) after the RESULT line FAILs the run
    # even though the protocol records say PASS.
    if tester.process is not None:
        try:
            tail = tester.process.observe(1.0)
        except Exception:
            tail = ""
        if _panic_in(tail):
            print("FAIL: kernel panic in 1-second observation window")
            return False

    # Protocol v1 is the only success gate (spec §6.1 / §6.2).
    if parse_v1 is None:
        print("FAIL: harness parse_v1 unavailable; cannot validate systest")
        return False
    pr = parse_v1(output, suite="systest", requested_case=requested_case)
    if not pr.ok:
        for err in pr.errors:
            print(f"FAIL: systest protocol: {err}")
        return False
    if pr.failed > 0:
        for cid, reason in sorted(pr.reasons.items()):
            print(f"FAIL: systest case {cid}: {reason}")
        return False
    if requested_case is not None:
        print(f"PASS: case {requested_case} passed")
    else:
        print(f"PASS: all {pr.passed} syscall tests passed")
    return True


def test_inittab_phase(tester):
    """Verify inittab phase dispatch order and error handling."""
    tester.start_qemu()

    # Wait for the last phase marker. Since SYSINIT and WAIT block
    # before ONCE runs, all three markers must be present by this point.
    buf = tester.read_until("ONCE_DONE", timeout=30)
    if not buf:
        print("FAIL: phase markers not found")
        return False

    # Assert order: SYSINIT_DONE before WAIT_DONE before ONCE_DONE.
    # read_until() re-reads the log from the start each call, so
    # sequential calls only check existence. Single-regex on the
    # returned buffer proves the sequence.
    if not re.search(r'SYSINIT_DONE.*WAIT_DONE.*ONCE_DONE', buf, re.DOTALL):
        print("FAIL: phase dispatch out of order")
        return False

    # Wait for terminal shell prompt
    prompt = tester.read_until("# ", timeout=15)
    if not prompt:
        print("FAIL: terminal not started")
        return False

    # Assert malformed-line warnings are present in the buffer
    if "unknown action 'unknown_action'" not in buf:
        print("FAIL: missing 'unknown action' warning")
        return False
    if "too many fields" not in buf:
        print("FAIL: missing 'too many fields' warning")
        return False

    # 1-second observation window (spec §5.3): a kernel panic that
    # arrives within observe(1) after the phase checks is a late fault
    # and FAILs the run even though every phase marker was present.
    if tester.process is not None:
        try:
            tail = tester.process.observe(1.0)
        except Exception:
            tail = ""
        if _panic_in(tail):
            print("FAIL: kernel panic in 1-second observation window")
            return False

    print("PASS: phase dispatch order verified, error paths exercised")
    return True


class _EchoTCPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(4096)
            if not data:
                return
            delay_ms = self.server.echo_delay_ms
            if delay_ms:
                time.sleep(delay_ms / 1000.0)
            self.request.sendall(data)


class _PayloadHTTPHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        payload = b"OS01 network test\n"
        if self.path != "/payload":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        pass


class _ReusableTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


class NetworkServices:
    """Deterministic host endpoints reachable from QEMU as 10.0.2.2."""
    def __init__(self):
        self.stop_event = threading.Event()
        self.servers = []
        self.threads = []
        self.udp_socket = None
        self.tcp_echo_delay_ms = int(os.environ.get("OS01_TCP_ECHO_DELAY_MS", "0"))
        if self.tcp_echo_delay_ms < 0:
            raise ValueError("OS01_TCP_ECHO_DELAY_MS must be non-negative")

    def start(self):
        endpoints = [
            (10002, _EchoTCPHandler),
            (18080, _PayloadHTTPHandler),
        ]
        for port, handler in endpoints:
            server = _ReusableTCPServer(("127.0.0.1", port), handler)
            if port == 10002:
                server.echo_delay_ms = self.tcp_echo_delay_ms
            else:
                server.echo_delay_ms = 0
            self.servers.append(server)
            server.daemon_threads = True
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.threads.append(thread)

        import socket
        self.udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp_socket.bind(("127.0.0.1", 10001))
        self.udp_socket.settimeout(0.25)

        def udp_echo():
            while not self.stop_event.is_set():
                try:
                    data, peer = self.udp_socket.recvfrom(4096)
                    self.udp_socket.sendto(data, peer)
                except socket.timeout:
                    continue
                except OSError:
                    break

        thread = threading.Thread(target=udp_echo, daemon=True)
        thread.start()
        self.threads.append(thread)

    def close(self):
        self.stop_event.set()
        if self.udp_socket:
            self.udp_socket.close()
        for server in self.servers:
            server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)


def test_network(tester):
    """Exercise DHCP, UDP, DNS, TCP, and wget through QEMU user networking."""
    services = NetworkServices()
    try:
        services.start()
        tester.start_qemu(network=True)
        output = tester.read_until("[NET TEST] RESULT:", timeout=tester.timeout)
        if not output:
            print("FAIL: network test did not complete")
            return False

        time.sleep(1)
        output = tester._read_available().decode('utf-8', errors='replace')
        # 1-second observation window (spec §5.3): a kernel panic in
        # the post-RESULT tail FAILs the run.
        if tester.process is not None:
            try:
                tail = tester.process.observe(1.0)
            except Exception:
                tail = ""
            if _panic_in(tail):
                print("FAIL: kernel panic in 1-second observation window")
                return False
        # Anchor the RESULT on THIS run's own start evidence: nettest
        # performs a DHCP handshake (``[NET TEST] DHCP: PASS``) before
        # it prints the RESULT summary, so the current run's output up
        # to the RESULT must carry the DHCP evidence.  A historical
        # generic ``PASS:`` line replayed from a previous run (the old
        # adapter's marker) has no DHCP evidence, so accepting its
        # stale ``RESULT: 6 passed`` would be a false green.
        if "DHCP" not in output:
            print("FAIL: network RESULT without current-run DHCP evidence "
                  "(stale/foreign output)")
            return False
        match = re.search(r'\[NET TEST\] RESULT:\s*(\d+)\s*passed,\s*(\d+)\s*failed', output)
        if not match:
            print("FAIL: could not parse network result")
            return False
        passed, failed = int(match.group(1)), int(match.group(2))
        if passed != 6 or failed:
            print(f"FAIL: network regression: {passed} passed, {failed} failed")
            return False
        print("PASS: DHCP, UDP, DNS, TCP, wget, and socket_exit")
        return True
    finally:
        services.close()


def test_gfx(tester):
    """Ring-3 gfx E2E (Task 5 of the 2D graphics API plan + Task 8 acceptance).

    The caller (``main()`` -> ``test_gfx`` dispatch) is responsible
    for invoking ``tester.start_qemu(serial_stdio=True)`` BEFORE this
    function runs (the gfx suite uses the same ``TestRunner`` instance
    as the other suites, and ``main()`` does not pre-start QEMU).
    This function is the shell-side flow: wait for the BusyBox ``#``
    prompt, then run ``/bin/test_gfx`` (the full-screen view E2E)
    AND ``/bin/tetris smoke`` (one-shot full-screen render+present).
    Both must print their PASS markers; either failure aborts the
    suite with FAIL.

    The test programs run the substantive assertion logic.  This
    runner is just the transport + marker gate, by design.

    On success, each binary prints exactly one PASS line; on any
    failure the binary prints a ``FAIL: <reason>`` line with the
    same prefix and exits non-zero.  We treat any non-PASS outcome
    as failure (including timeouts).
    """
    # If the caller forgot to start QEMU, we start it here too — but
    # only if no proc exists.  Calling start_qemu twice would clobber
    # the serial log file the first invocation already opened.
    if not tester.process:
        tester.start_qemu(serial_stdio=True)

    # Wait for the BusyBox ash prompt.  read_until() matches on the
    # serial log file (the tee side of the stdio pipe), so a normal
    # boot + login + ash start all complete before this returns.
    # The TestRunner already owns the timeout budget; we honour it
    # for both the prompt and the marker so the runner's deadline is
    # visible to the test (and so unit tests can shorten it).
    prompt = tester.read_until("# ", timeout=tester.timeout)
    if not prompt:
        print("FAIL: gfx runner never saw the shell prompt")
        return False

    # ── Step A: /bin/test_gfx — full-screen view E2E ───────
    # Task 5 of the 2D graphics API plan, extended by Task 8 to
    # use the actual framebuffer dimensions and assert a >= 5.18
    # MB pixels buffer + heap headroom.
    tester.send_line("/bin/test_gfx")

    passed = tester.read_until("[GFX TEST] PASS", timeout=tester.timeout)
    if passed is None:
        # Drain a moment so a slow FAIL line still makes it into the
        # log before we report the cause.
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[GFX TEST\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/test_gfx did not produce PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [GFX TEST] PASS marker observed")

    # ── Step B: /bin/tetris smoke — one-shot full-screen E2E ─
    # Task 8 of the user-heap/ELF-isolation plan: a non-interactive
    # tetris path that allocates the same full-screen pixels buffer,
    # performs one real render and present, then prints
    # ``[TETRIS] SMOKE PASS`` and exits cleanly.  The marker must
    # reach stdout / the serial port unmodified, so the smoke path
    # intentionally bypasses the alt-screen terminal mode.
    tester.send_line("/bin/tetris smoke")

    passed = tester.read_until("[TETRIS] SMOKE PASS", timeout=tester.timeout)
    if passed is None:
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[TETRIS\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/tetris smoke did not produce SMOKE PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [TETRIS] SMOKE PASS marker observed")

    # ── Step C: /bin/test_terminal_screen — visual screen E2E ─
    tester.send_line("/bin/test_terminal_screen")

    passed = tester.read_until("[TERM SCREEN TEST] PASS", timeout=tester.timeout)
    if passed is None:
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[TERM SCREEN TEST\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/test_terminal_screen did not produce PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [TERM SCREEN TEST] PASS marker observed")

    # ── Step D: /bin/desktop smoke — GUI desktop smoke E2E ─
    tester.send_line("/bin/desktop smoke")

    passed = tester.read_until("[DESKTOP] SMOKE PASS", timeout=tester.timeout)
    if passed is None:
        time.sleep(1)
        log = tester._read_available().decode('utf-8', errors='replace')
        marker_re = re.compile(r"\[DESKTOP\][^\n]*")
        m = marker_re.search(log)
        print(f"FAIL: /bin/desktop smoke did not produce SMOKE PASS marker "
              f"(last test marker: {m.group(0) if m else '<none>'!r})")
        return False
    print("PASS: [DESKTOP] SMOKE PASS marker observed")
    # 1-second observation window (spec §5.3): a kernel panic in the
    # post-completion tail FAILs the run.
    if tester.process is not None:
        try:
            tail = tester.process.observe(1.0)
        except Exception:
            tail = ""
        if _panic_in(tail):
            print("FAIL: kernel panic in 1-second observation window")
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description="OS01 test runner")
    parser.add_argument("--disk", default=DISK_IMG, help="Disk image to test")
    parser.add_argument("--timeout", type=int, default=TIMEOUT, help="Timeout in seconds")
    parser.add_argument("test_name", nargs="?", default="boot", help="Test to run")
    args = parser.parse_args()

    tester = TestRunner(args.disk, args.timeout, suite=args.test_name)

    # ``result`` is assigned only inside the branches below; initialize it
    # so ``finally`` never sees an unbound name if a suite function raises
    # (spec §6.2: an internal/launch error is an ERROR — exit 2, not 1).
    result = None
    error = None
    try:
        if args.test_name == "boot" or args.test_name == "phase-0":
            result = test_boot(tester)
        elif args.test_name == "systest":
            result = test_systest(tester)
        elif args.test_name == "inittab-phase":
            result = test_inittab_phase(tester)
        elif args.test_name == "network":
            result = test_network(tester)
        elif args.test_name == "gfx":
            result = test_gfx(tester)
        elif args.test_name == "resolution":
            # Imported lazily: test_resolution_switcher.py has no import-time
            # OVMF/QEMU coupling, but keeping the import here mirrors the
            # other suites and avoids a circular import at module load.
            from test_resolution_switcher import test_resolution
            result = test_resolution(tester)
        else:
            print(f"Unknown test: {args.test_name}")
            result = False
    except Exception as exc:  # noqa: BLE001 — surfaced below as ERROR/exit 2
        # A raising suite (e.g. a real QEMU launch OSError) is an ERROR:
        # capture it so the finally block still writes result.json and the
        # process exits 2 instead of masking the exception with a NameError.
        error = exc
    finally:
        tester.cleanup()
        _write_run_report(tester, args, result, error=error)

    if error is not None:
        traceback.print_exception(type(error), error, error.__traceback__)
        sys.exit(2)
    sys.exit(0 if result else 1)


def _write_run_report(tester, args, result, error=None):
    """Persist a RunReport to ``tester.run_archive`` if one was created.

    Best-effort: an ArchiveWriteError is logged but does not change
    the test exit code (the suite itself owns pass/fail; the archive
    is for forensics only)."""
    archive = getattr(tester, "run_archive", None)
    if archive is None or RunReport is None:
        return
    import subprocess as _sp
    from datetime import datetime, timezone
    try:
        # Best-effort git revision / dirty flag.
        git_rev = "unknown"
        git_dirty = False
        try:
            r = _sp.run(
                ["git", "rev-parse", "HEAD"],
                cwd=Path(__file__).resolve().parents[1],
                capture_output=True, text=True, timeout=5,
            )
            if r.returncode == 0 and r.stdout.strip():
                git_rev = r.stdout.strip()
            d = _sp.run(
                ["git", "status", "--porcelain"],
                cwd=Path(__file__).resolve().parents[1],
                capture_output=True, text=True, timeout=5,
            )
            if d.returncode == 0:
                git_dirty = bool(d.stdout.strip())
        except (OSError, _sp.TimeoutExpired):
            pass
        # Input-image hash pair (spec §7.2): ``before`` was captured in
        # start_qemu before QEMU could write the disk; ``after`` is taken
        # now.  A difference means the run mutated its input image.
        img_path = Path(tester.disk_img) if tester.disk_img else None
        img_sha_before = getattr(tester, "_image_sha_before", None)
        img_sha_after = _sha256_path(img_path) if img_path else None
        # Wall-clock duration measured from launch.
        started = getattr(tester, "_started_monotonic", None)
        duration_s = (
            max(0.0, time.monotonic() - started) if started else 0.0
        )
        # Compose the report.
        report = RunReport(
            schema_version=1,
            run_id=archive.run_dir.name,
            git_revision=git_rev,
            git_dirty=git_dirty,
            profile=os.environ.get("OS01_PROFILE", "default"),
            suite=args.test_name,
            request=None,
            declared_ids=None,
            observed_ids=None,
            argv=list(tester.process.argv) if tester.process else [],
            cpu_count=int(getattr(tester, "_effective_smp", "1") or 1),
            memory_mib=512,
            tool_versions={},
            firmware_path=OVMF_FIRMWARE,
            firmware_sha256_before=None,
            firmware_sha256_after=None,
            image_path=str(img_path) if img_path else None,
            image_sha256_before=img_sha_before,
            image_sha256_after=img_sha_after,
            utc_started_at=datetime.now(timezone.utc).isoformat(),
            duration_s=duration_s,
            runner_exit_code=2 if error is not None else (0 if result else 1),
            child_exit_code=(
                tester.process.returncode if tester.process else None
            ),
            stopped_by_runner=bool(
                tester.process and tester.process.stopped_by_runner
            ),
            status=(
                "ERROR" if error is not None
                else ("PASS" if result else "FAIL")
            ),
            count_unit="case",
            outcomes=[],
            stdout_log=str(archive.run_dir / "stdout.log"),
            stderr_log=str(archive.run_dir / "stderr.log"),
        )
        archive.write(report)
    except Exception as exc:
        print(f"[run_test] warning: failed to write RunReport: {exc}")


if __name__ == "__main__":
    main()
