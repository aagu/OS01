"""ProcessSession — spec §5.3 of the OS01 lightweight test framework.

Public interface (the frozen contract for Tasks 4–12):

    ProcessSession(argv: list[str],
                   run_dir: Path,
                   timeout_s: float,
                   *,
                   writable_stdin: bool = False)

Methods:
    start()                                  — launch the child
    send(data: bytes)                        — write to stdin (writable_stdin only)
    close_stdin()                            — close the stdin pipe
    wait_for(predicate) -> str               — read until predicate(text[cursor:]) matches
    observe(seconds: float) -> str           — post-completion observation window
    stop() -> int | None                     — terminate cleanly, return returncode
    close()                                  — idempotent finalizer

Properties:
    text: str                                — cumulative stdout text
    returncode: int | None                   — None until known
    stopped_by_runner: bool                  — True if we issued terminate/kill
    timed_out: bool                          — True if a wait_for hit the deadline
    deadline: float                          — monotonic time at which budget expires
    argv: list[str]                          — the launched command (read-only copy)

Persistent attributes:
    stdout.log                     — file under run_dir, written incrementally
    stderr.log                     — file under run_dir, written incrementally

Cleanup contract (stop / exception / interrupt):

    os.killpg(SIGTERM) → proc.wait(timeout=5)
        └── os.killpg(SIGKILL) → proc.wait(timeout=5)

``close()`` NEVER raises: it is a finalizer, and a teardown that raises
would mask the caller's error while leaving the child alive.  A live
child is an orphan that keeps the disk image's write lock, so the next
``make run`` fails with "Is another process using the image?".

The child is launched with ``preexec_fn=os.setsid`` so it leads its own
process group; ``os.killpg`` therefore reaches every inherited child
(grandchildren) too.  The same preexec installs ``PR_SET_PDEATHSIG``
(SIGKILL), so a child whose parent dies by an uncatchable signal (or a
crash) is killed by the kernel instead of surviving as an orphan.  The
deadline is set once at start() and is consumed (never reset) by every
subsequent wait_for / observe call.

The cursor advances after each successful wait_for / observe: a second
call does not re-match the slice that already satisfied the first.
The cursor is NOT reset by start(); tasks must call wait_for in the
order they want to consume output.
"""

from __future__ import annotations

import codecs
import os
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, List, Optional


_CHUNK_SIZE = 1 << 16           # 64 KiB per os.read() call
_TERMINATE_TIMEOUT_S = 5.0      # spec §5.3: terminate → wait up to 5s
_SELECT_TIMEOUT_S = 0.1         # re-check the deadline every 100ms

# PR_SET_PDEATHSIG — the anti-orphan backstop (see _child_preexec).
_PR_SET_PDEATHSIG = 1

# The cached libc handle for prctl(2).  It is loaded ONCE in the parent
# (see ProcessSession.start) so the forked child never has to dlopen()
# inside its preexec_fn — a post-fork allocation is exactly the kind of
# call preexec_fn is documented to be unsafe about.
_LIBC = None
_LIBC_FAILED = False


def _load_libc():
    """The cached libc handle, or None when prctl(2) is unavailable."""
    global _LIBC, _LIBC_FAILED
    if _LIBC is None and not _LIBC_FAILED:
        try:
            import ctypes

            libc = ctypes.CDLL("libc.so.6", use_errno=True)
            libc.prctl.argtypes = [
                ctypes.c_int,
                ctypes.c_ulong,
                ctypes.c_ulong,
                ctypes.c_ulong,
                ctypes.c_ulong,
            ]
            libc.prctl.restype = ctypes.c_int
            _LIBC = libc
        except Exception:
            # Remember the failure: do not retry it once per child.
            _LIBC_FAILED = True
    return _LIBC


def _child_preexec(parent_pid: int) -> None:
    """子进程的前置动作：新会话 + 父进程一死自己也死。

    ``os.setsid()`` 让子进程自成 pgid，``close()`` 才能用 ``os.killpg``
    打到继承了 pgid 的孙进程。

    ``PR_SET_PDEATHSIG`` 是**防孤儿**的最后一道：测试脚本被 Ctrl-C、
    ``timeout``、甚至 SIGKILL 打断时，子进程会跟着父进程一起死。没有它，
    被打断的 QEMU 会一直活着 —— 它持有磁盘镜像的文件锁（后续的
    ``make run`` 报「disk.img 被占用」）并继续吃内存。

    ``prctl`` 之后的父进程核对是必须的竞态兜底：父进程若在 ``prctl``
    之前就已退出，PDEATHSIG 不会补发，核对发现父进程已变就自己退出。
    """
    os.setsid()
    libc = _load_libc()
    if libc is not None:
        try:
            libc.prctl(_PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)
        except Exception:
            # prctl 不可用不是致命错误：close() 的 killpg 仍是兜底。
            pass
    # 父进程核对独立于 prctl 的成败：孤儿比少一道兜底更糟。
    if os.getppid() != parent_pid:
        os._exit(0)


class ProcessSession:
    """Lifecycle and incremental output for a single subprocess.

    The session owns:
      * the subprocess (argv-only, no shell),
      * stdout/stderr pipes (drained via ``selectors``),
      * an incremental UTF-8 decoder per stream,
      * persistent ``stdout.log`` / ``stderr.log`` files,
      * a monotonic deadline and a read cursor.

    It does NOT decide PASS/FAIL — that is the caller's job (the
    public runner returns 0/1/2/130; see spec §6.2).
    """

    def __init__(
        self,
        argv: List[str],
        run_dir: Path,
        timeout_s: float,
        *,
        writable_stdin: bool = False,
    ) -> None:
        self._argv = list(argv)
        self._run_dir = Path(run_dir)
        self._timeout_s = float(timeout_s)
        self._writable_stdin = bool(writable_stdin)
        # Child + state.
        self._proc: Optional[subprocess.Popen] = None
        self._stdout_decoder: codecs.IncrementalDecoder = (
            codecs.getincrementaldecoder("utf-8")(errors="replace")
        )
        self._stderr_decoder: codecs.IncrementalDecoder = (
            codecs.getincrementaldecoder("utf-8")(errors="replace")
        )
        self._text = ""
        self._stderr_text = ""
        self._cursor = 0
        # Public attributes.
        self._returncode: Optional[int] = None
        self._stopped_by_runner = False
        self._timed_out = False
        self._deadline = 0.0
        # Internal handles.
        self._stdout_log = None
        self._stderr_log = None
        self._selector: Optional[selectors.DefaultSelector] = None
        self._cleaned_up = False

    # ── public read-only state ────────────────────────────────

    @property
    def text(self) -> str:
        """Cumulative decoded stdout text (every byte ever received)."""
        return self._text

    @property
    def returncode(self) -> Optional[int]:
        return self._returncode

    @property
    def stopped_by_runner(self) -> bool:
        return self._stopped_by_runner

    @property
    def timed_out(self) -> bool:
        return self._timed_out

    @property
    def deadline(self) -> float:
        """Monotonic time at which the global budget expires."""
        return self._deadline

    @property
    def argv(self) -> List[str]:
        """The argv the child was launched with (a read-only copy).

        Exposed so a runner can record the real command in its RunReport
        (spec §7.2) through a public accessor rather than reaching into
        ``_argv``.  Returning a copy keeps the session's own list
        immutable from the caller's side.
        """
        return list(self._argv)

    @property
    def run_dir(self) -> Path:
        """The session's run directory (where stdout/stderr logs live).

        Exposed as a public property so test fixtures can assert the
        ProcessSession's working directory matches the RunArchive's
        ``run_dir``.
        """
        return self._run_dir

    @property
    def child_pid(self) -> Optional[int]:
        """The child's pid, or None before ``start()``.

        Deliberately lock-free: a signal handler may read it while the
        main thread sits inside ``Popen.poll()`` (which holds an
        internal, non-reentrant lock).  The handler uses this to SIGKILL
        the child's process group without touching any Popen state — the
        teardown path must never wait for a lock the interrupted thread
        might still be holding.
        """
        return self._proc.pid if self._proc is not None else None

    # ── lifecycle ──────────────────────────────────────────────

    def start(self) -> None:
        """Launch the child as its own process-group leader.

        Raises FileNotFoundError / PermissionError / OSError for a
        bad executable (subprocess.Popen behaviour).  Log files are
        only created AFTER the child successfully launches — so a bad
        executable leaves the run_dir untouched and close() has no
        half-opened file to clean up."""
        self._run_dir.mkdir(parents=True, exist_ok=True)
        self._deadline = time.monotonic() + self._timeout_s
        parent_pid = os.getpid()
        # Load libc in the PARENT so the forked child never has to
        # dlopen() inside its preexec_fn (post-fork allocation is what
        # preexec_fn is documented to be unsafe about).
        _load_libc()
        try:
            self._proc = subprocess.Popen(
                self._argv,
                stdin=subprocess.PIPE if self._writable_stdin else subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                # The child becomes its own pgid leader (close() then uses
                # os.killpg to reach grandchildren that inherited the pgid
                # by default) AND dies with its parent (see _child_preexec).
                preexec_fn=lambda: _child_preexec(parent_pid),
                bufsize=0,
            )
        except Exception:
            raise
        # Popen(..., stdout=PIPE, stderr=PIPE) guarantees non-None pipes;
        # the assert narrows the type for Pyright and documents the invariant.
        assert self._proc.stdout is not None and self._proc.stderr is not None
        # Child is alive — open log files and the selector.
        self._stdout_log = open(
            self._run_dir / "stdout.log", "w", encoding="utf-8", buffering=1,
        )
        self._stderr_log = open(
            self._run_dir / "stderr.log", "w", encoding="utf-8", buffering=1,
        )
        self._selector = selectors.DefaultSelector()
        self._selector.register(
            self._proc.stdout, selectors.EVENT_READ, data="stdout",
        )
        self._selector.register(
            self._proc.stderr, selectors.EVENT_READ, data="stderr",
        )

    def send(self, data: bytes) -> None:
        """Write bytes to the child's stdin (no shell, no escaping)."""
        if not self._writable_stdin:
            raise RuntimeError(
                "ProcessSession was not opened with writable_stdin=True",
            )
        if self._proc is None or self._proc.stdin is None:
            raise RuntimeError("stdin not available")
        self._proc.stdin.write(data)
        self._proc.stdin.flush()

    def close_stdin(self) -> None:
        """Close the stdin pipe.  The child is NOT killed by this."""
        if self._proc and self._proc.stdin is not None:
            try:
                self._proc.stdin.close()
            except Exception:
                pass

    # ── read loop ──────────────────────────────────────────────

    def _drain(self, timeout: float) -> bool:
        """Drain stdout/stderr while the selector fires.

        Returns True if any decoded text was appended in this call."""
        if self._selector is None or self._cleaned_up:
            return False
        sel_timeout = min(timeout, _SELECT_TIMEOUT_S) if timeout > 0 else 0.0
        try:
            events = self._selector.select(timeout=sel_timeout)
        except (OSError, ValueError):
            return False
        had_data = False
        for key, _ in events:
            tag = key.data
            if tag == "stdout":
                fd = self._proc.stdout if self._proc else None
                decoder = self._stdout_decoder
                log = self._stdout_log
            elif tag == "stderr":
                fd = self._proc.stderr if self._proc else None
                decoder = self._stderr_decoder
                log = self._stderr_log
            else:
                continue
            if fd is None:
                continue
            try:
                data = os.read(fd.fileno(), _CHUNK_SIZE)
            except OSError:
                data = b""
            if not data:
                # EOF on this stream: flush the decoder and unregister.
                try:
                    self._selector.unregister(fd)
                except Exception:
                    pass
                flushed = decoder.decode(b"", final=True)
                if flushed:
                    if tag == "stdout":
                        self._text += flushed
                    else:
                        self._stderr_text += flushed
                    if log:
                        log.write(flushed)
                    had_data = True
                continue
            decoded = decoder.decode(data, final=False)
            if decoded:
                if tag == "stdout":
                    self._text += decoded
                else:
                    self._stderr_text += decoded
                if log:
                    log.write(decoded)
                had_data = True
        return had_data

    def _has_data_pending(self) -> bool:
        """True iff at least one pipe is still registered for reading."""
        if self._selector is None:
            return False
        try:
            return bool(self._selector.get_map())
        except Exception:
            return False

    def _poll(self) -> Optional[int]:
        if self._proc is None:
            return None
        return self._proc.poll()

    def wait_for(self, predicate: Callable[[str], bool]) -> str:
        """Read until ``predicate(text[cursor:])`` matches or the
        deadline trips.

        Returns the slice that satisfied the predicate
        (``text[old_cursor:new_cursor]``).  On timeout, sets
        ``timed_out=True``, cleans up the child, and returns ``""``.

        On any exception (including KeyboardInterrupt) the child is
        cleaned up before the exception propagates."""
        try:
            while True:
                remaining = self._deadline - time.monotonic()
                if remaining <= 0:
                    self._timed_out = True
                    self._cleanup()
                    return ""
                self._drain(remaining)
                candidate = self._text[self._cursor:]
                if predicate(candidate):
                    old_cursor = self._cursor
                    self._cursor = len(self._text)
                    return self._text[old_cursor:self._cursor]
                # Process exited and no more data — final check.
                if (
                    self._poll() is not None
                    and not self._has_data_pending()
                ):
                    if predicate(self._text[self._cursor:]):
                        old_cursor = self._cursor
                        self._cursor = len(self._text)
                        return self._text[old_cursor:self._cursor]
                    # EOF with no match: loop; deadline will trip.
        except BaseException:
            self._cleanup()
            raise

    def observe(self, seconds: float) -> str:
        """Post-completion observation window.

        Drains stdout/stderr for up to ``seconds``.  If the child
        exits and no more data is pending, returns immediately.
        Advances the cursor and returns the new slice (text that
        arrived during the window)."""
        end = time.monotonic() + seconds
        while True:
            now = time.monotonic()
            if now >= end:
                break
            remaining = end - now
            self._drain(remaining)
            if self._poll() is not None and not self._has_data_pending():
                self._drain(0.0)
                # Reap the child so returncode is available without
                # forcing the caller to call stop() first.
                if (
                    self._proc is not None
                    and self._returncode is None
                ):
                    try:
                        self._returncode = self._proc.wait(timeout=0.5)
                    except Exception:
                        pass
                break
        old_cursor = self._cursor
        self._cursor = len(self._text)
        return self._text[old_cursor:self._cursor]

    # ── cleanup ────────────────────────────────────────────────

    def stop(self) -> Optional[int]:
        """Stop the child, drain, and reap.  Returns returncode."""
        self._cleanup()
        return self._returncode

    def close(self) -> None:
        """Idempotent finalizer (safe to call multiple times)."""
        self._cleanup()

    def _cleanup(self) -> None:
        """Terminate the child and release every handle.  NEVER raises.

        ``close()`` is a finalizer: it runs from ``finally`` blocks, from
        ``wait_for``'s exception path and from signal handlers.  A
        teardown that raises would (a) mask the caller's original error
        and (b) leave the child alive — and a live QEMU keeps the disk
        image's write lock, so the next ``make run`` fails with "Is
        another process using the image?".

        The kill sequence is unchanged (spec §5.3): SIGTERM the whole
        process group, wait up to 5s, then SIGKILL it.  SIGKILL cannot be
        caught, so it is the unconditional backstop.
        """
        if self._cleaned_up:
            return
        # Final drain BEFORE we set _cleaned_up (or kill the child):
        # capture bytes the child emitted between the caller's last
        # read_until/observe and our cleanup.  Without this, the trace
        # tail is lost when the caller returns from the test body
        # before the child has finished writing — and the resulting
        # stdout.log is 0 bytes (or missing entirely if the artifact
        # uploader skips files that were empty when the upload step
        # ran).  Two iterations: the first drains the already-buffered
        # bytes, the second closes out the EOF on each pipe.
        try:
            self._drain(timeout=0.5)
            self._drain(timeout=0.1)
        except Exception:
            pass
        self._cleaned_up = True
        if self._proc is None:
            self._close_files()
            return
        try:
            if self._proc.poll() is None:
                # Phase 1: SIGTERM the entire pgid, wait up to 5s.
                self._killpg(signal.SIGTERM)
                try:
                    self._returncode = self._proc.wait(
                        timeout=_TERMINATE_TIMEOUT_S,
                    )
                except subprocess.TimeoutExpired:
                    # Phase 2: SIGKILL the entire pgid.
                    self._killpg(signal.SIGKILL)
                    try:
                        self._returncode = self._proc.wait(
                            timeout=_TERMINATE_TIMEOUT_S,
                        )
                    except Exception:
                        pass
                self._stopped_by_runner = True
            else:
                # Already exited but not reaped: reap and record code.
                try:
                    self._returncode = self._proc.wait(timeout=0.5)
                except Exception:
                    self._returncode = self._proc.returncode
        except Exception:
            # A teardown failure must not mask the caller's error — but
            # it MUST be loud: a surviving child is an orphan that keeps
            # the disk image locked.
            print(
                f"WARNING: ProcessSession teardown failed for {self._argv!r}",
                file=sys.stderr,
            )
        # The child must not outlive the session, whatever happened
        # above.
        self._ensure_dead()
        # Drain one more time after wait() in case the child emitted
        # bytes between our pre-drain above and process exit.
        try:
            self._drain(timeout=0.5)
            self._drain(timeout=0.1)
        except Exception:
            pass

        self._close_files()

    def _killpg(self, sig: int) -> None:
        """Signal the child's whole process group (never raises)."""
        if self._proc is None:
            return
        try:
            os.killpg(self._proc.pid, sig)
        except (ProcessLookupError, PermissionError, OSError):
            pass

    def _ensure_dead(self) -> None:
        """The child must not outlive the session.

        The SIGTERM/SIGKILL sequence above is the normal path; this is
        the belt-and-braces check for what it cannot cover (the wait was
        interrupted, the child was never reaped).  SIGKILL cannot be
        caught, so a child still alive after this is in uninterruptible
        sleep — nothing more can be done for it from here.
        """
        if self._proc is None:
            return
        try:
            if self._proc.poll() is None:
                self._killpg(signal.SIGKILL)
                try:
                    self._proc.wait(timeout=_TERMINATE_TIMEOUT_S)
                except Exception:
                    pass
        except Exception:
            pass

    def _close_files(self) -> None:
        # Flush + close log files (independent of each other).
        for attr in ("_stdout_log", "_stderr_log"):
            log = getattr(self, attr)
            if log is not None:
                try:
                    log.flush()
                except Exception:
                    pass
                try:
                    log.close()
                except Exception:
                    pass
                setattr(self, attr, None)
        # Close stdin / pipes.
        if self._proc is not None:
            if self._proc.stdin is not None:
                try:
                    self._proc.stdin.close()
                except Exception:
                    pass
            for f in (self._proc.stdout, self._proc.stderr):
                if f is not None:
                    try:
                        f.close()
                    except Exception:
                        pass
        # Close the selector.
        if self._selector is not None:
            try:
                keys = list(self._selector.get_map().values())
                for key in keys:
                    try:
                        self._selector.unregister(key.fileobj)
                    except Exception:
                        pass
                self._selector.close()
            except Exception:
                pass
            self._selector = None