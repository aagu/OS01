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
