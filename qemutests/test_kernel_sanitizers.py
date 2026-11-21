#!/usr/bin/env python3
"""Sanitizer build contract + live probe evidence (roadmap P1 UBSan+KASan).

The contract this module pins:

  * the knobs exist, are whitelisted across the controlled sub-make
    boundary, and re-key kernel/image artifacts under san-ubsan / san-kasan
    slugs so an instrumented build can never share objects with the normal
    build;
  * the aarch64 profile rejects both knobs (x86-only first scope);
  * the sanitizer runtime TUs compile de-instrumented through object-
    specific rules, and the de-instrument flag spells each sanitizer kind
    explicitly (-fno-sanitize=address does NOT cover kernel-address);
  * the KASan low-VA exclusion list (uaccess, pmm_arch) and the head.S
    static zero-shadow tables + sanitizer boot entry are gated by the
    OS01_KASAN / OS01_UBSAN defines, so the normal build stays
    byte-identical (verified separately by the flags-stamp contract);
  * the probe fixture is only linked when KERNEL_SANITIZER_TEST selects it.

The live QEMU probe (both kernels boot; each violation report appears on
serial with its armed marker before it) runs only when
OS01_SANITIZER_LIVE=1 — it builds two full instrumented images and runs
two QEMU boots, so plain host-side CI does not pay for it by default;
`make test-kernel-sanitizers` sets the variable.
"""
from __future__ import annotations

import os
import signal
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT_MK = ROOT / "mk" / "project.mk"
KERNEL_MK = ROOT / "kernel" / "Makefile"
HEAD_S = ROOT / "kernel" / "arch" / "x86_64" / "head.S"
AARCH64_PROFILE = ROOT / "mk" / "profiles" / "aarch64-clang.mk"
X86_PROFILE = ROOT / "mk" / "profiles" / "x86_64-clang.mk"
FIXTURE = ROOT / "kernel" / "selftest" / "test_kernel_sanitizers.c"

PROFILE = os.environ.get("PROFILE", "x86_64-clang")
BUILD_DIR = ROOT / "build" / PROFILE


def read(path: Path) -> str:
    return path.read_text()


class SanitizerBuildContract(unittest.TestCase):
    """Static contract over the build files (no compilation)."""

    def test_knobs_validated_and_whitelisted(self):
        text = read(PROJECT_MK)
        for token in (
            "KERNEL_UBSAN ?= 0",
            "KERNEL_KASAN ?= 0",
            "KERNEL_SANITIZER_TEST ?= none",
            "must be 0 or 1",
            "is not one of: none ubsan kasan",
        ):
            self.assertIn(token, text, token)
        allowed_line = next(
            line for line in text.splitlines()
            if line.startswith("OS01_SUBMAKE_ALLOWED :=")
        )
        for knob in ("KERNEL_UBSAN", "KERNEL_KASAN", "KERNEL_SANITIZER_TEST"):
            self.assertIn(knob, allowed_line, knob)

    def test_variant_slugs_rekey_artifacts(self):
        project = read(PROJECT_MK)
        self.assertIn("$(if $(filter 1,$(KERNEL_UBSAN)),san-ubsan)", project)
        self.assertIn("$(if $(filter 1,$(KERNEL_KASAN)),san-kasan)", project)
        profile = read(X86_PROFILE)
        self.assertIn("KERNEL_VARIANT := san-ubsan", profile)
        self.assertIn("KERNEL_VARIANT := san-kasan", profile)

    def test_aarch64_profile_rejects_sanitizers(self):
        text = read(AARCH64_PROFILE)
        self.assertIn("KERNEL_UBSAN/KERNEL_KASAN are x86-only", text)
        self.assertIn("$(error KERNEL_UBSAN/KERNEL_KASAN are x86-only", text)

    def test_runtime_tus_deinstrumented_per_kind(self):
        text = read(KERNEL_MK)
        # Each knob spells its own sanitizer kind in the de-instrument flag.
        self.assertIn("SAN_RUNTIME_FNO_SANITIZE += -fno-sanitize=undefined",
                      text)
        self.assertIn("SAN_RUNTIME_FNO_SANITIZE += -fno-sanitize=kernel-address",
                      text)
        # The KASan low-VA exclusion list is explicit and gated.
        for obj in ("arch/x86_64/memory/uaccess.o", "arch/x86_64/memory/pmm_arch.o"):
            self.assertIn(f"$(BUILD_DIR)/{obj}:", text, obj)
        self.assertIn("ifeq ($(KERNEL_KASAN),1)\n$(BUILD_DIR)/arch/x86_64/memory/uaccess.o:",
                      text)

    def test_kasan_flag_set(self):
        text = read(KERNEL_MK)
        self.assertIn("-fsanitize=kernel-address -mllvm -asan-globals=0 "
                      "-mllvm -asan-stack=0 -DOS01_KASAN=1", text)
        self.assertIn("-fsanitize=undefined -fno-sanitize=object-size "
                      "-fno-sanitize=alignment -DOS01_UBSAN=1", text)

    def test_head_shadow_tables_and_boot_entry_gated(self):
        head = read(HEAD_S)
        self.assertIn("#if defined(OS01_KASAN)", head)
        self.assertIn("__KASAN_PDPT:", head)
        self.assertIn(".fill\t512,8,0x200081", head)
        self.assertIn("#if defined(OS01_KASAN) || defined(OS01_UBSAN)", head)
        self.assertIn(".quad\tkasan_boot_entry", head)
        boot = read(ROOT / "kernel" / "kasan" / "kasan_boot.c")
        self.assertIn("#if defined(OS01_KASAN) || defined(OS01_UBSAN)", boot)
        self.assertIn("arch_set_percpu_base(&percpu_data[0]);", boot)
        self.assertIn("percpu_data[0].self = (uint64_t)&percpu_data[0];", boot)

    def test_shadow_geometry_single_source(self):
        header = read(ROOT / "kernel" / "include" / "kasan" / "kasan.h")
        self.assertIn("#define KASAN_SHADOW_OFFSET 0xdffffc0000000000UL", header)
        self.assertIn("#define KASAN_SHADOW_PML4_INDEX ((KASAN_SHADOW_BASE >> 39) & 511)",
                      header)
        # head.S and the C runtime must agree on the zero-frame geometry.
        self.assertIn("KASAN_ZERO_FRAME_PA 0x200000UL", header)
        self.assertIn(".org\t0x100000", read(HEAD_S))

    def test_poison_hooks_and_arm_gate(self):
        slab = read(ROOT / "kernel" / "memory" / "slab.c")
        self.assertIn("#include <kasan/kasan.h>", slab)
        self.assertIn("kasan_kmalloc(result, size, kmalloc_cache_size[i].size);",
                      slab)
        self.assertIn("kasan_kfree(address, kmalloc_cache_size[i].size);",
                      slab)
        self.assertIn("kasan_kunpoison_page(slab->address);", slab)
        runtime = read(ROOT / "kernel" / "kasan" / "kasan.c")
        self.assertIn("static int kasan_poison_ready;", runtime)
        self.assertIn("if (!kasan_poison_ready)", runtime)
        shadow = read(ROOT / "kernel" / "arch" / "x86_64" / "memory" /
                      "kasan_shadow.c")
        self.assertIn("kasan_poison_arm();", shadow)

    def test_fixture_gated_by_kernel_sanitizer_test(self):
        kernel_mk = read(KERNEL_MK)
        self.assertIn("KERNEL_SANITIZER_FIXTURE_C_SOURCES :=", kernel_mk)
        self.assertIn("selftest/test_kernel_sanitizers.c", kernel_mk)
        # ... and filtered out of the selftest wildcard for the normal build.
        self.assertIn("selftest/test_kernel_sanitizers.c \\", kernel_mk)
        main_c = read(ROOT / "kernel" / "core" / "main.c")
        self.assertIn("#if defined(KERNEL_SANITIZER_TEST_UBSAN) || "
                      "defined(KERNEL_SANITIZER_TEST_KASAN)", main_c)
        self.assertIn("sanitizer_probe_run();", main_c)


class SanitizerLiveProbe(unittest.TestCase):
    """Live QEMU evidence — opt-in via OS01_SANITIZER_LIVE=1."""

    @unittest.skipUnless(os.environ.get("OS01_SANITIZER_LIVE") == "1",
                         "set OS01_SANITIZER_LIVE=1 (or make "
                         "test-kernel-sanitizers) to build + boot")
    def test_probe_reports_and_panics(self):
        cases = (
            ("ubsan", "KERNEL_UBSAN=1",
             ["[san-probe] armed:ubsan",
              "ubsan: signed integer add overflow at "
              "selftest/test_kernel_sanitizers.c",
              "[kernel panic] ubsan: undefined behavior detected"]),
            ("kasan", "KERNEL_KASAN=1",
             ["[san-probe] armed:kasan",
              "WRITE of 1 byte(s) at ",
              "heap redzone (past requested size)",
              "[kernel panic] kasan: invalid memory access detected"]),
        )
        for name, knob, needles in cases:
            with self.subTest(probe=name):
                image = (BUILD_DIR / "image" / f"san-{name}" / "disk.img")
                if not image.exists():
                    self._build_probe(name, knob)
                log = self._run_probe(name, knob)
                for needle in needles:
                    self.assertIn(needle, log, needle)

    def _build_probe(self, name: str, knob: str) -> None:
        subprocess.run(
            ["make", f"KERNEL_SANITIZER_TEST={name}",
             f"{knob}",
             str(BUILD_DIR / "image" / f"san-{name}" / "disk.img")],
            cwd=ROOT, check=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    def _run_probe(self, name: str, knob: str) -> str:
        # make run serves -serial stdio: the probe kpanic HALTS the kernel,
        # so QEMU never exits on its own.  Hold the pipe open for the boot
        # window (delayed-pipe shape the interactive smokes use), then
        # kill the process group and keep whatever reached the serial log.
        image = BUILD_DIR / "image" / f"san-{name}" / "disk.img"
        proc = subprocess.Popen(
            ["sh", "-c",
             "(sleep 55) | make run " + knob + " "
             f"KERNEL_SANITIZER_TEST={name} QEMU_IMAGE={image} DISPLAY=none"],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True)
        try:
            out, _ = proc.communicate(timeout=150)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, 15)
            try:
                out, _ = proc.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                out = ""
        return out


if __name__ == "__main__":
    unittest.main()
