#!/usr/bin/env python3
"""Bounded framework test supervisor (Task C1).

Runs unittest modules sequentially with per-module and total time budgets,
continuous stdout/stderr draining, and process-group cleanup on timeout
or interruption.
"""

from __future__ import annotations

import argparse
import os
import select
import signal
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple

_current_proc: Optional[subprocess.Popen] = None
_interrupted: bool = False


def _sigint_handler(signum, frame):
    global _interrupted
    _interrupted = True
    if _current_proc is not None and _current_proc.poll() is None:
        try:
            os.killpg(_current_proc.pid, signal.SIGTERM)
            time.sleep(0.5)
            if _current_proc.poll() is None:
                os.killpg(_current_proc.pid, signal.SIGKILL)
        except OSError:
            pass
    print("\n[framework] Interrupted by user (SIGINT)", file=sys.stderr)
    sys.exit(130)


def _kill_process_group(proc: subprocess.Popen, grace_seconds: float = 1.0) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except OSError:
        return
    t0 = time.monotonic()
    while time.monotonic() - t0 < grace_seconds:
        if proc.poll() is not None:
            return
        time.sleep(0.05)
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        pass


def run_module(
    module: str,
    module_timeout: float,
    remaining_total_budget: float,
) -> Tuple[str, float, str, str]:
    """Run a single test module.

    Returns (status, duration_s, stdout_text, stderr_text).
    status is one of "PASS", "FAIL", "TIMEOUT", "ERROR".
    """
    global _current_proc
    budget = min(module_timeout, remaining_total_budget)
    t0 = time.monotonic()

    print(f"[framework] START {module} (timeout={budget:.1f}s)", flush=True)

    proc = subprocess.Popen(
        [sys.executable, "-u", "-m", "unittest", "-v", module],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
        bufsize=1,
    )
    _current_proc = proc

    stdout_chunks: List[str] = []
    stderr_chunks: List[str] = []
    timed_out = False

    reads = [proc.stdout, proc.stderr]
    # Set non-blocking on streams
    for f in reads:
        if f is not None:
            os.set_blocking(f.fileno(), False)

    while reads:
        if time.monotonic() - t0 >= budget:
            timed_out = True
            _kill_process_group(proc)
            break

        timeout_slice = min(0.2, max(0.0, budget - (time.monotonic() - t0)))
        try:
            rlist, _, _ = select.select(reads, [], [], timeout_slice)
        except select.error:
            break

        for stream in rlist:
            data = stream.read()
            if not data:
                # EOF on stream
                reads.remove(stream)
            else:
                if stream is proc.stdout:
                    stdout_chunks.append(data)
                    sys.stdout.write(data)
                    sys.stdout.flush()
                else:
                    stderr_chunks.append(data)
                    sys.stderr.write(data)
                    sys.stderr.flush()

        if proc.poll() is not None and not rlist:
            # Child exited and pipes drained
            break

    # Final drain after process exit or timeout
    try:
        stdout_rem, stderr_rem = proc.communicate(timeout=0.5)
        if stdout_rem:
            stdout_chunks.append(stdout_rem)
            sys.stdout.write(stdout_rem)
            sys.stdout.flush()
        if stderr_rem:
            stderr_chunks.append(stderr_rem)
            sys.stderr.write(stderr_rem)
            sys.stderr.flush()
    except Exception:
        pass

    _current_proc = None
    duration = time.monotonic() - t0

    if timed_out:
        status = "TIMEOUT"
    elif proc.returncode == 0:
        status = "PASS"
    else:
        status = "FAIL"

    print(f"[framework] END {module} status={status} duration={duration:.2f}s", flush=True)
    return status, duration, "".join(stdout_chunks), "".join(stderr_chunks)


def main(argv: Optional[List[str]] = None) -> int:
    if argv is None:
        argv = sys.argv[1:]

    default_mod_to = float(os.environ.get("HARNESS_MODULE_TIMEOUT", 300.0))
    default_tot_to = float(os.environ.get("HARNESS_TOTAL_TIMEOUT", 1200.0))

    parser = argparse.ArgumentParser(description="Bounded test framework runner")
    parser.add_argument("--module-timeout", type=float, default=default_mod_to,
                        help=f"Per-module timeout in seconds (default: {default_mod_to})")
    parser.add_argument("--total-timeout", type=float, default=default_tot_to,
                        help=f"Total batch timeout in seconds (default: {default_tot_to})")
    parser.add_argument("modules", nargs="*", help="Unittest modules to run")

    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return 2

    if not args.modules:
        print("ERROR: no modules specified", file=sys.stderr)
        return 2

    signal.signal(signal.SIGINT, _sigint_handler)
    signal.signal(signal.SIGTERM, _sigint_handler)

    results: Dict[str, Tuple[str, float]] = {}
    batch_start = time.monotonic()
    total_budget = args.total_timeout

    for mod in args.modules:
        elapsed = time.monotonic() - batch_start
        remaining = total_budget - elapsed
        if remaining <= 0:
            print(f"[framework] SKIPPED {mod} (total budget exceeded)", flush=True)
            results[mod] = ("NOT_RUN", 0.0)
            continue

        status, dur, _, _ = run_module(mod, args.module_timeout, remaining)
        results[mod] = (status, dur)

    # Print summary
    print("\n" + "=" * 60)
    print("Framework Test Summary:")
    print("=" * 60)
    all_ok = True
    for mod, (st, dur) in results.items():
        print(f"  {st:8s}  {dur:6.2f}s  {mod}")
        if st != "PASS":
            all_ok = False
    print("=" * 60)

    total_dur = time.monotonic() - batch_start
    print(f"Total time: {total_dur:.2f}s")
    if all_ok:
        print("[framework] ALL MODULES PASSED")
        return 0
    else:
        print("[framework] SOME MODULES FAILED OR TIMED OUT", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
