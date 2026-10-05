#!/usr/bin/env python3
"""Acceptance harness for the aarch64 M3 shootdown probe (spec §7.3).

Drives the PRODUCTION (non-selftest) kernel image in QEMU
``virt,gic-version=2`` with two ``cortex-a53`` CPUs (`-smp 2` — the
probe target is only CPU1) and verifies the serial log shows the
spec'd probe sequence:

    M3-SHOOTDOWN-PROBE: START
    M3-SHOOTDOWN-PROBE: OK

exactly once each, in that order, with NO ``M3-SHOOTDOWN-PROBE: FAIL``
line anywhere. The probe halts the kernel on any failure, so a FAIL
line (or a missing OK) is fatal for acceptance. A
``M3-SHOOTDOWN-PROBE: SKIP (single-CPU boot)`` line under `-smp 2`
means the DTB topology lied about the CPU count and is also a failure.

The parser is the contract surface — it is intentionally host-pure and
used both by ``--self-test`` and the live QEMU driver. Timeouts never
count as success: if the OK line never appears, the harness exits
non-zero even when the QEMU process terminated cleanly (the kernel
halts forever after a probe FAIL, so the harness must terminate QEMU
itself — mirroring qemutests/aarch64_sync_fault.py).
"""

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


# A typical live QEMU run starts with a small amount of UEFI banner
# text before the kernel probe path runs. The synthetic fixture mirrors
# that shape so the parser is exercised against the same window it
# sees live.
valid_log = """\
UEFI: booting OS01
UEFI-A64: RAM ranges=3 pages2m=236 bytes=494927872
M3-SHOOTDOWN-PROBE: START
M3-SHOOTDOWN-PROBE: OK
"""

valid_log_crlf = valid_log.replace("\n", "\r\n")

# Failure shapes the parser must reject.
fail_ap_not_ready = """\
M3-SHOOTDOWN-PROBE: START
M3-SHOOTDOWN-PROBE: FAIL ap-not-ready
"""
fail_requires_one_ap = """\
M3-SHOOTDOWN-PROBE: FAIL requires-at-least-one-AP
"""
skip_single_cpu = """\
M3-SHOOTDOWN-PROBE: SKIP (single-CPU boot)
"""
ok_before_start = """\
M3-SHOOTDOWN-PROBE: OK
M3-SHOOTDOWN-PROBE: START
"""
duplicated_ok = valid_log + "M3-SHOOTDOWN-PROBE: OK\n"
missing_ok = "M3-SHOOTDOWN-PROBE: START\n"

_START_PREFIX = "M3-SHOOTDOWN-PROBE: START"
_OK_PREFIX = "M3-SHOOTDOWN-PROBE: OK"
_FAIL_PREFIX = "M3-SHOOTDOWN-PROBE: FAIL"
_SKIP_PREFIX = "M3-SHOOTDOWN-PROBE: SKIP"

# Line-anchored so e.g. a hypothetical "...: OKK" cannot slip through.
_START_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: START\r?$", re.MULTILINE)
_OK_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: OK\r?$", re.MULTILINE)
_FAIL_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: FAIL.*$", re.MULTILINE)
_SKIP_RE = re.compile(r"^M3-SHOOTDOWN-PROBE: SKIP.*$", re.MULTILINE)


def probe_evidence(text: str) -> bool:
    """Contract parser: exactly one START, exactly one OK, OK strictly
    after START, and no FAIL / SKIP line anywhere."""
    # The PL011 serial path emits LF+CR ("\n\r") line endings; strip
    # stray CRs so the line-anchored regexes see plain "\n".
    text = text.replace("\r\n", "\n").replace("\n\r", "\n")
    start_matches = list(_START_RE.finditer(text))
    ok_matches = list(_OK_RE.finditer(text))
    if len(start_matches) != 1 or len(ok_matches) != 1:
        return False
    if start_matches[0].start() >= ok_matches[0].start():
        return False
    if _FAIL_RE.search(text) or _SKIP_RE.search(text):
        return False
    return True


def self_test() -> None:
    # Positive
    assert probe_evidence(valid_log), "valid synthetic log must pass"
    assert probe_evidence(valid_log_crlf), \
        "valid log with CRLF line endings must pass"
    assert probe_evidence(valid_log.replace("\n", "\n\r")), \
        "valid log with PL011's LF+CR line endings must pass"

    # Negative
    assert not probe_evidence(""), "empty log must fail"
    assert not probe_evidence(fail_ap_not_ready), \
        "FAIL ap-not-ready must fail"
    assert not probe_evidence(fail_requires_one_ap), \
        "FAIL requires-at-least-one-AP must fail"
    assert not probe_evidence(skip_single_cpu), \
        "single-CPU SKIP under -smp 2 must fail"
    assert not probe_evidence(ok_before_start), \
        "OK before START must fail"
    assert not probe_evidence(duplicated_ok), \
        "duplicated OK must fail"
    assert not probe_evidence(missing_ok), \
        "missing OK must fail"
    assert not probe_evidence(valid_log.replace(
        "M3-SHOOTDOWN-PROBE: OK", "M3-SHOOTDOWN-PROBE: OKK")), \
        "non-line-anchored OK match must fail"


def qemu_command(args: argparse.Namespace, diagnostic_dtb: str | None) -> list[str]:
    # -smp 2 per spec §7.3: the probe targets exactly CPU1. With a
    # diagnostic DTB, `acpi=off` makes the kernel read /cpus from the
    # blob, so the DTB must be generated for the same CPU count.
    command = [
        args.qemu, "-M", "virt,gic-version=2" + (",acpi=off" if diagnostic_dtb else ""),
        "-cpu", "cortex-a53", "-smp", "2", "-m", "512",
        "-drive", f"if=pflash,format=raw,file={args.firmware}",
        "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        "-serial", "stdio", "-display", "none",
        "-no-reboot", "-no-shutdown",
    ]
    if diagnostic_dtb:
        command.extend(["-dtb", diagnostic_dtb])
    return command


def file_sha256(path: str) -> str:
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def generate_diagnostic_dtb(qemu: str, log_dir: str, cpus: int,
                            ram_mib: int = 512) -> str:
    """Materialize a packed QEMU-generated virt DTB (same mechanism as
    qemutests/aarch64_uefi_smp.py): some prebuilt UEFI firmwares do not
    expose the device tree through the EFI configuration table, and the
    kernel then dies at `[dtb] FATAL: UEFI handoff has no DTB`."""
    case_dir = os.path.join(log_dir, f"dtb-ram-{ram_mib}-cpus-{cpus}")
    Path(case_dir).mkdir(parents=True, exist_ok=True)
    sparse = os.path.join(case_dir, "qemu-virt.dtb.sparse")
    packed = os.path.join(case_dir, "qemu-virt.dtb")
    completed = subprocess.run(
        [qemu, "-M", "virt,gic-version=2", "-cpu", "cortex-a53", "-smp", str(cpus),
         "-machine", f"dumpdtb={sparse}", "-display", "none", "-m", str(ram_mib)],
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


def run_case(args: argparse.Namespace) -> bool:
    diagnostic_dtb = None
    if getattr(args, "diagnostic_dtb", None):
        diagnostic_dtb = generate_diagnostic_dtb(
            args.qemu, args.log_dir, 2)
    """Run the production-image probe case once, drain briefly after
    the OK line, then terminate QEMU. A timeout is a failure."""
    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    prefix = Path(args.log_dir) / "m3-probe-run"
    stdout_path = prefix.with_suffix(".stdout.log")
    stderr_path = prefix.with_suffix(".stderr.log")
    metadata_path = prefix.with_suffix(".metadata.json")
    command = qemu_command(args, diagnostic_dtb)
    metadata = {
        "command": command,
        "firmware": str(Path(args.firmware).resolve()),
        "firmware_sha256": file_sha256(args.firmware),
        "image": str(Path(args.image).resolve()),
        "image_sha256": file_sha256(args.image),
        "diagnostic_dtb": diagnostic_dtb,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    stdout = bytearray()
    stderr = bytearray()
    timed_out = False
    returncode = None
    harness_terminated = False
    post_ok_drain = 0.5
    ok_observed_at = None
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0)
    except OSError as error:
        print(json.dumps({"event": "spawn-error", "error": str(error)}))
        return False
    if process.stdout is None or process.stderr is None:
        return False
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
            if ok_observed_at is None and probe_evidence(text):
                ok_observed_at = time.monotonic()
                # Short drain so trailing kernel noise is captured; the
                # kernel keeps halting forever after OK, so QEMU never
                # exits on its own.
                drain_deadline = ok_observed_at + post_ok_drain
                while time.monotonic() < drain_deadline:
                    if not selector.get_map():
                        break
                    for key, _ in selector.select(min(0.2, drain_deadline - time.monotonic())):
                        chunk = os.read(key.fd, 4096)
                        if chunk:
                            streams[key.fd][1].extend(chunk)
                        else:
                            selector.unregister(key.fileobj)
                break
            if process.poll() is not None and not selector.get_map():
                break
    finally:
        selector.close()
        harness_terminated = process.poll() is None
        if harness_terminated:
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
    accepted = probe_evidence(text)
    result = accepted and not timed_out
    metadata.update({
        "timeout": timed_out, "returncode": returncode,
        "harness_terminated": harness_terminated,
        "ok_observed": ok_observed_at is not None,
        "result": "PASS" if result else "FAIL",
    })
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({
        "event": "case", "result": "PASS" if result else "FAIL",
        "timeout": timed_out, "returncode": returncode,
        "ok_observed": ok_observed_at is not None,
        "stdout": str(stdout_path), "stderr": str(stderr_path),
    }))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--firmware")
    parser.add_argument("--image")
    parser.add_argument("--qemu")
    parser.add_argument("--log-dir")
    parser.add_argument("--diagnostic-dtb", default=None)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print("aarch64_m3_probe: self-test passed")
        return 0
    if not all((args.firmware, args.image, args.qemu, args.log_dir)):
        parser.error("--firmware, --image, --qemu, and --log-dir are required outside --self-test")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return 0 if run_case(args) else 1


if __name__ == "__main__":
    sys.exit(main())
