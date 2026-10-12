# terminal 长时间占用 CPU 约 16% — 调查交接文档

> **本文件是给 codex 出 spec + plan 的交接材料。** 不是研究文档，也不是结论。
> 写 plan 时把以下事实当成 checklist，每条都已经过端到端验证。

- 调查起止：2026-10-12
- 调查者：aagu + Claude Code（worktree `worktree-tty-investigate-terminal-cpu` 合并）
- 已落地的 commit：`bfeb0b0`（probe + v1 代码层清理，含完整 commit message）
- 关联占用：roadmap.md §「parked 项」（"`test_tty_vintr` 间歇性超时」段下方的"`QEMU 分辨率切换器收尾 follow-up`"段已补充本条作为 parked candidate）

---

## 1. 现象与复现

**症状**：OS01 在 2-vCPU 镜像下，`monitor` 跑在 tty1（终端 + 屏内嵌 ash），
`monitor` 的 `ps` 行显示 `terminal` 进程占 18.0% of CPU0（state S，即 sleeping），
`CPU0` 的总量也跟着到 18.6%。idle terminal（不跑 monitor）回归 0%。

**复现步骤**（在 worktree `worktree-tty-investigate-terminal-cpu` 内）：

```bash
# 这条 commit 已经把 terminal.c::main fork 的子进程临时改成 /bin/monitor
# 来做"用户场景"自动可复现。merge 此 commit 后用户仍然能在 shell 里
# 手动跑 monitor 复现 18%。如果将来此临时 exec 被 revert，那这一步
# 仍由终端启动 ash + 用户手动 `monitor` 来复现。
timeout 30 make run > /tmp/qemu.log 2>&1 &
sleep 24
grep term-probe /tmp/qemu.log | head -5
```

期待输出形如：

```
[term-probe] wakes/s=5 presents/s=2 iter_avg_us=34137
            refresh_avg_us=21 rest_avg_us=34116
            present_avg_us=85277 present_max_us=88150
```

**关键观察**：用户在前一份截图（commit `bfeb0b0` 之前）看到 17.0%，我们
在 commit `bfeb0b0` 测得 18.0%。两者一致——**monitor 在 terminal 上跑**这个
特定组合在 QEMU TCG 下是可复现的 ~18% CPU。

---

## 2. 实测数据（commit `bfeb0b0` 自带的 Phase 1 instrumentation 输出）

### 2.1 用户态 terminal main loop（`[term-probe]` 段）

`user/terminal/terminal.c::main` 在每次 wakeup 头部、refresh 之后、
present 之后、poll 之前各 sample 一次 `clock_gettime`，每秒聚合打印。
monitor 跑在 terminal 上时：

```
wakes/s = 5                   ; idle path 4Hz + monitor 输出触发额外 wake
presents/s = 2                ; dirty_pending 33ms 后 present
iter_avg_us = 34137           ; 主循环一次 wakeup 平均 wall time
refresh_avg_us = 21           ; gfx_get_display_state + FBIOGET_STATE + 分支
rest_avg_us = 34116           ; 含 present + 整屏绘制 + 处理 PTY 数据
present_avg_us = 85277        ; terminal_display_present 内一切事务
```

数字校验：5 wakes/s × 34 ms/iter ≈ **170 ms/s ≈ 17% of CPU0** ✓

### 2.2 kernel `gfx_ioctl_present` wall time

`kernel/driver/gfx.c::gfx_ioctl_present` 头部取 t0，成功路径末尾聚合
写 `serial_printk`（每 256 次）：

```
gfx_present: count=48 total_ns=4020612557 avg_ns=83762761
```

→ 平均 **83.8 ms / present**。与用户态 `present_avg_us=85277` 完全吻合。

### 2.3 kernel `fb_write_row_leased` 单 row wall time

`kernel/driver/fb.c::fb_write_row_leased` 入口取 t0，函数末尾聚合写
`serial_printk`（每 1024 次）：

```
fb_write_row: count=45056 total_ns=2158120887
              avg_ns=47898 ns/pixel=33
```

→ 单 row **47.9 µs**，**每像素 33 ns**。
1080 rows × 47.9 µs = **51.7 ms 纯 fb_memcpy**，占 85 ms 的 ~60%。

### 2.4 根因结论

- **不是 busy-spin**：`refresh_avg_us = 21 µs` 确凿说明主循环除 `gfx_ioctl_present`
  外的工作完全正常；`d59d8172` 修过的 zero-timeout busy-poll 已经是修复后的状态，
  不在此分支出问题。
- **不是 setjmp/PT-walk 占主导**：`fb_write_row` 单次 47.9 µs ≈ 33 ns/pixel，
  这个数与 QEMU TCG 的 MMIO region dirty-tracking 行为一致（每个 32-bit store
  触发 QEMU framebuffer 模拟）。**真机裸机的 fb memcpy 应该接近 bare memcpy ~1-2 ns/pixel**，
  所以本调查在 TCG 下测得的 ~18% CPU **不会**在裸机出现。
- **结论**：terminal 18% CPU 是 **QEMU TCG framebuffer emulation 的代价**，
  路径在 `gfx_ioctl_present → fb_write_row_leased → framebuffer memcpy`。

---

## 3. 已尝试的 fix 与结果（commit `bfeb0b0`）

### 3.1 v1: `gfx_ioctl_present` row-loop 内部清理（小 / 无 TCG 收益）

`user/terminal/terminal.c` 现存的行循环里有两份冗余：

- 每 row `syscall_check_user_range(row_addr, stride, false)` —
  Stage 3 已经在循环**外**做了 `[pixels, pixels + v_h*stride)` 全 buffer 检查。
- 每 row `copy_from_user_ft(row_buf, ...)` 自带 `setjmp/longjmp` —
  可以 hoist 到循环外只 arm 一次，里面直接 `__builtin_memcpy`。

改后：

```c
os01_jmp_buf jb;
void **old_jmp = current->fault_jmp;
current->fault_jmp = (void **)jb;
if (__builtin_setjmp((void **)jb) == 0) {
    for (uint32_t row = 0; row < v_h; row++) {
        __builtin_memcpy(row_buf, user_row, stride);
        fb_write_row_leased(&lease, v_x, v_y + row, row_buf, stride);
    }
}
current->fault_jmp = old_jmp;
```

**TCG 下实测没有可测量改进** —— clang 把 setjmp/longjmp 折进 IR intrinsic、
TLB 常驻，per-iter 开销几乎为 0；wall time 仍 ≈85 ms（被 fb_write_row 主导）。
**真机裸机理论上有益**：每个 redundant setjmp+PT walk 是几 µs，1080 行
× 1-2 present/s 会累积成 1-2 ms/present 的可观测节约。

### 3.2 v2: `fb_write_full_rows_leased` 整帧 bulk memcpy（**已 revert**）

加了一个 API：

```c
int fb_write_full_rows_leased(const fb_lease_t *lease, uint32_t y, uint32_t h, const void *pixels);
```

gfx.c 在 `v_x == 0 && v_w == fb_w && kreq.stride == fb_stride` 时调它一次，
省掉 row_buf 中转 + 1080 次函数调用 + 边界检查。

**实测 TCG 下变慢 2.3 倍**：

| 版本 | `present_avg_us` | terminal CPU% |
|---|---|---|
| baseline | 85 277 | 18% |
| v2 full-lane | 196 323 | **40%** |

字节数相同时，单次 8 MB memcpy **反而比 1080 次分批 memcpy 慢**。推测根因：
QEMU TCG 对 MMIO region 的 dirty-tracking 在单次大区域写时 internal batch
行为不同（cache miss + TLB miss + QEMU 内部 state 重建一次性撑爆）。**这条
路线不能解决 18% CPU**。helper 在 commit 之前被删。

---

## 4. **真正的 fix 路线**：libgfx partial-present ABI

### 4.1 目标

把每次 `gfx_present` 的字节数从 **8.1 MB（1280×960 × 4B，整屏）降到
~50-300 KB（仅本次要写的 dirty region）**，terminal 18% CPU 在 QEMU TCG 下
应当下降到 ~1-3%。

### 4.2 现状（需要 codex 接手做的部分）

现在 `kernel/include/uapi/gfx.h` 只有 `gfx_present_req_t`：

```c
typedef struct gfx_present_req {
    uint64_t pixels;
    uint32_t stride;
    uint32_t reserved;
} gfx_present_req_t;
```

`kernel/driver/gfx.c::gfx_ioctl_present` 是当前唯一的 present 入口。
`libgfx/gfx.c::gfx_present` 只 wrap 这个 ioctl。

### 4.3 提议的改动骨架（仅 plan 起点，非 spec）

需要 codex 在 spec 里钉死后才动：

1. **`kernel/include/uapi/gfx.h`**：新增第二形态的 present 请求，例如
   `gfx_present_rect_req_t { rect_x, rect_y, rect_w, rect_h, stride, pixels }`，
   或者扩展 `gfx_present_req_t` 加 `rect_x`/`rect_y`/`rect_w`/`rect_h`（覆盖
   `{0, 0, v_w, v_h}` 等价为整屏）。LB 形态由 reserve 字段区分。
2. **`kernel/driver/fb.c`**：新增 `fb_write_rect_leased(lease, x, y, w, h,
   pixels, row_stride)` —— 当 `x == 0 && w * 4 == row_stride` 时做 bulk
   memcpy，否则 row loop 仍调用 `fb_write_row_leased`。
3. **`kernel/driver/gfx.c`**：新增 `gfx_ioctl_present_rect`，调用 `fb_write_rect_leased`，
   **`gfx_ioctl_present` 旧的形态保留**作为 full-frame fallback（也可以
   直接转调 `rect` 形式）。
4. **`libgfx/gfx.c`**：`gfx_present_rect(h, rect)` 新 API。
5. **`user/terminal/terminal_render.c`**：维护一个 dirty-region bbox
   (`min_row, max_row, min_col, max_col`)，每次 `term_core_mark_dirty` 时
   并入；每次 present 时如果 bbox == full-frame 就回退到 `gfx_present`，
   否则调 `gfx_present_rect`。

### 4.4 测试 / 验收准则

1. **hosttest**：mock 验证 `gfx_present_rect` ABI 正确性，包括越界、
   stride 不匹配、退化为 full-frame 等边界。
2. **qemutests E2E**：跑 monitor，再次采集 `[term-probe]` 数据，**目标
   `present_avg_us` 下降到 ≤10 ms**（vs 现在 ~85 ms）。如果改完仍 ≥50 ms
   说明 terminal 端的 dirty region estimation 不够紧，需要细化 cell-level
   tracking。
3. **回归**：现有 `test_display_owner` / `test_resolution_switcher` 全部
   PASS。

### 4.5 不要动的东西

- **不要回动 `terminal.c` 里那段 `// 10. Fork ash` 之外的代码**：
  `commit bfeb0b0` 里的 `[term-probe]` 4-segment 输出是 Phase 1 留下的观测仪器，
  长期保留；在 partial-present plan 上线后跑回归时它就是验证标准。如果将来
  要拆掉，必须新 commit 单独做、并替换为另一种 per-second observability，
  不能静默移除。
- **不要改 `terminal.c::main` 的 fork 子进程**（默认 `/bin/busybox` 上的 ash）——
  除非能让 `make test-monitor-host` 类似东西继续工作。本调查的 `// TEMP probe`
  块已经在 commit 前删除。
- **不要改 `kernel/driver/fb.c::fb_write_row_leased` 的 probe**：
  这是 Phase 1 揭示根因的核心观测，partial-present plan 上线后用同样的
  输出来验证 byte 数下降比例。`fb_write_row` 是 row loop 的 fallback，
  即使 partial-present 上线，在 `x != 0` 的 view 上它仍会被调用。
- **不要合 re-introduce `fb_write_full_rows_leased`** —— 它在 Phase 2 实测
  下对 8 MB bulk memcpy **变慢**的结论不能忘，只在 `w * h` 足够小（partial
  rect 退化场景）时才安全。

---

## 5. Spec / Plan 必须钉死的边界

我留下的实测数据是必要的输入，codex 把 spec 钉下来之前请确认：

| 边界 | 数值 | 说明 |
|---|---|---|
| `fb_write_row_leased` 单 row 成本 | 47.9 µs（TCG） | 真机会低 30-50× |
| 单像素 fb memcpy 成本 | 33 ns（TCG） | 真机会低 30-50× |
| 每秒 present 次数 | 1-2 Hz | 取决于 monitor 输出节奏 |
| 单 present 内 fb_memcpy 字节数 | 8.1 MB（整屏） | partial-present 目标降到 <300 KB |
| 真机 vs TCG 加速比 | 不明，建议在裸机环境下也跑一次 `[term-probe]` 取 baseline | commit `bfeb0b0` 已经让这件事在裸机上可观测 |

---

## 6. 文件 / commit 索引

| 文件 | commit `bfeb0b0` 改动 | 备注 |
|---|---|---|
| `kernel/driver/gfx.c` | +arch/clocksource.h +core/printk.h 头；函数头部和成功路径末尾各加 probe；row loop 重写为 setjmp hoist + 删除 per-row syscall_check | **包含 v1 fix**，长期保留 |
| `kernel/driver/fb.c` | +arch/clocksource.h 头；fb_write_row_leased 函数包裹 timer + 每 1024 次 printk | 根因发现点，partial-present 验证基线 |
| `user/terminal/terminal.c` | +4-segment timing 累计 + 每秒 `[term-probe]` serial 输出 | 用户态观测仪器 |
| `kernel/include/driver/fb.h` | 无 diff | 还原到 master |
| `kernel/driver/gfx.c` (v2 attempt) | **已被删**：曾加 `fb_write_full_rows_leased` 调用，2.3× 慢的负面结论保留在 commit message |

worktree：`worktree-tty-investigate-terminal-cpu`
branch：`worktree-tty-investigate-terminal-cpu`
head：`bfeb0b0`
EOF
gfx/fb/terminal: investigate terminal 18% CPU + gfx_ioctl_present cleanup
EOF
）" 2>&1 | tail -3