#!/usr/bin/env python3
"""User runtime consumer gate (monitor plan Task 5).

Verifies the consumer=user builtins archive contract from
docs/superpowers/specs/2026-10-09-monitor-design.md §2.3:

  - the user runtime receipt exists under
    build/<profile>/runtime/user/<provider>-<digest>/ and records
    consumer=user (receipt.consumer_is_user);
  - the kernel and user receipts live in DIFFERENT variant namespaces
    (runtime/kernel/... vs runtime/user/...) and the kernel receipt never
    claims consumer=user;
  - the probe ELF, linked by the real user toolchain against the user
    archive, has zero undefined symbols (undefined_symbol_count == 0) and
    defines __udivti3 from the archive — no host-symbol backfill: the ELF
    is a static freestanding x86_64 image with no interpreter/dynamic
    program headers;
  - the user receipt was built with the USER ABI/flags, not the kernel
    variant's -mcmodel=kernel flags;
  - an unsupported user runtime provider is rejected loudly at parse time.

hosttests passing is NOT a substitute: everything here runs against the
real target artifacts produced by `make PROFILE=x86_64-clang
test-monitor-runtime`.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise AssertionError(message)


def parse_receipt(path: Path) -> dict[str, str]:
    records: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or "=" not in line:
            continue
        key, _, value = line.partition("=")
        # The variant= line is a tuple, keep it verbatim under its own key.
        if key in records and key != "variant":
            fail(f"{path}: duplicate receipt key '{key}'")
        records[key] = value
    return records


def check_user_receipt(user_receipt: Path, profile: str) -> None:
    if not user_receipt.is_file():
        fail(f"user runtime receipt is missing: {user_receipt}")
    receipt = parse_receipt(user_receipt)

    # Key brief assertion: receipt.consumer_is_user
    if receipt.get("consumer") != "user":
        fail(f"receipt.consumer_is_user failed: consumer={receipt.get('consumer')!r} in {user_receipt}")

    expected = {
        "profile": profile,
        "provider": "selfhosted",
        "format": "ELF",
        "machine": "EM_X86_64",
        "abi": "x86_64-sysv",
    }
    for key, want in expected.items():
        got = receipt.get(key)
        if got != want:
            fail(f"user receipt {key}={got!r}; expected {want!r} ({user_receipt})")

    if "consumer=user" not in receipt.get("variant", ""):
        fail(f"user receipt variant tuple lacks consumer=user:\n{receipt.get('variant')}")

    cflags = receipt.get("cflags", "")
    if "-mcmodel=kernel" in cflags:
        fail(f"user runtime archive was built with kernel flags (-mcmodel=kernel): {cflags}")
    if "-mcmodel=small" not in cflags:
        fail(f"user runtime archive lacks the user code model (-mcmodel=small): {cflags}")

    if not receipt.get("source_sha256") or not receipt.get("archive_sha256"):
        fail(f"user receipt is missing source/archive digests:\n{receipt}")

    # Directory contract: build/<profile>/runtime/user/<provider>-<digest>/
    parts = user_receipt.parent.parts
    if parts[-3:-1] != ("runtime", "user"):
        fail(f"user receipt is not under runtime/user/: {user_receipt}")
    variant_dir = parts[-1]
    if not variant_dir.startswith("selfhosted-") or len(variant_dir) <= len("selfhosted-"):
        fail(f"user variant dir must be <provider>-<digest>: {variant_dir}")


def check_kernel_receipt_separate(kernel_receipt: Path, user_receipt: Path) -> None:
    if not kernel_receipt.is_file():
        fail(f"kernel runtime receipt is missing: {kernel_receipt}")
    kernel = parse_receipt(kernel_receipt)
    if kernel.get("consumer") != "kernel":
        fail(f"kernel receipt consumer={kernel.get('consumer')!r}; expected 'kernel'")
    if kernel_receipt.resolve() == user_receipt.resolve():
        fail("kernel and user runtime receipts share one path; the consumers must not collide")
    if kernel_receipt.parent == user_receipt.parent:
        fail(f"kernel and user archives share one variant dir: {kernel_receipt.parent}")
    kparts = kernel_receipt.parent.parts
    if kparts[-3:-1] != ("runtime", "kernel"):
        fail(f"kernel receipt is not under runtime/kernel/: {kernel_receipt}")


def check_probe_elf(probe: Path, llvm_nm: str) -> int:
    if not probe.is_file():
        fail(f"probe ELF is missing: {probe}")
    result = subprocess.run(
        [llvm_nm, "--undefined-only", str(probe)],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"llvm-nm failed on {probe}:\n{result.stdout}{result.stderr}")
    undefined = [line for line in result.stdout.splitlines() if line.strip()]
    # Key brief assertion: undefined_symbol_count == 0
    if undefined:
        fail(
            f"probe ELF has {len(undefined)} unresolved runtime symbol(s) "
            f"(host backfill or missing user archive):\n" + "\n".join(undefined)
        )
    defined = subprocess.run(
        [llvm_nm, "--defined-only", str(probe)],
        text=True,
        capture_output=True,
        check=False,
    )
    if defined.returncode != 0:
        fail(f"llvm-nm --defined-only failed on {probe}:\n{defined.stderr}")
    if not any(line.split()[-1] == "__udivti3" for line in defined.stdout.splitlines() if line.strip()):
        fail(f"probe ELF does not define __udivti3 from the user archive:\n{defined.stdout}")
    return len(undefined)


def check_no_host_backfill(probe: Path, llvm_readobj: str | None) -> None:
    readobj = llvm_readobj or "llvm-readobj"
    result = subprocess.run(
        [readobj, "--file-headers", "--program-headers", str(probe)],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        fail(f"llvm-readobj failed on {probe}:\n{result.stdout}{result.stderr}")
    output = result.stdout
    if "Machine: EM_X86_64" not in output:
        fail(f"probe ELF is not an x86_64 target image:\n{output}")
    if "Type: Executable" not in output:
        fail(f"probe ELF is not a static executable:\n{output}")
    for forbidden in ("INTERP", "DYNAMIC"):
        if forbidden in output:
            fail(f"probe ELF carries a {forbidden} program header (host-style link):\n{output}")


def check_unsupported_provider_rejected(repo_root: Path, profile: str, make: str) -> None:
    result = subprocess.run(
        [
            make,
            "--no-print-directory",
            "-n",
            f"PROFILE={profile}",
            "USER_RUNTIME_PROVIDER=compiler-rt",
            "test-monitor-runtime",
        ],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    combined = result.stdout + result.stderr
    if result.returncode == 0:
        fail(
            "USER_RUNTIME_PROVIDER=compiler-rt was accepted; the v1 user consumer "
            f"must reject unsupported providers explicitly:\n{combined}"
        )
    expected = "ERROR: user runtime provider 'compiler-rt' is unsupported"
    if expected not in combined:
        fail(f"unsupported provider diagnostic missing ({expected!r}):\n{combined}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, help="user_udivti3_link_probe.elf artifact")
    parser.add_argument("--user-receipt", required=True, help="user runtime receipt path")
    parser.add_argument("--kernel-receipt", required=True, help="kernel runtime receipt path")
    parser.add_argument("--llvm-nm", default="llvm-nm")
    parser.add_argument("--llvm-readobj", default="llvm-readobj")
    parser.add_argument("--make", default="make")
    parser.add_argument("--profile", default="x86_64-clang")
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    args = parser.parse_args()

    probe = Path(args.probe)
    user_receipt = Path(args.user_receipt)
    kernel_receipt = Path(args.kernel_receipt)

    check_user_receipt(user_receipt, args.profile)
    check_kernel_receipt_separate(kernel_receipt, user_receipt)
    undefined_symbol_count = check_probe_elf(probe, args.llvm_nm)
    check_no_host_backfill(probe, args.llvm_readobj)
    check_unsupported_provider_rejected(Path(args.repo_root), args.profile, args.make)

    print(
        "monitor runtime tests: passed "
        f"(consumer=user receipt OK, kernel/user paths differ, "
        f"undefined_symbol_count={undefined_symbol_count}, "
        f"unsupported provider rejected)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
