# Task 10 报告 — 回归、无设备证据与文档

日期：2026-09-27 ｜ worktree：`/home/aagu/OS01/.claude/worktrees/ps2-mouse`（base f13b34f）
执行者：Task 10 implementer（Sonnet）

## 1. 回归结果摘要

| 项 | 命令 / 方法 | 结果 |
|----|------------|------|
| host 测试 | `make PROFILE=x86_64-clang test-host` | **PASS**（31 suites / 0 failed，含 test_mouse* 全部用例；另 pmm_boot_reservation 8 例 PASS） |
| 静态审计 | `make PROFILE=x86_64-clang test-static` | **PASS**（runtime_audit / stack_canary / validate-kernel / runtime_link_order / kernel_runtime_link / kernel_layout / canary-contract / user-canary 7/7） |
| syscall-repeat | 见 §2（make 目标本身受既有 harness 缺陷阻塞） | **等价验证 2 次全 PASS** |
| 键盘 + /dev/keyboard | QEMU monitor `sendkey` 注入 + `cat /dev/keyboard &` | **PASS**（扫描码经 IRQ1→i8042→ring 可读；注入的 "hi x" 同时出现在控制台 TTY 提示符后 = TTY 出字） |
| /bin/mousetest ABI 自检 | headless 串口 shell 运行 `/bin/mousetest` | **PASS**（short read `EINVAL` / 空读 `EAGAIN` / 空队列 `poll(0)=0` 三项全过；Ctrl-C 可退出交互段回到 shell） |
| 鼠标事件注入 | QEMU monitor `mouse_move` / `mouse_button` | **PASS**（位移 (10,0)/(0,-5)/(-3,7) 精确复现，含 Y 方向；左/右/中三键 buttons=1/2/4 均触发并释放） |
| 无设备补充检查 | `-M q35,i8042=off`（同时禁 PS/2 键盘） | **PASS**（启动不挂、ash shell 可达；日志 `subsys: init  keyboard ... SKIP (optional, ret=-1)` 与 `subsys: init  mouse ... SKIP (optional, ret=-1)`；`/dev` 列表无 `mouse` 节点） |
| boot smoke（正常机） | `-M q35 -smp 4` + virtio-rng + e1000e | **PASS**（`subsys: init  keyboard ... ok`、`subsys: init  mouse ... ok`、`/dev/mouse` 与 `/dev/keyboard` 均为 crw------- 节点；~2 s 到 shell） |

无响应鼠标 500 ms 放弃 + IRQ/command-byte 回滚 + 无节点注册：引用
`.superpowers/sdd/2026-09-27-ps2-mouse-driver/task-7-report.md` 的 host mock
测试（fix round 2 后 248 focused assertions，覆盖 F5 有界尝试、回滚顺序与
`now <= 500000` 上限），本任务未重跑（属 Task 7 已闭环证据）。

## 2. 关于 `make test-syscall-repeat` 的重要发现（非鼠标回归）

`make PROFILE=x86_64-clang SMP=4 test-syscall-repeat` 以 stage -1 超时失败
（180 s 内未见 ash shell）——与 Task 5/8 记录的 post-DHCP 停滞完全一致，
且在 Task 5 前的 baseline 上即可复现（ledger 已 ruling）。本次进一步定位到
**根因**：

- 该 harness（`qemutests/x86_64_systest_repeat.py`）的 QEMU 命令**没有**
  `-device virtio-rng-pci`，而 `run` 目标有；
- 本机所有可用 OVMF（worktree 本地固件、retrage nightly 下载、Arch 系统
  edk2）在不挂 virtio-rng 时都不提供 UEFI GetRNG → 内核日志
  `CSPRNG: no hardware entropy source ... pool not ready, get_random_bytes fail-closed`；
- AAGU-5 之后 `setup_user_stack()` 的 `AT_RANDOM` 走 STRONG-only
  `kernel_random_get_strong()`，fail-closed 时 exec 直接失败 → init 永远
  起不来 → 停滞。给同一 harness 命令**仅加 virtio-rng-pci 一项**即可 2 s
  到 shell（已用三种固件分别验证）。
- 结论：这是 harness 缺设备的**既有仓库缺陷**（AAGU-5 落地时未同步更新
  该 harness），与鼠标驱动无关；受"代码改动只许文档"约束，本任务不改
  harness，建议 controller 单独派发一行修复。

**等价回归（2 次通过）**：`x86_64_systest_repeat.py` 的 /tmp 副本仅添加
`-object rng-random,... -device virtio-rng-pci`，`--smp 4`，固件
retrage nightly（与 `OVMF_FIRMWARE_SOURCE` 配置一致）：

```
RUN 1: SMP=4, stage=0: 1 suites passed / stage=1: 1 / stage=2: 3  → PASS
RUN 2: SMP=4, stage=0: 1 suites passed / stage=1: 1 / stage=2: 3  → PASS
```

（日志：/tmp/os01-systest-repeat-rng.log；脚本：/tmp/x86_64_systest_repeat_rng.py）

## 3. 文档变更（本任务唯一代码库改动）

- `docs/driver.md`：新增「i8042 共享控制器层」与「PS/2 鼠标驱动（/dev/mouse）」
  两节（API 表、探测时序上限表 400/460/20+20 ms ≤ 500 ms、回滚顺序、
  8 字节事件 ABI 字段表、事件 ring/poll 语义）；新增鼠标 devfs 注册小节；
  Phase 表补 mouse 行（Phase 6，可选）；文件清单补 i8042/mouse/mouse_proto
  及 uapi/mouse.h。
- `docs/roadmap.md`：P3 GUI 表删除「PS/2 鼠标驱动」行，移入基座已完成句。
- `docs/superpowers/specs/2026-09-26-ps2-mouse-driver-design.md`：状态改
  「已实现（2026-09-27）」；§8.2 按实际修订（monitor 注入可自动化移动/
  三键/Y 方向，滚轮保留人工）；§8.3 按实际修订（q35 无法单独禁鼠标保键盘
  → host mock 证明 500 ms 回滚 + 正常 QEMU 承担键盘保留证据 +
  `i8042=off` 补充启动检查），并记录 QEMU 环境限制。

`git diff --check` 干净；已按指定消息提交（仅 docs）。

## 4. 人工待办清单（需要人在可见 QEMU 窗口操作，本任务未验证）

环境：`make PROFILE=x86_64-clang SMP=4 run`（gtk 窗口；注意必须带
virtio-rng 的 run 目标，不要用 systest-repeat harness 的裸命令）。

1. **滚轮**：`/bin/mousetest` 运行中滚动滚轮，确认输出 `wheel=±1/±2` 且
   `buttons` 不变（HMP 无滚轮注入命令，无法自动化）。
2. **真实手感方向**：window 内划圈/斜向移动，确认 dx/dy 连续且方向与
   物理 movement 一致（monitor 注入已验证离散方向与 Y 取向，此项为
   体验性确认）。
3. **三键真实点击**：左/右/中键各自点击与组合按住（monitor 已验证单键
   状态位，真实点击序列/双击未覆盖）。
4. **Ctrl-C 退出**：mousetest 交互段按 Ctrl-C 应回 shell（headless 已
   验证串口 Ctrl-C；可见窗口下键盘 Ctrl-C 路径相同，顺手确认即可）。
5. 交互段 `return 1` 无法区分测试失败/操作员中断（Task 9 deferred 项），
   人工运行时以输出行内容为准，不以退出码为准。

## 5. 遗留 / 建议

- **BLOCKED 项**：`make test-syscall-repeat` 原样无法通过（harness 缺
  virtio-rng，见 §2）；建议单独派一行修复（`qemutests/x86_64_systest_repeat.py`
  cmd 列表加 `-object rng-random,filename=/dev/urandom,id=rng0
  -device virtio-rng-pci,rng=rng0`）后重跑两次原目标。其余全部验收项通过。
- brief 中「完成一次全分支代码评审」未在本任务执行：各 task 均有
  per-task review clean 记录（ledger），全分支 final review 是否补做请
  controller 裁决。
- Task 9 deferred 项（空队列 poll 检查固有竞态、交互循环 return 1）维持
  deferred，已在 §4.5 提示人工运行注意。
