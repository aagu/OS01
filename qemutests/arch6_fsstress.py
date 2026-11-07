"""Four-CPU ARCH-6 acceptance through the shared QEMU runner/archive.

The final parent PASS must follow all twelve unique worker results. Any
FAIL or panic, malformed worker set or wrong CPU evidence fails closed.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import re
import sys
from typing import Optional

FS_NAMES = ("ext2", "tmpfs", "vfat")
PERCPU_RE = re.compile(r"percpu: (\d+) CPU\(s\) registered")
WORKER_RE = re.compile(
    r"\[arch6-fsstress\] (ext2|tmpfs|vfat) worker(\d+) (PASS|FAIL)(?=$|\s|\()",
    re.MULTILINE,
)
FINAL_RE = re.compile(r"\[arch6-fsstress\] PASS\r?$", re.MULTILINE)
TERMINAL_RE = re.compile(r"\[arch6-fsstress\] (?:PASS|FAIL)\b")


@dataclass
class FsStressResult:
    passed: bool = False
    workers: dict = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)


def parse_fsstress_log(log: str, expected_smp: int = 4,
                       expected_workers: int = 4) -> FsStressResult:
    result = FsStressResult()
    text = str(log)
    if "kernel panic" in text.lower() or "panic:" in text.lower():
        result.errors.append("Kernel panic detected")
    if re.search(r"\[arch6-fsstress\][^\n]*\bFAIL\b", text):
        result.errors.append("Failure marker detected")
    cpus = PERCPU_RE.findall(text)
    if len(cpus) != 1 or int(cpus[0]) != expected_smp:
        result.errors.append(f"Expected one {expected_smp}-CPU boot record, saw {cpus}")

    expected = {(fs, worker) for fs in FS_NAMES
                for worker in range(1, expected_workers + 1)}
    last_worker_end = 0
    for match in WORKER_RE.finditer(text):
        fs, worker, outcome = match.group(1), int(match.group(2)), match.group(3)
        key = fs, worker
        last_worker_end = match.end()
        if key not in expected:
            result.errors.append(f"Unexpected worker: {key}")
        elif key in result.workers:
            result.errors.append(f"Duplicate worker: {key}")
        else:
            result.workers[key] = outcome == "PASS"
    if set(result.workers) != expected or not all(result.workers.values()):
        result.errors.append("Required twelve-worker PASS set is incomplete")
    final = list(FINAL_RE.finditer(text))
    if len(final) != 1 or final[0].start() < last_worker_end:
        result.errors.append("Expected one final parent PASS after all worker results")
    result.passed = not result.errors
    return result


def resolve_smp(smp: Optional[str], qemu_smp: Optional[str]) -> str:
    if smp != "4":
        raise ValueError(f"SUITE=arch6-fsstress requires SMP=4, got {smp!r}")
    if qemu_smp not in (None, "", "4"):
        raise ValueError(f"QEMU_SMP={qemu_smp!r} conflicts with SMP=4")
    return "4"


def test_arch6_fsstress(tester) -> bool:
    try:
        resolve_smp(str(getattr(tester, "_effective_smp", "1")), None)
    except ValueError as error:
        print(f"[arch6-fsstress] FAIL: {error}", file=sys.stderr)
        return False
    tester.timeout = 180
    # Serial stdio belongs to ProcessSession, so the standard stdout.log
    # archive contains the evidence rather than an unrelated temporary file.
    tester.start_qemu(serial_stdio=True)
    terminal = tester.read_until(TERMINAL_RE, timeout=180)
    tester.process.observe(1.0)             # catch a post-completion panic
    data = tester._read_available()
    text = data.decode("utf-8", errors="replace") if isinstance(data, bytes) else str(data)
    result = parse_fsstress_log(text)
    if terminal is None:
        result.errors.append("No terminal result within the suite deadline")
        result.passed = False
    if result.passed:
        print("[arch6-fsstress] All 12 worker results and final parent PASS verified")
    else:
        for error in result.errors:
            print(f"[arch6-fsstress] FAIL: {error}", file=sys.stderr)
    return result.passed
