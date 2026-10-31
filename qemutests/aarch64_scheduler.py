#!/usr/bin/env python3
"""AArch64 Phase 3 normal-image scheduler idle smoke.

Boot an already-built normal image at each requested CPU count, retain the
serial log, and require published CPU0 idle readiness followed by real ticks.
This is intentionally a focused handoff/idle check, not a full selftest suite.
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))
from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb  # noqa: E402

READY = "[sched] idle-ready cpu=0 scheduler_ok=1 irq=on"
FATAL = re.compile(r"(\[.*FATAL|\bPANIC\b|\bOops\b|Exception level .* fatal|context fatal)", re.I)
TICK = re.compile(r"^\[tick\]\s+(\d+)\s*$", re.M)


def qemu_command(qemu, cpus, image, firmware, dtb):
    cmd = [qemu, "-M", "virt,gic-version=2,acpi=off", "-cpu", "cortex-a53",
           "-smp", str(cpus), "-m", "512",
           "-drive", f"if=pflash,format=raw,file={firmware}",
           "-drive", f"if=none,file={image},format=raw,readonly=on,id=disk",
           "-device", "virtio-blk-device,drive=disk", "-serial", "stdio",
           "-display", "none", "-no-reboot", "-no-shutdown", "-dtb", dtb]
    return cmd


def run_one(args, cpus, log_dir):
    case_dir = log_dir / f"cpus-{cpus}"
    case_dir.mkdir(parents=True, exist_ok=True)
    log_path = case_dir / "serial.log"
    dtb = generate_diagnostic_dtb(args.qemu, str(case_dir), cpus)
    command = qemu_command(args.qemu, cpus, args.image, args.firmware, dtb)
    started = time.monotonic()
    with log_path.open("wb") as stream:
        proc = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        try:
            while time.monotonic() - started < args.timeout:
                if proc.poll() is not None:
                    break
                stream.flush()
                text = log_path.read_text(errors="replace")
                if FATAL.search(text):
                    raise RuntimeError(f"fatal/crash marker in {log_path}")
                marker_pos = text.find(READY)
                if marker_pos >= 0:
                    after = text[marker_pos + len(READY):]
                    ticks = [int(v) for v in TICK.findall(after)]
                    if len(ticks) >= 3 and all(a < b for a, b in zip(ticks, ticks[1:])):
                        return 0, log_path, command
                time.sleep(0.2)
            else:
                raise TimeoutError(f"idle-ready + 3 increasing ticks not seen in {args.timeout}s")
            stream.flush()
            text = log_path.read_text(errors="replace")
            if FATAL.search(text):
                raise RuntimeError(f"fatal/crash marker in {log_path}")
            raise RuntimeError(f"QEMU exited before scheduler smoke completed (rc={proc.returncode})")
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--idle-smoke", action="store_true", required=True)
    parser.add_argument("--cpus", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--qemu", default=os.environ.get("QEMU_BIN", "qemu-system-aarch64"))
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--log-dir")
    args = parser.parse_args()
    if not Path(args.image).is_file() or not Path(args.firmware).is_file():
        parser.error("--image and --firmware must name existing files")
    if args.timeout != 120:
        parser.error("Phase 3 idle smoke uses a fixed 120-second deadline")
    if any(c not in (1, 2, 4, 8) for c in args.cpus):
        parser.error("--cpus values must be selected from 1 2 4 8")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log_dir = Path(args.log_dir) if args.log_dir else _ROOT / "test-results" / "aarch64-scheduler" / stamp
    log_dir.mkdir(parents=True, exist_ok=True)
    failed = False
    for cpus in args.cpus:
        try:
            _, log_path, command = run_one(args, cpus, log_dir)
            print(f"[aarch64-scheduler] cpus={cpus} status=PASS log={log_path}")
            print("[aarch64-scheduler] command=" + " ".join(command))
        except Exception as exc:  # evidence collection should continue across counts
            failed = True
            print(f"[aarch64-scheduler] cpus={cpus} status=FAIL error={exc}", file=sys.stderr)
            print(f"[aarch64-scheduler] log={log_dir / f'cpus-{cpus}' / 'serial.log'}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
