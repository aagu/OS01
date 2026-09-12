#!/usr/bin/env python3
"""Exercise kernel runtime identity switching and failure-atomic publication."""

from __future__ import annotations

import argparse
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROFILE = ROOT / "mk/profiles/x86_64-clang.mk"
DEFAULT_SOURCE_RECEIPT = ROOT / "build/x86_64-clang/runtime/kernel-link.receipt"
DEFAULT_SYSROOT = ROOT / "build/x86_64-clang/sysroot"
PROFILE = DEFAULT_PROFILE
PROFILE_NAME = "x86_64-clang"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, check=False, text=True, capture_output=True)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def source_link_inputs(source_receipt: Path, sysroot: Path) -> tuple[list[str], Path, Path]:
    require(source_receipt.is_file(), "build kernel.bin before kernel_runtime_link_test.py")
    stage_line = source_receipt.read_text(encoding="utf-8").splitlines()[0]
    argv = shlex.split(stage_line.removeprefix("stage1="))
    objects = [arg for arg in argv if arg.endswith(".o")]
    runtimes = [Path(arg) for arg in argv if arg.endswith((".a", ".lib")) and "runtime/kernel" in arg]
    require(bool(objects), "stage1 receipt has no object inputs")
    require(len(runtimes) == 1 and runtimes[0].is_file(), "stage1 receipt has no unique runtime archive")
    sysroot = sysroot.resolve()
    require((sysroot / "usr/lib/libk.a").is_file(), "built immutable sysroot is missing libk.a")
    return objects, runtimes[0], sysroot


def inner_make(
    build: Path,
    objects: list[str],
    sysroot: Path,
    runtime: Path,
    provider_receipt: Path,
    fault: str = "",
) -> subprocess.CompletedProcess[str]:
    return run(
        [
            "make", "--no-print-directory", "-C", "kernel",
            f"OS01_PROFILE_FILE={PROFILE}", f"PROFILE={PROFILE_NAME}", "ARCH=x86_64",
            f"KERNEL_BUILD_DIR={build}", f"SYSROOT_GENERATION_DIR={sysroot}",
            f"KERNEL_OBJECTS={' '.join(objects)}", f"KERNEL_RUNTIME_INPUTS={runtime}",
            f"KERNEL_RUNTIME_PREREQ={provider_receipt}",
            f"KERNEL_RUNTIME_LINK_RECEIPT={build / 'kernel-link.receipt'}",
            f"KERNEL_LINK_FAULT_INJECT={fault}", str(build / "kernel.elf"),
        ]
    )


def receipt_runtimes(receipt: Path) -> tuple[str, str]:
    records = receipt.read_text(encoding="utf-8").splitlines()
    require(len(records) == 2, "published receipt must contain both link records")
    runtimes: list[str] = []
    for label, record in zip(("stage1=", "final="), records):
        require(record.startswith(label), f"receipt record is missing {label}")
        argv = shlex.split(record.removeprefix(label))
        runtimes.append(
            next(arg for arg in argv if arg.endswith((".a", ".lib")) and arg != "-lk")
        )
    return tuple(runtimes)


def assert_failure_atomic(build: Path, result: subprocess.CompletedProcess[str], fault: str) -> None:
    require(result.returncode != 0, f"fault {fault!r} unexpectedly succeeded")
    for name in (
        "kernel.elf.stage1",
        "kernel.elf",
        "kernel-link.receipt.stage1",
        "kernel-link.receipt",
        ".runtime-link.identity",
    ):
        require(not (build / name).exists(), f"fault {fault!r} left published {name}")
    leftovers = list(build.rglob("*.tmp.*"))
    require(not leftovers, f"fault {fault!r} left temporary files: {leftovers}")


def fresh_fixture(base: Path, source_runtime: Path) -> tuple[Path, Path, Path, Path]:
    build = base / "kernel"
    providers = base / "providers"
    providers.mkdir(parents=True)
    runtime = base / "runtime" / "libos01-builtins.a"
    runtime.parent.mkdir()
    shutil.copy2(source_runtime, runtime)
    receipt_a = providers / "A" / "runtime.receipt"
    receipt_b = providers / "B" / "runtime.receipt"
    receipt_a.parent.mkdir()
    receipt_b.parent.mkdir()
    receipt_a.write_text("provider=A\n", encoding="utf-8")
    receipt_b.write_text("provider=B\n", encoding="utf-8")
    return build, runtime, receipt_a, receipt_b


def published_link_mtimes(build: Path) -> tuple[int, int]:
    return (
        (build / "kernel.elf.stage1").stat().st_mtime_ns,
        (build / "kernel.elf").stat().st_mtime_ns,
    )


def require_identity(build: Path, provider_receipt: Path, runtime: Path) -> None:
    identity = (build / ".runtime-link.identity").read_text(encoding="utf-8")
    require(
        f"prereq={provider_receipt}|input={runtime}|" in identity,
        "published identity does not match the selected provider",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-receipt", type=Path, default=DEFAULT_SOURCE_RECEIPT)
    parser.add_argument("--sysroot", type=Path, default=DEFAULT_SYSROOT)
    parser.add_argument("--profile-file", type=Path, default=DEFAULT_PROFILE)
    parser.add_argument("--profile", default="x86_64-clang")
    args = parser.parse_args()
    global PROFILE, PROFILE_NAME
    PROFILE = args.profile_file.resolve()
    PROFILE_NAME = args.profile
    objects, source_runtime, sysroot = source_link_inputs(args.source_receipt, args.sysroot)
    with tempfile.TemporaryDirectory(prefix="os01-kernel-runtime-link-") as raw:
        base = Path(raw)

        build, runtime, receipt_a, receipt_b = fresh_fixture(base / "switch", source_runtime)
        first = inner_make(build, objects, sysroot, runtime, receipt_a)
        require(first.returncode == 0, f"initial A link failed:\n{first.stdout}{first.stderr}")
        require(receipt_runtimes(build / "kernel-link.receipt") == (str(runtime), str(runtime)), "A did not link both stages with the selected runtime")
        require_identity(build, receipt_a, runtime)
        a_mtimes = published_link_mtimes(build)
        second = inner_make(build, objects, sysroot, runtime, receipt_b)
        require(second.returncode == 0, f"B link failed:\n{second.stdout}{second.stderr}")
        require(published_link_mtimes(build)[0] > a_mtimes[0], "A→B did not relink stage1")
        require(published_link_mtimes(build)[1] > a_mtimes[1], "A→B did not relink final")
        require(receipt_runtimes(build / "kernel-link.receipt") == (str(runtime), str(runtime)), "A→B did not link both stages")
        require_identity(build, receipt_b, runtime)
        b_mtimes = published_link_mtimes(build)
        third = inner_make(build, objects, sysroot, runtime, receipt_a)
        require(third.returncode == 0, f"return-to-A link failed:\n{third.stdout}{third.stderr}")
        require(published_link_mtimes(build)[0] > b_mtimes[0], "A→B→A did not relink stage1")
        require(published_link_mtimes(build)[1] > b_mtimes[1], "A→B→A did not relink final")
        require(receipt_runtimes(build / "kernel-link.receipt") == (str(runtime), str(runtime)), "A→B→A did not link both stages")
        require_identity(build, receipt_a, runtime)

        for fault in ("stage1-link", "stage1-publish", "final-link", "receipt-publish"):
            build, runtime, receipt_a, receipt_b = fresh_fixture(base / fault, source_runtime)
            success = inner_make(build, objects, sysroot, runtime, receipt_a)
            require(success.returncode == 0, f"setup link for {fault!r} failed:\n{success.stdout}{success.stderr}")
            if fault.startswith("stage1-"):
                result = inner_make(build, objects, sysroot, runtime, receipt_b, fault)
            else:
                (build / "kernel.elf").unlink()
                result = inner_make(build, objects, sysroot, runtime, receipt_a, fault)
            assert_failure_atomic(build, result, fault)

    print("kernel runtime link tests: identity-only A→B→A and 4 fault cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
