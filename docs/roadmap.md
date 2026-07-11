# OS01 优化路线图 v5

> **基准**: `c03a36a` (test: fix 13 failing syscall tests)
> **日期**: 2026-07-11
> **来源**: v4 路线图 + ext2/tmpfs/GPT 文件系统升级

标记: ✅ 已完成 | 🔴 P0 本周 | 🟡 P1 本月 | 🟢 P2 下月 | 🔵 P3 远期

---

## 自 v4 以来的进展 (2026-07-11)

| 项目 | 说明 |
|------|------|
| **GPT 分区表** | `gpt_scan()` — 解析 GPT header + partition entries + CRC32 校验，创建 partition block device wrapper |
| **分区块设备** | `block_device_register_raw()` — 不绑 AHCI hook 的注册路径；partition wrapper 对 LBA 加偏移、检查越界 |
| **/dev 块设备** | `devfs_register_blkdev()` + `devfs_read/devfs_write` blkdev dispatch |
| **ext2 只读驱动** | ~370 行 `ext2.c`：superblock → bgdesc → inode table → direct + single indirect → VFS read/readdir |
| **tmpfs** | ~430 行 `tmpfs.c`：4KB 页链表 + 子节点数组，完整 VFS ops (read/write/readdir/create/mkdir/…)，挂载 `/tmp` |
| **VFS 大小写敏感** | `vfs_ops_t.flags` + `VFS_OPS_CASE_INSENSITIVE` — FAT32 不敏感，ext2/tmpfs/devfs/procfs 敏感 |
| **`vfs_dirent_t.ino` uint64** | tmpfs 存储内核指针需要 64 位 inode 号 |
| **disk.img 双分区** | `tools/mkdisk.c` — GPT + FAT32 ESP (64MB) + ext2 root (128MB) + backup GPT |
| **/boot 挂载** | FAT32 ESP 挂载 `/boot`，ext2 root 挂载 `/` |
| **fat32_mount → fat32_init** | 驱动只做 init，调用方负责 vfs_mount — ext2 风格一致 |
| **selftest 恢复** | `selftest_register()` 注册 10 测试，10/10 PASS |
| **systest 修复** | PTY→file serial runner，ext2 readdir 填 i_size，写操作测试迁移 `/tmp` — **70/70 PASS** |
| **init 路径迁移** | `/init.elf` → `/bin/init`，`/systest.elf` → `/bin/systest`，`/busybox.elf` → `/bin/busybox` |

---

## Phase 1: COW + 内存管理升级

> **全部完成。** COW fork 是 v3 路线图 P0 的最大单项，现已就绪。

### 1.1 Copy-On-Write Fork

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | 4KB 页面支持 | `alloc_4k_page` / `free_4k_page` + `vmm_map_4k_page` / `vmm_unmap_4k_page` + `vmm_pt_walk` |
| ✅ | `fork_mm_copy` → COW | 4KB PTE 循环：PATH_COW-before-PAGE_R_W 检查，可写页→双方 R/O+COW，只读页→直接共享 |
| ✅ | COW fault handler | `do_page_fault` COW 分支：refs>1→alloc+copy+put，refs==1→in-place promote + put |
| ✅ | COW 释放路径 | `vmm_unmap_4k_page` COW 感知 + `vma_free_all` 递减 cow_count |
| ✅ | `do_mprotect` COW 适配 | PROT_NONE stash COW、R/W restore 查 refcount、PROTNONE restore 保留 COW |
| ✅ | `page_cow_get/put/refs` | `subpage_lock` 保护，`subpage_pool.cow_count[512]` 追踪 |
| ✅ | 集成测试 | `test_cow.elf` 5/5 PASS；`systest.elf` 70/70 无回归；ash fork+exec 冒烟通过 |

### 1.2 mmap/mprotect (COW 前置)

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | `SYS_mmap` | 匿名映射 (MAP_ANONYMOUS) + 文件映射 (fd≥0)，VMA 管理 |
| ✅ | `SYS_mprotect` | 页权限修改，COW 感知 PTE 更新 |
| ✅ | `SYS_munmap` | VMA 移除 + 物理页释放 |
| ✅ | demand paging | `do_page_fault` P=0 → VM_ANON alloc + VM_FILE vfs_read |
| ✅ | libc wrapper | `mmap`/`mprotect`/`munmap` syscall wrapper + `sys/mman.h` |

### 1.3 暂不实现

| 项目 | 理由 |
|------|------|
| 2MB COW (大页拆分) | 2MB 区段 (ELF/栈) 无 VMA 覆盖 → COW fault 前被 vma_find=NULL 杀；alloc_pages 页不在 subpage_pool → split 后 free_4k_page 静默 no-op。4KB COW 已捕获 fork+exec 主要收益。 |
| Slab OOM fallback | 低优先级 — 当前 alloc_pages 失败已 panic，非静默错误 |
| Kernel heap frag 统计 | 诊断工具，P2 延后 |

---

## Phase 2: 质量基础设施

> 大部分在 v3 已完成。剩余：日志级别、Mock 框架。

### 2.1 测试体系

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | 单元测试 + selftest | `test/` 9 套件 + selftest 10/10 PASS (ext2 structs, tmpfs mount, GPT CRC32, slab, vfs, procfs, spinlock, pipe) |
| ✅ | 系统测试 (syscall 级) | systest.elf **70/70 PASS**，覆盖 43 syscall |
| 🟡 P1 | Mock 框架完善 | host 编译测试非 arch 内核代码 (Tilck `mocking.h` 模式) |

### 2.2 调试日志

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | DEBUG channel 机制 | `debug_<channel>()` 宏 + `DEBUG_CHANNELS=` 运行时开关 |
| 🟡 P1 | 日志级别 (ERR/WARN/INFO/DEBUG) | 替代无差别 `printk`，编译时可屏蔽低优先级 |
| ✅ | stacktrace 符号解析 | `kallsyms` + `backtrace()` 自动符号名+偏移 |
| ✅ | Hang detector | 500ms watchdog per CPU → dump 所有 task 状态 |
| ✅ | 内核级 strace | `DEBUG_CHANNELS=syscall` — pid + 参数 + 返回值 |

---

## Phase 3: 调度器升级 🟡

> 当前: O(n) 链表扫描 + max-counter 优先级。目标: O(log n) AVL 树 + vruntime 公平调度。

### 3.1 EEVDF 公平调度器 [借鉴 Tilck]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | AVL/rbtree 可运行队列 | 替代 O(n) 扫描 |
| 🟡 P1 | vruntime 累积 + min_vruntime | 子刻度精度 (`VRUNTIME_SCALE=16`) |
| 🟡 P1 | eligibility + deadline 选择 | `vruntime ≤ avg_vruntime` 且 deadline 最早 |
| 🟡 P2 | SMP 负载均衡 | CPU affinity → work stealing / push-pull |
| 🟡 P2 | SMP IRQ4 验证 | v1 遗留 — smp=2 时 IRQ 交付路径验证 |

---

## Phase 4: 文件系统

### 4.1 文件系统布局 ✅ 已完成

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | GPT 分区表扫描 | `gpt_scan()` + CRC32 + partition block device wrapper |
| ✅ | ext2 只读驱动 | ~370 行：superblock→bgdesc→inode table→direct+single indirect→VFS |
| ✅ | tmpfs 内存文件系统 | ~430 行：4KB 页链表，完整 VFS ops，`/tmp` 挂载 |
| ✅ | /dev 块设备 | `devfs_register_blkdev()` — /dev/hda, /dev/hda1, /dev/hda2 |
| ✅ | disk.img GPT 双分区 | `tools/mkdisk.c` — ESP FAT32 (64MB, `/boot`) + ext2 root (128MB, `/`) |
| ✅ | VFS per-fs 大小写 | `vfs_ops_t.flags` — FAT32 case-insensitive, ext2/tmpfs case-sensitive |
| ✅ | `fat32_mount` → `fat32_init` | 驱动只做 init，调用方 vfs_mount |
| 🟡 P2 | ext2 读写驱动 | inode 分配/释放、block bitmap、目录操作 |

### 4.2 /proc 完善

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | `/proc/<pid>/fd/` | 查看进程打开的文件描述符 |
| 🟡 P1 | `/proc/<pid>/maps` | 进程内存映射信息 |

### 4.3 缺失的 syscall

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | `readlink`/`symlink` | ext2 软硬链接的前置条件 |
| 🟡 P1 | `poll`/`select` | 多路 I/O，解锁 busybox 网络相关 applet |
| 🟡 P2 | `setitimer`/`alarm` | 用户态定时器 |

---

## Phase 5: 网络栈 🟡→🟢

### 5.1 lwIP 集成 [借鉴 cavOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P1 | lwIP 移植到内核 | cavOS 的开箱即用方案 |
| 🟡 P1 | NIC 驱动 (E1000) | QEMU 最广泛支持的虚拟网卡 |
| 🟡 P1 | socket syscall 层 | `UserSocket` + `lwip_send`/`lwip_recv` 封装 |
| 🟡 P2 | AF_UNIX sockets | 本地 IPC |

---

## Phase 6: 内核加固 🟡

| 优先级 | 任务 | 说明 |
|--------|------|------|
| ✅ | 内核栈 canary | `-fstack-protector-strong` + `__stack_chk_guard` + `__stack_chk_fail` |
| 🟡 P1 | 用户栈 canary | 用户程序编译时启用 SSP |
| 🟡 P2 | ASLR 基础 | 随机化用户程序加载基址 |
| 🟡 P2 | rwlock / seqlock | 读多写少场景 (VFS lookup, /proc read) 优化 |
| 🟢 P3 | UBSan 集成 | ArvernOS 模式：`UBSAN=1` debug build 选项 |

---

## Phase 7: 用户态生态 🟡→🔵

### 7.1 更多 busybox applet

| 优先级 | 任务 | 阻塞项 |
|--------|------|--------|
| 🟡 P1 | grep/sed | regex (regcomp/regexec) — 需从 musl 移植或手写 |
| 🟡 P1 | find | glob/fnmatch |
| 🟡 P2 | awk | regex + 浮点数支持 |
| 🟡 P2 | vi | 完整 TTY termios + signal (SIGWINCH) |

### 7.2 动态链接 [借鉴 cavOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🟡 P2 | ELF64 动态链接器 | 共享 musl libc，减小二进制体积 |
| 🔵 P3 | Alpine apk 用户态 | cavOS 终极方案 — 直接安装 Alpine Linux musl 二进制包 |

### 7.3 GUI 框架 [借鉴 opuntiaOS + HackOS]

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 🔵 P3 | 可缩放字体渲染器 | HackOS 字体方案 |
| 🔵 P3 | Window Server + Compositor | opuntiaOS 架构 |
| 🔵 P3 | LibG / LibUI | 独立图形库 + UI 控件库 |

---

## Phase 8: 多架构 + 构建 🔵

| 优先级 | 任务 | 借鉴 | 说明 |
|--------|------|------|------|
| 🟢 P2 | 多架构抽象 (`arch/`) | ArvernOS | x86_64 → aarch64 |
| 🔵 P3 | 真机测试 (USB boot) | Tilck | "不能在真机上测试就别实现" |
| 🔵 P3 | clang-tidy / coverity | — | 静态分析 CI 集成 |
| 🔵 P3 | GDB 脚本增强 | Tilck | `tasklist`、`handles`、`vfs` helper |

---

## 实施优先级总览

```
P0 (本周):
  - (无 — 已全部完成)

P1 (本月):
 1. EEVDF 调度器              — O(n)→O(log n)，公平调度
 2. poll/select               — 多路 I/O，解锁 busybox
 3. SMP 负载均衡              — 多核利用
 4. readlink/symlink          — ext2 前置
 5. /proc/<pid>/fd/ + /proc/<pid>/maps
 6. 日志级别 (ERR/WARN/INFO/DEBUG) — 调试效率质变
 7. lwIP 网络栈 + E1000       — 第一个网络能力
 8. 用户栈 canary             — 用户态加固

P2 (下月):
 9. ext2 读写
10. rwlock                    — 读多写少优化
11. 用户栈 canary
12. 更多 busybox applet (grep/sed/find)
13. 动态链接器

P3 (远期):
14. ASLR
15. 多架构 (aarch64)
16. UBSan
17. GUI 框架
18. Alpine apk 用户态
```

---

## 关键设计决策

| # | 决策 | 选择 | 理由 |
|----|------|------|------|
| 1 | COW 粒度 | 4KB-only V1（2MB 推迟） | 2MB 区段无 VMA 覆盖 + split 子页不在 subpage_pool → 风险过高；4KB COW 已捕获 fork+exec 主要收益 |
| 2 | COW 引用计数 | `subpage_pool.cow_count[512]` | 4KB 页来自 alloc_4k_page → 在 pool 中；无需改 struct Page |
| 3 | 调度器 | EEVDF (Tilck 路线) | Linux 6.6+ 生产级算法 |
| 4 | 网络栈 | lwIP (cavOS 路线) | 快速获得 TCP/IP，后期可替换自研模块 |
| 5 | 文件系统 | FAT32 + ext2 (只读) + tmpfs + devfs + procfs | ext2 只读 370 行，UNIX 权限/inode 完整；tmpfs 430 行，供 /tmp 可写存储 |
| 6 | 测试 | Tilck 3 层 (unit+self+sys) | 经过验证的模式 |
| 7 | 用户态 | busybox → 动态链接 → Alpine apk | cavOS 已验证可行 |
| 8 | COW PTE 标记 | `PAGE_COW` (bit 10) | bit 9 已被 `PAGE_PROTNONE` 占用 |
| 9 | COW 并发保护 | `subpage_lock` (已有 spinlock) | 保护 cow_count RMW + alloc/free 位图，无需新增锁 |
| 10 | 内核栈 canary | 全局 `__stack_chk_guard` (rdtsc 种子) + `-fstack-protector-strong` | clang 在 `-ffreestanding -fpie` 下生成 RIP-relative 全局引用，无需 TLS/FS |
| 11 | 磁盘布局 | GPT 双分区 (FAT32 ESP + ext2 root) | UEFI 标准分区表，`/boot` 和 `/` 分离，内核自解析 GPT |

---

## v4→v5 变更摘要

| 项目 | v4 状态 | v5 状态 |
|------|---------|---------|
| ext2 只读驱动 | 🟡 P1 | ✅ |
| ext2 读写驱动 | 🟡 P2 | 🟡 P2 (未变) |
| GPT 分区 + /dev 块设备 | — | ✅ |
| tmpfs 内存文件系统 | — | ✅ |
| disk.img GPT 双分区 | — | ✅ |
| fat32_mount → fat32_init | — | ✅ |
| VFS per-fs 大小写敏感 | — | ✅ |
| `vfs_dirent_t.ino` uint64 | — | ✅ |
| selftest 框架恢复 | — | ✅ (10/10) |
| systest 修复 (路径+写测试) | — | ✅ (70/70) |
| init 路径迁移 /init.elf→/bin/init | — | ✅ |
| `/boot` 挂载点 | — | ✅ |
| tools/mkdisk 构建工具 | — | ✅ |
| EEVDF 调度器 | 🟡 P1 | 🟡 P1 (未变) |
| 网络栈 | 🟡 P1 | 🟡 P1 (未变) |

---

## 已完成汇总 (截至 2026-07-11)

| 项目 | 工作量 | 日期 |
|------|--------|------|
| 信号 handler 用户态投递 | 2 天 | 07-05 |
| systest 系统测试 (70/70) | 1 天 | 07-05 |
| busybox ash `#` shell | — | 07-05 |
| 4KB 页面 + VMA + mmap/mprotect | 3 天 | 07-08 |
| **COW fork (4KB-only)** | 2 天 | 07-11 |
| VFS mount point getdents | 半天 | 07-11 |
| 内核栈 canary | 30 分钟 | 07-11 |
| SMP stack smashing 修复 (ext2 buf[256] 栈溢出) | 30 分钟 | 07-11 |
| **ext2 只读驱动 + GPT + tmpfs + /dev 块设备** | 1 天 | 07-11 |
| disk.img GPT 双分区 + tools/mkdisk | 1 天 | 07-11 |
| selftest 10/10 + systest 70/70 | 2 小时 | 07-11 |

## 下一个改进建议

### 🟡 P1: EEVDF 调度器

**借鉴**: Tilck EEVDF 实现 (`kernel/sched/`)
**工作量**: 3-5 天
**收益**: O(n)→O(log n)，公平调度，反饿死

### ✅ SMP stack smashing 修复 + 多核启动

**根因**: `ext2_read_inode` 的 `buf[256]` 写入 `fs->sectors_per_block * 512` (最大 4096) — 栈溢出覆写 canary → `*** Kernel stack smashing detected ***`
**修复**: `8d2f855` — `buf[256]` → `buf[4096]`
**验证**: ext2 栈溢出修复后，`make DEBUG=1` (smp=2) 不再触发 canary 错误；VFS 多核路径上的唯一已知栈溢出已消除

**收益**: 恢复 `-smp 2` 调试模式，多核 VFS I/O 安全得到验证

---

## 附录：开源 OS 项目分析摘要

> 完整分析见 `memory/opensource-os-analysis.md`。

| 项目 | 语言 | 平台 | 亮点 | 对 OS01 核心价值 |
|------|------|------|------|-----------------|
| **Tilck** | C | i686+riscv64 | EEVDF调度器、3层测试、hang detector | 调度器设计、测试体系 |
| **cavOS** | C | x86_64 | Alpine apk用户态、lwIP网络栈、内核strace | 网络栈、用户态终极方案 |
| **Aquila** | C | x86 | ext2 R/W (221行)、完整VFS | ext2 驱动范本 |
| **ArvernOS** | C | x86_64+aarch32/64 | 多架构抽象、模块化日志 | 多架构、debug日志 |
| **opuntiaOS** | C/C++/ObjC | x86+ARMv7+ARM64 | Window Server GUI、Compositor | GUI 架构远期参考 |
| **HackOS** | C | x86_64 | 可缩放字体、VESA图形 | 字体渲染参考 |

### 各项目技术栈对照

| 特性 | OS01 | Tilck | cavOS | Aquila | ArvernOS |
|------|------|-------|-------|--------|----------|
| 调度器 | RR+优先级 | EEVDF | 抢占式 | 多线程 | 抢占式 |
| 文件系统 | ext2, FAT32, devfs, procfs, tmpfs | ramfs, devfs, FAT32, sysfs | FAT32, ext2, /proc, /sys, /dev | ext2, tmpfs, devfs, procfs, devpts | — |
| 网络 | ❌ | ❌ | lwIP TCP/IP | socket骨架 | 自研 ARP→UDP |
| COW | ✅ (4KB) | ✅ | ✅ | — | — |
| mmap | ✅ | ✅ | ✅ | ✅ | — |
| 测试 | unit+self+sys | gtest+self+sys | — | — | 单元测试 |
| 用户态 | busybox(10) | busybox+vim+tcc | Alpine apk | aqbox+tcc+lua | homemade |
| Syscall | ~50 (+ Linux ABI) | ~100 | ~200+ | POSIX | homemade |
