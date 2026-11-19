# gfx 损伤区域批量 present 设计规范

日期：2026-11-19。状态：待评审；本文定义方案与验收，不授权开始实现。

输入：[roadmap P3 优化项「脏矩形与批量提交」](../../roadmap.md)；基座契约：[terminal partial-present](2026-10-12-terminal-partial-present-design.md)、[2D graphics API](2026-09-30-2d-graphics-api-design.md)。基线 commit `33d67ba7`。

## 1. 目标与范围

roadmap 任务的三个要求，逐条落成验收：

1. **一次逻辑刷新最多一次内核提交**：terminal 部分损伤从「每矩形一次 `GFX_PRESENT_RECT`」（至多 64 次 ioctl）改为一次批量 ioctl；LVGL 后端保持每步一次提交，但从「整视图 present」改为「本步脏区域批量 present」。
2. **受边界校验的损伤区域/批量 present ABI**：新增 `GFX_PRESENT_RECTS`，矩形数组 + 计数上限，逐矩形减法式边界校验；任一矩形非法则整批拒绝、零 framebuffer 写入。
3. **减少额外缓冲复制的评估**：只评估、不改实现（用户已确认）。结论写入本文与 `docs/gui/gui.md`。

验收（对应 roadmap）：静态界面零无效提交；小区域变化上传字节下降（内核只写 Σ w×h×4）；越界/用户指针防护全部保留。

范围：UAPI + 内核 x86_64 gfx 驱动 + libgfx + terminal display 迁移 + LVGL 后端迁移 + hosttests/QEMU 测试 + 文档。

不包含：page flip/vsync、`/dev/fb` mmap 路径、fb_write_row 租约协议变更、A/B 诊断 ioctl 语义变更、aarch64 显示设备、内核侧损伤累积（损伤跟踪留在用户态）、packed-rect 像素格式。

## 2. 当前代码事实（已核实）

- `GFX_PRESENT_RECT=0x4709` 已是**机制上通用**的单矩形路径：减法式校验、整包包络 `syscall_check_user_range`、单租约、单 setjmp、逐行 bounce；但 libgfx 之外只有 terminal 在用，UAPI 无数组/批量形态。
- `GFX_TEST_PRESENT_AB=0x470C` 是最大命令号；`0x470D` 空闲。
- terminal `display_submit`（`user/terminal/terminal_display.c`）：full → 1 次 `gfx_present`；count==0 → noop；列表 → 逐矩形 `ops.gfx_present_rect`，首个失败即停、保留损伤。[term-present] version=1 行格式被 `qemutests/terminal_present_test.py` 正则与 baseline manifest 钉死。
- LVGL `platform_flush_cb`（`libgui/src/platform.c`）：ONSCREEN 下每次 flush 把 area blit 进私有缓冲并置 `pending_present`；step 末尾至多一次**整视图** `gfx_present`。EAGAIN 走 250 ms retry deadline；LOST_OWNERSHIP/DISPLAY_CHANGED/error 进终态。OFFSCREEN 不 present。
- 内核 `gfx_present_stats`（1 Hz 节流 `log_debug`）**无测试钉死格式**，可扩展字段。
- x86_64 未启用 SMAP（`uaccess.c` 仅注释提及 lazy SMAP），生产 present 用 `__builtin_memcpy` 直读用户指针；安全性来自整包 range check + fault_jmp + `fb_write_row_leased` 的目的端校验。
- 复制开销现状：每行两次拷贝（user→bounce memcpy，bounce→fb 宽写）。历史实测 fb 宽写 ≈33 ns/pixel 主导；bounce memcpy 来自 WB 匿名页，量级远小于 fb 写入。`GFX_TEST_PRESENT_AB` 已能输出 `copy_ns`/`fb_ns` 分段。

## 3. 方案选择

| 方案 | 收益与限制 | 决策 |
|---|---|---|
| A：批量矩形数组 ioctl（一次 ioctl 提交本次损伤列表） | 一次逻辑刷新一次提交；逐矩形校验语义与 0x4709 完全一致；损伤跟踪留在用户态 | **采用** |
| B：内核侧损伤累积（DAMAGE_ADD/DAMAGE_FLUSH） | 内核新增 per-view 状态与生命周期；用户态（terminal、LVGL）本就有损伤列表，内核累积只增复杂度 | 不采用 |
| C：仅 bbox 合并（单矩形提交包围盒） | 最简单，但分散脏区间的空白照传，浪费上传字节 | 不作为批量替代 |

packed-rect（每矩形独立像素指针/紧缩缓冲）不采纳：要求用户态重排像素 = 额外复制，与任务③方向相反；沿用「pixels = 完整 view 私有缓冲首地址」契约。

## 4. UAPI 与 libgfx 契约

新增 `GFX_PRESENT_RECTS=0x470D`，保留全部旧命令号、旧结构尺寸、旧 reserved 校验；不得以旧 reserved 区分请求版本。

```c
#define GFX_PRESENT_RECTS      0x470D
#define GFX_PRESENT_RECTS_MAX  64u   /* 与 TERM_DAMAGE_MAX_RECTS 对齐 */

typedef struct {                  /* view-local 像素，半开语义同 0x4709 */
    uint32_t x;
    uint32_t y;
    uint32_t w;                   /* > 0 */
    uint32_t h;                   /* > 0 */
} gfx_rect_t;                     /* 16 B */

typedef struct {
    uint64_t pixels;          /* in：完整 view 私有缓冲首地址（非 packed） */
    uint32_t stride;          /* in：必须 == view_w*4 */
    uint32_t reserved;        /* in：0 */
    uint64_t rects;           /* in：gfx_rect_t 数组的用户指针 */
    uint32_t count;           /* in：1..GFX_PRESENT_RECTS_MAX */
    uint32_t completed_rects; /* in：0；out：失败前完整写成的矩形数 */
} gfx_present_rects_req_t;        /* 32 B */
```

ABI 固定宽度类型 + sizeof/offsetof 静态断言：pixels=0、stride=8、reserved=12、rects=16、count=24、completed_rects=28、sizeof==32（`gfx_rect_t` sizeof==16）。

```c
/* completed_out（可 NULL）：成功写 count；失败写内核报告的已完成矩形数
 * k；k 不可知（写回未落地）时显式写 0。本签名与 k 交付约定为设计裁定。 */
int gfx_present_rects(gfx_handle_t *handle,
                      const gfx_rect_t *rects, uint32_t count,
                      uint32_t *completed_out);
```

- 语义逐条沿用 `GFX_PRESENT_RECT`：坐标 view-local；目的地址 = view origin + (x,y)；stride 严格 `view_w*4`；不隐式裁剪；绘制 clip 不影响 present 请求。
- 每矩形校验 `w>0 && h>0 && x<=view_w && w<=view_w-x`（y/h 同理）；**任何**矩形非法 → EINVAL，整批拒绝、零 fb 写入。
- count==0 → EINVAL（调用层 bug，全栈一致：内核 EINVAL；libgfx 发 ioctl 前返回 -1/EINVAL）。count>GFX_PRESENT_RECTS_MAX → EINVAL。rects 为 NULL/不可读/复制 fault → EFAULT（零 fb 写入）。
- 矩形间允许重叠，按数组顺序处理，后写覆盖先写；不承诺批内或批间画面原子性。
- `completed_rects`：入口必须传 0（否则 EINVAL）。成功时写回 `count`；失败时写回失败点之前**完整**写成的矩形数 k（第 k+1 个矩形的已写行前缀不计入、保持 unknown）。写回是尽力而为（best-effort `copy_to_user_ft`）：写回 fault 不改变本次 ioctl 的返回值与 errno；此时用户态拿到的 k 视为 0（unknown 由 `failed_prefix=unknown` 字面量与内核侧 bytes 统计补足——设计裁定）。
- libgfx：NULL handle、NULL rects（count>0）、count==0、count>MAX、任一矩形几何非法 → 发 ioctl 前返回 -1/EINVAL；`completed_out` 非 NULL 时在上述发 ioctl 前的失败路径写 0。合法请求恰发一次新 ioctl；每次重建请求，不缓存用户指针；成功不改 errno，失败透传 ioctl errno；不自动探测旧 kernel，不在 ENOTTY 静默 fallback（仓库内 kernel 与 libgfx 同步发布）。批内矩形即使覆盖全 view 也不路由到 `gfx_present`（直通，与单矩形「full-view 路由」不同，行为差异写入 gfx.h 注释）。

## 5. Kernel 执行与安全边界

`gfx_ioctl_present_rects` 阶段顺序与 0x4709 一致：容错复制请求 → 短锁快照 configured view/`mode_seq` → 无锁校验 → 单租约 → 逐矩形逐行 bounce 写入 → 清理。

1. 请求结构体 `syscall_check_user_range` + `copy_from_user_ft`；`reserved!=0`、`completed_rects!=0`、stride 不符 → EINVAL（零 fb 写入）。
2. `count` 上限与 `rects` 数组：`syscall_check_user_range(rects, count*16)`（乘法 uint64 防溢出）+ 一次 `copy_from_user_ft` 进 `kmalloc(count*16)`；失败 EFAULT（零 fb 写入）。
3. 单遍校验循环：逐矩形几何（减法式）；同遍计算 `max_w = max(w)` 与每矩形包络 `first = pixels + y*stride + x*4`、`len = (h-1)*stride + w*4`，全部乘加 uint64 显式防溢出；逐矩形 `syscall_check_user_range(first, len)`。任何失败按各字段语义返回（EINVAL/EFAULT），零 fb 写入。包络内行间 padding 必须可读但只拷贝每行 `w*4`。
4. bounce 缓冲一次 `kmalloc(max_w*4)`，全部矩形共用；ENOMEM → -ENOMEM（零 fb 写入）。
5. `fb_gfx_writer_begin` 一次（EACCES/EAGAIN/ESTALE/EIO 原样返回，零 fb 写入）。
6. 复用单次 fault 上下文（`fault_jmp`/`fault_cleanup`/`fault_cleanup_arg` 保存恢复）；`__builtin_setjmp` 一次覆盖整批。矩形按序、行按序：`memcpy` bounce → `fb_write_row_leased(lease, view_x+x, view_y+y+i, bounce, w*4)`。用户读 fault → -EFAULT（当前矩形已写行保持可见，后续矩形不执行，`completed_rects`=k）；`fb_write_row_leased` 失败 → 同样中止并返回其负值。
7. 清理路径全覆盖：正常/ENOMEM/EFAULT/写行失败都归还租约、释放 bounce、恢复 fault 上下文；setjmp 后只读 longjmp 后仍有效的变量。
8. 成功路径 best-effort 写回 `completed_rects=count`，失败路径写回 k；写回结果不影响返回值。返回前不得持任何自旋锁做 uaccess。
9. 统计：`gfx_present_stats` 追加批量字段（`batch=<成功批量帧数> batch_avg_ns=<...>`），`bytes` 与 `failed` 继续累计；`gfx_present: count=` 的每 256 次打点纳入批量帧。新日志只走 `log_debug`/`debug_<channel>`。
10. 失败前缀字节折叠沿用既有 full/rect 的累计模式，批量失败的前缀字节计入内核侧 bytes（用户态不可靠推算的部分由 `completed_rects` + unknown 标注补足）。

`kernel/driver/fb.c`、`fb_state.c`、租约协议、A/B ioctl 全部不动。

## 6. terminal 迁移（N 次 ioctl → 1 次）

`terminal_display_ops_t` 新增 `gfx_present_rects` hook，签名镜像 libgfx（含 `completed_out` 出参，fake 借此表达 k）。`display_submit` 列表路径改为：

- hook 存在且 `damage.count<=GFX_PRESENT_RECTS_MAX`：构造栈上 `gfx_rect_t[TERM_DAMAGE_MAX_RECTS]`，一次 `gfx_present_rects(gfx, rects, count, &k)`。成功 → `partial_frames++`、`success_bytes += Σ w*h*4`、ack、清 retry；失败 → `failed_attempts++`、`failed_completed_rect_bytes += Σ_{i<k} w*h*4`（k 来自出参；写回未落地时 libgfx 已折算为 0）、整个损伤列表保留（重试覆盖此前已成功区域，与现行为等价）。
- hook 为 NULL：**保留现逐矩形循环**（注入测试兼容路径，不用于掩盖生产 ENOTTY——真实 terminal 必须接线新 hook）。
- `damage.count>GFX_PRESENT_RECTS_MAX`：现 renderer 容量即 64，实际不可达；仍按 `want_full=true` 保守处理并注释说明。
- `[term-present] version=1` 行格式不变；`rect_calls` 语义改为「present 族 ioctl 次数」（full 一次、批量一次各计 1，与现状 full 路径计 1 一致）；`failed_prefix=unknown` 字面量保留。
- 7.1 节 deadline/classify 机制全部不动：批量失败与原「第 i 个矩形失败」走同一条 `terminal_present_errno_classify` 路径。

## 7. LVGL 后端迁移（整视图 present → 脏区域批量 present）

`gui_platform_t` 新增：固定容量 `GUI_DIRTY_MAX=64` 的矩形列表、`dirty_count`、`dirty_overflow` 标志。

- `platform_flush_cb`（仅 ONSCREEN 且 `gfx` 有效路径）：blit 后把 LVGL area（已校验在 view 内）以 (x,y,w,h) 追加进列表；`dirty_count==GUI_DIRTY_MAX` 时置 `dirty_overflow` 并**清空列表**（full 覆盖一切，列表只为省字节）。不合并相邻矩形（LVGL PARTIAL 模式每次刷新面积数少，合并不值得复杂度）。
- step 末尾 present 点：`pending_present` 时——`dirty_overflow || dirty_count==0` → 一次整视图 `gfx_present`（清列表/标志）；否则一次 `gfx_present_rects(gfx, list, dirty_count)`。
- 失败分类沿用 `gfx_client_classify`：RETRY → 保留列表与 `pending_present`，250 ms deadline 后重试（重试提交的是「失败轮列表 ∪ 等待期新增」，列表超容自然落到 full）；LOST_OWNERSHIP/DISPLAY_CHANGED/error → 进终态，列表保留（恢复流程重建 gfx 句柄时随 platform 重置清零）。
- 成功（批量或 full）→ 清列表、`dirty_overflow=false`、`pending_present=false`。
- 一次刷新 N 个 flush area → 至多一次 ioctl（批量或 full）；静态界面 `pending_present` 不置位 → 零提交（现状保持）。
- OFFSCREEN 模式不累积、不 present（现状保持）；platform create/destroy 重置列表。

## 8. 复制评估（任务③，只评估不实现）

现状每行两次拷贝：user→bounce（`__builtin_memcpy`，WB 源）+ bounce→fb（`fb_copy_rgb32` 32 位宽写）。去掉 bounce、user 直写 fb 的候选被**否决**：

1. 模糊 uaccess/fb 层边界——用户指针进 fb 层正是 2026-10-12 spec §3 明确拒绝的形态；
2. 绕过 `fb_write_row_leased` 的目的端五项校验与租约记账；
3. 收益上限小：bounce 源是 WB 内存，memcpy 吞吐远高于 fb 写入（历史实测 fb 宽写 ≈33 ns/pixel 主导；`GFX_TEST_PRESENT_AB` 的 `copy_ns`/`fb_ns` 分段可在验收时实测占比，预期 bounce <5%）。

批量路径已把 bounce 缓冲从「每矩形一个」降为「整批一个」（`max_w*4`），这是本任务对复制次数的全部改动。后续若要进一步降复制，正确方向是 page flip/双缓冲（另列 roadmap，不在本任务）。

## 9. 验证矩阵

| 层 | 必须覆盖 |
|---|---|
| UAPI/libgfx host | 新结构布局/命令号/`completed_rects` 入 0 校验；count 0/-1 EINVAL（不发 ioctl）/1/MAX/MAX+1；NULL rects；`completed_out` NULL 与非 NULL 两条路径；逐矩形越界（每个字段）；批内重叠按序覆盖；成功不碰 errno、失败透传；全 view 矩形不路由 full |
| kernel gfx host | 多矩形各写各的区域、rect 外 sentinel 不动、行 padding 不写；非零 view origin；单像素/最右下角矩形；任一矩形非法 → 零 fb 写入；rects 数组 copy fault → EFAULT 零写入；批中间行 fault → 前缀可见、后续矩形未写、`completed_rects`==k；EACCES 零写入；ENOMEM/EFAULT/lease 失败清理；非 owner/ESTALE/EAGAIN/EIO |
| terminal display host | 一次批量 ioctl（fake hook 计数==1）；失败整列表保留、`failed_completed_rect_bytes` 用 k 折算；hook NULL 走旧逐矩形路径；count 超 MAX → full；metrics 字段/anti-dilution 不回归 |
| LVGL host/审计 | `gui_build_audit`/`gui_test`/`gui_desktop_test` 全绿；OFFSCREEN 不受影响 |
| QEMU 像素级 | `user/test_gfx` 新增 batch surface 模式：多矩形稀疏写入前后截屏逐像素比对，rect 外不变；`fb_pixel_test` 增 BATCH 帧型，`fb_pixel_verify.py` 对应断言；`terminal_present_test.py` 现有字节/耗时门（300 KB/帧、10 ms/帧）与 baseline 不回归 |
| 静态审计 | 新内核日志走 log_debug；无 serial_printk/color_printk；UAPI 头断言齐 |

新增宿主测试按 `@os01-test` 元数据接入并 `make regen-test-catalog`；QEMU 场景登记进现有 suite（`SUITE=gfx`），scenario-catalog 契约同步。验收入口：`make test-host`、`make test-static`、`make test-qemu SUITE=gfx`、terminal present 套件；「小区域字节下降」的证据 = kernel sentinel 写入范围测试 + QEMU 逐像素比对 + `gfx_present_stats` bytes 前后对照。

## 10. 文件边界与落地约束

- `kernel/include/uapi/gfx.h`：唯一 UAPI 定义，sysroot 同步传播。
- `kernel/arch/x86_64/driver/gfx.c`：新 dispatch 与批量执行路径；与 0x4709 共用内部 helper 时不得改变旧路径的错误契约与验证范围。
- `libgfx/gfx.h`、`libgfx/gfx.c`：公开 `gfx_rect_t`（经 uapi）与 `gfx_present_rects`；handle 继续 opaque。
- `user/terminal/terminal_display.[ch]`：新 hook 与列表路径替换；`terminal_render.[ch]`、`terminal.c` 不动。
- `libgui/src/platform.c`（+ 头）：脏列表与批量 present；`lv_conf.h`、LVGL 本体不动。
- 测试：`hosttests/cases/`、`qemutests/`、`user/test_gfx/`、`user/fb_pixel_test/`；构建注册与 catalog 生成物随变更纳入。
- 文档：`docs/gui/gui.md`（新 ABI 小节 + 修正「无 dirty-rect present」陈旧表述）、`docs/changelog.md`、`docs/roadmap.md` 任务行回填。
- 不新增内核子系统、不改 `boot_context`/syscall 编号；UAPI 变更后 clean 重建验证。
