# parked T6：PTY termios 读写同步（2026-10-24）

## 根因与修复

roadmap 的分辨率切换器 follow-up 记录了 `TCSETS` 写 `pty->term`
未持 `state_lock`。调查确认它是实际缺锁，且只修写端不足以解决问题：
主、从端的 `TCGETS` 原先都把共享 `pty->term` 直接传给用户拷贝，
可能在并发设置时返回来自不同代的 flags、控制字符和 speed 字段。

本次修改：

- `kernel/tty/pty.c`：`TCSETS` / `TCSETSW` 先完成用户范围检查和
  `copy_from_user_ft()`，再在 `state_lock` 下提交完整 termios。
- 主、从端 `TCGETS` 共用一个静态 helper：在同一把锁下复制到本地
  `kterm`，释放锁后才 `copy_to_user_ft()`。使用完整大小的 `memcpy`，
  确保用户拷贝包含的结构体 padding 也来自被保护对象，而非本地栈残留。
- `kernel/fs/file.c` 的 weak master fallback 也按相同顺序读取快照，
  避免强实现不参与链接时保留不安全的读取路径。
- `pty.h` 仅补充锁契约注释，未更改字段、结构体布局或 UAPI。

初始化路径在 PTY 对外发布前设置默认 termios，不需要额外取 state lock。
本次不扩展 ioctl 支持集合，不实现 `TCSETSW` 的输出排空语义，也不处理
同一 T6 列表的匿名 pgrp 联合体或 SIGWINCH pgrp 判断。

## 可复现证据

扩展已有 `hosttests/cases/test_pty_winsize.c`，通过真实 `fd_ioctl()`
分发和生产 `pty.c` / `file.c` 测试，不以 mock 替代 ioctl 实现。
已有 pthread 安全锁替身增加可选边界观察钩子，其他测试不启用观察。

新增三个场景：

1. 对 `TCSETS` 和 `TCSETSW` 各验证一次提交：acquire 时仍是旧值，
   release 时已是新值，实际 `state_lock` acquire/release 各一次。
   同时检查用户拷贝不在锁内、返回时锁已释放。
2. 主、从端分别读 `TCGETS`：用户拷贝先复制一半，再通过真实 slave
   `TCSETS` 更新所有 termios 字段，最后复制剩余部分。旧实现返回
   旧值前半与新值后半；修复后返回完整旧快照，PTY 内保存完整新设置。
   字段比较包含 flags、line、全部 NCCS 控制字符和两个 speed，不依赖 padding。
3. 检查 NULL、范围校验失败、copy-from/copy-to fault：写失败不改状态，
   读取失败不遗留锁，faultable 用户拷贝始终位于锁外。

这是确定性的并发交错模拟，不声称已捕获一次自然发生的 guest SMP 撕裂。
先写回归、保留旧生产实现时：11 个场景中 2 个失败，149 条断言中
18 条失败（写提交场景 8 条，读快照场景 10 条）。修复后全部通过。

为单独验证 weak fallback，在 `/tmp` 副本中只重命名强 master ioctl，
使原 fixture 的主端调用实际落到 `file.c` weak 实现：旧 weak 版本
149 条断言中 5 条失败；修复后 149/149 通过。没有改工作区链接规则。
脚本：`/tmp/os01-parked-pty-termios-weak-check.py`。

## 验证

- `make PROFILE=x86_64-clang test-resolution-host RES_CASE=pty`：
  11 个场景、149 条断言全部通过。
- `make PROFILE=x86_64-clang test-resolution-host`：完整九组及 profile
  contract 检查通过。
- `make PROFILE=x86_64-clang test-host`：退出 0，完整宿主二进制、PMM
  reservation、kernel-selftest runner（43 项）和 user build（7 项）通过。
- `make PROFILE=x86_64-clang test-qemu SUITE=phase-0`：退出 0，内核启动
  标记与 BusyBox ash 提示符验证通过，覆盖真实 PTY 初始化及 shell 启动。
  不将该启动检查描述为 guest 并发 termios 压力测试。
- 最后将 getter 改为完整大小 `memcpy` 后，重新执行 PTY 聚焦回归、
  weak fallback 对照、完整 `test-host` 及 QEMU phase-0，全部通过。
- `git diff --check` 通过。

红/绿日志：`/tmp/os01-parked-pty-termios-{red,green}.log`；
weak 路径：`/tmp/os01-parked-pty-termios-{old_weak,fixed_weak}.log`；
完整分辨率宿主日志：`/tmp/os01-parked-pty-termios-resolution.log`。
完整 host 与 QEMU 日志：`/tmp/os01-parked-pty-termios-host.log`、
`/tmp/os01-parked-pty-termios-phase0.log`。
