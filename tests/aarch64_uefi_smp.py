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
UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872\n
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
UEFI: booting OS01\n
UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872\n
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
UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872
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
UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872
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
    assert degraded_passed(current_degraded_log.rstrip("\n"))
    # A serial drain may end exactly at SKIP, with no final newline. Old
    # ticks must not make run_case terminate QEMU before recovery ticks.
    late_skip = current_degraded_log.replace("[spinlock] status=SKIP\n", "") + "[spinlock] status=SKIP"
    assert not degraded_passed(late_skip)
    negative_args = argparse.Namespace(expect_no_ack=1)
    for suffix in ("", "\n", "\n[tick] 4\n", "\n[tick] 4\n[tick] 5\n"):
        assert not acceptance_evidence(negative_args, late_skip + suffix, 2)
    assert acceptance_evidence(negative_args, late_skip + "\n[tick] 4\n[tick] 5\n[tick] 6", 2)
    for extra in ("[smp] cpu=1 online mpidr=0x1\n", "[smp] cpu=0 online\n",
                  "[spinlock] cpu=0 done=1000000\n", "[spinlock] total=1000000 status=PASS\n",
                  "[smp-test] no_ack_cpu=0\n", "[smp] FATAL: bad state\n"):
        assert not degraded_passed(current_degraded_log + extra), extra
    for old, new in (("no_ack_cpu=1", "no_ack_cpu=0"),
                     ("reason=online-timeout", "reason=cpu-on-error"),
                     ("online=1 status=DEGRADED", "online=2 status=PASS"),
                     ("[spinlock] status=SKIP\n", ""), ("[tick] 3\n", "")):
        assert not degraded_passed(current_degraded_log.replace(old, new)), old

    # RAM-summary evidence must be present, unique, and arithmetically
    # consistent across all four positive fixtures. The live QEMU virt
    # -m 512 layout under this firmware produces three 2 MiB-aligned
    # ranges totalling 236 pages = 494,927,872 bytes (236 * 2097152).
    RAM_LINE = "UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872"
    for label, fixture, predicate in (
        ("complete_log_for_4_cpus", complete_log_for_4_cpus,
         lambda text: passed(text, cpus=4)),
        ("complete_degraded_log", complete_degraded_log, degraded_passed),
        ("current_log_for_2_cpus", current_log_for_2_cpus,
         lambda text: passed(text, cpus=2)),
        ("current_degraded_log", current_degraded_log, degraded_passed),
    ):
        assert predicate(fixture), f"unmutated {label} must pass"
        # Mutation: remove the RAM line.
        assert not predicate(fixture.replace(RAM_LINE + "\n", "")), \
            f"{label}: removing RAM line must reject"
        # Mutation: duplicate the RAM line.
        assert not predicate(fixture + RAM_LINE + "\n"), \
            f"{label}: duplicating RAM line must reject"
        # Mutation: ranges=0 violates the 1..16 bound.
        assert not predicate(fixture.replace(
            "ranges=3", "ranges=0")), \
            f"{label}: ranges=0 must reject"
        # Mutation: non-decimal field; regex anchors reject any non-digit.
        assert not predicate(fixture.replace(
            "pages2m=236", "pages2m=12a")), \
            f"{label}: non-decimal pages2m must reject"
        # Mutation: bytes off by one breaks the arithmetical check.
        assert not predicate(fixture.replace(
            "bytes=494927872", "bytes=494927871")), \
            f"{label}: bytes != pages2m * 2097152 must reject"

    command_args = argparse.Namespace(
        qemu="qemu-system-aarch64", firmware="firmware.fd", image="disk.img"
    )
    assert "if=none,file=disk.img,format=raw,readonly=on,id=disk" in qemu_command(command_args, 4, None)


def kernel_failure(text: str) -> bool:
    """Return true only for structured kernel failure diagnostics."""
    return bool(re.search(
        r"^\[(?:smp|spinlock)\][^\n]*\b(?:FATAL|PANIC|FAIL|DEGRADED)\b",
        text,
        re.MULTILINE,
    ))


def ram_summary_ok(text: str) -> bool:
    """Return True iff exactly one syntactically valid RAM summary
    line is present, with ranges in 1..16, pages2m > 0, and
    bytes == pages2m * 2097152."""
    text = text.replace("\r", "")
    lines = re.findall(
        r"^UEFI-A64: RAM ranges=(\d+) pages2m=(\d+) bytes=(\d+)$",
        text, re.MULTILINE,
    )
    if len(lines) != 1:
        return False
    ranges, pages2m, bytes_ = (int(x) for x in lines[0])
    return (1 <= ranges <= 16 and pages2m > 0
            and bytes_ == pages2m * 2097152)


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
    if not ram_summary_ok(text):
        return False
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
    if not ram_summary_ok(text):
        return False
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
    spinlock = list(re.finditer(r"^\[spinlock\][^\n]*$", text, re.MULTILINE))
    if len(spinlock) != 1 or spinlock[0].group() != "[spinlock] status=SKIP":
        return False
    after_skip = text[spinlock[0].end():]
    if len(re.findall(r"^\[tick\] \d+$", after_skip, re.MULTILINE)) < 3:
        return False
    return not bool(re.search(r"^\[[^]]*(?:bench|spinlock)[^]]*\][^\n]*\bPASS\b", text, re.MULTILINE | re.IGNORECASE))


def acceptance_evidence(args: argparse.Namespace, text: str, cpus: int) -> bool:
    return degraded_passed(text) if args.expect_no_ack is not None else passed(text, cpus)


def qemu_command(args: argparse.Namespace, cpus: int, diagnostic_dtb: str | None) -> list[str]:
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


def generate_diagnostic_dtb(qemu: str, log_dir: str, cpus: int) -> str:
    """Materialize a packed QEMU-generated virt DTB into the run log dir.

    Some prebuilt UEFI firmwares do not expose the device tree through the EFI
    configuration table. The kernel then cannot boot beyond `[dtb] FATAL:
    UEFI handoff has no DTB`. Passing a QEMU-emitted DTB via `-dtb` (with
    `acpi=off`) is the documented diagnostic workaround; this helper produces
    such a file on demand so the matrix harness remains runnable on those
    firmwares.

    The DTB carries the visible CPU count: with `acpi=off` the kernel reads
    `/cpus` directly from the blob, so `-smp N` only succeeds when the DTB
    was generated for the same N. Each matrix run therefore materializes its
    own DTB in a per-case log dir. The sparse QEMU dumpdtb output is repacked
    via `dtc` (its raw form is ~1 MiB of zero padding; a few KiB is what
    UEFI actually consumes).
    """
    case_dir = os.path.join(log_dir, f"dtb-cpus-{cpus}")
    Path(case_dir).mkdir(parents=True, exist_ok=True)
    sparse = os.path.join(case_dir, "qemu-virt.dtb.sparse")
    packed = os.path.join(case_dir, "qemu-virt.dtb")
    completed = subprocess.run(
        [qemu, "-M", "virt,gic-version=2", "-cpu", "cortex-a53", "-smp", str(cpus),
         "-machine", f"dumpdtb={sparse}", "-display", "none", "-m", "512"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    )
    if completed.returncode != 0 or not os.path.exists(sparse):
        raise RuntimeError(
            f"failed to generate diagnostic DTB via '{qemu} -machine dumpdtb='"
        )
    subprocess.run(["dtc", "-I", "dtb", "-O", "dtb", "-o", packed, sparse],
                   check=True)
    os.unlink(sparse)
    return packed


def run_case(args: argparse.Namespace, cpus: int, iteration: int) -> bool:
    """Run one QEMU case with a monotonic deadline and non-blocking drains."""
    diagnostic_dtb = None
    if getattr(args, "diagnostic_dtb", None):
        diagnostic_dtb = generate_diagnostic_dtb(args.qemu, args.log_dir, cpus)
    prefix = Path(args.log_dir) / f"cpus-{cpus}-run-{iteration}"
    stdout_path = prefix.with_suffix(".stdout.log")
    stderr_path = prefix.with_suffix(".stderr.log")
    command = qemu_command(args, cpus, diagnostic_dtb)
    started = time.monotonic()
    metadata_path = prefix.with_suffix(".metadata.json")
    metadata = {
        "command": command, "cpus": cpus, "run": iteration,
        "expected_no_ack_cpu": args.expect_no_ack or 0,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
        "diagnostic_dtb": diagnostic_dtb,
    }
    if diagnostic_dtb:
        metadata["diagnostic_dtb_sha256"] = file_sha256(diagnostic_dtb)
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
    if process.stdout is None or process.stderr is None:
        # stdout=PIPE/stderr=PIPE guarantees both are set; this guards the
        # type narrowing that selectors.DefaultSelector.register needs.
        return False

    # key by file descriptor (int) so selector.get_key and select()'s
    # key.fd both stay int-keyed and consistent with the streams map.
    streams = {process.stdout.fileno(): (process.stdout, stdout),
               process.stderr.fileno(): (process.stderr, stderr)}
    selector = selectors.DefaultSelector()
    deadline = time.monotonic() + args.timeout
    try:
        for fd, (stream, _) in streams.items():
            os.set_blocking(fd, False)
            selector.register(stream, selectors.EVENT_READ)
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            for key, _ in selector.select(remaining):
                chunk = os.read(key.fd, 4096)
                if chunk:
                    streams[key.fd][1].extend(chunk)
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
    parser.add_argument("--diagnostic-dtb", metavar="PATH_OR_AUTO",
                        help="firmware does not expose DTB via EFI config table: "
                             "use acpi=off and a QEMU-generated DTB. Pass an explicit "
                             "PATH to a prebuilt DTB or 'auto' to materialize one "
                             "into the run log dir. One --cpus value matching the DTB.")
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
    if args.diagnostic_dtb and args.diagnostic_dtb != "auto" and len(args.cpus) != 1:
        parser.error("--diagnostic-dtb requires one --cpus value matching the DTB")
    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    outcomes = [run_case(args, cpus, iteration)
                for cpus in args.cpus for iteration in range(1, args.repeat + 1)]
    return 0 if all(outcomes) else 1


if __name__ == "__main__":
    sys.exit(main())
