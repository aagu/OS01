#!/usr/bin/env python3
"""Run the AArch64 M1 RAM/CPU matrix against normal and self-test images."""

import argparse
import json
import os
import selectors
import subprocess
import sys
import time
from pathlib import Path

try:
    from aarch64_m1_evidence import m1_failure_evidence_ok
except ImportError:
    from qemutests.aarch64_m1_evidence import m1_failure_evidence_ok


MATRIX = ((256, 1), (512, 1), (512, 2), (512, 4),
          (2048, 1), (2048, 2), (2048, 4), (4096, 1))


def run_matrix(args: argparse.Namespace) -> bool:
    script = Path(__file__).with_name("aarch64_uefi_smp.py")
    outcomes: list[bool] = []
    for label, image, selftest in (
        ("normal", args.normal_image, False),
        ("selftest", args.selftest_image, True),
    ):
        for ram_mib, cpus in MATRIX:
            log_dir = Path(args.log_dir) / f"{label}-ram-{ram_mib}-cpus-{cpus}"
            log_dir.mkdir(parents=True, exist_ok=True)
            command = [
                sys.executable, str(script), "--firmware", args.firmware,
                "--image", image, "--qemu", args.qemu, "--log-dir", str(log_dir),
                "--cpus", str(cpus), "--repeat", "1", "--timeout", str(args.timeout),
                "--ram-mib", str(ram_mib), "--diagnostic-dtb", "auto",
                "--expect-m1",
            ]
            if selftest:
                command.extend(("--expect-selftest", "--expect-gic", "--expect-clk"))
            print(f"M1 matrix: {label}, RAM={ram_mib} MiB, CPUs={cpus}")
            outcomes.append(subprocess.run(command, check=False).returncode == 0)
    return all(outcomes)


def run_sparse_variant(args: argparse.Namespace) -> bool:
    script = Path(__file__).with_name("aarch64_uefi_smp.py")
    log_dir = Path(args.log_dir) / "sparse-ram-512-cpus-1"
    log_dir.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable, str(script), "--firmware", args.firmware,
        "--image", args.image, "--qemu", args.qemu, "--log-dir", str(log_dir),
        "--cpus", "1", "--repeat", "1", "--timeout", str(args.timeout),
        "--ram-mib", "512", "--diagnostic-dtb", "auto",
        "--expect-m1", "--m1-variant", "sparse", "--expect-selftest",
        "--expect-gic", "--expect-clk",
    ]
    return subprocess.run(command, check=False).returncode == 0


def run_expected_failure(args: argparse.Namespace) -> bool:
    """Run one intentional failure and verify its signature while QEMU lives."""
    try:
        from aarch64_uefi_smp import generate_diagnostic_dtb, qemu_command
    except ImportError:
        from qemutests.aarch64_uefi_smp import generate_diagnostic_dtb, qemu_command

    cpus = 2 if args.variant == "ap-bad-root" else 1
    ram_mib = 512
    case_dir = Path(args.log_dir) / f"{args.variant}-ram-{ram_mib}-cpus-{cpus}"
    case_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = case_dir / "qemu.log"
    metadata_path = case_dir / "metadata.json"
    diagnostic = generate_diagnostic_dtb(args.qemu, str(case_dir), cpus, ram_mib)
    command_args = argparse.Namespace(
        qemu=args.qemu, firmware=args.firmware, image=args.image, ram_mib=ram_mib)
    command = qemu_command(command_args, cpus, diagnostic)
    metadata = {"command": command, "variant": args.variant, "cpus": cpus,
                "ram_mib": ram_mib, "image": str(Path(args.image).resolve()),
                "firmware": str(Path(args.firmware).resolve()),
                "diagnostic_dtb": diagnostic, "timeout_seconds": args.timeout}
    output = bytearray()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               bufsize=0)
    text = ""
    if process.stdout is None:
        process.terminate()
        return False
    selector = selectors.DefaultSelector()
    fd = process.stdout.fileno()
    os.set_blocking(fd, False)
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + args.timeout
    evidence_at: float | None = None
    stayed_alive = False
    exited_early = False
    try:
        while time.monotonic() < deadline:
            for key, _ in selector.select(0.1):
                chunk = os.read(key.fd, 4096)
                if chunk:
                    output.extend(chunk)
                else:
                    selector.unregister(key.fileobj)
            text = output.decode("utf-8", errors="replace")
            if process.poll() is not None:
                exited_early = True
                break
            if evidence_at is None and m1_failure_evidence_ok(
                    text, args.variant, cpus, stayed_alive=True):
                evidence_at = time.monotonic()
            if evidence_at is not None and time.monotonic() - evidence_at >= 2.0:
                stayed_alive = process.poll() is None
                break
        text = output.decode("utf-8", errors="replace")
        if process.poll() is not None:
            exited_early = True
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        stdout_path.write_text(text if "text" in locals() else output.decode("utf-8", errors="replace"))
    accepted = (not exited_early and stayed_alive
                and m1_failure_evidence_ok(text, args.variant, cpus, stayed_alive=True))
    metadata.update({"stayed_alive_after_evidence": stayed_alive,
                     "exited_before_acceptance": exited_early,
                     "returncode_after_harness_stop": process.returncode,
                     "result": "PASS" if accepted else "FAIL"})
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(json.dumps({"event": "m1-negative-case", "variant": args.variant,
                      "cpus": cpus, "result": "PASS" if accepted else "FAIL",
                      "log": str(stdout_path)}))
    return accepted


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--normal-image")
    parser.add_argument("--selftest-image")
    parser.add_argument("--variant", choices=("sparse", "arena-exhaust",
                                               "table-exhaust", "ap-bad-root"))
    parser.add_argument("--image", help="image for --variant failure/sparse run")
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.variant:
        if not args.image:
            parser.error("--image is required with --variant")
    elif not args.normal_image or not args.selftest_image:
        parser.error("--normal-image and --selftest-image are required for the default matrix")
    elif args.image:
        parser.error("--image is only valid with --variant")
    Path(args.log_dir).mkdir(parents=True, exist_ok=True)
    if args.variant == "sparse":
        accepted = run_sparse_variant(args)
    elif args.variant:
        accepted = run_expected_failure(args)
    else:
        accepted = run_matrix(args)
    return 0 if accepted else 1


if __name__ == "__main__":
    sys.exit(main())
