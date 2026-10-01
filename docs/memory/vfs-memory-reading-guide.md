# VFS + 内存管理源码导读（fs/ + memory/，两个子系统联动读）

> `docs/filesystem.md` + `docs/memory.md` + `docs/cow-mmap.md` 是参考手册；本文件是阅读路线。VFS 和 VMA 强耦合（mmap 缺页、文件映射、COW 都横跨两者），所以放一份导读。

## 文件清单（按体量）

```
kernel/fs/     ext2.c 1938 | vfs.c 1292 | fat.c 1270 | file.c 1124 | procfs.c 578
               poll.c 551 | devfs.c 534 | select.c 503 | tmpfs.c 463 | gpt.c 219 | elf.c 183
kernel/memory/ pmm.c 579 | vma.c 562 | slab.c 412 | vmm.c 285 | pmm_arch.c 165 | uaccess.c 121 | tlb.c | dump.c
```

## vfs.c 全景（1292 行）

```
13-40     静态状态（mount 列表）+ vfs_init
43-130    vfs_mount() — 挂载点注册
119-247   ★ normalize_vfs_path (L131) — 路径归一化（. / .. // 折叠）
248-740   ★ 路径解析引擎：
          __vfs_lookup_raw (L248) → splice_symlink_path (L482)
          → resolve_at (L549) → vfs_lookup_resolved (L595)
          → vfs_lookup_at (L740 对外入口)
          symlink 采用"中途命中提前返回"协议（L231 注释：返回 0=终点，1=命中 symlink 带剩余路径）
755-860   vfs_read / vfs_write / readdir / node_put / stat — 节点操作薄封装
886-1000  vfs_getdents（linux_dirent64 格式，busybox ls 依赖）
1077-1260 unlink / mkdir / rmdir / rename / truncate — 目录操作
```

## memory/ 三层结构

```
pmm.c    物理页分配器：pmm_init (L140, boot_context 驱动) → alloc_4k_page (L503)
         → free_pages (L410) → page_cow_get (L462, COW 引用计数)
         └─ arch 弱覆盖：pmm_arch.c (x86_64 强实现) + RAM-relative indexing（9 月修复系列）
vmm.c    页表原语：vmm_map_page (L29) / vmm_unmap_page (L53)
         四级遍历 get_next_level，PGD/PUD/PMD 层级（Linux/ARM 命名，9 月统一）
vma.c    进程地址空间：vma_insert/remove/free_all + mmap/mprotect 语义
         ★ user_leaf_pte (L511) + user_write_range_begin/end (L536/558)
         —— uaccess 写前把目标页临时提权，读驱动/ioctl 代码时见到这对就是它
slab.c   内核对象缓存，task_struct/vma_t 等
```

## 推荐阅读顺序（3 遍法）

**第 1 遍·一次文件 open() 的完整旅程**：
`user/open()` → syscall (trap.c case SYS_open L1452) → `vfs_lookup_at` (L740) → normalize → 逐组件 walk → FS 后端（fat.c L675 read / ext2.c）→ `file.c` fd 表分配。
读完能回答"路径里的 symlink 和 .. 是在哪一层被消掉的"。

**第 2 遍·一次 mmap 写的旅程**：
`test_mmap.c` → vma.c 建映射 → 访问触发 do_page_fault (trap.c L435) → vma_find → 分配物理页 (pmm.c) → vmm_map_page (vmm.c L29) → 返回重执行。
这条线串起两个子系统，是全 OS 最有价值的一条走读线。

**第 3 遍·选读**：COW fork（page_cow_get + sched 侧配合 docs/cow-mmap.md）、ext2 块分配自测组（ext2_selftest_* L1613-1855）、poll/select 实现（io-multiplexing.md 配套）。

## 已知坑

- vfs.c symlink 解析是迭代+提前返回协议，不是递归归一化——改路径解析前先读 L231-247 注释
- `vmm_map_page` 改 kernel_map 时多核要同步（L48 注释），TLB shootdown 见 docs/smp.md
- pmm 的 bits_map 索引是 RAM-relative 不是物理 PFN（`0ba888a`/`0809100` 修复），读 alloc/free 代码时别按老习惯换算
- ext2.c 里散布 `ext2_symlink_test_fault` 注入点（L25），是 fault-injection 测试钩子不是死代码

## 自测问题

1. devfs/procfs/tmpfs 没有块设备，vfs_mount 传的 dev 参数是什么？读目录时数据从哪来？
2. 两个进程 mmap 同一文件私有+只读页，各写一下，物理页怎么变化？涉及几次缺页？
3. vfs_getdents 为什么用 linux_dirent64 布局而不是自定 dirent？如果不兼容 busybox 会炸哪个 applet？
