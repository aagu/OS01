# aarch64 产品用户态接入（M4 剩余）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** aarch64 走 `/bin/init` → `ash` 的产品启动链路（`OS01_HAS_USERSPACE=1`），并补齐 aarch64 侧文件映射、MAP_FIXED/共享映射、设备映射。

**Architecture:** 复用 x86 已闭环的产品链路骨架：`kernel_start_userspace()`（arch 中立，`kernel/init/userspace.c:43`）→ `spawn_user_task(path, argv)`（目前 x86-only，`kernel/arch/x86_64/sched/exec.c:357`）。aarch64 侧把 M4 已闭环的 image 机制（`aarch64_user_image_load`，memory-resident ET_EXEC）接到 VFS 读路径上，并扩展 userland 构建链（libc libc.a / crt0 / linker script / runtime）。mm 三件套在 `kernel/arch/aarch64/memory/user_vm_policy.c` 的 v1（仅 private-anonymous）基础上按 x86 `kernel/arch/x86_64/memory/mmap.c` 的事务模式扩展。

**Tech Stack:** clang -target aarch64-none-elf、ld.lld、QEMU virt + edk2-aarch64（诊断 DTB 机制）、mtools/GPT 镜像、现有 hosttests/qemutests/selftest 三层测试框架。

**Spec:** docs/roadmap.md:75「M4 剩余：产品用户态接入」（本计划为其全部剩余项的实施方案）

## 研究结论（事实基线，实施者必读）

| 事实 | 位置 |
|---|---|
| `OS01_HAS_USERSPACE` aarch64 现为 0；`OS01_HAS_VFS` 已翻 1 | `kernel/include/arch/features.h:26-34` |
| `kernel_main` 第 6/7 段（fs、tty）已在 aarch64 生效；第 9 段 `OS01_HAS_USERSPACE` 分叉到 `kernel_start_userspace()`，否则 `arch_start_kernel_idle()` | `kernel/core/main.c:119-195` |
| `kernel_start_userspace()` 是 arch 中立编排：creation window → init_mm → stdio（/dev/tty fd 0/1/2）→ `vfs_end_bootstrap` → `spawn_user_task("/bin/init", NULL)`（要求 PID==1）→ sched start | `kernel/init/userspace.c:43-97` |
| `spawn_user_task` x86 实现：staged-failure 私有构建 + 单点发布；vfs_lookup → elf_validate → task 结构 → files_dup 继承 → PGD 复制 → elf_load → 2M 栈 → SysV 初始栈 → 发布 | `kernel/arch/x86_64/sched/exec.c:357-600` |
| aarch64 已有 image 机制：`aarch64_user_image_load(bytes,size,&img)`（ELF 校验 + mm 私有根 + 逐页映射）、`aarch64_user_image_setup_stack`、`aarch64_user_image_destroy`、`aarch64_user_image_init_regs` | `kernel/include/arch/aarch64/user_image.h`、`kernel/arch/aarch64/memory/user_image.c` |
| aarch64 任务发布：`aarch64_user_task_spawn(catalog,...)` → `sched_alloc_and_publish_task`（单点发布，`user_task_prepare` 置 `tsk->files=NULL`、parent=idle） | `kernel/arch/aarch64/cpu/user_task.c:1033-1093,871-921` |
| aarch64 `arch_syscall_exec` 只认任务的 in-memory **catalog**，不经 VFS | `kernel/arch/aarch64/cpu/user_task.c:562-648` |
| 共享 syscall 表 aarch64 已服务：exit/getpid/getppid/clock_gettime/brk/mmap/mprotect/munmap/fork/waitpid/exec/kill/signal/sigprocmask/sigreturn + fs 组；无 sigaction/futex/net | `kernel/syscall/dispatch.c:62-70`、changelog 2026-11-18 |
| aarch64 mmap v1：仅 private-anonymous；MAP_SHARED-only/file-backed/MAP_FIXED → `-EOPNOTSUPP`；fd 必须UINT64_MAX | `kernel/arch/aarch64/memory/user_vm_policy.c:578-627` |
| x86 do_mmap 已实现 MAP_FIXED 事务 + 设备映射（VMA_IO）+ 文件 demand fault（`vm_file`/`vm_pgoff` 中立字段已存在） | `kernel/arch/x86_64/memory/mmap.c`（901 行） |
| aarch64 demand fault 入口：`aarch64_user_vm_fault` → `user_vm_fault(mm,far,write)`（只补零页 anon leaf） | `kernel/arch/aarch64/memory/user_vm.c:188-222`、`user_vm_policy.c:887-918` |
| libc `syscall()` 是 header 内 `static inline`，x86 专用 `int $0x80`；aarch64 只编 libk.a 不编 libc.a | `libc/include/sys/syscall.h:110-117`、`libc/Makefile:115-121` |
| userland 构建链 x86-only：`user/crt0.S`（`__libc_start_main` 契约）、`user/linker.ld`（base 0x400000、PHDRS text/data、`__bss_start/__bss_end`）、`user/Makefile`（`-m elf_x86_64`）、`RUNTIME_TARGET_user` 仅 x86 profile 定义 | `user/crt0.S`、`user/linker.ld`、`user/Makefile:59`、`mk/profiles/x86_64-clang.mk:175` |
| aarch64 profile 无 `userland` capability；`USER_BUILD_DIR` 已定义但"builds no userland" | `mk/profiles/aarch64-clang.mk:6,269-275` |
| image 限制：`AARCH64_ELF_MAX_PAGES=16`；栈固定 1 页 `[0x20400000,0x20401000)`，guard 在下方 `0x203ff000`；argv/envp ≤ 1 页 payload；ELF 拒 PT_INTERP/PT_DYNAMIC/W+X/PT_TLS | `kernel/include/arch/aarch64/user_elf.h:11`、`user_image.h:31-46`、`user_image.c:289-311` |
| aarch64 mmap 窗口：`USER_VM_MMAP_BASE=0x40000000`，`AARCH64_USER_LIMIT=1<<48` | `user_vm_policy.c:48`、`mmu.h:18` |
| init 用户程序已存在：BusyBox 式四阶段，读 `/etc/inittab`（fallback respawn `/bin/terminal`） | `user/init.c` |
| aarch64 产品镜像 = 64 MiB FAT32 superfloppy（mtools 填充，BOOTAA64.EFI + kernel.elf）；selftest 变体 = GPT 双分区（ESP + ext2 rootfs） | `mk/components/image.mk:246-291` |
| PL011 RX IRQ → `tty_push_input` 已接好；`/dev/tty` devfs 节点已验证 | changelog 2026-11-18 console 门面 |
| busybox.elf 仅 x86 构建（`mk/components/user.mk` 私有副本 + overlay）；terminal.elf 依赖 libgfx（fb） | `mk/components/user.mk:121-151`、`user/terminal/terminal.c` |

**关键设计决策（已定）：**

1. **ash = 自研串口 shell**（新 user 程序 `user/ash/`），不用 busybox：aarch64 内核无 `sigaction`（busybox ash 硬依赖 POSIX sigaction/ioctl 面），busybox aarch64 移植是独立大任务。roadmap 验收的本意是「产品启动链路」，不是 busybox 本身。边界如实记录。
2. **spawn 不复制 x86 的 2M 栈/PGD 半区复制**：aarch64 走自己的 image 机制（mm 私有根 + TTBR1 内核半区），只对齐其 staged-failure + 单点发布语义。
3. **栈参数化而非改常量**：`aarch64_user_image_setup_stack` 增加 stack 页数参数（默认 1 页 = selftest 合同不变；产品 spawn 传 8 页，向上扩展到 0x20408000，guard/段顶 0x203ff000 契约不变）。
4. **共享映射 v1 范围 = 匿名 MAP_SHARED（同 inode 语义复用 shmem 注册表）**：文件 MAP_SHARED 与匿名 MAP_SHARED 统一为「(node|NULL, pgoff) → 共享页注册表，首次缺页分配并填页，后续进程缺页查表接线」；不引入完整 page cache。
5. **文件映射 demand fault 复用 VFS 同步读**：缺页时按 `vm_pgoff` 对应偏移 `vfs_read` 一页进新分配的 leaf 页（无页缓存；同一 VMA 内已驻留页不重读）。

## Global Constraints

- 双架构共用 `boot_context` ABI 与 `kernel_main`；任何 `kernel/include/core/bootinfo.h` 变更跑 `make test-host`（本计划不改 bootinfo）。
- 内核运行日志用 `log_info/log_warn/log_err/log_debug`/`debug_*()`，不用 `serial_printk/color_printk`。
- 公共头放 `kernel/include/<subsys>/`；架构专属实现在 `kernel/arch/<arch>/`；`kernel/include/uapi/` 只放用户态 ABI。
- 两套 syscall 编号共享 `kernel/include/uapi/syscall.h`（libc 侧 `_Static_assert` 防漂移，`libc/include/sys/syscall.h:93-98`）。
- aarch64 内核栈帧预算 512 B（`-Wframe-larger-than=512`）；ELF phdr 数组堆分配，头部对齐本地拷贝（`user_elf.c:18-24` 的既有规则）。
- 每个任务完成即 commit；`make test-host` 是宿主回归底线；aarch64 QEMU 验收走诊断 DTB 机制（`--diagnostic-dtb=auto`）。
- selftest 冻结合同（`aarch64_uefi_smp` 15 用例、m4-1 28 用例、`[sched-evidence]`/`[ap-sched]` 字段）不得破坏；动到自我观察行需同步 harness fixture。

## Review Focus

1. **ELF 校验器与新 userland 产物的交互**：aarch64 linker script 产出的段若违反「4K 对齐 / 无 W+X / entry 文件内 / ≤16 页」任一条，spawn 静默 `-ENOEXEC`。→ Task 2 步骤 4 的构建期自检（llvm-readelf 断言）。
2. **PID 1 与 stdio 继承**：spawn 若不继承 idle 的 /dev/tty fds，init 的 printf 全部丢失且 shell 无输入。→ Task 3 步骤 2 的验收直接观察串口输出。
3. **`arch_syscall_exec` 的 catalog/VFS 双路**：selftest（AARCH64_EL0_TEST）必须继续走 catalog，否则 28/28 崩；产品路径必须走 VFS。→ Task 2 步骤 6 的双变体回归。
4. **文件映射缺页与 fork/COW 交互**：文件 VMA fork 后两侧各缺页各自读文件是允许的（MAP_PRIVATE）；但 COW 升级路径若把文件页当 anon 处理会丢 `vm_file` 引用计数。→ Task 5 的 host 测试断言引用计数。
5. **MAP_FIXED 释放重叠区的事务性**：预分配失败必须整体回滚（对齐 x86 prepare/commit 模式），半释放状态会让后续 fault 落进已 unmap 的窗口。→ Task 4 的注入测试。

---

### Task 1: aarch64 userland 构建链（libc libc.a + crt0 + linker script + runtime）

**Files:**
- Modify: `libc/include/sys/syscall.h:110-117`（syscall() 按 `__aarch64__` 分支）
- Modify: `libc/Makefile`（aarch64 分支补 libc.a 构建；`libc/arch/aarch64/make.config` 补 `ARCH_HOSTEDOBJS`）
- Create: `user/crt0_aarch64.S`
- Create: `user/linker_aarch64.ld`
- Modify: `user/Makefile`（按 profile 的 `RUNTIME_TARGET_user` 选择 flags/ld/crt0/linker script）
- Modify: `mk/profiles/aarch64-clang.mk`（`PROFILE_CAPABILITIES` 加 `userland`；`RUNTIME_TARGET_user := aarch64-none-elf`、`RUNTIME_OBJECT_FORMAT_user := ELF64`、`RUNTIME_MACHINE_user := EM_AARCH64`、`RUNTIME_ABI_user` 置对应值）
- Modify: `mk/components/user.mk`（busybox 目标 gate：aarch64 不构建 busybox，USER_ARTIFACTS 不含之）

**Interfaces:**
- Produces: aarch64 sysroot（`build/aarch64-clang/sysroot`）含 `libc.a`；`user/*.elf` 以 `clang -target aarch64-none-elf -mgeneral-regs-only -ffreestanding -nostdlib` + `crt0_aarch64.o` + `-T linker_aarch64.ld` 静态 ET_EXEC 产出；entry `_start`，`__libc_start_main(main, argc, argv, 0, 0, 0, 0)` 契约与 x86 相同（aarch64 寄存器传参 x0=main x1=argc x2=argv）。

- [ ] **Step 1: syscall() aarch64 分支**

```c
#elif defined(__aarch64__)
static inline int64_t syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a1;
    register uint64_t x1 __asm__("x1") = a2;
    register uint64_t x2 __asm__("x2") = a3;
    __asm__ volatile ("svc #0"
                      : "=r"(x0)
                      : "r"(x8), "0"(x0), "r"(x1), "r"(x2)
                      : "memory", "cc");
    return (int64_t)x0;
}
#endif
```

（与 M4.9 既有内核侧约定一致：`svc #0` 经 x8 选择共享表条目。）

- [ ] **Step 2: libc/Makefile aarch64 userland 分支** — `HOSTARCH=aarch64` 时除 libk.a 外构建 libc.a：沿用 x86 源清单去掉 `int $0x80` 依赖文件（syscall 路径已变 header 分支，重编验证）；`libc/arch/aarch64/make.config` 的 `ARCH_HOSTEDOBJS` 按编译结果补（先空，编译器报错驱动）。产物装进 sysroot staging（`mk/components/sysroot.mk` 的 aarch64 无 lease 路径需要补：aarch64 profile 加 `userland` capability 后走统一 sysroot 发布，与 x86 相同）。

- [ ] **Step 3: crt0 + linker script**

`user/crt0_aarch64.S`：`.text.start` 段、清 BSS（`__bss_start..__bss_end`）、x29=0、`ldr x0, =main` / `ldr x1, [sp]`（argc）/ `add x2, sp, #8`（argv）、`bl __libc_start_main`、返回后 `mov w0, w19`（返回值）+ `mov x8, #2; svc #0`（SYS_exit）。`user/linker_aarch64.ld`：`OUTPUT_FORMAT(elf64-littleaarch64)`，`. = 0x400000`（段落在 image 校验窗口内、远低于 `USER_VM_MMAP_BASE`），PHDRS text(RX 无 W)/data(RW)，段 4K 对齐（校验器要求 `p_vaddr` 4K 对齐），`/DISCARD/` 同 x86 去掉 `.eh_frame`（`-fno-asynchronous-unwind-tables` 下本不生成）。

- [ ] **Step 4: 构建 user/hello 冒烟程序并静态验证**

临时 `user/hello.c`（printf + exit(0)），构建后 `llvm-readelf -l` 断言：ET_EXEC、两个 PT_LOAD、无 W+X 段、无 PT_INTERP/PT_DYNAMIC/PT_TLS、entry 在第一个 PF_X 段的 file offset 内；`llvm-nm` 确认 `__libc_start_main` 已链接（非 undefined）。

- [ ] **Step 5: Commit** `feat(libc+user): aarch64 userland build chain (libc.a/crt0/linker)`

### Task 2: aarch64 spawn_user_task + exec VFS 化

**Files:**
- Create: `kernel/arch/aarch64/sched/exec.c`（spawn_user_task + VFS ELF 读）
- Create: `kernel/include/arch/aarch64/exec.h`（或并入既有 `user_task.h`，按 subsys 头规则放 `kernel/include/arch/aarch64/`）
- Modify: `kernel/arch/aarch64/cpu/user_task.c`（`arch_syscall_exec` VFS 分支、`user_task_prepare` files/pgrp 继承、栈页数参数化）
- Modify: `kernel/include/arch/aarch64/user_image.h`（`aarch64_user_image_setup_stack` 加 `stack_pages` 参数；`AARCH64_PRODUCT_STACK_PAGES 8`）
- Modify: `kernel/arch/aarch64/memory/user_image.c`（多页栈映射 + setup_stack 用参数化栈顶）
- Modify: `kernel/Makefile`（ARCH_SOURCE_DIRS 若 aarch64/sched/ 是新目录需登记）
- Test: `hosttests/cases/test_aarch64_spawn_vfs.c`（VFS ELF 读 + staged failure 顺序的宿主可测部分）

**Interfaces:**
- Consumes: `aarch64_user_image_load/setup_stack/destroy/init_regs`、`sched_alloc_and_publish_task(0, true, PF_PROCESS, name, user_task_prepare, user_task_discard, &pctx, &tsk, &pid)`、`vfs_lookup/vfs_node_get/vfs_node_put`、`files_dup/files_unpin`
- Produces: `int64_t spawn_user_task(const char *path, const char *const *argv)`（与 `kernel/include/sched/task.h:292` 已声明签名一致——该声明已存在，aarch64 侧补强符号）；`arch_syscall_exec` 行为变更：产品路径 VFS 读。

- [ ] **Step 1: VFS ELF 读辅助** `kernel/arch/aarch64/sched/exec.c`：`static int read_elf_bytes(vfs_node_t *node, void **out, size_t *out_size)` — fstat 大小 → kmalloc → 循环 `vfs_read`（对齐 x86 `elf_validate`+`elf_load` 的 node 生命周期：读毕即 `vfs_node_put`，内核缓冲由 image 生命周期持有至 destroy 释放）。

- [ ] **Step 2: spawn_user_task** 顺序对齐 x86 staged-failure：`aarch64_user_mm_runtime_check` → `vfs_lookup`+type 检查 → 读 ELF → `aarch64_user_image_load` → `aarch64_user_image_setup_stack(image, argv, NULL, AARCH64_PRODUCT_STACK_PAGES)` → `sched_alloc_and_publish_task`（pctx 带栈页数；失败时 image destroy）。每级失败回滚已获得资源，注释标注 single-owner 释放（对齐 `user_task_discard` 既有模式）。

- [ ] **Step 3: user_task_prepare 继承扩展** — `tsk->files = current && current->files ? files_dup(current->files) : NULL`（idle 的 /dev/tty fd 0/1/2 经此传给 init）；`tsk->pgrp = current->pgrp; tsk->session = current->session`；parent 语义不变（spawn 时 parent=idle 保持既有）。dup 失败 → prepare 返回负值触发 `user_task_discard`（检查 discard 是否覆盖 files 释放，不覆盖则补）。

- [ ] **Step 4: 栈参数化** — `aarch64_user_image_setup_stack(image, argv, envp, stack_pages)`：栈映射 `[STACK_BASE, STACK_BASE + stack_pages*4K)`，SP 顶 = `STACK_BASE + stack_pages*4K`；guard 契约（段顶 ≤ 0x203ff000）不变；argv/envp payload 上限变为 `stack_pages*4K - HEADROOM`。默认参数 1 页保持 selftest 调用点不变（改签名后 grep 全部调用点同步）。

- [ ] **Step 5: arch_syscall_exec VFS 分支** — `#if AARCH64_EL0_TEST` 保留 catalog 查找原样；`#else` 读路径：`read_elf_bytes(vfs_lookup(path))` → `aarch64_user_image_load` → setup_stack(产品栈页数) → 换根/换 image/清 sighand/`init_regs`/destroy old（对齐现函数 595-647 行结构）。

- [ ] **Step 6: 宿主测试 + 双变体回归** — `make test-host`；`make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1` 构建 m4-1 镜像（不需要跑 QEMU，此处只验证编译与 selftest 合同编译期无恙；QEMU 回归在 Task 7 统一跑）。

- [ ] **Step 7: Commit** `feat(aarch64): spawn_user_task from VFS + exec VFS path + product stack`

### Task 3: userspace 启动编排接入（features 门 + init/ash + inittab + 镜像 + QEMU 验收）

**Files:**
- Modify: `kernel/include/arch/features.h:30`（`OS01_HAS_USERSPACE` aarch64 → 1）
- Modify: `user/sh.c`（新增 `ASH_TTY_MODE` 编译分支：fd0 是 /dev/tty canonical ASCII 时，readline 直读字节、不手动回显（tty 自回显）、退格按 0x7f 处理；扫描码路径保持 x86 默认不动）
- Modify: `config/inittab`（aarch64 用 `respawn:/bin/sh`——即 sh.elf；x86 `/bin/terminal` 保留，落地以镜像 manifest 选择 inittab 变体为准）
- Modify: `mk/components/image.mk`（aarch64 产品 variant 的 mcopy 行补 `/bin/init.elf /bin/ash /etc/inittab`；selftest variant 的 rootfs manifest 同步补）
- Modify: `mk/profiles/aarch64-clang.mk` / `user/apps.mk` 消费侧（若 USER_PROGRAMS 发现机制对 aarch64 生效则零改动）

**Interfaces:**
- Consumes: Task 1 的 init.elf/ash.elf 产物、Task 2 的 spawn_user_task
- Produces: 产品启动链 `/bin/init`(pid 1) → inittab respawn → `/bin/ash`；串口可见 init banner、ash 提示符、命令回显

- [ ] **Step 1: features 门翻转 + 编译探路** — 翻 `OS01_HAS_USERSPACE=1`，`make PROFILE=aarch64-clang` 编译；按报错补 aarch64 缺的链接面（预期：`init/userspace.c` 依赖的 `sched_boot_*`/`boot_services_*`/`selftest_boot_*` 中可能有 x86-only 符号，逐个下沉或门控，沿用 VFS 门的 weak 默认模式）。**此步骤只做「链接通过」，不宣告完成。**

- [ ] **Step 2: sh.c 复用（ASH_TTY_MODE）** — `user/sh.c` 加编译分支：默认扫描码路径原样；`ASH_TTY_MODE` 下 readline 直读 fd0 ASCII 行（canonical tty 自回显；0x7f 退格；'\r'/'\n' 结束）。aarch64 构建以 `APP_CFLAGS += -DASH_TTY_MODE` 生效（flat 程序 `sh` → `sh.elf`；镜像 manifest 安装为 `/bin/sh` 与 `/bin/ash` 两个名字）。既有内建/管道/重定向解析不动，边界记录（无 job control）。

- [ ] **Step 3: inittab + 镜像** — aarch64 inittab 变体行 `tty1:respawn:/bin/ash`；产品镜像 mcopy 清单补 `/bin/init.elf /bin/sh.elf /etc/inittab`；确认 init.c fallback（无 inittab 时 respawn `/bin/terminal`）在 aarch64 不会生效（inittab 必在镜像内）。

- [ ] **Step 4: QEMU 手工验收** — `make PROFILE=aarch64-clang run-aarch64-uefi`（诊断 DTB 默认）：串口依次看到 `[boot-5] init-created pid=1`、init banner、`init: phase SYSINIT/WAIT/ONCE`、supervision loop、ash 提示符；键入 `echo hi`（ash 内建或 /bin/echo——若 /bin/echo 未随镜像，ash 内建 echo）得到回显。

- [ ] **Step 5: Commit** `feat(aarch64): product userspace boot chain (/bin/init -> ash)`

### Task 4: aarch64 MAP_FIXED（私有匿名）

**Files:**
- Modify: `kernel/arch/aarch64/memory/user_vm_policy.c`（`user_vm_mmap` 放行 MAP_FIXED + 重叠事务）
- Test: `kernel/selftest/` 新 late 用例或扩展现有 `test_aarch64_user_vm.c`（selftest 镜像内验收）

**Interfaces:**
- Consumes: `range_free`、`authorize_absent`、`arch_user_vm_remove`、既有 stash 页语义
- Produces: `mmap(addr, len, prot, MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, ...)` 就地替换重叠 VMA

- [ ] **Step 1: 固定地址事务** — 对齐 x86 `prepare_unmap_locked` 模式（`kernel/arch/x86_64/memory/mmap.c:166`）：先全量校验（对齐、窗口、重叠清单），再预分配新 leaf（可失败点前置），后原子换入：重叠 VMA 拆分 + 既有驻留 leaf 解映射释放 + 新 VMA 插入。任一失败整体回滚，不留半释放窗口。
- [ ] **Step 2: selftest 用例** — 固定地址覆盖既有映射读回新页、固定地址命中 guard/内核窗拒绝、失败注入（预分配 OOM）后旧映射原样可读。计入 selftest 总数并同步 harness 契约（若该镜像走 `aarch64_uefi_smp` 冻结 15 用例则改为 m4-1/独立变体承载，避免动冻结合同——落地时按测试规范决定并记录）。
- [ ] **Step 3: Commit** `feat(aarch64): MAP_FIXED for private-anonymous mmap`

### Task 5: aarch64 文件映射 + 共享映射（按需缺页）

**Files:**
- Modify: `kernel/arch/aarch64/memory/user_vm_policy.c`（mmap 放行 file-backed / MAP_SHARED；fault 路径文件页填充；shmem 注册表）
- Create: `kernel/arch/aarch64/memory/user_shmem.c` + `kernel/include/arch/aarch64/user_shmem.h`（(node,pgoff) → PA 共享页注册表）
- Modify: `kernel/arch/aarch64/memory/user_vm.c`（fault 授权把文件 VMA 放进 `user_vm_fault`）
- Test: host 用例（注册表 + 引用计数）+ selftest 用例（文件映射读回 inittab 内容；fork 后 MAP_PRIVATE 文件页独立；MAP_SHARED 匿名页跨 fork 同页）

**Interfaces:**
- Consumes: `vma_t.vm_file/vm_pgoff`（中立字段已存在）、`vfs_node_get/vfs_node_put`（引用计数）、`vfs_read`、`files` 表 fd→file 解引用
- Produces: `mmap(fd, offset)` 文件私有/共享映射；fork 后 COW 与文件页正确交互

- [ ] **Step 1: mmap fd/off 接线** — fd 经当前任务 files 表取 `file_t->node`，`vfs_node_get` 引用转移到 VMA；`vm_pgoff = offset >> 12`；非法 fd/越界 offset 拒绝。MAP_SHARED 文件映射 v1 降级为 MAP_PRIVATE 行为 + 边界记录？——不：共享语义由 shmem 注册表承载（匿名与文件共用），见 Step 2。
- [ ] **Step 2: user_shmem 注册表** — key=(node 指针或 NULL, pgoff)，value=PA+引用计数；首次缺页：分配 4K 页 + 文件映射时 `vfs_read` 一页填充（短读零填）；后续查表直接接线共享 PA；VMA 解映射/任务销毁减引用，归零释放。表规模上限 + OOM 注入回滚。
- [ ] **Step 3: fault 路径扩展** — `user_vm_fault` 在 `authorize_absent` 后分流：VMA 有 `vm_file` 或 `VMA_SHARED` → shmem 接线；否则现有 anon 零页。COW fork：文件 MAP_PRIVATE 页 fork 时按现有 COW（引用 + PAGE_COW）；写升级不再读文件。
- [ ] **Step 4: 测试**（先宿主后 selftest）+ **Step 5: Commit** `feat(aarch64): file-backed mmap + MAP_SHARED via shmem registry`

### Task 6: aarch64 设备映射

**Files:**
- Modify: `kernel/arch/aarch64/memory/user_vm_policy.c`（VMA_IO 路径）
- Modify: `kernel/fs/devfs.c` 或新增 devfs 节点（`/dev/mem`，受门控）
- Test: selftest 用例（映射 D 设备窗口内已知寄存器只读页——GICD IIDR 之类，读值非零 + 写拒绝）

**Interfaces:**
- Consumes: x86 设备事务参照（`mmap.c:330-446` VMA_IO + MAP_FIXED 设备窗口）、aarch64 TTBR1 Device 块映射窗口 D `[0x08000000,0x0a000000)`（changelog 2026-11-18）
- Produces: `mmap(NULL, len, PROT_READ|..., MAP_SHARED, fd("/dev/mem"), phys_off)` → 用户 VA 映射 Device 属性叶

- [ ] **Step 1: /dev/mem 节点 + VMA_IO mmap** — 仿 x86：仅 MAP_SHARED + MAP_FIXED 语义、设备窗口白名单（D 窗口内，拒绝 RAM/启动块）、PTE Device 属性、无 COW/无 stash、`vm_file=NULL` + `VMA_IO`。
- [ ] **Step 2: fork/销毁交互** — VMA_IO VMA fork 不复制映射（跟随 x86），销毁只清叶。
- [ ] **Step 3: 测试 + Commit** `feat(aarch64): device mmap (VMA_IO, /dev/mem)`

### Task 7: 全量回归 + 文档

- [ ] **Step 1: 宿主** `make test-host`（新增用例纳入 catalog：`make regen-test-catalog`，生成物随变更提交）。
- [ ] **Step 2: x86 回归** `make OS01_SYSTEST=1 test-syscall`（确认共享表/libc 变更未伤 x86 E2E）。
- [ ] **Step 3: aarch64 QEMU 套件** `make PROFILE=aarch64-clang test-aarch64 MODE=smp`、`MODE=sched`、m4-1（`KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1`，28/28）、VFS selftest 变体 15/15。
- [ ] **Step 4: 产品链路最终验收** 产品镜像启动：init pid=1 → inittab → ash 提示符 → 交互命令成功；`test-static` 过。
- [ ] **Step 5: 文档** — `docs/roadmap.md` 行 75 状态更新（剩余项逐条闭环/边界）、`docs/changelog.md` 新条目、`docs/build/build.md` 若 profile capability 变化。
- [ ] **Step 6: Commit** `docs: record aarch64 product userspace closure`
