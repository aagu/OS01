# 2D Graphics API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 x86_64 OS01 提供可裁剪的用户态 2D 绘图库、按矩形受限的 `/dev/gfx0` 呈现接口，并迁移 Tetris。

**Architecture:** `libgfx.a` 在视图局部坐标的私有缓冲内绘图；每帧一次 `GFX_PRESENT` 将缓冲传给内核。内核按 open-file 保存的矩形逐行校验和拷贝，不把 framebuffer MMIO 映射给该 handle；现有 `/dev/fb` 保留给 terminal。

**Tech Stack:** C17、x86_64 内核 devfs/file/uaccess、静态 archive、GNU Make、hosttests、Python QEMU runner。

**Spec:** `docs/superpowers/specs/2026-09-30-2d-graphics-api-design.md`（执行前阅读全文）。

## Global Constraints

- 首阶段仅 x86_64，`GFX_FORMAT_RGB32=0`，32 bpp host-endian；视图和绘图坐标分别为全屏和局部坐标。
- ABI 只在 `kernel/include/uapi/gfx.h` 定义；内核实现头位于 `kernel/include/driver/`，用户库公有头随 sysroot 发布。
- `/dev/gfx0` 每 open file 一视图，最多 16 个；`dup`/`fork` 共享，最后 `file_put` 释放；本阶段不提供跨进程权限隔离。
- `GFX_PRESENT` 逐行检查用户地址并 fault-tolerant copy，不在 spinlock 内调用 uaccess；允许部分行已显示后返回 `EFAULT`。
- sysroot generation 只有 `mk/components/sysroot.mk` 可以发布；修改 `file_t` 结构后运行 `make clean`。
- Tetris 迁移，terminal 不迁移；QEMU `gfx` suite 使用正常镜像；最终验证命令为 `make test-host`、`make test-qemu SUITE=phase-0`、`make test-qemu SUITE=gfx`。

## Review Focus

以下五类输入必须分别由所列任务的失败测试固定下来：

1. 内核地址伪装成 present buffer → `EFAULT` 且不读取内核内容（Task 2）。
2. present 时另一线程关闭/复用 fd → 调用期视图存活，最终恰好释放一次（Task 1/2）。
3. 视图触及 fb 最右/最下边界或尺寸溢出 → 合法边界通过、越界拒绝（Task 2）。
4. `color_key=0`、负坐标、极大线端点 → 黑色可透明、裁剪正确、无整数溢出（Task 4）。
5. 白对角线覆盖红绿底色 → QEMU 按采样坐标检查，不错误断言两色各占半屏（Task 5）。

---

## File map

- **内核 ABI/生命周期：** `kernel/include/uapi/gfx.h` 定义 wire structs；`kernel/include/fs/{devfs,file}.h`、`kernel/fs/{devfs,file}.c` 和 `kernel/arch/x86_64/intr/trap.c` 提供按 file 分发、pin、release。
- **图形设备：** `kernel/include/driver/gfx.h`、`kernel/driver/gfx.c` 保存视图和实现 ioctl；`kernel/include/driver/fb.h`、`kernel/driver/fb.c` 提供内核写行；`kernel/arch/x86_64/platform/boot.c` 注册节点。
- **用户库/构建：** `libgfx/{gfx.h,internal.h,gfx.c,line.c,sprite.c,Makefile}`；`mk/components/sysroot.mk` 增 staging 与 generation manifest；`user/Makefile`、`mk/components/user.mk`、`config/rootfs.mk` 接入程序和镜像。
- **测试/迁移：** `hosttests/cases/test_gfx_file_lifecycle.c`、`test_gfx_device.c`、`test_gfx_client.c`、`test_gfx_primitives.c`，`hosttests/Makefile`；`user/test_gfx.c`、`qemutests/run_test.py`、`qemutests/test_gfx_runner.py`、`mk/components/run.mk`（focused hosttest 转发及 gfx QEMU suite）；`user/tetris.c` 与相关文档。

### Task 1: UAPI、file 生命周期与 ioctl errno

**Files:** Create `kernel/include/uapi/gfx.h`, `hosttests/cases/test_gfx_file_lifecycle.c`; modify `kernel/include/fs/devfs.h`, `kernel/fs/devfs.c`, `kernel/include/fs/file.h`, `kernel/fs/file.c`, `kernel/arch/x86_64/intr/trap.c`, `libc/unistd/ioctl.c`, `hosttests/Makefile`, `mk/components/run.mk`。

**Interfaces:** Produce `GFX_CREATE_VIEW=0x4701`, `GFX_GET_INFO=0x4702`, `GFX_PRESENT=0x4703`, `GFX_FORMAT_RGB32=0`, `gfx_view_desc_t`, `gfx_info_t`, `gfx_present_req_t` with exact fields from spec §4. Add `void *dev_private` to `file_t`; add `int (*ioctl_file)(file_t *,int,void *)` and `void (*release_file)(file_t *)` to `devfs_ops`. Export `int devfs_ioctl_file(file_t *f,int cmd,void *arg)` and `void devfs_release_file(file_t *f)` from `devfs.c` via `devfs.h`: both resolve `f->node` through private `devices[]`; ioctl calls `ioctl_file` when present, otherwise existing node ioctl, otherwise returns `-ENOTTY`; release calls `release_file` when present, otherwise no-op. `fd_ioctl` calls `devfs_ioctl_file` for its existing FD_DEV/FD_VFS cases; `file_free` calls `devfs_release_file` before dropping `node`. On successful custom `FD_DEV` open, `devfs_open_node` assigns `(*out)->node=vfs_node_get(node)` before return. `SYS_ioctl` uses `files_get_file()`/`file_put()` around dispatch.

- [ ] **Step 1: Write failing host test.** `test_gfx_file_lifecycle`: fake custom `FD_DEV` open returns a file without `node`; assert `devfs_open_node` attaches one reference, `fd_ioctl` reaches `ioctl_file` through `devfs_ioctl_file`, `dup`/`fork` style refs plus a pinned ioctl survive concurrent close, `devfs_release_file` invokes `release_file` exactly once on final `file_put`, and the node ref returns to baseline. Also assert fallback to legacy node-only ioctl and no-op release when file callbacks are absent. Add binary to `TEST_BINS`, host target `test-gfx-file-lifecycle`, and root forwarding target of the same name in `mk/components/run.mk` using `os01_submake`.
- [ ] **Step 2: Run** `make test-gfx-file-lifecycle` from the repository root; expected compile/test failure because file callback and private field do not exist.
- [ ] **Step 3: Implement ABI and lifecycle.** Mirror spec structs with `_Static_assert` for 16-byte `gfx_view_desc_t`, 16-byte `gfx_info_t`, 16-byte `gfx_present_req_t`. Attach `vfs_node_get(node)` to a successful custom `FD_DEV` file without double-getting a callback-provided node; implement exported devfs helpers and make `file_free` invoke `devfs_release_file` before its existing `vfs_node_put`. `SYS_ioctl` gets a pinned file under fd-table lock and always puts it after `fd_ioctl`, including negative return. Preserve current node callback for `/dev/fb`, tty and other devices. Make libc `ioctl()` translate a raw `-errno` into `-1` and `errno`.
- [ ] **Step 4: Clean and verify.** Run `make clean && make test-gfx-file-lifecycle && make test-host`; expected focused and full host suites pass. Run `make OS01_SYSTEST=1 test-syscall` separately for existing ioctl/FD behavior; expected 0 failures. Do not combine with `KERNEL_SELFTEST=1`.
- [ ] **Step 5: Commit.** Stage only Task 1 files; message `feat(gfx): add per-file device ioctl lifecycle`.

### Task 2: Kernel `/dev/gfx0` create, info and present

**Files:** Create `kernel/include/driver/gfx.h`, `kernel/driver/gfx.c`, `hosttests/cases/test_gfx_device.c`; modify `kernel/include/driver/fb.h`, `kernel/driver/fb.c`, `kernel/arch/x86_64/platform/boot.c`, `kernel/Makefile`, `hosttests/Makefile`, `mk/components/run.mk`。

**Interfaces:** `extern const struct devfs_ops gfx_ops`; `int fb_get_info(struct fb_info *out)`; `int fb_write_row(uint32_t x,uint32_t y,const void *pixels,uint32_t row_bytes)` (kernel buffer only; validates against width, height and `Pos.FB_length`). `gfx_ops.open` returns a per-file unconfigured view; `ioctl_file` handles Task 1 UAPI; `release_file` returns the slot. No gfx mmap callback.

- [ ] **Step 1: Write failing fake-fb device test.** Exercise `GFX_CREATE_VIEW` exact-fit right/bottom, zero width/height, `UINT32_MAX` additions, reconfigure rejection, 16-view limit, `GFX_GET_INFO` local width/height/stride/format, two independent views and release/reopen. Add present test with an odd-width buffer, sentinel pixels immediately outside the view and in fb row padding, overlapping views in present order, invalid stride/reserved, and a user pointer in kernel range; assert no outside write and `-EFAULT` for the pointer. Mock `syscall_check_user_range` and fault-tolerant copy so a missing range check fails this test. Add host and root forwarding targets `test-gfx-device`.
- [ ] **Step 2: Run** `make test-gfx-device`; expected missing `gfx_ops`/fb helpers.
- [ ] **Step 3: Implement fb helpers and gfx device.** Validate framebuffer availability and RGB32 format; under a small spinlock allocate/configure/free one of 16 slots. Snapshot immutable view under lock, then unlock before allocation, `syscall_check_user_range` or `copy_*_ft`. Check request structure's readable/writable range; for each source row check `row_addr`, `row_bytes`, then fault-tolerant copy into a reusable heap row buffer and call `fb_write_row`. Free buffer on every return path. Stop on first fault; earlier rows may remain visible. Register `gfx0` in `x86_64_boot_device_nodes()` after devfs exists.
- [ ] **Step 4: Verify.** Run `make test-gfx-device`, `make test-host` and `make kernel.bin`; expected all tests pass and kernel links with `gfx.o`.
- [ ] **Step 5: Commit.** Stage Task 2 files; message `feat(gfx): add bounded framebuffer present device`.

### Task 3: Static libgfx, sysroot integration and wrappers

**Files:** Create `libgfx/Makefile`, `libgfx/gfx.h`, `libgfx/internal.h`, `libgfx/gfx.c`, `hosttests/cases/test_gfx_client.c`, `user/test_gfx.c`; modify `mk/components/sysroot.mk`, `hosttests/Makefile`, `mk/components/run.mk`, `user/Makefile`, `mk/components/user.mk`, `config/rootfs.mk`。

**Interfaces:** Implement `gfx_open`, `gfx_close`, `gfx_get_info`, `gfx_set_clip`, `gfx_present` with spec §4 signatures. Private `struct gfx_handle` contains fd, `gfx_info_t`, heap pixels, and local clip; source files share `libgfx/internal.h` (never installed). `gfx_open` calls open/create/get-info, allocates zeroed `width*height*4` buffer; `gfx_present` sends `gfx_present_req_t` with this buffer. Task 3 archive contains only `gfx.c`; Task 4 adds `line.c` and `sprite.c`.

- [ ] **Step 1: Write failing client test.** Mock open/ioctl/close/malloc; assert call order, `ENOENT→ENODEV`, cleanup at each failure point, `NULL` behavior for `gfx_get_info` and `gfx_set_clip`, and one ioctl per `gfx_present`. Check no `GFX_SET_CLIP` ioctl is emitted. Add host and root forwarding targets `test-gfx-client`.
- [ ] **Step 2: Run** `make test-gfx-client`; expected missing gfx client symbols.
- [ ] **Step 3: Implement wrappers and staging.** Compile `libgfx.a` with the profile target compiler/ar into a component-private build dir. In `mk/components/sysroot.mk` add a `libgfx-install` staging stamp ordered after kernel headers and libc, publish its manifest in the generation loop, and list it as a `SYSROOT_STAMP` prerequisite. Install only `usr/include/gfx.h` and `usr/lib/libgfx.a`; UAPI comes from kernel headers. Add `test_gfx` to `USER_PROGRAMS` and `/bin/test_gfx` to rootfs manifest; add explicit user link rules with `-lgfx -lc` after objects. Create `user/test_gfx.c` as a small compile/link smoke program, replaced by the substantive test in Task 5.
- [ ] **Step 4: Verify.** Run `make test-gfx-client`, `make lib`, `make user`; inspect the generated sysroot generation for `gfx.h`, `uapi/gfx.h`, `libgfx.a` and confirm no duplicate manifest destination. Expected all commands exit 0.
- [ ] **Step 5: Commit.** Stage Task 3 files; message `feat(gfx): stage libgfx and add client lifecycle`.

### Task 4: Inlined primitives, Bresenham and sprite blits

**Files:** Create `libgfx/line.c`, `libgfx/sprite.c`, `hosttests/cases/test_gfx_primitives.c`; modify `libgfx/gfx.h`, `libgfx/internal.h`, `libgfx/gfx.c`, `libgfx/Makefile`, `hosttests/Makefile`, `mk/components/run.mk`。

**Interfaces:** `gfx_pixel/hline/vline/rect/fill_rect` are `static inline` in installed `gfx.h`; `gfx_line`, `gfx_sprite_blit`, `gfx_sprite_blit_mask` are archive functions with exact spec §4 signatures. Because `gfx_handle_t` is opaque, each inline is a thin zero-syscall wrapper around a correspondingly named `gfx__*_impl` archive function that accesses `internal.h`; the installed header must not reveal `pixels`. All coordinates are view-local. RGB32 source stride is bytes; mask is MSB-first with `mask_stride>=ceil(src_w/8)`; explicit `use_color_key` permits key 0.

- [ ] **Step 1: Write failing bitmap test.** Use guard words around a 7×5 buffer; assert pixel/line/rect/fill colors, clipping for negative and oversized coordinates, all Bresenham octants and both endpoints, `INT32_MIN`/`INT32_MAX` lines that do not overflow or run forever, opaque and transparent-black sprite modes, mask bit order and padded source stride. Add host and root forwarding targets `test-gfx-primitives`.
- [ ] **Step 2: Run** `make test-gfx-primitives`; expected missing primitive definitions.
- [ ] **Step 3: Implement minimal primitives.** `gfx__*_impl` functions use one shared rectangle intersection helper with 64-bit endpoints before clipping; `gfx_line` first clips to the view/clip or uses bounded iteration over visible pixels so extreme endpoints cannot cause unbounded loops. Sprite functions validate stride arithmetic and iterate only visible source pixels. Keep all drawing in private memory with zero syscalls.
- [ ] **Step 4: Verify.** Run `make test-gfx-primitives`, `make test-host`, `make lib`, `make user`; expected 0 failures and no undefined gfx symbols.
- [ ] **Step 5: Commit.** Stage Task 4 files; message `feat(gfx): add clipped 2D primitives`.

### Task 5: Ring-3 QEMU gfx suite on the normal image

**Files:** Implement `user/test_gfx.c`; modify `qemutests/run_test.py`, `mk/components/run.mk` (and `user/Makefile`/`config/rootfs.mk` only if Task 3 has not already added the wiring).

**Interfaces:** `make test-qemu SUITE=gfx` selects `TEST_QEMU_IMG_gfx=$(NORMAL_IMAGE)`, builds normal `image`, then `run_test.py gfx`. The pre/post normal-image hash guard in `mk/components/run.mk` applies only to variant suites: exclude both `phase-0` and `gfx` from its two existing `if` conditions so a gfx rebuild of the normal image is allowed. Runner gains an interactive `TestRunner.start_qemu(serial_stdio=True)` path using `-serial stdio`, stdin PIPE and stdout log file; `send_line()` writes and flushes a shell command after `wait_for_prompt()`.

- [ ] **Step 1: Write failing runner test.** Create `qemutests/test_gfx_runner.py` with a fake process/log: assert `gfx` suite is recognized, waits for the shell prompt, sends `/bin/test_gfx\n`, and rejects missing or FAIL result. Ensure existing file-serial suites still use their old mode. Add a Make dry-run fixture that sees no normal-image hash guard for `SUITE=gfx` but retains both guards for `SUITE=systest`. Run `python3 -m unittest qemutests.test_gfx_runner`; expected failure until runner/Make mode exists.
- [ ] **Step 2: Implement ring-3 test program.** Full-screen view: red left, green right, white diagonal, present; read chosen white and nonwhite pixels through existing `/dev/fb` mapping, not a full serial dump. Small central view: prefill surrounding sentinels, present and assert outside pixels unchanged. Exercise unconfigured fd and out-of-bounds create. Emit exactly `[GFX TEST] PASS` or `[GFX TEST] FAIL: <reason>` to stdout/serial.
- [ ] **Step 3: Implement runner and Make suite.** Extend `TestRunner` only for `gfx` with serial stdio and writable stdin; retain file serial for other suites. Extend run.mk flavor/image maps, suite allowlist and help text, and update **both** normal-image snapshot/compare conditions to exclude `gfx`. Runner waits for prompt, runs test executable, waits for unique marker, fails on timeout/nonzero marker.
- [ ] **Step 4: Verify.** Run Python fixture, `make test-qemu SUITE=gfx`, then `make test-qemu SUITE=phase-0`; expected PASS marker and both suites exit 0. If shell input is not available, fix runner transport without replacing the normal-image requirement.
- [ ] **Step 5: Commit.** Stage Task 5 files; message `test(gfx): exercise bounded present in QEMU`.

### Task 6: Tetris migration, regressions and docs

**Files:** Modify `user/tetris.c`, `docs/roadmap.md`, `docs/gui.md`, `docs/driver.md`, `docs/structure.md`。

**Interfaces:** Tetris opens `/dev/fb`, reads existing `struct fb_info` metadata, closes that metadata fd, then calls `gfx_open(0,0,fb_info.width,fb_info.height)`; no new screen-query API. It draws via `gfx_fill_rect`, calls `gfx_present` once after each visual update/flash, and calls `gfx_close` on every exit path. Keep keyboard, timing and board logic intact.

- [ ] **Step 1: Capture current Tetris behavior.** Run the existing Tetris logic hosttest, then manually run Tetris in QEMU and record board colors, placement, flash, input, score and terminal restore before changing rendering. Use the same checks after migration; a source-text check would not verify behavior.
- [ ] **Step 2: Migrate rendering.** Replace `draw_cell`, `draw_rect` and framebuffer mmap path with `gfx_fill_rect`; preserve diffing and clear-line flash. Present once after clear screen, each render and flash, before the pause; do not present per cell. Preserve `FBIOSURRENDER` behavior if needed for screen ownership before new gfx writes.
- [ ] **Step 3: Verify full gate.** After any structure change run `make clean`. Run `make test-host`, `make test-qemu SUITE=phase-0`, `make test-qemu SUITE=gfx`, and `make OS01_SYSTEST=1 test-syscall` separately. Manually play Tetris in QEMU, verify move/rotate/drop/clear/score, and terminal redraw before/after exit. Record actual results; do not claim visual equivalence from automated tests alone.
- [ ] **Step 4: Update docs and commit.** Mark roadmap item complete only if the gate passes; document libgfx, `/dev/gfx0`, build/test entry points and deferred owner/delegation. Commit Task 6 files with message `feat(gfx): migrate Tetris and document API`.

## Plan self-review

- [x] Spec §1–8 maps to Tasks 1–6: ABI/lifecycle, bounded present, library, primitives, QEMU, Tetris/docs.
- [x] Tasks 1–5 name a failing test before implementation and a passing command afterward; Task 6 uses the existing Tetris logic test plus the same manual visual checks before/after migration.
- [x] UAPI values, callback names, `gfx_*` symbols and Make targets agree across the Interfaces blocks.
- [x] Each of the five Review Focus cases has a test in its owning task.
- [x] The plan specifies interfaces and checks, leaving function bodies to the implementer.
