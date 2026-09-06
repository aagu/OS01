#!/usr/bin/env python3
"""Acceptance harness for the AArch64 UEFI PSCI SMP bring-up."""

import argparse
import hashlib
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
[smp] topology requested=4 discovered=4\n
[smp] cpu=0 online\n
[smp] cpu=1 online\n
[smp] cpu=2 online\n
[smp] cpu=3 online\n
[smp] summary requested=4 online=4 status=PASS\n
[spinlock] cpu 0: done=1000000/1000000\n
[spinlock] cpu 1: done=1000000/1000000\n
[spinlock] cpu 2: done=1000000/1000000\n
[spinlock] cpu 3: done=1000000/1000000\n
[spinlock] total=4000000 (active_cpus=4 × 1000000, PASS)\n
[smp-test] no_ack_cpu=0\n
[tick] 1\n
[tick] 2\n
[tick] 3\n
"""

log_with_only_uefi_banner = "UEFI: booting OS01\n"
log_with_online4_but_no_done = "\n".join(
    line for line in complete_log_for_4_cpus.splitlines() if " done=1000000" not in line
)
log_with_degraded_and_ticks = complete_log_for_4_cpus.replace(
    "[smp] summary requested=4 online=4 status=PASS",
    "[smp] summary requested=4 online=1 status=DEGRADED",
)
log_with_duplicate_cpu_ack = complete_log_for_4_cpus.replace(
    "[spinlock] cpu 3: done=1000000/1000000", "[spinlock] cpu 2: done=1000000/1000000"
)
log_with_wrong_total = complete_log_for_4_cpus.replace(
    "total=4000000", "total=3999999"
)
log_with_missing_total = complete_log_for_4_cpus.replace(
    "[spinlock] total=4000000 (active_cpus=4 × 1000000, PASS)\n", ""
)
complete_degraded_log = """\
[smp] topology requested=2 discovered=2\n
[smp] cpu=0 online\n
[smp] timeout cpu=1 reason=online-timeout\n
[smp] summary requested=2 online=1 status=DEGRADED\n
[spinlock] status=SKIP\n
[smp-test] no_ack_cpu=1\n
[tick] 1\n
[tick] 2\n
[tick] 3\n
"""

current_log_for_2_cpus = """\
[smp] topology source=uefi-dtb cpus=2
[smp] cpu=0 online mpidr=0x0
[smp] cpu=1 online mpidr=0x1
[smp] requested=2 online=2 status=PASS
[spinlock] cpu=0 done=1000000
[spinlock] cpu=1 done=1000000
[spinlock] active=2 iterations=1000000 total=2000000 status=PASS
[smp-test] no_ack_cpu=0
[tick] 1
[tick] 2
[tick] 3
"""

current_degraded_log = """\
[smp] topology source=uefi-dtb cpus=2
[smp-test] no_ack_cpu=1
[smp] cpu=0 online mpidr=0x0
[smp] cpu=1 reason=online-timeout
[smp] cpu=1 target_mpidr=0x1 rc=0
[smp] requested=2 online=1 status=DEGRADED
[spinlock] status=SKIP
[tick] 1
[tick] 2
[tick] 3
"""


def self_test() -> None:
    assert not passed(log_with_only_uefi_banner, cpus=4)
    assert not passed(log_with_online4_but_no_done, cpus=4)
    assert not passed(log_with_degraded_and_ticks, cpus=4)
    assert not passed(log_with_duplicate_cpu_ack, cpus=4)
    assert not passed(log_with_wrong_total, cpus=4)
    assert not passed(log_with_missing_total, cpus=4)
    assert passed(complete_log_for_4_cpus, cpus=4)
    assert passed(current_log_for_2_cpus, cpus=2)
    assert passed(current_log_for_2_cpus.replace("\n", "\n\r"), cpus=2)
    assert not passed(current_log_for_2_cpus.replace(
        "cpu=1 done=1000000", "cpu=0 done=1000000"), cpus=2)
    assert not passed(current_log_for_2_cpus.replace(
        "total=2000000", "total=1999999"), cpus=2)
    assert not passed(current_log_for_2_cpus + "[smp] cpu=1 online mpidr=0x1\n", cpus=2)
    assert not passed(current_log_for_2_cpus.replace(
        "requested=2 online=2", "requested=2 online=1"), cpus=2)
    assert not passed(current_log_for_2_cpus.replace("[spinlock] cpu=1 done=1000000\n", ""), cpus=2)
    assert not passed(current_log_for_2_cpus.replace("no_ack_cpu=0", "no_ack_cpu=01"), cpus=2)
    single = current_log_for_2_cpus.replace("cpus=2", "cpus=1").replace(
        "[smp] cpu=1 online mpidr=0x1\n", "").replace(
        "requested=2 online=2", "requested=1 online=1").replace(
        "[spinlock] cpu=1 done=1000000\n", "").replace(
        "active=2", "active=1").replace("total=2000000", "total=1000000")
    assert passed(single, cpus=1)
    assert not passed(single, cpus=2)
    assert degraded_passed(complete_degraded_log)
    assert not degraded_passed(complete_degraded_log + "[smp] FATAL: test failure\n")
    assert degraded_passed(current_degraded_log)
    assert degraded_passed(current_degraded_log.replace("\n", "\n\r"))
    for extra in ("[smp] cpu=1 online mpidr=0x1\n", "[smp] cpu=0 online\n",
                  "[spinlock] cpu=0 done=1000000\n", "[spinlock] total=1000000 status=PASS\n",
                  "[smp-test] no_ack_cpu=0\n", "[smp] FATAL: bad state\n"):
        assert not degraded_passed(current_degraded_log + extra), extra
    for old, new in (("no_ack_cpu=1", "no_ack_cpu=0"),
                     ("reason=online-timeout", "reason=cpu-on-error"),
                     ("online=1 status=DEGRADED", "online=2 status=PASS"),
                     ("[spinlock] status=SKIP\n", ""), ("[tick] 3\n", "")):
        assert not degraded_passed(current_degraded_log.replace(old, new)), old
    command_args = argparse.Namespace(
        qemu="qemu-system-aarch64", firmware="firmware.fd", image="disk.img"
    )
    assert "if=none,file=disk.img,format=raw,readonly=on,id=disk" in qemu_command(command_args, 4)


def kernel_failure(text: str) -> bool:
    """Return true only for structured kernel failure diagnostics."""
    return bool(re.search(
        r"^\[(?:smp|spinlock)\][^\n]*\b(?:FATAL|PANIC|FAIL|DEGRADED)\b",
        text,
        re.MULTILINE,
    ))


def hard_kernel_failure(text: str) -> bool:
    return bool(re.search(
        r"^\[(?:smp|spinlock)\][^\n]*\b(?:FATAL|PANIC|FAIL)\b",
        text,
        re.MULTILINE,
    ))


def passed(text: str, cpus: int) -> bool:
    """Recognize a complete normal-mode SMP run without QEMU dependencies."""
    # PL011 currently emits LF+CR. Match lines consistently for saved logs
    # and live serial drains, while retaining the original fixture format.
    text = text.replace("\r", "")
    if kernel_failure(text):
        return False
    topology = re.search(r"^\[smp\] topology\b[^\n]*\brequested=(\d+)\b[^\n]*\bdiscovered=(\d+)\b", text, re.MULTILINE)
    if topology:
        if tuple(map(int, topology.groups())) != (cpus, cpus):
            return False
    elif f"[smp] topology source=uefi-dtb cpus={cpus}\n" not in text:
        return False
    online = re.findall(r"^\[smp\] cpu=(\d+) online(?: mpidr=0x[0-9a-fA-F]+)?$", text, re.MULTILINE)
    if len(online) != cpus or {int(cpu) for cpu in online} != set(range(cpus)):
        return False
    summary = re.search(r"^\[smp\] (?:summary )?requested=(\d+) online=(\d+) status=PASS$", text, re.MULTILINE)
    if not summary or tuple(map(int, summary.groups())) != (cpus, cpus):
        return False
    done = re.findall(r"^\[spinlock\] cpu[ =](\d+):? done=1000000(?:/1000000)?$", text, re.MULTILINE)
    if len(done) != cpus or {int(cpu) for cpu in done} != set(range(cpus)):
        return False
    total = cpus * 1000000
    legacy_total = f"[spinlock] total={total} (active_cpus={cpus} × 1000000, PASS)"
    current_total = f"[spinlock] active={cpus} iterations=1000000 total={total} status=PASS"
    if legacy_total not in text and current_total not in text:
        return False
    if re.findall(r"^\[smp-test\] no_ack_cpu=([^\n]+)$", text, re.MULTILINE) != ["0"]:
        return False
    return len(re.findall(r"^\[tick\] \d+$", text, re.MULTILINE)) >= 3


def degraded_passed(text: str) -> bool:
    """Recognize the one intentionally degraded, non-benchmark case."""
    text = text.replace("\r", "")
    if hard_kernel_failure(text):
        return False
    if not re.search(r"^\[smp\] topology (?:requested=2 discovered=2|source=uefi-dtb cpus=2)$", text, re.MULTILINE):
        return False
    online = re.findall(r"^\[smp\] cpu=(\d+) online(?: mpidr=0x[0-9a-fA-F]+)?$", text, re.MULTILINE)
    if online != ["0"]:
        return False
    timeouts = re.findall(r"^\[smp\] (?:timeout )?cpu=(\d+) reason=online-timeout$", text, re.MULTILINE)
    summaries = re.findall(r"^\[smp\] (?:summary )?requested=(\d+) online=(\d+) status=(\w+)$", text, re.MULTILINE)
    injections = re.findall(r"^\[smp-test\] no_ack_cpu=([^\n]+)$", text, re.MULTILINE)
    if timeouts != ["1"] or summaries != [("2", "1", "DEGRADED")] or injections != ["1"]:
        return False
    # A no-ACK case must never start even a partial shared-count test.
    spinlock = re.findall(r"^\[spinlock\][^\n]*$", text, re.MULTILINE)
    if spinlock != ["[spinlock] status=SKIP"]:
        return False
    after_skip = text.split("[spinlock] status=SKIP\n", 1)[-1]
    if len(re.findall(r"^\[tick\] \d+$", after_skip, re.MULTILINE)) < 3:
        return False
    return not bool(re.search(r"^\[[^]]*(?:bench|spinlock)[^]]*\][^\n]*\bPASS\b", text, re.MULTILINE | re.IGNORECASE))


def acceptance_evidence(args: argparse.Namespace, text: str, cpus: int) -> bool:
    return degraded_passed(text) if args.expect_no_ack is not None else passed(text, cpus)


def qemu_command(args: argparse.Namespace, cpus: int) -> list[str]:
    diagnostic_dtb = getattr(args, "diagnostic_dtb", None)
    command = [
        args.qemu, "-M", "virt,gic-version=2" + (",acpi=off" if diagnostic_dtb else ""),
        "-cpu", "cortex-a53", "-smp", str(cpus),
        "-m", "512", "-drive", f"if=pflash,format=raw,file={args.firmware}",
        "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk", "-serial", "stdio", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]
    if diagnostic_dtb:
        command.extend(["-dtb", diagnostic_dtb])
    return command


def file_sha256(path: str) -> str:
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run_case(args: argparse.Namespace, cpus: int, iteration: int) -> bool:
    """Run one QEMU case with a monotonic deadline and non-blocking drains."""
    prefix = Path(args.log_dir) / f"cpus-{cpus}-run-{iteration}"
    stdout_path = prefix.with_suffix(".stdout.log")
    stderr_path = prefix.with_suffix(".stderr.log")
    command = qemu_command(args, cpus)
    started = time.monotonic()
    metadata_path = prefix.with_suffix(".metadata.json")
    metadata = {
        "command": command, "cpus": cpus, "run": iteration,
        "expected_no_ack_cpu": args.expect_no_ack or 0,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
        "diagnostic_dtb": getattr(args, "diagnostic_dtb", None),
    }
    if metadata["diagnostic_dtb"]:
        metadata["diagnostic_dtb_sha256"] = file_sha256(metadata["diagnostic_dtb"])
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    stdout = bytearray()
    stderr = bytearray()
    timed_out = False
    complete = False
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
            text = (stdout + stderr).decode("utf-8", errors="replace")
            if acceptance_evidence(args, text, cpus):
                complete = True
                break
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
    accepted = acceptance_evidence(args, text, cpus)
    result = accepted and not timed_out
    metadata.update({
        "compiled_no_ack_cpu": [int(value) for value in re.findall(
            r"^\[smp-test\] no_ack_cpu=(\d+)$", text.replace("\r", ""), re.MULTILINE)],
        "elapsed_seconds": time.monotonic() - started,
        "timeout": timed_out, "complete": complete, "returncode": returncode,
        "result": "PASS" if result else "FAIL",
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({
        "event": "case", "cpus": cpus, "run": iteration, "result": "PASS" if result else "FAIL",
        "timeout": timed_out, "complete": complete, "returncode": returncode,
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
    parser.add_argument("--diagnostic-dtb", metavar="PATH",
                        help="explicit firmware diagnostic: acpi=off with this packed QEMU DTB (one CPU count only)")
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
    if args.diagnostic_dtb and len(args.cpus) != 1:
        parser.error("--diagnostic-dtb requires one --cpus value matching the DTB")
    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    outcomes = [run_case(args, cpus, iteration)
                for cpus in args.cpus for iteration in range(1, args.repeat + 1)]
    return 0 if all(outcomes) else 1


if __name__ == "__main__":
    sys.exit(main())
