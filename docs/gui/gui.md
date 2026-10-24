# GUI 与图形子系统

> 本文档归集图形/终端/游戏相关工作的实施总结与后续计划。
> 范围：fb 像素渲染、VT100 终端、alt-screen 协议、Tetris 游戏（已完成），P3 GUI「2D 图形 API」闭环（2026-09-30），以及后续路线图。

---

## 2D 图形 API（libgfx + /dev/gfx0，2026-09-30 闭环）

> 状态：**已完成**。提交 `f81f092`（per-file ioctl lifecycle）、`fa062aa`（bounded present 设备）、`1d74245`（libgfx 客户端生命周期）、`93a870b`（clipped 2D primitives）、`b555be9`（QEMU ring-3 E2E suite）、`feat(gfx): migrate Tetris and document API`（Task 6，Tetris 迁移 + 文档）。

### 目标

在用户态提供 `libgfx.a` 静态库，封装点 / 线 / 矩形 / 填充矩形 / 位图 blit；内核提供 `/dev/gfx0`，把用户像素缓冲按受控矩形拷贝到帧缓冲——应用不直接 mmap fb（避免暴露同页区外像素），由内核保证每个 handle 只能写到自己的视图矩形。

**Tetris 是首个用户**：从 `/dev/fb` mmap 改为 `gfx_open(0,0,w,h)`，分配私有像素缓冲 + 受限 present。

### 架构决策（已确认）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 公开 ABI 位置 | `kernel/include/uapi/gfx.h` 唯一一份 | libgfx 与 kernel build 同一头；无副本分歧 |
| 2 | 部署 | x86_64 上的 `libgfx.a` + `/dev/gfx0`；aarch64 暂无 fb 驱动，本阶段不出 | 隔离 aarch64 风险 |
| 3 | 绘制时机 | 用户私有缓冲绘制；`GFX_PRESENT` 才交给内核 | 页表按 4 KiB 授权，直接 mmap 任意 fb 矩形会暴露同页区外像素 |
| 4 | 坐标 | 创建视图用全屏坐标 `(x,y,w,h)`；primitive / clip / buffer 索引用视图局部坐标，左上为 `(0,0)` | 与 fb 设备坐标系解耦，窗口语义预留 |
| 5 | 格式 | 仅现有 32 bpp `GFX_FORMAT_RGB32=0`；行跨度 `w*4` 字节 | host-endian RGB32，与现有 fb 一致 |
| 6 | 呈现 | 每视图逐行拷贝 `w*h` 像素到 fb 对应矩形；不接受 partial present | QEMU stdvga 同步返回；不实现 vsync / page-flip / 硬件双缓冲 |
| 7 | 隔离 | 每视图只允许写到内核保存的矩形；本阶段任何进程仍可打开 `/dev/fb` 或创建重叠视图 | 不提供跨进程图形权限隔离（spec §2 明示） |
| 8 | 视图表 | 16 项全局表（`g_gfx_table`），spinlock 保护分配/释放；`dup`/`fork` 共享 `file_t` 引用计数 | 简单；超出需动态扩表 |
| 9 | 用户堆上限 | `USER_PAGE_SIZE` 从 2 MiB 升到 16 MiB（注：2026-10-02/04 进一步更名为 `USER_ENVELOPE_SIZE` 并扩至 512 MiB；像素缓冲分配已迁至匿名 mmap） | 1440×900 RGB32 = 5.18 MiB 像素缓冲超过旧上限 |
| 10 | heap VMA | `spawn_user_task` / `sys_exec` 初始插入 heap VMA（注：2026-10-02 用户堆与 ELF 隔离落地后，改由 `mm_init_user_heap` 插入零长度 `VM_HEAP`，由 `mm_set_brk` 动态管理已提交 4KB 页） | 之前没有 heap VMA，brk 扩展遇到页缺失直接 SIGSEGV |
| 11 | 用户栈位置 | `USER_STACK_BASE = 0x1400000`（受 `mm_user_range_protected` 保护） | 紧贴 heap 上限之上；stack guard 隐含在 `end_brk ≤ 0x13FF000` 与 stack base 之间 |
| 12 | Tetris 输入 | `/dev/keyboard` 原始扫描码（沿用） | tty TCSETS 是 no-op，raw 模式改造不值 |
| 13 | Tetris 渲染 | libgfx 全屏视图 `gfx_open(0,0,w,h)` + `gfx_fill_rect` + 每视觉事件一次 `gfx_present`（不按 cell 多次 present） | 与 spec §6 / plan §6 Task 6 契约一致；闪屏时只 present 一次 |
| 14 | FBIOSURRENDER 保留 | Tetris 在 `gfx_open` 之前 issue | 与迁移前一致；保持 kernel console 不踩像素 |

### 公有 ABI（`libgfx/gfx.h`）

```c
/* 视图创建 / 销毁 */
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void          gfx_close(gfx_handle_t *h);

/* 信息 / 裁剪（库本地，不走 ioctl） */
gfx_info_t    gfx_get_info(const gfx_handle_t *h);    /* 快照；不会失败 */
int           gfx_set_clip(gfx_handle_t *h, int32_t x, int32_t y,
                            uint32_t w, uint32_t h_);

/* 呈现（每帧一次 ioctl） */
int gfx_present(gfx_handle_t *h);                       /* 0 成功；-1 + errno */

/* primitive（static inline 转发到 libgfx.a 内部函数；不暴露像素指针） */
void gfx_pixel       (gfx_handle_t *h, int32_t x, int32_t y, uint32_t color);
void gfx_hline       (gfx_handle_t *h, int32_t x, int32_t y, uint32_t w, uint32_t color);
void gfx_vline       (gfx_handle_t *h, int32_t x, int32_t y, uint32_t h_, uint32_t color);
void gfx_rect        (gfx_handle_t *h, int32_t x, int32_t y, uint32_t w, uint32_t h_, uint32_t color);
void gfx_fill_rect   (gfx_handle_t *h, int32_t x, int32_t y, uint32_t w, uint32_t h_, uint32_t color);

/* Bresenham + sprite blit（库函数，签名见 `libgfx/gfx.h`） */
void gfx_line            (gfx_handle_t *h, int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1, uint32_t color);
void gfx_sprite_blit     (gfx_handle_t *h, int32_t dx, int32_t dy,
                         const uint32_t *src, uint32_t src_stride,
                         uint32_t src_w, uint32_t src_h,
                         bool use_color_key, uint32_t color_key);
void gfx_sprite_blit_mask(gfx_handle_t *h, int32_t dx, int32_t dy,
                         const uint32_t *src, uint32_t src_stride,
                         const uint8_t *mask, uint32_t mask_stride,
                         uint32_t src_w, uint32_t src_h);
```

**错误约定**：

| 情况 | 结果 |
|------|------|
| `/dev/gfx0` 不存在 | `gfx_open` 返回 NULL，`errno=ENODEV` |
| 零尺寸 / 越界 / 尺寸溢出 | `gfx_open` 返回 NULL，`errno=EINVAL` |
| 16 视图已满 | `gfx_open` 返回 NULL，`errno=EMFILE` |
| `mmap`/heap 失败 | `gfx_open` 返回 NULL，`errno=ENOMEM` |
| ioctl 失败（如 invalid pointer） | `gfx_present` 返回 -1，`errno=EFAULT` |
| NULL handle | `gfx_get_info` 返回全零 + `errno=EINVAL`；`gfx_present` / `gfx_set_clip` 返回 -1 + `errno=EINVAL`，不触发 ioctl |

### UAPI（`kernel/include/uapi/gfx.h`，由 sysroot `usr/include/uapi/gfx.h` 发布）

```c
#define GFX_FORMAT_RGB32 0u
#define GFX_CREATE_VIEW  0x4701
#define GFX_GET_INFO     0x4702
#define GFX_PRESENT      0x4703

typedef struct { uint32_t x, y, w, h; } gfx_view_desc_t;     /* 16 B，全字段定宽 */
typedef struct { uint32_t width, height, stride, format; } gfx_info_t;
typedef struct { uint64_t pixels; uint32_t stride; uint32_t reserved; } gfx_present_req_t;
```

### `/dev/gfx0` 实现（`kernel/driver/gfx.c`）

- `gfx_init()`：`x86_64_boot_device_nodes()` 在 devfs_init 之后注册 `/dev/gfx0`（不早于 devfs）
- `gfx_ops.open`：分配未配置视图（不占视图表），返回 `file_t` + `dev_private = gfx_view_t`
- `gfx_ops.ioctl_file` 处理 3 个 ioctl，校验参数后：
  - `GFX_CREATE_VIEW`：在视图表分配一个槽，写入矩形；减法形式溢出检查 `x <= fb_w && w <= fb_w - x`
  - `GFX_GET_INFO`：返回视图的 `gfx_info_t`（width/height/stride/format）
  - `GFX_PRESENT`：快照视图矩形（持锁）→ 释放锁 → 逐行 `syscall_check_user_range` + `copy_from_user_ft` → `fb_write_row`；中途中断返回 `-EFAULT`，前面行可能已更新
- `gfx_ops.release_file`：从视图表清槽，释放视图结构
- **无 mmap 回调**：用户像素缓冲始终由用户提供，不通过 mmap

### 限制（spec §2 / §4 明示）

- **没有 owner / 委托 / compositor**：任何进程可打开 `/dev/gfx0`，任何进程可继续打开 `/dev/fb`，视图矩形重叠时由 present 顺序覆盖；不提供跨进程权限隔离
- **无 vsync / page-flip**：present 同步返回，QEMU stdvga 行为；真实硬件需自实现刷新策略
- **无 alpha blend**：仅 RGB32 host-endian
- **无多 fb 格式**：仅 `format=0` (32 bpp)；行跨度固定为 `width*4`
- **无 dirty-rect present**：每次 present 拷贝整个视图矩形；后续可优化
- **视图表大小固定 16**：超出需动态扩表（本阶段不做）

### Tetris 迁移要点（plan §6 Task 6）

1. `user/tetris.c`：用 `libgfx` 替换 `/dev/fb` mmap
2. `gfx_open(0, 0, fb_info.width, fb_info.height)` 创建全屏视图（`USER_PAGE_SIZE=16 MiB` 后 5.18 MiB 缓冲能放进用户堆）
3. `gfx_fill_rect` 替代 `draw_cell` / `draw_rect`（局部坐标，从视图 (0,0) 起算）
4. 保留脏矩形 diff + clear-line flash 行为；present 一次每视觉事件（视觉更新后 / flash 后 / pause 前），不按 cell 多次 present
5. 每个 exit path 调 `gfx_close`
6. `FBIOSURRENDER` 仍在 `gfx_open` 之前 issue（保留与迁移前行为一致）

### 验证（task-6-report.md）

- `make test-host`：35/35 suites PASS
- `make test-qemu SUITE=phase-0`：PASS
- `make test-qemu SUITE=gfx`：`[GFX TEST] PASS`
- `make OS01_SYSTEST=1 test-qemu SUITE=systest`：287/287 PASS
- 手动 QEMU（`.superpowers/sdd/2026-09-30-2d-graphics-api/manual_tetris_test.py`）：screendump 验证 bg 黑、边框灰、I-piece 青色（`0x00FFFF`）；terminal 退出后 alt-screen 恢复，shell prompt 重新出现

### 构建/测试入口

```bash
make lib                                    # 仅 libgfx.a
make user                                  # 链 libgfx 的 user 程序（tetris / test_gfx）
make PROFILE=x86_64-clang image             # 镜像含 libgfx.a + tetris + test_gfx
make PROFILE=x86_64-clang test-host        # hosttest（gfx device / client / primitives / lifecycle）
make PROFILE=x86_64-clang test-qemu SUITE=gfx   # ring-3 QEMU 验证
make PROFILE=x86_64-clang test-qemu SUITE=systest # syscall suite
```

---

## Tetris 游戏实施总结（已完成，2026-08-16，Task 6 迁移到 libgfx）

> 状态：**已完成**。原始提交 `79f1179`（framebuffer + alt-screen 协议）、`d8e5c05`（UX：慢速重力 + 种子 RNG + 消行闪烁）、`ae0cc04`（消行白残留修复）。Task 6（2026-09-30）迁移到 libgfx，提交 `feat(gfx): migrate Tetris and document API`，渲染路径改为 `/dev/gfx0` + `libgfx.a`，游戏逻辑（`user/tetris/tetris_logic.c`）零变更。QEMU 手工 `exec /bin/tetris` 可玩，退出终端内容恢复。

### 目标

OS01 上可玩的俄罗斯方块：用户态 `user/tetris.c`，**手工启动**（不进 inittab），libgfx 像素渲染 + `/dev/keyboard` 原始扫描码输入，退出后终端内容自动恢复（Linux alt screen 协议）。

**明确不做**：serial 渲染后端（38400 baud 下增量重绘虽可行，但优先级低，已讨论砍掉，收敛范围）。

### 架构决策（已确认）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 31 | tetris 输入 | `/dev/keyboard` 原始扫描码（E0 48/4B/4D/50 方向键） | tty TCSETS 是 no-op，raw 模式改造不值；扫描码自解析 E0 前缀 + release 位 |
| 32 | tetris 渲染 | libgfx `gfx_open` 全屏视图 + `gfx_fill_rect`（Task 6） | Task 6 之前用 fb mmap（被 libgfx 替代）；FBIOSURRENDER 仍保留 |
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
- Task 6 之后：tetris 通过 `gfx_open(0,0,w,h)` 拿到 5.18 MiB 像素缓冲；`USER_PAGE_SIZE` 升到 16 MiB；`USER_STACK_BASE = 0x1400000`；`spawn_user_task` / `sys_exec` 显式插入 heap VMA 让 brk 扩展不 SIGSEGV

### 分步实现

| Step | Commit 主题 | 文件 | 内容 | 验证 |
|------|------------|------|------|------|
| **1** | `feat(driver): keyboard poll support`（~31 行） | `kernel/driver/keyboard.c` + `kernel/core/main.c` | scancode wait queue（spinlock+list）；`keyboard_handler` push ring 后 wake；`keyboard_poll_dev()`：ring 非空→POLLIN，否则 poll_wait；`keyboard_ops.poll` 挂上（main.c 1 行） | `make clean && make`；QEMU 内测试程序 `poll(/dev/keyboard)` 阻塞等键立即返回 |
| **2** | `fix(tty): make termios honest`（~60 行） | `kernel/include/tty/tty.h` + `kernel/tty/tty.c` | tty_t 加 `struct termios term`；默认 raw（`c_lflag=0, ICRNL, OPOST\|ONLCR, VMIN=1`）；TCGETS 返回真值 / TCSETS 真存储；tty_read 尊重 ICANON（攒行等 `\n`）+ ECHO 回显。**不做**：ISIG/pgrp（TODO）、OPOST 输出转换（无消费者） | `make clean && make`（结构体变更！）；QEMU 回归 terminal/ash；小程序 TCSETS 切换 raw/canonical 验证行为差异 |
| **3** | `feat(terminal): alt screen double buffer`（~80 行） | `user/terminal/terminal.c` | offscreen 主缓冲 + alt 缓冲；put_glyph 写当前缓冲；CSI 解析器加 `?1049h`（保存+清屏+切 alt）/ `?1049l`（切回主缓冲全量重绘） | 测试程序发 `\e[?1049h` 画图 → `\e[?1049l`，原终端内容完整恢复 |
| **4** | `feat(applets): tetris game`（~400 行） | `user/tetris.c`（新）+ `Makefile` | 游戏逻辑（7 Tetromino、4 旋转、碰撞、消行、计分、等级加速）；输入解析（E0 前缀 + release 位 → 归一化 K_LEFT/RIGHT/DOWN/UP/ROTATE/DROP）；渲染 fb 像素块 + **20×10 逻辑屏脏矩形 diff**（防闪烁）；主循环 `poll(/dev/keyboard, 500ms 超时=下落 tick)`；`\e[?1049h` 进入 / `\e[?1049l` 退出 | QEMU 手工 `exec /bin/tetris` 可玩 |
| **4b** | `feat(gfx): migrate Tetris and document API`（Task 6） | `user/tetris.c` + `user/Makefile` | 用 `gfx_open(0,0,w,h)` 替代 `/dev/fb` mmap；`gfx_fill_rect` 替代直接像素写；每视觉事件一次 `gfx_present`；脏矩形 diff / clear-line flash 行为保留 | `make test-host` / `make test-qemu SUITE={phase-0,gfx,systest}` 全 PASS；QEMU 手工 screendump 验证 bg 黑、边框灰、I-piece 青色 |
| **5** | 集成验证（无代码） | — | 全量 `make clean && make`；启动 → terminal/ash 正常 → 玩 tetris → 退出终端恢复；systest 回归 | QEMU 实跑 |

### 实施顺序与依赖

```
Step 1 (keyboard poll) ──┐
                          ├──→ Step 4 (tetris 游戏，依赖 1 的 poll + 3 的 alt screen)
Step 2 (tty termios)  ──┤    Step 4 内部：游戏逻辑可先于渲染写（无依赖）
                          │
Step 3 (terminal 双缓冲) ─┘

Step 4b (libgfx 迁移)  ──→ Step 1..3 全部完成 + 2D 图形 API 6 个 task 闭环
Step 5 (集成验证) ──→ 所有 Step 完成后
```

- Step 1/2/3 **互不依赖**，可并行推进；Step 4 依赖 1+3
- Step 1+2 为**同一批内核修改**，但**两个独立 commit**（主题不同：driver vs tty）
- Step 4b 依赖 2D 图形 API 的 6 个 task（UAPI + 设备 + 库 + primitive + QEMU 套件 + Tetris 迁移）
- 每步独立验证，**通了再报进展**（QEMU 实证，非静态分析）

### 风险 / 注意

| 风险 | 缓解 |
|------|------|
| tty_t 结构体变更（sizeof 变化） | Step 2 必须 `make clean && make`，旧 .o 会静默崩 |
| keyboard ring 满丢键（RING 256，连按风险） | 一般够用；游戏输入事件率低；如丢键再加 ring 大小 |
| poll 唤醒丢失 | 参照 tty_wake_waiters 双检模式 + `this_cpu()->need_resched=1` |
| 主循环被信号中断 | poll/nanosleep EINTR 处理（`errno==EINTR → continue`） |
| 脏矩形 diff 复杂度 | V1 每 tick 全量 diff（20×10 数组比较，~200 次 memcmp，开销可忽略） |
| 像素缓冲超过用户堆上限 | 历史：Task 6 把 `USER_PAGE_SIZE` 升至 16 MiB；现行：`USER_ENVELOPE_SIZE` 扩至 512 MiB，`libgfx` 像素缓冲已切至匿名 `mmap`，与堆完全隔离 |

### 完成后（可选项，不阻塞）

- tty ISIG/pgrp 真实现（Step 2 留的 TODO）→ 内核层 Ctrl-C 信号
- serial 后端 `tetris -serial`（增量 ANSI，QEMU -nographic 直玩）

---

## P3 GUI 后续（规划中）

基座（已完成）：fb ✅、fb mmap ✅、terminal 双缓冲 + alt-screen ✅、键盘扫描码 ✅、PS/2 鼠标 ✅、**libgfx + /dev/gfx0 ✅**

| 项 | 内容 | 依赖 | 借鉴 |
|----|------|------|------|
| 可缩放字体渲染器 | 矢量/位图缩放 | 2D API ✅ | HackOS |
| Window Server + compositor | 多窗口管理 + 合成 | 字体/2D/鼠标 | opuntiaOS + HackOS |
| 字体层 alpha blend | draw_text with anti-aliasing | libgfx + 字体 | |
| 多格式 fb | RGB16/RGB24/etc. | libgfx | |
| aarch64 fb 驱动 | pl11 / virtio-gpu | 2D API | |

### 依赖链

```
P3: fb ✅ → 2D API ✅ → 字体 → Window Server；PS/2 鼠标 ✅ 并行
```

---

## 显示所有权协调协议（Display Ownership Protocol，2026-10-11）

> 状态：**已完成**。提供排他性前台显示控制权协调、崩溃恢复与 libgfx 接口封装。

### 架构与核心概念

1. **排他性前台（Exclusive Foreground Owner）**：
   - 同一时刻内核只允许一个活跃前台所有者（`g_owner`）。
   - 只有持有前台所有权的句柄才能通过 `GFX_PRESENT` 向物理帧缓冲提交像素，非所有者 present 立即返回 `-EACCES`。
   - 所有权在协调器中记录为打开视图的非所有权 cookie（`const void *cookie`，对应 `f->dev_private`，而非裸 PID 或 `file_t *`），支持 `dup`/`fork` 共享引用，并在最终关闭（final close，由 `gfx_release_file` 调用 `fb_owner_detach`）时触发自动清理。

2. **恢复目标（Fallback Owner）**：
   - 桌面虚拟终端（`terminal.elf`）初始化时注册为系统级 fallback 所有者（`GFX_REGISTER_FALLBACK`）。
   - 当没有前台 GUI 启动时，fallback 兼任前台所有者。
   - 当全屏应用（如 `test_lvgl`、`desktop`、`tetris`）通过 `GFX_ACQUIRE` 抢占前台时，fallback 降为后台等待态。
   - 当全屏应用主动调用 `GFX_RELEASE`、正常关闭或异常退出（SIGKILL/崩溃）由内核执行最终关闭时，前台所有权自动回退到 fallback（若无 fallback 则退回内核 console），触发终端完整重绘。

3. **libgfx 用户态封装（`libgfx/gfx.h`）**：
   - `int gfx_acquire(gfx_handle_t *h)`：前台抢占（`GFX_ACQUIRE`），成功后可调用 `gfx_present`。
   - `int gfx_release(gfx_handle_t *h)`：主动归还所有权（`GFX_RELEASE`），前台恢复至 fallback。
   - `int gfx_register_fallback(gfx_handle_t *h)`：注册为全屏 fallback 恢复目标。
   - `int gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out)`：查询当前所有权代数及前台/fallback 状态标志。
   - `int gfx_replace_fallback(gfx_handle_t *old_h, gfx_handle_t *new_h)`：在分辨率切换等场景下原子换绑 fallback。

---

## GUI-2 输入与焦点（Input & Focus，2026-10-22 闭环）

> 状态：**已完成**。把 `/dev/keyboard` 扫描码与 `/dev/mouse` 事件接入 LVGL indev，实现光标、修饰键、按下/释放、重复、Tab 导航与控件焦点，并用**独占物理输入租约**保证同一时刻只有一个读取者。
> 设计见 [spec](../superpowers/specs/2026-10-20-gui-2-input-focus-design.md)，实施见 [plan](../superpowers/plans/2026-10-20-gui-2-input-focus.md)，验证证据见 [验证报告](../superpowers/reports/2026-10-22-gui-2-validation.md)。

### 1. 架构：输入路由与独占租约（the input lease）

`kernel/driver/input_route.c` 是 PS/2 驱动与**所有**输入字节读者之间的唯一通道，位于 `/dev/keyboard`、`/dev/mouse` 的读路径之上。它持有三样东西：

| 职责 | 说明 |
|------|------|
| **物理键状态** | 「哪些键真的按住了」的唯一权威；Caps Lock 锁存也只在这里翻转一次，NORMAL 路径的翻译器绝不二次翻转 Caps |
| **租约（lease）** | 同一时刻至多一个活跃租约，绑定一个 display view；租约存活期间所有其他文件被挡在设备外（读返回 `EBUSY`，`poll` 报 `POLLERR`） |
| **路由决定** | 一个**完整**的扫描序列（绝不半个）被发布到 NORMAL 路径或租约的 ACTIVE 路径之一 |

**两条路径**：

- **NORMAL 路径**：租约之外的普通读者（控制台 TTY 之前的历史行为），驱动把帧发布到注册的 sink。
- **ACTIVE 路径**：租约持有者的文件；队列、错误与授权全部由 route 裁决。

**锁序**：`route → NORMAL queue/poll`。route 从不取 i8042 锁、显示状态锁或显示互斥锁；NORMAL 的 read/poll 在 route 自己的临界区里完成授权、出队与 waiter 登记，因此与 publish 的竞态不会丢事件。`input_route_lease_live()` 只取 route 锁，可在显示锁下调用（锁序 `display → route`），所以「有没有租约」的两个答案永远不会分叉。

**租约与显示所有权同生共死**：`GFX_BIND_INPUT` 把调用者的 keyboard/mouse fd 绑定到调用者的 **display view**，租约因此随显示所有权一起获得与释放；绑定文件的最终关闭（final close）在内核内部撤销租约。

### 2. UAPI 布局（`kernel/include/uapi/gfx.h`）

```c
#define GFX_BIND_INPUT   0x470A   /* INOUT：绑定物理输入租约到调用者的 view */
#define GFX_UNBIND_INPUT 0x470B   /* 释放调用者自己的租约 */

typedef struct {
    int32_t  keyboard_fd;       /* +0  必需的 /dev/keyboard fd */
    int32_t  mouse_fd;          /* +4  /dev/mouse fd；-1 = 只租键盘 */
    uint32_t flags;             /* +8  v1 必须为 0 */
    uint32_t reserved;          /* +12 必须为 0 */
    uint64_t generation;        /* +16 in: 0；out: 租约代数（非零、单调递增） */
    uint32_t initial_modifiers; /* +24 in: 0；out: bit0 = Caps Lock，其余为 0 */
    uint32_t reserved2;         /* +28 in: 0；out: 0 */
} gfx_bind_input_t;              /* sizeof == 32，字段偏移由 _Static_assert 固定 */
```

- 整个结构体必须**可读且可写**，否则 `EFAULT`；结果字段（`generation` / `initial_modifiers`）进入时必须为 0。
- `mouse_fd` 只有 `-1` 表示「只租键盘」；其他负值一律 `EBADF`。
- 保留的 ABI 不动：`mouse_event_t` 仍是 **8 字节**（`kernel/include/uapi/mouse.h` 的 `_Static_assert` 固定），键盘仍是 **Set 1 原始扫描码负载**（E0 前缀键在用户态记为 `0x100 | 扫描码`）。
- 租约不靠 termios 实现：TTY 的 `ISIG` 保持开启，隔离由内核的路由层完成。

### 3. 原生 API（`libgui/include/gui/gui_input.h`）

公开 sysroot 的 GUI 树仍然**只有** `gui.h`、`gui_input.h`、`gui_platform.h` 三个头，不暴露任何 `lv_*` 类型、私有桥接头或测试注入接口。

```c
gui_input_t *gui_input_create(gui_platform_t *platform, const gui_input_config_t *cfg);
void         gui_input_destroy(gui_input_t *input);
int          gui_input_get_pollfds(gui_input_t *input, struct pollfd out[2]);
bool         gui_input_has_mouse(const gui_input_t *input);
int          gui_input_dispatch(gui_input_t *input, const struct pollfd *fds, unsigned count);
void         gui_input_set_key_handler(gui_input_t *input, gui_key_handler_t handler, void *userdata);
void         gui_input_set_pointer_handler(gui_input_t *input, gui_pointer_handler_t handler, void *userdata);
```

- `gui_input_config_t.mode`：`GUI_INPUT_DEVICES`（真实设备，**独占物理输入租约**，读 `/dev/keyboard` 与 `/dev/mouse`）或 `GUI_INPUT_SYNTHETIC`（合成输入，不打开设备、不占租约）。真实设备只允许屏幕平台，合成输入只允许离屏平台。
- `gui_input_get_pollfds()` 顺序固定为 keyboard、mouse；fd 只可借用，不得 read/close。
- `gui_input_dispatch()` 只读取与入队，不渲染；应用持有主循环。
- 所有权：输入对象挂接在平台上，`gui_platform_destroy()` 会自动销毁它；要自己掌控生命周期必须**先**销毁输入再销毁平台。
- 事件类型：`gui_key_event_t`（`physical_key` / `modifiers` / `codepoint` / `action`）与 `gui_pointer_event_t`（`x` / `y` / `buttons` / `changed_buttons` / `wheel`）。`codepoint` 只可能是 ASCII 字符或 0，绝不会是 LVGL 的按键常量。
- **回调先于控件路径**：`gui_key_handler_t` 返回 false 的事件才会继续送进 LVGL keypad。因此存在一处应用可见 / 控件可见的分歧——当 LVGL 侧的重新就绪协议尚未完成时，控件路径**不会**收到该键，但回调已经看到了它的 DOWN。需要「键一定到达控件」的应用必须等重新就绪后再让用户重新按下。
- 焦点组（`gui_input_focus_*`）是 **libgui 私有**的：应用不直接登记可聚焦控件，焦点由 libgui 内部的焦点组维护（容量 64，focus outline 宽 2 像素）。

### 4. errno 表面

**内核绑定（`GFX_BIND_INPUT`）**：

| errno | 含义 |
|-------|------|
| `EFAULT` | 结构体不可读且可写 |
| `EBADF` | 描述符越界、未打开，或 `mouse_fd` 是 -1 以外的负值 |
| `EINVAL` | 描述符不可读 / 不是输入设备，保留字段或结果字段非 0，或 view 未配置 |
| `EACCES` | 调用者不是显示所有者 |
| `ESTALE` | view 已失效（显示已易主） |
| `EBUSY` | 已经存在一个租约 |
| `EAGAIN` | 显示尚未初始化 |
| `EIO` | 显示后端失败 |
| `ENODEV` | 本内核没有输入路由（无 PS/2 链路） |

**ACTIVE 路径的带外错误**（字节流本身保持原始 Set 1 负载）：

| errno | 语义 | 粘性 |
|-------|------|------|
| `EOVERFLOW` | ACTIVE 流不可信：要么一个序列没能装进 ACTIVE 队列（**数据流有丢失**），要么一个序列跨越了 epoch 边界被吞掉（消费者的**键状态**可能已与物理状态分叉） | 整个租约生命周期内粘住：read 与 poll 都持续报告它，直到应用交还租约并重新绑定（新租约从已知一致状态开始） |

**libgui 用户态 API**：

| errno | 出现位置 |
|-------|----------|
| `EINVAL` | `gui_input_create()`：平台为空 / 平台已死 / 已挂接输入 / mode 非法 / 模式与平台不匹配；`gui_input_dispatch()`：fd 不属于本对象、`count > 2`、`(fds == NULL && count != 0)`；`gui_input_get_pollfds()`：参数为空；`gui_input_set_*_handler()`：input 为 NULL |
| `EBUSY` | `gui_input_create()`：该平台已挂接一个输入对象 |
| `EBADF` | 已停止的输入对象上的 `gui_input_dispatch()` / `gui_input_get_pollfds()` |
| `EOVERFLOW` | libgui 自己的 512 项 FIFO 满时入队 |
| `EIO` | 鼠标记录长度不是 `sizeof(mouse_event_t)` 的整数倍；绑定失败且内核未给出 errno 时的兜底 |
| `ESTALE` | dispatch 时设备出现 `POLLHUP` |
| `EAGAIN` | 内核返回 `EAGAIN`：本轮读完了，不是错误 |

`gui_input_create()` 失败时会**回滚**已做的步骤（关 fd、摘除显示绑定）并把最初失败的 errno 原样带回。

### 5. 恢复路径

| 事件 | 表现 | 恢复 |
|------|------|------|
| 显示所有权丢失（非前台） | `gfx_get_display_state()` 报 `EACCES`，`GUI_STATUS_LOST_OWNERSHIP` | 应用应交还租约并退出输入循环 |
| 租约被内核撤销（owner 进程死亡 / 文件最终关闭） | 内核在 route 内部撤销租约；租约随显示 view 一起释放 | 键盘立刻回到 NORMAL 路径，控制台 TTY 恢复；下一个客户端可以重新绑定 |
| 租约被抢占（新 owner 绑定） | 旧 owner 的租约代数不再匹配 | 旧 owner 的后续操作得到 `ESTALE` / `EACCES`，非零退出 |
| 队列溢出 | `EOVERFLOW` | 数据流已丢失，**不可恢复**；消费者必须重新同步键状态 |
| 跨 epoch 的序列 | `EOVERFLOW`（与队列溢出同一个 errno） | **不可恢复**：租约必须交还并重新绑定；新租约从已知一致状态开始 |
| owner 崩溃（SIGKILL / LVGL fatal / Ctrl-Q） | 内核 final-close 回收租约 | 终端与键盘一并回到正常状态（不留幽灵点击） |

### 6. 固定容量与限制（明示边界）

- **FIFO 固定 512 项**；每轮 dispatch 最多读 **256 个键盘字节**、**64 个鼠标事件**；每步最多投递 **256 项**。
- 软件重复：**首次延迟 400 ms、之后 40 ms**；每步最多产生一个到期重复，无补发风暴。
- Ctrl/Alt 从无到有会**取消旧活动键**，释放后不恢复。
- 指针 **1 计数 = 1 像素**；光标是程序绘制的固定 **12×18** 箭头，热点 **(0,0)**，不引入图片 / 字体依赖；**垂直滚轮每刻度 32 px**，到边界时余量传播给父滚动容器，无可用目标则完全不动作。
- **单活动键（single active key）**：LVGL 控件路径一次只跟踪**一个**活动键。新键的 DOWN 会先取消旧活动键（`gui_input_lvgl_cancel_key()`），此后**迟到的不匹配释放被拒绝**，不会误释放别的键。因此同时按住两个普通键时，只有最后一个能作用于控件；这不是丢事件，而是「一个活动键」语义的直接后果。
- 每进程单 platform、每 platform 至多一个 input；单 UI 线程，应用持有主循环。
- 合成输入（`GUI_INPUT_SYNTHETIC`）只用于离屏平台，不接触设备、不占租约。

### 7. 构建/测试入口

```sh
# 聚焦的宿主 CASE（单一列表 GUI_HOST_REGISTERED_CASES）
make PROFILE=x86_64-clang test-gui-host CASE=input-decode
make PROFILE=x86_64-clang test-gui-host CASE=all

# QEMU E2E：12 个场景 × SMP 1/2 = 24 个结果
make PROFILE=x86_64-clang test-qemu SUITE=gui
```

`CASE` 的合法取值：`platform`、`lvgl`、`devfs-input`、`input-frame`、`input-route`、`input-decode`、`input-nonce`、`input-platform`、`input-lvgl`（全部从仓库根 make 入口运行，不使用 `make -C hosttests`）。


## GUI-3 Material 风格桌面（2026-10-24 闭环）

### 1. 范围与边界

用 LVGL 重写 `user/desktop`，把「Material 风格」从**画出来的像素**变成**可核验的设计系统**：

| 做 | 不做 |
|----|------|
| 单一令牌表驱动颜色 / 字号 / 间距 / 圆角 / elevation / 六类交互状态 | 深色主题、主题切换（色彩**角色**结构已做对，将来加一套角色表即可） |
| 状态栏、启动器、应用抽屉、基本设置 | 窗口系统：没有最小化 / 最大化 / 关闭，没有标题栏边框，没有 Z-order |
| 四个**壳内应用**（文件 / 终端 / 系统 / 设置） | fork/exec 子进程应用、显示所有权与输入租约交接 |
| 键鼠一律走 GUI-2 的输入通道 | 直接 `open("/dev/keyboard")` / `open("/dev/mouse")` |

**应用语义**（§6 明确写入 spec 并由断言钉住）：

- **启动** = `HOME → APP(id)` 的**整屏替换**：旧表面的控件树销毁，新表面从零构建。
- **退出** = 销毁应用表面，回到 `HOME`。
- **全屏切换** = 整屏替换，**不存在窗口化中间态**。
- 应用状态（终端命令行与历史、文件浏览器当前目录）存在**外壳持有的 C 变量**里，不寄居在会被销毁的控件树中。

### 2. 设计系统：令牌表在 libgui，主题落地也在 libgui

| 文件 | 作用 | 对外可见性 |
|------|------|------------|
| `libgui/include/gui/gui_theme.h` | 令牌表：32 个色彩角色、5 级字号、5 档间距、5 档圆角、elevation 0–5 | **公共**（纯 C，无 LVGL 符号） |
| `libgui/src/theme.c` | 令牌表实现 | libgui 内部 |
| `libgui/src/theme_apply.c` | 令牌表 → LVGL 主题 | libgui 内部 |
| `libgui/src/platform.c` | `gui_platform_create()` 自动套用主题；屏幕显式 `lv_theme_apply()` | libgui 内部 |

**公共 API 刻意保持 LVGL 无关**：sysroot 的已发布头有一条硬约束（`qemutests/gui_build_audit.py`）——**任何**已发布头不得出现 LVGL 符号。因此「把 `lv_display_t` 提升为公共 API」这条路走不通（`typedef struct _lv_display_t lv_display_t;` 本身就是 LVGL 符号）。改为：① 令牌表是纯数据，可以发布；② 令牌表 → LVGL 主题的落地整体留在 libgui 内部；③ 业务应用自行包含 LVGL 头，用 `lv_display_get_default()` 取 display（惯用做法）。

主题必须与平台**同生共死**：`gui_platform_destroy()` 的顺序固定为「先删 display → 再拆主题 → 最后 `lv_deinit()`」（反序会 use-after-free）。

### 3. 外壳结构

```
user/desktop/
  desktop_state.{c,h}    纯 C 状态机（HOME / DRAWER / SETTINGS / APP），与 LVGL 无关
  desktop_registry.{c,h} 应用注册表（纯 C 数据；accent 用角色，不写死颜色）
  desktop_shell.{c,h}    外壳：表面、状态栏 / 启动器 / 抽屉 / 设置、焦点模型
  desktop_apps.{c,h}     四个壳内应用
  desktop.c              主入口：libgui 平台 + GUI-2 输入 + 主循环
```

- **迁移只发生在主循环**：UI 回调只**投递意图**（`desktop_shell_request()`），由外壳在主循环里串行执行。LVGL 回调期间销毁控件树是未定义行为，因此设置项变更也走同一条投递通道。
- **状态机不携带 app id**：四个表面状态是纯函数可穷举的；「哪个应用在跑」由外壳记在 `shell->current_app`。
- **表面恒为全屏**，应用表面根对象覆盖整个显示区。

### 4. 键盘契约与焦点模型

| 键 | 行为 |
|----|------|
| `Tab` / `Shift+Tab` | 在当前表面的焦点项之间前进 / 后退 |
| `Enter` | 激活当前焦点项 |
| `ESC` | 返回主屏 |
| 可打印字符 | 交给当前应用（终端消费并回显） |

**桌面自己维护焦点序列**：libgui 的焦点组是**私有**的（`gui_input_focus_*` 在 `libgui/internal/`），公共 API 没有「登记可聚焦控件」的入口，而业务应用只能用公共 API。因此外壳用 LVGL 的 `LV_STATE_FOCUSED` 状态实现同一语义，焦点环（2px primary 轮廓）由 Material 主题给出。序列随表面重建清空，不会残留已销毁对象。

> **这是临时方案。** roadmap 的 **GUI-2b libgui 公共焦点 API**（见 `docs/roadmap.md` §P3 后续功能任务）完成后，桌面改用那条公共 API，本节的自建序列（`desktop_shell_focus_*`）随之删除——GUI-3 与 libgui 的焦点组之间不应该长期存在两套模型。

### 5. 界面文案用 ASCII（不是中文）

GUI-3 只启用 Montserrat（12/14/16/20/24），**没有汉字字形**——中文文案会整片渲染成方框。spec 已把「中文字体」划归 **GUI-4 字体与资源**，因此 GUI-3 的界面文案一律用 ASCII（`Files` / `Terminal` / `System` / `Settings`）。这是**已知边界**，不是遗漏。

### 6. 验证

| 层次 | 手段 | 覆盖 |
|------|------|------|
| 令牌 / 主题 | `hosttests/cases/test_gui_theme.c`、`test_gui_theme_apply.c` | 令牌取值、非法枚举、五类交互状态 |
| 状态机 | `hosttests/cases/test_desktop_state.c` | 迁移集合穷举 |
| 外壳与像素 | `hosttests/cases/test_desktop_shell.c` | 表面像素、四个应用逐一启动 / 全屏 / 退出、终端消费字符、退出后焦点仍可导航 |
| 静态审计 | `hosttests/cases/test_desktop_audit.c` | 禁颜色字面量、禁裸字号、禁手绘 primitive、禁直开设备、禁私有头、禁窗口装饰标识 |
| QEMU 端到端 | `qemutests/gui_desktop_test.py`（`SUITE=gui-desktop`） | 冒烟、桌面接管显示、真实 PS/2 按键启动应用并返回、杀掉桌面后 guest 仍存活；SMP 1/2 |
| 门禁收尾 | `qemutests/test_gui_desktop_runner.py` | 收尾必须真的杀掉 QEMU；信号路径必须杀掉进程组；子进程随父进程而死（PDEATHSIG） |

```sh
make PROFILE=x86_64-clang test-gui-host CASE=theme
make PROFILE=x86_64-clang test-gui-host CASE=theme-apply
make PROFILE=x86_64-clang test-gui-host CASE=desktop-state
make PROFILE=x86_64-clang test-gui-host CASE=desktop-shell
make PROFILE=x86_64-clang test-gui-host CASE=desktop-audit
make PROFILE=x86_64-clang test-qemu SUITE=gui-desktop
```

**门禁收尾有防孤儿契约。** QEMU 持有磁盘镜像的**写锁**（`-snapshot` 模式也一样），一个活着的孤儿 QEMU 会让后续的 `make run` 报 `Failed to get "write" lock / Is another process using the image [...]/disk.img`。因此门禁的三层保障缺一不可：

1. **正常收尾**：`DesktopCaseRunner.close()` 调真实的 `ProcessSession.close()`（不是别的方法名），收尾失败只记录不抛出，但记录必须显眼。
2. **信号路径**：SIGINT / SIGTERM 触发时**无锁硬杀**在跑的 QEMU 进程组（`SIGKILL` 不可捕获），再退出。
3. **内核兜底**：`ProcessSession` 的 `preexec_fn` 装上 `PR_SET_PDEATHSIG`，父进程被 SIGKILL 或崩溃时由内核杀掉 QEMU。

对应回归在 `qemutests/test_gui_desktop_runner.py`，其中一条用的假 session **只有 `close()`、故意没有 `cleanup()`**，把当初那个方法名笔误钉死。
