# Architecture Source Groups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Group flat architecture source files without changing kernel behavior.

**Architecture:** Retain root build anchors, move implementation files into shallow responsibility directories, then update exact source discovery and fixed-path consumers.

**Tech Stack:** GNU Make, C, assembly, linker scripts, Python and C tests.

**Spec:** `docs/superpowers/specs/2026-09-25-arch-source-groups-design.md`

## Global Constraints

- Do not change symbols, boot order, ABI, or generated trampoline binary symbol names.
- Leave public headers in `kernel/include/arch/` and keep their interfaces unchanged.
- Keep root `head.S`, `linker.ld`, `make.config`, plus x86_64 `trampoline.S` and `trampoline.ld`.
- Update fixed source paths in tests and current docs; do not rewrite historical design documents.

## Review Focus

- A relocated C or assembly source missing from the build must be caught by source-to-object comparison and profile builds.
- An object listed twice must be caught by checking expanded object lists.
- The x86_64 trampoline embedded symbol names must remain identical.
- Host tests that compile production files must use their relocated paths.
- AArch64 boot and x86_64 kernel builds must both complete after a clean build.

---

### Task 1: Move files and update build and path consumers

**Files:** `kernel/arch/{x86_64,aarch64}/**`, `kernel/Makefile`, `kernel/arch/x86_64/make.config`, `hosttests/Makefile`, `hosttests/cases/*.c`, `qemutests/*.py`, `docs/arch.md`, `docs/architecture.md`, `mk/targets/aarch64.mk` only if needed.

**Interfaces:** All existing exported names and public headers remain identical. The build must produce one object for every regular architecture source, excluding the specially built trampoline.

- [ ] Move x86_64 sources to the directories defined in the spec, using `git mv` where possible.
- [ ] Move aarch64 sources to the directories defined in the spec, using `git mv` where possible.
- [ ] Update `kernel/Makefile` source globs, AArch64 whitelist, and x86_64 platform globs. Keep the existing trampoline recipe and generated path intact.
- [ ] Update all fixed path consumers in tests and current architecture docs; leave historical specs/plans unchanged.
- [ ] Clean and build both profiles, run directly affected host/QEMU tests, and compare source/object lists for omissions and duplicates.
- [ ] Review diff for unintended content changes and commit the finished migration.
