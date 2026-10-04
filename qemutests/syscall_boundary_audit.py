#!/usr/bin/env python3
"""Static checks for the ARCH-1 syscall boundary and table coverage."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
TRAP = ROOT / "kernel/arch/x86_64/intr/trap.c"
DISPATCH = ROOT / "kernel/syscall/dispatch.c"
UAPI = ROOT / "kernel/include/uapi/syscall.h"
DOC = ROOT / "docs/syscall/syscall.md"


def fail(message: str) -> None:
    print(f"syscall boundary audit: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


trap = TRAP.read_text()
dispatch = DISPATCH.read_text()
uapi = UAPI.read_text()
doc = DOC.read_text()

entry_start = trap.index("void do_system_call(")
entry_end = trap.index("void sys_vector_install(", entry_start)
entry = trap[entry_start:entry_end]
if re.search(r"\bswitch\s*\(", entry) or re.search(r"\bcase\s+SYS_", entry):
    fail("x86 syscall entry still contains a business switch or SYS_* case")

for path in sorted((ROOT / "kernel/syscall").glob("*.c")):
    source = path.read_text()
    if re.search(r"^\s*#\s*include\s*[<\"]arch/(?:x86_64|aarch64)/", source, re.M):
        fail(f"{path.relative_to(ROOT)} includes a per-architecture header")
    if re.search(r"\b(?:pt_regs_t|struct\s+pt_regs)\b", source):
        fail(f"{path.relative_to(ROOT)} depends on an architecture register-frame type")
    if re.search(r"\b(?:regs|frame|pt_regs|registers)\s*->\s*(?:rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r8|r9|r10|r11|r12|r13|r14|r15|rip|cs|ss|eflags|sp|pc|pstate)\b", source):
        fail(f"{path.relative_to(ROOT)} accesses architecture register/frame fields")

defined = {
    name: int(number)
    for name, number in re.findall(r"^\s*#\s*define\s+SYS_(\w+)\s+(\d+)\b", uapi, re.M)
}
entries = {
    name: (handler, trace_name)
    for name, handler, trace_name in re.findall(
        r'\[SYS_(\w+)\]\s*=\s*\{\s*(NULL|0|\w+)\s*,\s*"([^"]+)"\s*\}',
        dispatch,
    )
}
if set(defined.values()) != set(range(75)):
    fail("kernel UAPI syscall definitions are not the complete 0..74 range")
missing = sorted(set(defined) - set(entries))
unexpected = sorted(set(entries) - set(defined))
if missing or unexpected:
    fail(f"handler table keys differ from UAPI; missing={missing}, unexpected={unexpected}")
wrong_names = sorted(name for name, (_, trace_name) in entries.items() if trace_name != name)
if wrong_names:
    fail(f"handler table trace names differ from SYS_* names: {wrong_names}")
null_handlers = sorted(name for name, (handler, _) in entries.items() if handler in ("NULL", "0"))
if null_handlers != ["getpeername"] or defined.get("getpeername") != 62:
    fail(f"only SYS_getpeername=62 may have a null handler; found {null_handlers}")
if len({defined[name] for name in entries}) != len(entries):
    fail("handler table maps multiple names to the same syscall number")
if len(entries) - len(null_handlers) != 74:
    fail(f"expected 74 implemented syscall handlers; found {len(entries) - len(null_handlers)}")

documented = {
    name: int(number)
    for number, name in re.findall(r"^\|\s*(\d+)\s*\|\s*`SYS_(\w+)`\s*\|", doc, re.M)
}
if documented != defined:
    fail("documentation syscall table does not exactly match UAPI names and numbers")

print("syscall boundary audit: PASS (no legacy switch, generic handlers stay arch-neutral, docs/UAPI 0..74 aligned, 74 handlers, named absent 62)")
