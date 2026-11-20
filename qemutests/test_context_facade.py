#!/usr/bin/env python3
"""Cross-architecture compile and caller tests for <arch/context.h> facade."""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import unittest

TEST_MANIFEST = {
    "id": "test_context_facade",
    "layer": "harness",
    "subsystem": "test-framework",
    "timing": "strict",
    "tier": "pr",
    "scenarios": [],
}

ROOT = Path(__file__).resolve().parents[1]
INCLUDE_DIR = ROOT / "kernel" / "include"


def _strip_c_comments(text: str) -> str:
    pattern = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
    return pattern.sub("", text)


def _extract_c_function(source: str, func_name: str) -> str:
    clean = _strip_c_comments(source)
    match = re.search(rf"\b{re.escape(func_name)}\s*\([^;{{}}]*\)\s*\{{", clean)
    if not match:
        raise AssertionError(f"Could not find definition of {func_name}")
    start = match.start()
    brace_start = clean.index("{", match.start())
    depth = 0
    for idx in range(brace_start, len(clean)):
        ch = clean[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return clean[start : idx + 1]
    raise AssertionError(f"Unbalanced braces in {func_name}")


class ContextFacadeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clang = shutil.which("clang")
        if not self.clang:
            self.skipTest("clang is not available in PATH")

    def _run_clang(
        self,
        target: str,
        source: str,
        extra_flags: list[str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        cmd = [
            self.clang,
            "-target",
            target,
            f"-I{INCLUDE_DIR}",
            "-ffreestanding",
            "-Werror",
            "-fsyntax-only",
        ]
        if extra_flags:
            cmd.extend(extra_flags)
        cmd.extend(["-x", "c", "-"])
        return subprocess.run(
            cmd,
            input=source,
            text=True,
            capture_output=True,
        )

    def test_prepare_hook_header(self) -> None:
        src = """
        #include <arch/context.h>

        static void (*const prepare_hook_probe)(task_t *, struct pt_regs *) =
            arch_task_prepare_user_entry;

        int context_facade_probe(void) {
            return prepare_hook_probe != 0;
        }
        """
        for target in ("x86_64-unknown-none", "aarch64-none-elf"):
            with self.subTest(target=target):
                res = self._run_clang(target, src)
                self.assertEqual(
                    res.returncode,
                    0,
                    f"{target} <arch/context.h> probe failed:\n"
                    f"stdout: {res.stdout}\nstderr: {res.stderr}",
                )

    def test_production_prepare_callers(self) -> None:
        task_h = (ROOT / "kernel" / "include" / "sched" / "task.h").read_text(
            encoding="utf-8"
        )
        self.assertNotIn(
            "ret_from_intr",
            _strip_c_comments(task_h),
            "kernel/include/sched/task.h must no longer declare ret_from_intr",
        )

        exec_src = (ROOT / "kernel" / "arch" / "x86_64" / "sched" / "exec.c").read_text(
            encoding="utf-8"
        )
        spawn_fn = _extract_c_function(exec_src, "spawn_user_task")
        spawn_call_idx = spawn_fn.find("arch_task_prepare_user_entry(")
        self.assertNotEqual(
            spawn_call_idx,
            -1,
            "spawn_user_task must call arch_task_prepare_user_entry",
        )
        spawn_rdx_idx = spawn_fn.find("regs->rdx")
        self.assertNotEqual(spawn_rdx_idx, -1)
        self.assertLess(
            spawn_rdx_idx,
            spawn_call_idx,
            "spawn_user_task must populate pt_regs before calling arch_task_prepare_user_entry",
        )
        self.assertNotIn("ret_from_intr", spawn_fn)

        fork_src = (ROOT / "kernel" / "arch" / "x86_64" / "sched" / "fork.c").read_text(
            encoding="utf-8"
        )
        fork_fn = _extract_c_function(fork_src, "do_fork")
        fork_call_idx = fork_fn.find("arch_task_prepare_user_entry(")
        self.assertNotEqual(
            fork_call_idx,
            -1,
            "do_fork must call arch_task_prepare_user_entry",
        )
        rax_zero_idx = fork_fn.find("child_regs->rax = 0")
        self.assertNotEqual(rax_zero_idx, -1)
        self.assertLess(
            rax_zero_idx,
            fork_call_idx,
            "do_fork must zero child_regs->rax before calling arch_task_prepare_user_entry",
        )
        self.assertIn("PF_KTHREAD", fork_fn)
        self.assertIn("thd->rip = regs->rip", fork_fn)
        self.assertIn("thd->rsp =", fork_fn)
        self.assertNotIn("ret_from_intr", fork_fn)

        user_task_src = (
            ROOT / "kernel" / "arch" / "aarch64" / "cpu" / "user_task.c"
        ).read_text(encoding="utf-8")
        prepare_fn = _extract_c_function(user_task_src, "user_task_prepare")
        prepare_call_idx = prepare_fn.find("arch_task_prepare_user_entry(")
        self.assertNotEqual(
            prepare_call_idx,
            -1,
            "user_task_prepare must call arch_task_prepare_user_entry",
        )
        x0_zero_idx = prepare_fn.find("->x0 = 0")
        init_regs_idx = prepare_fn.find("aarch64_user_image_init_regs(")
        self.assertNotEqual(x0_zero_idx, -1)
        self.assertNotEqual(init_regs_idx, -1)
        self.assertLess(
            x0_zero_idx,
            prepare_call_idx,
            "user_task_prepare must zero child frame x0 before calling arch_task_prepare_user_entry",
        )
        self.assertLess(
            init_regs_idx,
            prepare_call_idx,
            "user_task_prepare must initialize fresh-image regs before calling arch_task_prepare_user_entry",
        )

    def test_restore_symbol_assembler_only(self) -> None:
        asm_src = """
        #include <arch/context.h>
        .globl ARCH_CONTEXT_RESTORE_FRAME
        ARCH_CONTEXT_RESTORE_FRAME:
        """
        c_src = """
        #include <arch/context.h>
        #ifdef ARCH_CONTEXT_RESTORE_FRAME
        #error "ARCH_CONTEXT_RESTORE_FRAME must not be defined for C"
        #endif
        int context_facade_asm_macro_absent(void) { return 1; }
        """
        for target in ("x86_64-unknown-none", "aarch64-none-elf"):
            with self.subTest(target=target, mode="asm"):
                cmd = [
                    self.clang,
                    "-target",
                    target,
                    f"-I{INCLUDE_DIR}",
                    "-E",
                    "-P",
                    "-x",
                    "assembler-with-cpp",
                    "-",
                ]
                res = subprocess.run(cmd, input=asm_src, text=True, capture_output=True)
                self.assertEqual(
                    res.returncode,
                    0,
                    f"{target} asm preprocess failed:\n{res.stderr}",
                )
                self.assertIn(".globl arch_context_restore_frame", res.stdout)
                self.assertIn("arch_context_restore_frame:", res.stdout)
                self.assertNotIn("void arch_context_restore_frame", res.stdout)
            with self.subTest(target=target, mode="c"):
                res = self._run_clang(target, c_src)
                self.assertEqual(
                    res.returncode,
                    0,
                    f"{target} C macro guard failed:\n{res.stderr}",
                )

        x86_entry = (
            ROOT / "kernel" / "arch" / "x86_64" / "intr" / "entry.S"
        ).read_text(encoding="utf-8")
        self.assertIn("#include <arch/context.h>", x86_entry)
        self.assertIn(".global ARCH_CONTEXT_RESTORE_FRAME", x86_entry)
        self.assertIn("ARCH_CONTEXT_RESTORE_FRAME:", x86_entry)
        check_signal_block = re.search(
            r"check_signal:.*?boot2_test_restore:", x86_entry, re.DOTALL
        )
        self.assertIsNotNone(check_signal_block)
        assert check_signal_block is not None
        self.assertRegex(
            check_signal_block.group(0), r"\bje\s+ARCH_CONTEXT_RESTORE_FRAME\b"
        )
        self.assertRegex(
            check_signal_block.group(0), r"\bjz\s+ARCH_CONTEXT_RESTORE_FRAME\b"
        )
        boot2_block = re.search(
            r"boot2_test_restore:.*?#endif", x86_entry, re.DOTALL
        )
        self.assertIsNotNone(boot2_block)
        assert boot2_block is not None
        self.assertRegex(
            boot2_block.group(0), r"\bjmp\s+ARCH_CONTEXT_RESTORE_FRAME\b"
        )
        ret_exc_block = re.search(
            r"ret_from_exception:.*?ENTRY\(ret_from_intr\)", x86_entry, re.DOTALL
        )
        self.assertIsNotNone(ret_exc_block)
        assert ret_exc_block is not None
        self.assertRegex(ret_exc_block.group(0), r"\bjmp\s+RESTORE_ALL\b")
        self.assertNotIn("ARCH_CONTEXT_RESTORE_FRAME", ret_exc_block.group(0))

        a64_entry = (
            ROOT / "kernel" / "arch" / "aarch64" / "intr" / "entry.S"
        ).read_text(encoding="utf-8")
        self.assertIn("#include <arch/context.h>", a64_entry)
        self.assertRegex(
            a64_entry,
            r"restore_pt_regs:\s*\n\s*\.globl\s+ARCH_CONTEXT_RESTORE_FRAME\s*\n\s*ARCH_CONTEXT_RESTORE_FRAME:"
            r"|\.globl\s+ARCH_CONTEXT_RESTORE_FRAME\s*\nrestore_pt_regs:\s*\nARCH_CONTEXT_RESTORE_FRAME:",
        )
        el0_restore_block = re.search(
            r"aarch64_el0_restore:.*?\.size\s+aarch64_el0_restore", a64_entry, re.DOTALL
        )
        self.assertIsNotNone(el0_restore_block)
        assert el0_restore_block is not None
        self.assertRegex(
            el0_restore_block.group(0),
            r"mov\s+sp,\s*x0\s*\n\s*b\s+ARCH_CONTEXT_RESTORE_FRAME\b",
        )
        el0_irq_block = re.search(
            r"el0_irq_entry:.*?el0_sync_entry:", a64_entry, re.DOTALL
        )
        self.assertIsNotNone(el0_irq_block)
        assert el0_irq_block is not None
        self.assertRegex(
            el0_irq_block.group(0), r"\bb\s+ARCH_CONTEXT_RESTORE_FRAME\b"
        )
        el0_sync_block = re.search(
            r"el0_sync_entry:.*?#if\s+AARCH64_EL0_TEST(.*?)#else", a64_entry, re.DOTALL
        )
        self.assertIsNotNone(el0_sync_block)
        assert el0_sync_block is not None
        self.assertRegex(
            el0_sync_block.group(0), r"\bcbnz\s+w0,\s*ARCH_CONTEXT_RESTORE_FRAME\b"
        )
        self.assertRegex(
            el0_sync_block.group(1), r"\bb\s+restore_pt_regs\b"
        )
        el1_sync_block = re.search(
            r"el1_sync_entry:.*?el0_irq_entry:", a64_entry, re.DOTALL
        )
        self.assertIsNotNone(el1_sync_block)
        assert el1_sync_block is not None
        self.assertRegex(el1_sync_block.group(0), r"\bb\s+restore_pt_regs\b")
        self.assertNotIn("ARCH_CONTEXT_RESTORE_FRAME", el1_sync_block.group(0))


if __name__ == "__main__":
    unittest.main()
