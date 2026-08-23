# GUI 与图形子系统

> 本文档归集图形/终端/游戏相关工作的实施总结与后续计划。
> 范围：fb 像素渲染、VT100 终端、alt-screen 协议、Tetris 游戏（已完成），以及 P3 GUI 路线图（规划中）。

---

## Tetris 游戏实施总结（已完成，2026-08-16）

> 状态：**已完成**。提交 `79f1179`（framebuffer + alt-screen 协议）、`d8e5c05`（UX：慢速重力 + 种子 RNG + 消行闪烁）、`ae0cc04`（消行白残留修复）。QEMU 手工 `exec /bin/tetris` 可玩，退出终端内容恢复。

### 目标

OS01 上可玩的俄罗斯方块：用户态 `user/tetris.c`，**手工启动**（不进 inittab），fb 像素渲染 + `/dev/keyboard` 原始扫描码输入，退出后终端内容自动恢复（Linux alt screen 协议）。

**明确不做**：serial 渲染后端（38400 baud 下增量重绘虽可行，但优先级低，已讨论砍掉，收敛范围）。

### 架构决策（已确认）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 31 | tetris 输入 | `/dev/keyboard` 原始扫描码（E0 48/4B/4D/50 方向键） | tty TCSETS 是 no-op，raw 模式改造不值；扫描码自解析 E0 前缀 + release 位 |
| 32 | tetris 渲染 | fb mmap 像素块（20×10 网格 × 30px = 600×300） | fb_mmap 是 per-VMA，多进程共享；FBIOSURRENDER 只停内核 console 不排他 |
| 33 | 终端恢复协议 | Linux alt screen：`\e[?1049h` / `\e[?1049l` | 标准化（vim/less 可复用）；terminal.elf 零轮询零 waitpid |
| 34 | terminal.elf 双缓冲 | offscreen 主缓冲 + alt 缓冲 | 退出后内容 100% 一致、恢复零重绘成本 |
| 35 | 游戏生命周期 | 手工 `exec /bin/tetris` | 不进 inittab |
| 36 | serial 后端 | ❌ 不并入 | 增量 ANSI 虽可行（38400 baud ≈ 38 帧/s 上限），但串口只读单字节无 poll，投入产出比低 |

### 关键现状事实（已核实）

- `/dev/keyboard` ring 存**含 0xE0 前缀的原始 PS/2 流**（release 位 0x80 也入 ring）；`keyboard_devfs_read` **非阻塞**、`keyboard_ops` **无 .poll** → devfs_poll 无回调时默认 always-ready（poll 空转）
- `devfs_ops` 含 `.poll` 槽位；poll 基建（poll_table/poll_wait/fd_poll）完备
- `tty_read` 四阶段阻塞协议（drain → 非空返回 → 挂 read_wait 双检 → schedule）是 keyboard 改造的现成范本
- tty termios **假**：TCGETS 硬编码谎报 `ICANON|ECHO|ISIG`，TCSETS **no-op**；实际 raw 搬运（行编辑靠用户态 busybox ash FEATURE_EDITING）
- PTY termios **真存储但语义未实现**（pipe 读路径不看 c_lflag）——本次范围外
- fb_mmap per-VMA、FBIOSURRENDER 非排他；terminal.elf 无离屏缓冲、VT100 无 `?1049h/l`
- 方向键扫描码（Set 1 + E0）：UP=E0 48、LEFT=E0 4B、RIGHT=E0 4D、DOWN=E0 50
- `Makefile` 无头文件依赖 → **改 tty_t 结构体必须 `make clean`**（AGENTS.md）

### 分步实现

| Step | Commit 主题 | 文件 | 内容 | 验证 |
|------|------------|------|------|------|
| **1** | `feat(driver): keyboard poll support`（~31 行） | `kernel/driver/keyboard.c` + `kernel/kernel/main.c` | scancode wait queue（spinlock+list）；`keyboard_handler` push ring 后 wake；`keyboard_poll_dev()`：ring 非空→POLLIN，否则 poll_wait；`keyboard_ops.poll` 挂上（main.c 1 行） | `make clean && make`；QEMU 内测试程序 `poll(/dev/keyboard)` 阻塞等键立即返回 |
| **2** | `fix(tty): make termios honest`（~60 行） | `kernel/include/kernel/tty.h` + `kernel/tty/tty.c` | tty_t 加 `struct termios term`；默认 raw（`c_lflag=0, ICRNL, OPOST\|ONLCR, VMIN=1`）；TCGETS 返回真值 / TCSETS 真存储；tty_read 尊重 ICANON（攒行等 `\n`）+ ECHO 回显。**不做**：ISIG/pgrp（TODO）、OPOST 输出转换（无消费者） | `make clean && make`（结构体变更！）；QEMU 回归 terminal/ash；小程序 TCSETS 切换 raw/canonical 验证行为差异 |
| **3** | `feat(terminal): alt screen double buffer`（~80 行） | `user/terminal.c` | offscreen 主缓冲 + alt 缓冲；put_glyph 写当前缓冲；CSI 解析器加 `?1049h`（保存+清屏+切 alt）/ `?1049l`（切回主缓冲全量重绘） | 测试程序发 `\e[?1049h` 画图 → `\e[?1049l`，原终端内容完整恢复 |
| **4** | `feat(applets): tetris game`（~400 行） | `user/tetris.c`（新）+ `Makefile` | 游戏逻辑（7 Tetromino、4 旋转、碰撞、消行、计分、等级加速）；输入解析（E0 前缀 + release 位 → 归一化 K_LEFT/RIGHT/DOWN/UP/ROTATE/DROP）；渲染 fb 像素块 + **20×10 逻辑屏脏矩形 diff**（防闪烁）；主循环 `poll(/dev/keyboard, 500ms 超时=下落 tick)`；`\e[?1049h` 进入 / `\e[?1049l` 退出 | QEMU 手工 `exec /bin/tetris` 可玩 |
| **5** | 集成验证（无代码） | — | 全量 `make clean && make`；启动 → terminal/ash 正常 → 玩 tetris → 退出终端恢复；systest 回归 | QEMU 实跑 |

### 实施顺序与依赖

```
Step 1 (keyboard poll) ──┐
                          ├──→ Step 4 (tetris 游戏，依赖 1 的 poll + 3 的 alt screen)
Step 2 (tty termios)  ──┤    Step 4 内部：游戏逻辑可先于渲染写（无依赖）
                          │
Step 3 (terminal 双缓冲) ─┘

Step 5 (集成验证) ──→ 所有 Step 完成后
```

- Step 1/2/3 **互不依赖**，可并行推进；Step 4 依赖 1+3
- Step 1+2 为**同一批内核修改**，但**两个独立 commit**（主题不同：driver vs tty）
- 每步独立验证，**通了再报进展**（QEMU 实证，非静态分析）

### 风险 / 注意

| 风险 | 缓解 |
|------|------|
| tty_t 结构体变更（sizeof 变化） | Step 2 必须 `make clean && make`，旧 .o 会静默崩 |
| keyboard ring 满丢键（RING 256，连按风险） | 一般够用；游戏输入事件率低；如丢键再加 ring 大小 |
| poll 唤醒丢失 | 参照 tty_wake_waiters 双检模式 + `this_cpu()->need_resched=1` |
| 主循环被信号中断 | poll/nanosleep EINTR 处理（`errno==EINTR → continue`） |
| 脏矩形 diff 复杂度 | V1 每 tick 全量 diff（20×10 数组比较，~200 次 memcmp，开销可忽略） |

### 完成后（可选项，不阻塞）

- tty ISIG/pgrp 真实现（Step 2 留的 TODO）→ 内核层 Ctrl-C 信号
- serial 后端 `tetris -serial`（增量 ANSI，QEMU -nographic 直玩）

---

## P3 GUI 路线图（规划中）

已有基座：fb ✅、fb mmap ✅、terminal 双缓冲 + alt-screen ✅、键盘扫描码 ✅

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| PS/2 鼠标驱动 | `/dev/mouse`，扩展 keyboard.c 的 PS/2 协议处理 | 独立 | |
| 2D 图形 API | fb 之上画线/矩形/位图 blit | 独立 | |
| 可缩放字体渲染器 | 矢量/位图缩放 | 2D API | HackOS |
| Window Server + compositor | 多窗口管理 + 合成（原 P3#15） | 字体/2D/鼠标 | opuntiaOS + HackOS |

### 依赖链

```
P3: fb ✅ → 2D API → 字体 → Window Server；PS/2 鼠标并行
```
