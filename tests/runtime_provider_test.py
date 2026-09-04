#!/usr/bin/env python3
"""Behavior tests for compiler-runtime provider parsing and validation."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_TOOLS = ("make", "clang", "llvm-ar", "llvm-nm", "llvm-readobj")


def tool(name: str) -> str:
    selected = os.environ.get(name.upper().replace("-", "_"), name)
    resolved = shutil.which(selected)
    if resolved is None:
        print(f"SKIP: required selected LLVM tool is unavailable: {selected}", file=sys.stderr)
        raise SystemExit(0)
    return resolved


TOOLS = {name: tool(name) for name in REQUIRED_TOOLS}


class Fixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.profile = root / "fixture-profile.mk"
        self.harness = root / "harness.mk"
        self.query = root / "query.py"
        self.build = root / "build"

        self.profile.write_text(
            textwrap.dedent(
                f"""\
                PROFILE_CAPABILITIES := kernel
                OS01_ROOT := {ROOT}
                BUILD_DIR := {self.build}
                TARGET_TRIPLE := x86_64-unknown-none
                CLANG := {self.query}
                CLANG_ID := fixture clang 1.0
                CLANG_RESOURCE_DIR := /fixture/clang/resource
                LLVM_AR := {TOOLS['llvm-ar']}
                LLVM_NM := {TOOLS['llvm-nm']}
                LLVM_READOBJ := {TOOLS['llvm-readobj']}
                TARGET_AR := $(LLVM_AR)
                RUNTIME_TARGET_kernel := $(TARGET_TRIPLE)
                RUNTIME_CFLAGS_kernel := -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone -mcmodel=kernel
                RUNTIME_OBJECT_FORMAT_kernel := ELF
                RUNTIME_MACHINE_kernel := EM_X86_64
                RUNTIME_ABI_kernel := x86_64-sysv
                """
            ),
            encoding="utf-8",
        )
        self.harness.write_text(
            textwrap.dedent(
                f"""\
                PROFILE := fixture
                OS01_PROFILE_FILE := {self.profile}
                include {ROOT / 'mk/components/runtime.mk'}

                .PHONY: probe
                probe: $(KERNEL_RUNTIME_PREREQ)
                \t@printf 'input=%s\\n' "$(KERNEL_RUNTIME_INPUTS)"
                """
            ),
            encoding="utf-8",
        )

    def set_query(self, *, output: str = "", status: int = 0) -> None:
        self.query.write_text(
            textwrap.dedent(
                f"""\
                #!{sys.executable}
                import sys
                if "-print-libgcc-file-name" not in sys.argv:
                    raise SystemExit(97)
                print({output!r})
                raise SystemExit({status})
                """
            ),
            encoding="utf-8",
        )
        self.query.chmod(0o755)

    def make(self, provider: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                TOOLS["make"],
                "--no-print-directory",
                "-f",
                str(self.harness),
                f"RUNTIME_PROVIDER={provider}",
                "probe",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def compile_object(self, target: str, output: Path) -> None:
        result = subprocess.run(
            [
                TOOLS["clang"],
                f"--target={target}",
                "-ffreestanding",
                "-x",
                "c",
                "-c",
                "-",
                "-o",
                str(output),
            ],
            input="int runtime_fixture_symbol(void) { return 1; }\n",
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"failed to create {target} fixture:\n{result.stdout}{result.stderr}"
            )

    def archive(self, output: Path, *members: Path) -> None:
        result = subprocess.run(
            [TOOLS["llvm-ar"], "rc", str(output), *(str(member) for member in members)],
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(f"failed to create archive:\n{result.stdout}{result.stderr}")


def require_failure(
    name: str,
    fixture: Fixture,
    provider: str,
    expected_diagnostic: str,
) -> None:
    result = fixture.make(provider)
    combined = result.stdout + result.stderr
    if result.returncode == 0:
        raise AssertionError(f"{name}: make unexpectedly succeeded:\n{combined}")
    if expected_diagnostic not in combined:
        raise AssertionError(
            f"{name}: expected exact diagnostic fragment:\n{expected_diagnostic}\n"
            f"actual output:\n{combined}"
        )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="os01-runtime-provider-") as tmp:
        fixture = Fixture(Path(tmp))

        require_failure(
            "unknown provider",
            fixture,
            "unknown",
            "ERROR: runtime provider 'unknown' is unsupported; expected selfhosted or compiler-rt",
        )

        require_failure(
            "empty provider",
            fixture,
            "",
            "ERROR: runtime provider '' is unsupported; expected selfhosted or compiler-rt",
        )

        fixture.set_query()
        require_failure(
            "empty query output",
            fixture,
            "compiler-rt",
            "ERROR: compiler-rt query returned no archive for consumer 'kernel' target 'x86_64-unknown-none'",
        )

        fixture.set_query(output="ignored", status=23)
        require_failure(
            "query failure",
            fixture,
            "compiler-rt",
            "ERROR: compiler-rt query failed for consumer 'kernel' target 'x86_64-unknown-none' (exit 23)",
        )

        directory = fixture.root / "libclang_rt.builtins.a"
        directory.mkdir()
        fixture.set_query(output=str(directory))
        require_failure(
            "directory candidate",
            fixture,
            "compiler-rt",
            f"ERROR: compiler-rt candidate is not a regular file: {directory}",
        )

        nonarchive = fixture.root / "not-archive" / "libclang_rt.builtins.a"
        nonarchive.parent.mkdir()
        nonarchive.write_text("not an archive\n", encoding="utf-8")
        fixture.set_query(output=str(nonarchive))
        require_failure(
            "non-archive candidate",
            fixture,
            "compiler-rt",
            f"ERROR: compiler-rt candidate is not a readable archive: {nonarchive}",
        )

        x86_object = fixture.root / "x86_64.o"
        fixture.compile_object("x86_64-unknown-none", x86_object)
        forbidden = fixture.root / "libgcc-private" / "libclang_rt.builtins.a"
        forbidden.parent.mkdir()
        fixture.archive(forbidden, x86_object)
        fixture.set_query(output=str(forbidden))
        require_failure(
            "libgcc path",
            fixture,
            "compiler-rt",
            f"ERROR: compiler-rt candidate path must not contain 'libgcc': {forbidden}",
        )

        wrong_object = fixture.root / "coff-x86_64.obj"
        fixture.compile_object("x86_64-pc-windows-msvc", wrong_object)
        wrong_archive = fixture.root / "wrong" / "libclang_rt.builtins.a"
        wrong_archive.parent.mkdir()
        fixture.archive(wrong_archive, wrong_object)
        fixture.set_query(output=str(wrong_archive))
        require_failure(
            "wrong-format archive member",
            fixture,
            "compiler-rt",
            "ERROR: compiler-rt member 'coff-x86_64.obj' has wrong object format/machine for consumer 'kernel'; expected ELF/EM_X86_64",
        )

        aarch64_object = fixture.root / "aarch64.o"
        fixture.compile_object("aarch64-unknown-none", aarch64_object)
        mixed_archive = fixture.root / "mixed" / "libclang_rt.builtins.a"
        mixed_archive.parent.mkdir()
        fixture.archive(mixed_archive, x86_object, aarch64_object)
        fixture.set_query(output=str(mixed_archive))
        require_failure(
            "mixed archive members",
            fixture,
            "compiler-rt",
            "ERROR: compiler-rt member 'aarch64.o' has wrong object format/machine for consumer 'kernel'; expected ELF/EM_X86_64",
        )

    print("runtime provider tests: 9 passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
