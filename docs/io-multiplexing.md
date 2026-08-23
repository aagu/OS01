# I/O 多路复用 — poll / select / pselect

> 本文档归集 poll/select/pselect 系统调用的实施总结。相关阻塞语义（pipe、tty、devfs）见各自子系统文档。

---

## select/pselect 实施总结

### 架构

```
              libc
              ┌──────────────────────────────┐
              │ select(nfds, r, w, e, tv)     │
              │ pselect(nfds, r, w, e, ts, m) │
              └──────────────────────────────┘
                       │ SYS_select=50 / SYS_pselect6=51
                       ▼
              kernel/fs/select.c
              ┌─────────────────────────────────────────┐
              │ do_select(nfds, r, w, e, tv)            │
              │   → fd_set → pollfd[nfds]               │
              │   → do_poll_core(pollfd, nfds, ms, &pt) │
              │   → revents → fd_set                    │
              │                                         │
              │ do_pselect6(nfds, r, w, e, ts, packed)  │
              │   → 解包 sigmask + swap + restore        │
              │   → 同上                                 │
              └─────────────────────────────────────────┘
                       │
                       ▼
              kernel/fs/poll.c
              ┌─────────────────────────────────────────┐
              │          do_poll_core() (共享)           │
              │ do_poll() = copy + do_poll_core + copy  │
              └─────────────────────────────────────────┘
```

- **poll_table_t 动态化**: `entries[16]` → `*entries + max_entries`，支持 select 的 1024 fd
- **do_poll_core 共享**: poll 和 select/pselect 共享轮询循环，接收内核侧 pollfd[]，不碰用户内存
- **do_select_common 去重**: select/pselect 共享 fd_set↔pollfd 转换 + 反向映射 + 写回逻辑
- **pselect6 sigmask 原子性**: goto-out 模式，所有错误路径恢复 blocked
- **fd_set 128 字节**: `kernel_fd_set { uint64_t __bits[16] }` 匹配 libc `long __fds_bits[16]`

### 文件变更

| 类别 | 文件 | 行数 | 说明 |
|------|------|------|------|
| **修改** | `kernel/include/kernel/poll.h` | +24/−8 | poll_table_t 动态化 + do_poll_core 声明 |
| **修改** | `kernel/fs/poll.c` | +60/−25 | 提取 do_poll_core() + 包装器重构 |
| **新增** | `kernel/include/kernel/select.h` | 73 | kernel_fd_set、sigset_t、pselect6_sigmask、原型 |
| **新增** | `kernel/fs/select.c` | 423 | do_select、do_pselect6、do_select_common、do_select_nofds |
| **修改** | `kernel/arch/x86_64/trap.c` | +15 | SYS_select + SYS_pselect6 dispatch + 移除 sigset_t |
| **修改** | `kernel/include/uapi/syscall.h` | +1 | SYS_pselect6=51 |
| **新增** | `test/include/kernel/select.h` | 73 | 镜像 |
| **修改** | `test/include/kernel/poll.h` | 镜像 | 同步动态化 |
| **新增** | `libc/include/sys/select.h` | 59 | FD_ZERO/SET/CLR/ISSET + select/pselect 声明 |
| **新增** | `libc/unistd/select.c` | 52 | libc wrapper + pselect6 打包 |
| **修改** | `libc/include/sys/syscall.h` | +1 | SYS_pselect6 |
| **修改** | `user/systest.c` | +258 | 10 测试组 (20 个断言) |
| **修改** | `tests/run_test.py` | +3 | QEMU AHCI 驱动参数 |

**总计: 11 commits, 14 files, +1042 / −33**

### systest 结果: 126/126 passed (poll 8/8 + select 20/20)

---

## 后续相关项（规划中，见 roadmap P5）

- requested-event-aware poll/select 注册（按请求方向唤醒，修复复合 flags + PTY 双注册容量）
- per-poll timeout registry（修复 lost-wakeup + 并发 clobber）
- poll 注册方向语义：`requested & legal & unavailable` 才注册；readiness 只由 open mode 决定（见 `docs/decisions.md` #38/#39）
