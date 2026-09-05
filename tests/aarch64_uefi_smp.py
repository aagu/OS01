#!/usr/bin/env python3
"""Acceptance harness for the AArch64 UEFI PSCI SMP bring-up."""

import argparse
import json
import os
import re
import selectors
import subprocess
import sys
import time
from pathlib import Path


complete_log_for_4_cpus = """\
UEFI: booting OS01\n
[smp-test] requested=4 online=4 status=PASS\n
[smp-test] cpu=0 state=done\n
[smp-test] cpu=1 state=done\n
[smp-test] cpu=2 state=done\n
[smp-test] cpu=3 state=done\n
[spinlock] status=PASS\n
[smp-test] no_ack_cpu=0\n
[smp-test] tick=1\n
[smp-test] tick=2\n
[smp-test] tick=3\n
"""

log_with_only_uefi_banner = "UEFI: booting OS01\n"
log_with_online4_but_no_done = "\n".join(
    line for line in complete_log_for_4_cpus.splitlines() if " state=done" not in line
)
log_with_degraded_and_ticks = complete_log_for_4_cpus.replace(
    "requested=4 online=4 status=PASS", "requested=4 online=1 status=DEGRADED"
)
log_with_duplicate_cpu_ack = complete_log_for_4_cpus.replace(
    "[smp-test] cpu=3 state=done", "[smp-test] cpu=2 state=done"
)
log_with_wrong_total = complete_log_for_4_cpus.replace(
    "requested=4 online=4 status=PASS", "requested=4 online=3 status=PASS"
)


def self_test() -> None:
    assert not passed(log_with_only_uefi_banner, cpus=4)
    assert not passed(log_with_online4_but_no_done, cpus=4)
    assert not passed(log_with_degraded_and_ticks, cpus=4)
    assert not passed(log_with_duplicate_cpu_ack, cpus=4)
    assert not passed(log_with_wrong_total, cpus=4)
    assert passed(complete_log_for_4_cpus, cpus=4)
    command_args = argparse.Namespace(
        qemu="qemu-system-aarch64", firmware="firmware.fd", image="disk.img"
    )
    assert "if=none,file=disk.img,format=raw,readonly=on,id=disk" in qemu_command(command_args, 4)


def kernel_failure(text: str) -> bool:
    """Return true only for structured kernel failure diagnostics."""
    return bool(re.search(
        r"^\[(?:smp-test|spinlock)\][^\n]*\b(?:FATAL|PANIC|FAIL|DEGRADED)\b",
        text,
        re.MULTILINE,
    ))


def passed(text: str, cpus: int) -> bool:
    """Recognize a complete normal-mode SMP run without QEMU dependencies."""
    if kernel_failure(text):
        return False
    if f"[smp-test] requested={cpus} online={cpus} status=PASS" not in text:
        return False
    done = re.findall(r"^\[smp-test\] cpu=(\d+) state=done$", text, re.MULTILINE)
    if len(done) != cpus or {int(cpu) for cpu in done} != set(range(cpus)):
        return False
    if "[smp-test] no_ack_cpu=0" not in text:
        return False
    return len(re.findall(r"^\[smp-test\] tick=\d+$", text, re.MULTILINE)) >= 3


def degraded_passed(text: str) -> bool:
    """Recognize the one intentionally degraded, non-benchmark case."""
    required = (
        "[smp-test] requested=2 online=1 status=DEGRADED",
        "[smp-test] cpu=1 reason=online-timeout",
        "[spinlock] status=SKIP",
        "[smp-test] no_ack_cpu=1",
    )
    if not all(marker in text for marker in required):
        return False
    if len(re.findall(r"^\[smp-test\] tick=\d+$", text, re.MULTILINE)) < 3:
        return False
    return not bool(re.search(r"^\[[^]]*(?:bench|spinlock)[^]]*\][^\n]*\bPASS\b", text, re.MULTILINE | re.IGNORECASE))


def qemu_command(args: argparse.Namespace, cpus: int) -> list[str]:
    return [
        args.qemu, "-M", "virt,gic-version=2", "-cpu", "cortex-a53", "-smp", str(cpus),
        "-m", "512", "-drive", f"if=pflash,format=raw,file={args.firmware}",
        "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk", "-serial", "stdio", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]


def run_case(args: argparse.Namespace, cpus: int, iteration: int) -> bool:
    """Run one QEMU case with a monotonic deadline and non-blocking drains."""
    prefix = Path(args.log_dir) / f"cpus-{cpus}-run-{iteration}"
    stdout_path = prefix.with_suffix(".stdout.log")
    stderr_path = prefix.with_suffix(".stderr.log")
    command = qemu_command(args, cpus)
    stdout = bytearray()
    stderr = bytearray()
    timed_out = False
    returncode = None
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    except OSError as error:
        print(json.dumps({"event": "spawn-error", "cpus": cpus, "run": iteration, "error": str(error)}))
        return False

    streams = {process.stdout: stdout, process.stderr: stderr}
    selector = selectors.DefaultSelector()
    deadline = time.monotonic() + args.timeout
    try:
        for stream in streams:
            os.set_blocking(stream.fileno(), False)
            selector.register(stream, selectors.EVENT_READ)
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            for key, _ in selector.select(remaining):
                chunk = os.read(key.fileobj.fileno(), 4096)
                if chunk:
                    streams[key.fileobj].extend(chunk)
                else:
                    selector.unregister(key.fileobj)
            if process.poll() is not None and not selector.get_map():
                break
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        returncode = process.returncode
        stdout_path.write_bytes(stdout)
        stderr_path.write_bytes(stderr)

    text = (stdout + stderr).decode("utf-8", errors="replace")
    accepted = degraded_passed(text) if args.expect_no_ack is not None else passed(text, cpus)
    result = accepted and not timed_out
    print(json.dumps({
        "event": "case", "cpus": cpus, "run": iteration, "result": "PASS" if result else "FAIL",
        "timeout": timed_out, "returncode": returncode,
        "stdout": str(stdout_path), "stderr": str(stderr_path),
    }))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--cpus", nargs="+", type=int, default=[1, 2, 4])
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--qemu")
    parser.add_argument("--log-dir")
    parser.add_argument("--expect-no-ack", type=int, metavar="CPU_ID")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("aarch64_uefi_smp: self-test passed")
        return 0
    if not all((args.firmware, args.image, args.qemu, args.log_dir)):
        parser.error("--firmware, --image, --qemu, and --log-dir are required outside --self-test")
    if any(cpus < 1 for cpus in args.cpus) or args.repeat < 1 or args.timeout <= 0:
        parser.error("--cpus and --repeat must be positive; --timeout must be greater than zero")
    if args.expect_no_ack is not None and (args.cpus != [2] or args.repeat != 1 or args.expect_no_ack != 1):
        parser.error("--expect-no-ack only accepts CPU_ID=1 with --cpus 2 --repeat 1")
    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    outcomes = [run_case(args, cpus, iteration)
                for cpus in args.cpus for iteration in range(1, args.repeat + 1)]
    return 0 if all(outcomes) else 1


if __name__ == "__main__":
    sys.exit(main())
