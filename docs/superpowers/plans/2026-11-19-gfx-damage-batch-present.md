# gfx 损伤区域批量 present 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增 `GFX_PRESENT_RECTS` 批量 present ABI（内核 + libgfx），terminal 与 LVGL 后端迁移为一次逻辑刷新一次批量提交。

**Architecture:** UAPI 加一个矩形数组 ioctl（0x470D），内核路径逐条镜像 `GFX_PRESENT_RECT` 的阶段顺序（整包校验 → 单租约 → 单 setjmp → 逐行 bounce 宽写），加 `completed_rects` INOUT 出参；libgfx 加薄包装；terminal `display_submit` 列表路径改单次批量 ioctl（NULL hook 保留旧逐矩形路径）；LVGL 后端 flush_cb 累积脏矩形、present 点批量替代整视图，溢出回退 full。

**Tech Stack:** C99/fixed-width UAPI、hosttests（真实生产源码 + mock）、QEMU E2E（fb_pixel 逐像素比对 + terminal-present 门禁）。

**Spec:** [docs/superpowers/specs/2026-11-19-gfx-damage-batch-present-design.md](../specs/2026-11-19-gfx-damage-batch-present-design.md)（实现争议以 spec 为准）

## Global Constraints

- UAPI 固定宽度类型；`gfx_rect_t` sizeof==16；`gfx_present_rects_req_t` sizeof==32，offsetof 断言 pixels=0/stride=8/reserved=12/rects=16/count=24/completed_rects=28。
- `GFX_PRESENT_RECTS=0x470D`、`GFX_PRESENT_RECTS_MAX=64u`。`reserved`、`completed_rects`（入向）非 0 → EINVAL。
- 任何拒绝路径（几何/range/count/ENOMEM/租约失败）**零 fb 写入**；用户 fault 前缀行可见、`completed_rects`=k。
- 不改 `kernel/driver/fb.c`、`fb_state.c`、租约协议、A/B 诊断 ioctl；旧 present 路径错误契约不变。
- 新内核日志只用 `log_debug`/`debug_<channel>`；不新增 `serial_printk`/`color_printk`。
- `[term-present] version=1` 行格式不变；`gfx_present_stats` 只追加字段。
- libgfx 不做 ENOTTY fallback、不路由批量到 `gfx_present`、不缓存用户指针。
- 头文件变更后先重建再跑测试（警惕陈旧 sysroot 头与陈旧归档）。

## Review Focus

1. **批中 fault 语义**：第 i 矩形中途 fault → 前 k 个矩形完整可见、第 i 矩形已写行可见、i 之后未写、errno=EFAULT、`completed_rects`=k。Task 2 host 断言 + Task 6 QEMU。
2. **拒绝零写入**：count 超限/某矩形越界/rects 数组不可读/EACCES → fb sentinel 不动。Task 2 sentinel 断言。
3. **completed_rects 协议**：入向非 0 → EINVAL；写回 fault 不改 rc；libgfx 出参 NULL 安全、unknown→0。Task 2/3 断言。
4. **重试不丢不重**：terminal 批量失败后损伤列表整体保留、k 折算 `failed_completed_rect_bytes`；LVGL 失败保留脏列表、成功才清。Task 4/5 断言。
5. **溢出与空批回退**：LVGL 第 65 块脏区 → overflow→full，内容不丢；`pending_present` 但列表空 → full；count==0 不出 ioctl。Task 5 断言。

---

### Task 1: UAPI 头扩展

**Files:**
- Modify: `kernel/include/uapi/gfx.h`（`GFX_TEST_PRESENT_AB` 块之后追加）

**Interfaces:**
- Produces（后续所有任务消费）:
  - `#define GFX_PRESENT_RECTS 0x470D`、`#define GFX_PRESENT_RECTS_MAX 64u`
  - `typedef struct { uint32_t x, y, w, h; } gfx_rect_t;`
  - `typedef struct { uint64_t pixels; uint32_t stride; uint32_t reserved; uint64_t rects; uint32_t count; uint32_t completed_rects; } gfx_present_rects_req_t;`
  - 字段注释照 spec §4 抄（pixels=完整 view 缓冲首址、rects=数组用户指针、completed_rects 入 0/出 k）。

- [ ] **Step 1: 追加 UAPI 定义**：在 `GFX_TEST_PRESENT_AB` 之后加命令号、`gfx_rect_t`、`gfx_present_rects_req_t`，附 sizeof/offsetof `_Static_assert`（Global Constraints 的精确值）；每个字段带 spec §4 语义注释。
- [ ] **Step 2: 验证 ABI 断言编译 + 旧测试不回归**：`make test-host`（新头会随 sysroot 传播到 libgfx/terminal/gui 的宿主编译）。Expected: 全绿，无新告警。
- [ ] **Step 3: 确认 sysroot 传播**：`grep -r GFX_PRESENT_RECTS build/*/sysroot/usr/include/uapi/gfx.h`（profile 目录名以实际构建为准）。Expected: 命中。
- [ ] **Step 4: Commit** `feat(gfx): add GFX_PRESENT_RECTS batch present UAPI`

### Task 2: 内核批量 present 路径

**Files:**
- Modify: `kernel/arch/x86_64/driver/gfx.c`（dispatch 约 :815 `GFX_PRESENT_RECT` case 之后；新 handler 与 stats 辅助放在 present_rect 实现之后）
- Test: `hosttests/cases/test_gfx_device.c`（TEST_LIST 追加）

**Interfaces:**
- Consumes: Task 1 的 UAPI；现有 `fb_gfx_writer_begin`/`fb_write_row_leased`/`fb_writer_end`/`syscall_check_user_range`/`fault_jmp` 机制（gfx.c 内已有）。
- Produces: dispatch case `GFX_PRESENT_RECTS` → `static long gfx_ioctl_present_rects(...)`（签名与参数传递方式与 `gfx_ioctl_present_rect` 完全同构）。

- [ ] **Step 1: 写失败测试**（`test_gfx_device.c`，全部镜像文件内现有 case 的夹具/断言风格；视图非零原点，sentinel 填充 fb 堆缓冲）：
  - `test_rects_happy_two_rects_exact_writes`：两不相交矩形，逐像素断言只这两块变化、rect 外与行 padding sentinel 不动、`g_write_row_*` 计数吻合；请求 `completed_rects` 入 0、成功后出 count。
  - `test_rects_reject_zero_writes`：逐字段矩阵——count=0、count=MAX+1、rects=NULL、reserved!=0、completed_rects!=0、stride 错、每个矩形字段越界（x/y/w/h 各一）→ rc=EINVAL 且 fb sentinel 零变化、零 write_row。
  - `test_rects_rects_array_fault`：rects 指向无效用户地址 → EFAULT、零写入。
  - `test_rects_mid_fault_prefix`（用 gfx_fault_bridge）：3 矩形，第 2 个中途 fault → 前缀可见、第 3 个未写、EFAULT、写回 k==1。
  - `test_rects_eacces_zero_writes`：非 owner → EACCES、零写入、租约已释放（后续 present 正常）。
  - `test_rects_enomem_cleanup`：`g_kmalloc_fail_after` 注入 → ENOMEM、租约/bounce/fault 上下文清理。
  - `test_rects_overlap_in_order`：两重叠矩形，后者内容胜出。
- [ ] **Step 2: 跑测试确认红**：`make test-host`。Expected: 新 case 全 FAIL/编译错（ENOTTY 或符号缺失）。
- [ ] **Step 3: 实现**：`gfx_ioctl_present_rects` 阶段顺序照 spec §5（镜像 `gfx_ioctl_present_rect` 的结构：请求容错复制 → 短锁快照 → 无锁校验（单遍：逐矩形几何 + 包络 range check + max_w）→ `kmalloc(max_w*4)` → 单租约 → 单 setjmp → 按序逐矩形逐行 memcpy+bounce+`fb_write_row_leased` → best-effort 写回 completed_rects → 清理）。统计：`gfx_present_stats` 行追加 `batch=%llu batch_avg_ns=%llu`，批量帧计入既有 `bytes`/`failed` 与每 256 次打点；失败前缀字节折叠沿用现有模式。
- [ ] **Step 4: 跑测试确认绿**：`make test-host`（全套，含旧 present/rect case 不回归）。Expected: PASS。
- [ ] **Step 5: Commit** `feat(gfx): implement GFX_PRESENT_RECTS batch present`

### Task 3: libgfx `gfx_present_rects`

**Files:**
- Modify: `libgfx/gfx.h`（`gfx_present_rect` 声明之后）、`libgfx/gfx.c`（`gfx_present_rect` 实现之后）
- Test: `hosttests/cases/test_gfx_client.c`

**Interfaces:**
- Consumes: Task 1 UAPI（sysroot 头）；Task 2 的内核命令号。
- Produces: `int gfx_present_rects(gfx_handle_t *h, const gfx_rect_t *rects, uint32_t count, uint32_t *completed_out);`（spec §4 签名注释一并写入 gfx.h）

- [ ] **Step 1: 写失败测试**（`test_gfx_client.c`，镜像现有 mock_ioctl_log 夹具）：
  - `test_present_rects_one_ioctl_request_shape`：合法 2 矩形 → 恰一次 ioctl，cmd==GFX_PRESENT_RECTS，arg 结构逐字段断言（pixels==handle 缓冲、stride==info.stride、reserved==0、completed_rects 入 0、rects 数组指针、count）。
  - `test_present_rects_shortcircuit`：NULL handle、NULL rects(count>0)、count=0、count=MAX+1、单个矩形越界 → -1/EINVAL 且 `mock_ioctl_count` 不增、`completed_out` 写 0。
  - `test_present_rects_errno_passthrough`：mock ioctl 返回 -1/EAGAIN / EINVAL / ESTALE → 返回 -1 且 errno 原样；成功不碰 errno；`completed_out`==count（成功）。
  - `test_present_rects_completed_out_null`：`completed_out==NULL` 不崩、不写。
  - `test_present_rects_fullview_no_route`：单个全 view 矩形也走 GFX_PRESENT_RECTS（不路由 gfx_present）。
  - `test_present_rects_writeback_unknown_zero`：mock ioctl 失败且不回写 completed_rects → `completed_out`==0。
- [ ] **Step 2: 跑测试确认红**：`make test-host`。Expected: 编译错（符号缺失）。
- [ ] **Step 3: 实现**：`libgfx/gfx.c` 镜像 `gfx_present_rect` 的前置校验风格；一次 `ioctl(fd, GFX_PRESENT_RECTS, &req)`；返回前按 spec §4 折算 `completed_out`（成功 count、失败取内核写回值、写回未落地 0、NULL 不写）；gfx.h 加契约注释（含「全 view 不路由」差异说明）。
- [ ] **Step 4: 跑测试确认绿**：`make test-host`。Expected: PASS。
- [ ] **Step 5: Commit** `feat(libgfx): add gfx_present_rects batch wrapper`

### Task 4: terminal 迁移

**Files:**
- Modify: `user/terminal/terminal_display.h`（ops 结构 `gfx_present_rect` 字段之后）、`user/terminal/terminal_display.c`（`real_present_rect` 附近 + `display_submit` 列表路径）
- Test: `hosttests/cases/test_terminal_display.c`

**Interfaces:**
- Consumes: Task 3 的 `gfx_present_rects`。
- Produces: ops hook `int (*gfx_present_rects)(gfx_handle_t *h, const gfx_rect_t *rects, uint32_t count, uint32_t *completed_out);`；`terminal_display_init` 默认接线 `real_present_rects`（薄包装直调 libgfx，镜像 `real_present_rect`）。

- [ ] **Step 1: 写失败测试**（镜像现有 fake ops 注入风格）：
  - `test_display_batch_one_ioctl_per_round`：3 矩形损伤 + 新 hook → batch hook 恰调 1 次、参数矩形/顺序与损伤一致、旧 `gfx_present_rect` hook 调 0 次、ack 生效。
  - `test_display_batch_failure_keeps_list_and_k_bytes`：batch hook 失败并回写 k=1 → 整列表保留（下轮重试同列表）、`failed_completed_rect_bytes`==rects[0].w*h*4、errno 分类走既有 retry 路径。
  - `test_display_batch_null_hook_legacy_loop`：新 hook NULL → 旧逐矩形路径（rect hook 每矩形一次）。
  - `test_display_batch_metrics_anti_dilution`：noop/skipped 混入不改变 partial_frames 分母；`rect_calls` 每批量轮 +1。
- [ ] **Step 2: 跑测试确认红**：`make test-host`。Expected: 新 case FAIL（hook 未接、走 legacy）。
- [ ] **Step 3: 实现**：`display_submit` 列表路径按 spec §6：栈上 `gfx_rect_t batch[TERM_DAMAGE_MAX_RECTS]` 从 `damage.rects[]` 拷贝 → 新 hook 存在且 `count<=GFX_PRESENT_RECTS_MAX` 走单次批量（成功/失败记账按 spec，`failed_completed_rect_bytes += Σ_{i<k}`）；hook NULL 或 count 超限（注释说明 renderer 容量 64 实际不可达，保守）走现循环/full。`[term-present]` 行不动。
- [ ] **Step 4: 跑测试确认绿**：`make test-host`（含既有 terminal present/retry/metrics case 全绿）。Expected: PASS。
- [ ] **Step 5: Commit** `feat(terminal): submit damage list via one batch present ioctl`

### Task 5: LVGL 后端脏矩形批量 present

**Files:**
- Modify: `libgui/internal/gui_platform_private.h`（struct 尾部加脏列表字段）、`libgui/src/platform.c`（flush_cb ONSCREEN 分支 + step present 点 + create/enter_terminal_state 重置）
- Test: `hosttests/cases/test_gui_platform.c`；`hosttests/mock/gui_test_runtime.[ch]`（加 `__wrap_gfx_present_rects` 记录器）

**Interfaces:**
- Consumes: Task 3 的 `gfx_present_rects`（libgui 链接 libgfx，wrap 生效）。
- Produces（private.h 内）: `#define GUI_DIRTY_MAX 64u` + 字段 `gfx_rect_t dirty_rects[GUI_DIRTY_MAX]; uint32_t dirty_count; bool dirty_overflow;`
- Produces（mock）: `__wrap_gfx_present_rects` 记录 (count, rects 内容, completed_out 写回值) + 全局 `g_gui_test_fail_gfx_present_rects`/`g_gui_test_present_rects_calls`/记录数组 + `gui_test_reset()` 清零（镜像 `__wrap_gfx_present` 现有模式）。

- [ ] **Step 1: mock 加包装**：`gui_test_runtime.c/h` 加 `__wrap_gfx_present_rects`（默认成功写回 count；`g_gui_test_fail_gfx_present_rects` 置 errno=EAGAIN 失败）与记录全局；`gui_test_reset()` 清零。
- [ ] **Step 2: 写失败测试**（`test_gui_platform.c`，镜像现有 flush→step 驱动模式）：
  - `test_platform_batch_present_one_ioctl_exact_rects`：两次 flush 两个 area（触发 `__wrap_gfx_sprite_blit`）后 step → `gfx_present` 调 0 次、`gfx_present_rects` 恰 1 次、rects 与 area 一致、成功后列表清空（再 step 无新 ioctl）。
  - `test_platform_batch_overflow_falls_back_full`：65 个 flush area → step 走 `gfx_present`（full）恰 1 次、rects 调 0 次、pending 清除。
  - `test_platform_batch_retry_keeps_dirty`：present 失败 EAGAIN → 下一步不立即重试（deadline）过 deadline 后再 step → 重试提交「旧列表∪新增」，成功后清空。
  - `test_platform_offscreen_no_dirty`：OFFSCREEN 模式无累积无 ioctl。
  - `test_platform_terminal_state_resets_dirty`：进入终态后 dirty_count/overflow 归零。
- [ ] **Step 3: 跑测试确认红**：`make test-host`。Expected: 新 case FAIL（无字段/未调用 batch）。
- [ ] **Step 4: 实现**：按 spec §7——`gui_dirty_record()`（overflow 置位并清列表；否则追加）；flush_cb ONSCREEN blit 后记录 area；present 点 `dirty_overflow||dirty_count==0 → gfx_present` else `gfx_present_rects`；成功清列表/overflow/pending；失败保留（既有 classify/retry 不动）；create 与 enter_terminal_state 重置；OFFSCREEN 不记录。
- [ ] **Step 5: 跑测试确认绿**：`make test-host`（含全部 test_gui_* 回归）。Expected: PASS。
- [ ] **Step 6: Commit** `feat(gui): batch-present LVGL dirty rects instead of full view`

### Task 6: QEMU 像素级与 E2E 验收

**Files:**
- Modify: `user/fb_pixel_test/fb_pixel_test.c`（加 `FRAME_BATCH 4u` 帧型：一次 `gfx_present_rects` 提交两个不相交矩形，几何镜像 FRAME_RECT 的常量风格）、`qemutests/fb_pixel_verify.py`（加 batch 帧断言：batch 前后逐像素比对，两矩形外零变化）
- Test: 现有 QEMU 套件（无新增宿主测试文件 → test-catalog 不变）

- [ ] **Step 1: fb_pixel_test 加 FRAME_BATCH**：镜像 `FRAME_RECT` 分支（`fill_full` 两矩形区域后一次批量 ioctl，帧后输出 marker）。
- [ ] **Step 2: fb_pixel_verify.py 加 batch 断言**：镜像既有 rect 前后帧比对逻辑，断言仅两矩形内像素变化。
- [ ] **Step 3: 跑 QEMU 套件**：
  - `make test-qemu SUITE=gfx`（gfx E2E + fb-pixel 场景；若 fb-pixel 场景挂在其他 suite，先 `grep -rn fb_pixel mk/components/run.mk` 确认归属再跑对应 suite）
  - `make test-qemu SUITE=terminal-present`（批量迁移后的 300 KB/帧、10 ms/帧门禁 + baseline manifest 不回归）
  - `make test-qemu SUITE=gui`（LVGL 批量 present 桌面 E2E）
  Expected: 全 PASS；fb-pixel batch 断言通过（kernel 只写两矩形区域的像素级证据）。
- [ ] **Step 4: 失败则修复并重跑**；成功 Commit `test(gfx): QEMU pixel-level batch present verification`

### Task 7: 文档与全量验证

**Files:**
- Modify: `docs/gui/gui.md`（修 :113 陈旧「无 dirty-rect present」表述；新 UAPI 小节；补 spec §8 复制评估结论）、`docs/changelog.md`（顶部新条目，格式照现条目 `- feat(gfx,gui,terminal): **标题**（sha）: 做了什么/验收/已知边界`）、`docs/roadmap.md`（:162 任务行回填完成状态与验收证据）

- [ ] **Step 1: 更新三处文档**（changelog 的 sha 用 Task 1–6 的实际 commit）。
- [ ] **Step 2: 全量验证**：`make test-host` && `make test-static` && `make test-qemu SUITE=gfx` && `make test-qemu SUITE=terminal-present`。Expected: 全绿。
- [ ] **Step 3: 若有新测试文件/场景变更**：`make regen-test-catalog` 并纳入生成物（本计划默认无新文件，仅核对）。
- [ ] **Step 4: Commit** `docs: record damage batch present delivery`

## Self-Review 备忘

- Spec §5.9 统计字段、§6 metrics 语义、§7 重置时机均已落 Task 2/4/5 步骤。
- fb_pixel 取代 test_gfx batch-surface 模式：像素级证据同一强度（宿主直读 fb 内存逐像素比对），且免新增 scenario 注册——对 spec §9 的简化，Task 7 文档里注明。
- 类型一致性：`gfx_rect_t`/`gfx_present_rects` 签名在 Task 1/3/4/5 完全一致（含 `uint32_t *completed_out` 尾参）。
