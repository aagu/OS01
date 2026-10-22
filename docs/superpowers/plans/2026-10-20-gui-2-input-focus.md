# GUI-2 Input and Focus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为单进程全屏 libgui 应用交付独占物理输入、LVGL 控件交互、确定的焦点导航及退出后终端恢复。

**Architecture:** 在 raw keyboard/mouse 与 TTY 分流之前建立与显示 view 绑定的输入租约。libgui 负责非阻塞泵、Set 1 解码、pointer/keypad 和进程内焦点，应用继续拥有 poll 主循环。内核队列失步显式失败；显示与输入生命周期一起收敛到 GUI-1 既有终止状态。

**Tech Stack:** C11、OS01 devfs/poll、PS/2 Set 1、libgfx、libgui、锁定的 LVGL v9.5.0、原生 hosttests、Python unittest、QEMU/QMP。

**Spec:** [2026-10-20-gui-2-input-focus-design.md](../specs/2026-10-20-gui-2-input-focus-design.md)，Draft v2，子 agent 两轮评审后 APPROVE。本文以已提交该 spec 的 `8a1bcf9` 为基准；此前产品代码基准为 `d829b36f`。

**Plan状态：** Revision 2，子 agent 两轮评审后 APPROVE（2026-10-20）；**已实施并通过最终整分支评审（2026-10-22）**。13 个任务全部完成并通过逐任务评审；最终整分支评审 SPEC PASS；唯一的修复波与一次 scoped re-review 全部 ADDRESSED。分支 `feat/gui-2-input-focus`（36 个提交，base `6126504a`），尚未合并、尚未推送。交付证据见 `docs/superpowers/reports/2026-10-22-gui-2-validation.md`。

**执行交接：** 原计划交给 **Luna** 会话使用 `superpowers:executing-plans` 实施。**实际执行改由 `superpowers:subagent-driven-development` 完成（2026-10-22）**：在独立 worktree `/home/aagu/OS01/.worktrees/gui-2-input-focus`（分支 `feat/gui-2-input-focus`）中，按下面的依赖顺序逐任务派发实现子 agent，每个任务配一次 spec + 质量评审，最后做一次整分支评审与唯一修复波。分支**未合并、未推送**。

## Global Constraints

- 单 UI 线程、每进程单 platform、每 platform 至多一个 input；应用持有主循环。
- 公共 sysroot 不暴露 `lv_*` 类型、私有桥接头或测试注入接口；不修改 LVGL 子模块。
- 保留 keyboard raw payload 与 `mouse_event_t` 8 字节 ABI；不关闭 TTY ISIG，不用 termios 实现隔离。
- FIFO 固定 512 项；每次 dispatch 最多 256 keyboard 字节、64 mouse 事件；每 step 最多投递 256 项。
- 重复首次延迟 400 ms、后续 40 ms；无补发风暴；Ctrl/Alt 从无到有取消旧活动键，释放后不恢复。
- 指针 1 计数=1 像素，光标 12×18、热点 (0,0)；垂直滚轮每刻度 32 px；不新增图片/字体依赖。
- 4 MiB TLSF 池；可失败分配逆序回滚，fatal 仍用 `_exit(1)` 与内核 final-close 回收。
- kernel 源/头对称，运行日志使用 `log_*`；改 kernel struct 后 `make clean`。
- 宿主对象全部在选定 profile 的 `host-test`，不得混用目标库/目标 libc；sysroot 仍由单一发布器写入。
- 不扩张到 GUI-3 桌面、GUI-5 窗口服务器、IME、USB HID 或 aarch64 用户态 GUI。
- 所有测试从仓库根 make 入口运行；syscall 用 `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall`，不带 `KERNEL_SELFTEST=1`。

## Review Focus

以下五项是实施时最容易遗漏的故障输入；对应测试已归入任务，不以“后续人工看看”代替：

1. bind 结果 copy_to_user 失败后，新一代同 view 租约不能被旧回滚撤销——Task 4 generation 匹配回滚。
2. 键盘序列前缀到达后切换消费者，不能留下幽灵 Ctrl/Shift——Tasks 2–3 每个序列分片点切换。
3. Enter 按住时控件被回调删除，不能幽灵 click、旧对象访问或吞掉下一键——Task 7 真实 LVGL。
4. GUI 静止且没有 present 时失去前台，不能继续自动输入——Task 6 无 dirty frame 的 owner/error 检查。
5. GUI 已退出但关闭键的 break 尚未到达，shell 不能继承修饰键或执行 GUI 文本——Task 12 PS/2 恢复与泄漏 canary。

## 工作方式、验证入口与文件分工

先读本 plan、spec 和适用 AGENTS.md；存在 `.codegraph/` 时先 `codegraph node/explore` 定位代码，未索引文档/文件再直接读。执行前用 using-git-worktrees 检查是否需要隔离；不要覆盖别人的修改。以下路径以仓库根为基准，跟随实际 worktree，不硬编码原 checkout。

每个任务都先添加断言并跑出预期失败，再实现并跑绿，最后提交该任务列出的文件。RED 可以是缺符号/缺声明，但不得是无关环境失败；记录失败原因。已有行为回归是 GREEN 门禁的一部分。遇到可重复故障使用 systematic-debugging；不靠增大超时、屏蔽断言或改掉验收条件“修复”。

### 宿主 focused 入口（随所属任务引入）

扩展已有 `make PROFILE=x86_64-clang test-gui-host CASE=...`，不新建根测试 bucket：

| CASE | 二进制与引入任务 |
|---|---|
| `devfs-input` | `test_devfs_input_dispatch.elf`，Task 1 |
| `input-frame` | `test_input_ps2_frame.elf`，Task 2 |
| `input-route` | `test_input_route.elf`，Task 3；Task 4 追加 `test_gfx_input.elf`、既有 gfx/client 回归 |
| `input-decode` | `test_gui_input_decode.elf`，Task 5 |
| `input-platform` | `test_gui_input_platform.elf`，Task 6 |
| `input-lvgl` | `test_gui_input_lvgl.elf`，Tasks 7–8 |
| `platform` / `lvgl` | 原 GUI-1 两项，保留 |
| `all` | 所有已注册项，始终与 `TEST_BINS` 接入一致 |

各 CASE 的 hosttests 子 make 目标命名 `test-gui-<case>`，由 root `os01_submake` 转发；不得从 shell 直接 `make -C hosttests`。每新增 binary 同时加入 `hosttests/Makefile::TEST_BINS`、现有 run_hosttests 协议及 focused 入口。未知 CASE 在构建前非零失败；复用现有 `GUI_HOST_REGISTERED_CASES`，不平行维护第二份不同内容的名单。

### 文件责任图

| 单元 | 文件与责任 |
|---|---|
| devfs 适配 | `kernel/include/fs/devfs.h`、`kernel/fs/{devfs,poll}.c`：将 file 身份传到 read/poll，保持旧回退 |
| 内核纯组帧 | 新 `kernel/driver/input_ps2.c` + `kernel/include/driver/input_ps2.h`：完整 Set 1 序列，不访问设备/TTY/显示 |
| 内核租约 | 新 `kernel/driver/input_route.c` + `kernel/include/driver/input_route.h`：物理状态、epoch、授权、ACTIVE 队列、endpoint 生命周期 |
| 驱动接线 | `kernel/driver/{keyboard,mouse}.c` 及对应头、x86 boot ops：保留 NORMAL 队列/TTY，调用 route；不重新实现路由 |
| 显示绑定 | `kernel/driver/{gfx,fb_state}.c`、`kernel/include/driver/fb_state.h`、UAPI、`libgfx/gfx.{h,c}`：owner 原子校验、撤销与包装 |
| 原生解码 | 新 `libgui/src/input_ps2.c`、私有 `gui_input_ps2.h`：US ASCII、键状态、typed events，不调用 LVGL |
| 输入生命周期 | 新 `libgui/src/input.c`、公共 `gui_input.h`、私有 `gui_input_private.h`：fd、FIFO、泵、错误和 platform 挂接 |
| 控件适配 | 新 `libgui/src/input_lvgl.c`、`input_focus.c`：keypad/pointer、取消、wheel、焦点注册，避免单文件混入设备 IO |
| 平台共享状态 | 新 `libgui/internal/gui_platform_private.h`：将既有 struct 原样迁入，添加 input 指针；不改变公共 opaque 类型 |
| 构建/发布 | `libgui/Makefile`、`hosttests/Makefile`、`mk/components/run.mk`：源码/测试/安装接线，Task 9 完整审计 |
| 端到端 | 新 `user/test_gui_input/{app.mk,test_gui_input.c}`、`qemutests/gui_input_test.py`，既有 `gui_test.py` 导入场景；`test_gui_runner.py` 补 fake 契约 |

新增 `input_ps2.c/input_route.c` 为 x86 当前平台选用，不让它们因 wildcard 误入其他架构：更新 `kernel/Makefile` 的平台排除清单及 `kernel/arch/x86_64/make.config::ARCH_PLATFORM_C_SOURCES`。不改 aarch64 产品功能。

---

### Task 1: file-aware devfs read/poll 与兼容回退

**Files:** Modify `kernel/include/fs/devfs.h`、`kernel/fs/{devfs,poll}.c`；Create `hosttests/cases/test_devfs_input_dispatch.c`；Modify `hosttests/Makefile`、`mk/components/run.mk`。

**Interfaces:** 在 `devfs_ops` 末尾追加 `int (*read_file)(struct file *,uint64_t offset,uint64_t size,void *buffer)` 和 `uint32_t (*poll_file)(struct file *,uint32_t requested,struct poll_table *)`。新增 `uint32_t devfs_poll_file(struct file *,uint32_t,struct poll_table *)`；devfs 的 VFS `read_file` 桥接优先 file-aware，缺失才调用原 node read。`fd_poll(FD_DEV)` 调新入口；不改普通 VFS/PTY 语义。

- [x] **1. 写 RED fixture。** `test_file_identity_reaches_device`：同 node 的两个 file 到达不同 mock 标识；`test_legacy_fallback`：无新 hook 时原 read/poll 各调用一次；`test_invalid_node`：poll=POLLNVAL；`test_file_read_uses_bounce`：hook 接收内核 buffer，uaccess 仍由 fd_read 完成。沿用 `test_gfx_file_lifecycle.c` 的真实 file/devfs 编译隔离，禁止把整个行为 mock 掉。
- [x] **2. 接入 `CASE=devfs-input` 后执行** `make PROFILE=x86_64-clang test-gui-host CASE=devfs-input`，确认因新分发缺失失败。
- [x] **3. 实现上述分发。** 保留原 devfs read/write/ioctl/mmap；read_file 错误不无条件降级到 node read，避免 EACCES 被绕过；poll_file 返回 0 也不能回退到 always-ready。先检查 node/type/ops，再调用设备。
- [x] **4. 执行 `make clean`，重跑 focused 与 `make PROFILE=x86_64-clang test-host`。** 新断言及旧 gfx/file/poll/PTY 全通过；struct 布局一致。
- [x] **5. 提交** `refactor(devfs): support file-aware input reads and polling`。
  已提交：`d8570d73`。

### Task 2: 内核完整 Set 1 组帧与边界测试

**Files:** Create `kernel/driver/input_ps2.c`、`kernel/include/driver/input_ps2.h`、`hosttests/cases/test_input_ps2_frame.c`；Modify x86 source lists、hosttests/focused 注册。

**Interfaces:** 私有头定义可栈分配 `input_ps2_frame_state_t`（最多 6 字节）、`input_ps2_frame_t`（`bytes[6]`、length、physical_key、make/break、special、start_epoch）。`void input_ps2_frame_reset(input_ps2_frame_state_t *)`；`int input_ps2_frame_feed(input_ps2_frame_state_t *,uint8_t byte,uint64_t epoch,input_ps2_frame_t *out)`：1=完整，0=等待，负 errno=损坏。普通 physical_key=code，E0=`0x100|code`，PrintScreen/Pause 标为 special，不伪装 Shift/Ctrl。

- [x] **1. 写表驱动断言。** `{E0,1D}` 只在第二字节输出右 Ctrl；`{E0,2A,E0,37}`、`{E0,B7,E0,AA}` 各一次 special；Pause 六字节一次 special；每个分片点更改传入 epoch，输出 start_epoch 必须仍为首字节 epoch。损坏 special 不能输出假 Shift；普通 `1E/9E` 输出 A make/break。
- [x] **2. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-frame`，确认缺组帧实现的 RED。
- [x] **3. 实现纯组帧器及 x86 构建接线。** 不持锁、不做 IO、不改物理位图；遇损坏返回错误并恢复到确定边界，不能把 special 已消费字节重新当独立修饰键。开始 epoch 从首字节缓存，不从完成时读取。
- [x] **4. 重跑 focused 并检查构建源码清单。** 用 `make -n PROFILE=x86_64-clang kernel.bin` 确认仅一次编译新文件；spec 的 NORMAL/ACTIVE 错误策略由下一任务决定，不在纯组帧器吞错。
- [x] **5. 提交** `feat(input): frame complete PS2 scan sequences before routing`。
  已提交：`bb6e5a4d → 56cfe013`。

### Task 3: 输入 endpoint、租约核心、设备路由与 final-close

**Files:** Create `kernel/driver/input_route.c`、`kernel/include/driver/input_route.h`、`hosttests/cases/test_input_route.c`、所需 `hosttests/mock/input_route_test_runtime.{c,h}`；Modify `keyboard.c/mouse.c` 与其头、`kernel/arch/x86_64/platform/boot.c`、构建/测试注册。

**Interfaces:** 以 `input_route.h` 统一以下内核私有契约：

```c
typedef enum { INPUT_DEVICE_KEYBOARD, INPUT_DEVICE_MOUSE } input_device_t;
/* NORMAL 路径 sink：driver 在 init 时向 route 注册，route 持 route_lock 调用。
 * 注入而非 route 直接调用 driver 的原因：route 必须在自己的临界区内完成
 * NORMAL 的授权/出队/挂 waiter，直接回调会形成 route↔driver 循环依赖，
 * 并把 driver 头泄漏给所有消费者。 */
typedef struct {
    void     (*publish)(const input_ps2_frame_t *frame, uint32_t modifiers);
    int      (*read)(uint64_t offset, uint64_t size, void *buf);
    uint32_t (*poll)(uint32_t requested, struct poll_table *pt);
} input_route_kbd_sink_t;
typedef struct {
    void     (*publish)(const mouse_event_t *event);
    int      (*read)(uint64_t offset, uint64_t size, void *buf);
    uint32_t (*poll)(uint32_t requested, struct poll_table *pt);
} input_route_mouse_sink_t;
void input_route_set_normal_sinks(const input_route_kbd_sink_t *kbd,
                                  const input_route_mouse_sink_t *mouse);
void input_route_init(void);
int input_route_open(input_device_t device, struct file **out);
void input_route_release_file(struct file *f);
int input_route_bind(const void *view, struct file *kbd, struct file *mouse,
                     uint64_t *generation, uint32_t *initial_modifiers);
void input_route_unbind(const void *view, uint64_t generation, int reason);
int input_route_read(struct file *f,uint64_t offset,uint64_t size,void *buf);
uint32_t input_route_poll(struct file *f,uint32_t requested,struct poll_table *pt);
void input_route_keyboard_byte(uint8_t byte);
void input_route_mouse_event(const mouse_event_t *event);
```

`bind` 由调用者保证处于显示状态锁内（Task 4）；route 自取 IRQ-safe route_lock。generation 非零单调增长，unbind 的非零 generation 必须精确匹配；0 仅供当前 view 显式释放/owner 撤销。reason=0 表示显式清理；EACCES/ESTALE/EIO 为撤销原因。重复清理不影响其他 view/代次。具体 per-file state 只在 route 内定义，不作为 UAPI。

**状态实现决定：** 保留驱动现有 NORMAL 队列；route 单独持有 ACTIVE keyboard 255 字节有效容量及 mouse 64 项队列。route_lock 保护全局租约、物理/隔离位图、组帧器、ACTIVE 队列和 endpoint tombstone；NORMAL 队列沿用设备锁，锁序 `route → NORMAL queue/poll`。open 用 `file_alloc()` 创建 FD_DEV，`dev_private` 指向 endpoint；final-close 在 route_lock 下撤销/摘除，再解锁 free。全局只存受锁保护的弱 endpoint/view 身份，不持有阻止 final-close 的 file 引用。旧 endpoint 记录 generation/error，不依赖已释放 view 指针可解引用。

- [x] **1. 写 RED 路由 fixture。** 绑定前 NORMAL，绑定后 owner file 可读、其他 EBUSY/POLLERR；空 ACTIVE 为 EAGAIN；dup 的两个引用只在最终 close 撤销；关闭任一绑定 raw file 撤销整租约；fd 地址复用不解除新租约；keyboard 整序列空间不足及 mouse 第 65 项均为 EOVERFLOW/POLLERR。
- [x] **2. 补边界和可控 interleaving 断言。** 每个 E0/PrintScreen/Pause 分片处 bind/unbind/撤销：不发布半序列，物理状态继续更新、跨 epoch make 隔离到 break；按住 Shift/按钮后绑定不产生新 DOWN；Caps typematic 不翻转、初始快照一致；read/poll 注册边界注入一次发布不得 lost wakeup。fixture 记录实际锁持有与顺序，不能以 no-op spinlock 证明并发。
- [x] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-route`，确认在租约缺失时失败。
- [x] **4. 实现 route 并接入驱动。** keyboard init 在安装 consumer/启用 IRQ 前调用一次 route_init。kbd_consume 只把硬件 byte 交 route；route 使用 Task 2 组帧、维护物理状态，并向 NORMAL 整序列发布或 ACTIVE 队列发布。mouse 解包后先 route，再决定 NORMAL/ACTIVE，不能先拿 event_lock 再拿 route_lock。
- [x] **5. 完成 NORMAL bridge 与 ops 身份。** 将 boot 中 static keyboard_ops/mouse_ops 移为对应 driver 中导出的 `const struct devfs_ops keyboard_ops/mouse_ops`，boot 只注册；open/read_file/poll_file/release_file 接 route。保留旧 node 回调只作兼容桥接，真实 FD_DEV 使用 file-aware 路径。NORMAL read/poll 的授权检查和出队/挂 waiter 必须在同一 route 临界区，不能授权后解锁再读旧 ring。
- [x] **6. 实现 TTY 边界。** 内核 Caps 只有 route 的物理状态为权威；NORMAL translator 使用 route 提供的逻辑 modifier/Caps 快照，不再次翻转 Caps。bind/unbind 重置 TTY 前缀/瞬态修饰状态，但不清物理组帧器；跨 epoch Caps make 完成时 ACTIVE 进入 DESYNC。held 键必须整序列吞掉，mouse 按钮隔离不能丢正常位移。保持串口 TTY 路径原样。
- [x] **7. 重跑 route、frame、devfs 三个 CASE 与 `make PROFILE=x86_64-clang test-host`。** 更新原 `test_i8042_demux`/`test_mouse_init` 的链接依赖，使其链接真实 route/frame 或只在不测路由的 fixture 提供明确 shim；不能删除原 PS/2 断言。
- [x] **8. 提交** `feat(input): enforce exclusive input leases and boundary isolation`。
  已提交：`fc0a5379 → a1ac4158`。

### Task 4: gfx bind UAPI、owner 撤销与 libgfx 包装

**Files:** Modify `kernel/include/uapi/gfx.h`、`kernel/include/driver/fb_state.h`、`kernel/driver/{gfx,fb_state}.c`、`libgfx/gfx.{h,c}`；Create `hosttests/cases/test_gfx_input.c`；Extend existing `test_display_owner.c`、`test_gfx_client.c`、route focused group。

**Interfaces:** UAPI 的 `GFX_BIND_INPUT=0x470A`、`GFX_UNBIND_INPUT=0x470B`、32 字节 `gfx_bind_input_t` 和 libgfx 两个签名完全照 spec §4.1，不能重新选择字段/编号。fb_state 内新增：

```c
int fb_input_bind(const void *view,uint64_t expected_mode,
                  struct file *kbd,struct file *mouse,
                  uint64_t *generation,uint32_t *initial_modifiers);
void fb_input_unbind(const void *view,uint64_t generation,int reason);
```

前者在现有 display_mutex/state_lock 范围核对 owner、mode、后端状态后调用 route_bind；不能持 `g_gfx_table.lock` 再拿 display mutex。后者沿用显示控制锁序调用 route_unbind。copy_to_user 在所有 spinlock 外，失败回滚使用捕获的**非零 generation**。

- [x] **1. 写 RED 参数与 errno 断言。** size=32，offset 为 `0,4,8,12,16,24,28`；只读/坏用户地址 EFAULT；坏 fd EBADF；非读 fd/错误 ops/非零 flags 或 reserved/输入结果非零 EINVAL；非 owner EACCES；旧 view ESTALE；重复 bind EBUSY。libgfx 成功结果/NULL out_state/errno 透传都覆盖。

```c
/* test_gfx_input_uapi_layout，使用真实UAPI头，不复制结构体。 */
assert_eq(32, sizeof(gfx_bind_input_t));
assert_eq(16, offsetof(gfx_bind_input_t, generation));
assert_eq(24, offsetof(gfx_bind_input_t, initial_modifiers));
assert_eq(28, offsetof(gfx_bind_input_t, reserved2));
```

- [x] **2. 写 owner 生命周期断言。** acquire 新 view、release、detach、replace_fallback 真正改变 owner、分辨率 generation 更新、backend failure 各撤销；只改变 fallback 而当前 owner 未变不无故撤销。正常解绑只清本 view。`test_copyout_rollback_does_not_revoke_new_generation`：注入旧 copyout 失败与撤销/重新绑定交错，新租约仍可读。
- [x] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-route`，确认 UAPI/owner 缺口产生 RED。
- [x] **4. 实现 ioctl 与包装。** 复用 gfx 现有 uaccess、`files_get_file`、`file_put` 和精确 ops 核验；所有失败路径释放暂时引用。bind 不 acquire、不永久引用 raw files。将撤销钩子放在 fb_state 的实际 owner/mode 状态提交点，先撤销输入再让旧 view 被 free；IRQ 不拿 display 锁。
- [x] **5. 验证** route focused、既有 `make PROFILE=x86_64-clang test-display-owner` 与 `make PROFILE=x86_64-clang test-host`。检查 NORMAL 无租约客户端行为不变、无锁倒序和引用环。
- [x] **6. 提交** `feat(gfx): bind input leases to display ownership`。
  已提交：`8ab94330 → dddb51d2`。

### Task 5: 原生输入头与纯用户态键盘解码

**Files:** Create `libgui/include/gui/gui_input.h`、`libgui/internal/gui_input_ps2.h`、`libgui/src/input_ps2.c`、`hosttests/cases/test_gui_input_decode.c`；Modify `gui.h`、hosttests/focused 构建。

**Interfaces:** public create/destroy/get_pollfds/has_mouse/dispatch 与 config 照 spec §5.1；补全原生类型：DOWN=0、UP=1、REPEAT=2；modifier 位依次 LSHIFT=1<<0、RSHIFT=1<<1、LCTRL=1<<2、RCTRL=1<<3、LALT=1<<4、RALT=1<<5、CAPS=1<<6。`gui_key_event_t` 字段照 spec，`bool (*gui_key_handler_t)(const gui_key_event_t *,void *)`；pointer event 字段为 `int32_t x,y; uint8_t buttons,changed_buttons; int8_t wheel;`，handler 返回 void。setter 返回 void，NULL handler 为取消注册，NULL input 设置 EINVAL。公共头含 extern C，不含 LVGL。

私有解码器类型 `gui_ps2_state_t` 可栈分配，保存 prefix、按下位图、各 DOWN 字符、modifiers；`void gui_ps2_init(gui_ps2_state_t *,uint16_t initial_modifiers)`；`int gui_ps2_feed(gui_ps2_state_t *,uint8_t,uint64_t now_ms,gui_key_event_t *out)`（1 边沿/0 无事件/-1 errno）；`void gui_ps2_expire(gui_ps2_state_t *,uint64_t now_ms)`。不直接复用内核私有头/对象，也不执行 LVGL。

- [x] **1. 写 RED 表驱动 tests。** a↓=97、Shift+a=65、Caps+a=65、Caps+Shift+a=97、Shift+1=33；左右 Ctrl/Alt physical 身份；UP codepoint=0；重复 make 无事件；PrintScreen/Pause 不污染修饰；E0 每个分片点；99 ms 保留前缀、100 ms 到期清除；未知码无字符。KP slash/Enter 支持，PageUp/PageDown/Insert/NumLock 数字区仅报告物理事件而不冒充文本/方向。
- [x] **2. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-decode`，确认 decoder RED。
- [x] **3. 实现解码与公共声明。** DOWN 翻译、UP 身份、Caps 边沿按 spec；完整按键重复策略留给 Task 7，解码器只报告硬件真实边沿。调用 expire 的截止时间纳入后续 input 最小等待时间，不创建线程或信号定时器。
- [x] **4. 重跑 focused；分别用 C/C++ consumer 仅包含 `gui/gui.h` 做语法编译。** 不添加 thirdpart/LVGL include 路径，也应成功。此阶段可以仅编译不链接 create/dispatch，未实现函数不能先以假成功 stub 投入产品。
- [x] **5. 提交** `feat(gui): define native input events and PS2 decoding`。
  已提交：`7d2978a7 → b1a43b10`。

### Task 6: 生命周期、FIFO、公平泵与平台错误收敛

**Files:** Create `libgui/src/{input,input_lvgl}.c`、`libgui/internal/{gui_input_private,gui_platform_private}.h`、`hosttests/cases/test_gui_input_platform.c`；Modify `platform.c`、`gui_test_runtime.{c,h}`、libgui/hosttests 构建及 focused 入口。

**Interfaces:** 实现 Task 5 所有公共函数及 setter。将 `struct gui_platform` 原样移到私有头，仅新增 `gui_input_t *input`；不改公开 gui_config_t。跨文件 hook 固定：

```c
gui_status_t gui_input_advance(gui_input_t *,uint64_t now_ms,uint32_t *delay_ms);
void gui_input_stop(gui_input_t *,int reason); /* 幂等，释放lease/fd但保留对象供destroy */
int gui_input_lvgl_create(gui_input_t *);
void gui_input_lvgl_destroy(gui_input_t *);
void gui_input_lvgl_cancel(gui_input_t *);
int gui_input_lvgl_deliver(gui_input_t *,const gui_key_event_t *);
int gui_input_lvgl_pointer(gui_input_t *,const mouse_event_t *);
```

本任务在 input_lvgl.c 提供这些 hook 的最小真实实现：创建并绑定本 display 的 pointer/keypad、read_cb、空 group，销毁全部对象；key hook 能投递 ASCII/基本LVGL键，pointer hook 能投递位置/左键；cancel 使用 spec 的 wait-release 重新就绪。尚无注册控件时 keyboard 无目标是合法空组行为。Tasks 7–8 在此基础上补完整策略、焦点注册、cursor/wheel；不能留下未定义产品符号或返回成功的空 hook。本任务 deterministic fixture 可以在测试 TU 用明确 fake 记录调用顺序，但产品链接和真实生命周期检查必须使用实际 hook。

FIFO 元素带 kind、事件时刻，保存**物理/键盘边沿原序**；dispatch 解码时得到的 modifier 快照随事件入队，step 不可使用已经包含未来事件的 decoder 当前状态覆盖它。repeat 所用 delivered state 另存；处理排队的 modifier DOWN 后才取消当前键。

- [x] **1. 写 RED 生命周期 fixture。** 每个 open/分配/acquire/bind/LVGL create 阶段注入失败，校验首 errno、fd/lease/映射净计数；mouse 仅 ENOENT/ENODEV 可降级；二次 create EBUSY 不影响第一次；无新 acquire 的失败不 release 原 owner；offscreen synthetic 从不碰设备。
- [x] **2. 写泵与平台断言。** mouse 读 7 字节→EIO、keyboard 短读拼接、EINTR/EAGAIN/旧空0、POLLERR 取 EOVERFLOW、HUP 取 ESTALE、NVAL→EBADF；512 FIFO 容量不足停止 read，down/up 不合并；每种设备预算、256 投递预算、余项 delay=0。`test_static_view_lost_owner` 无 flush/present 时仍停止投递；所有回调仅在 step，dispatch 不调用 LVGL。
- [x] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-platform`，确认缺生命周期/FIFO 的 RED。
- [x] **4. 实现 create/destroy/dispatch/get_pollfds。** config、顺序、借用 fd、Caps 初值照 spec。每个 read 上限还受 FIFO 剩余容量约束，keyboard 与 mouse 每轮均有配额；解析状态在 input 对象内，失败锁存。get_pollfds 不泄漏关闭权，synthetic=0、keyboard-only=1、完整=2。
- [x] **5. 接入 platform step/terminal/destroy。** step 先检查 gfx state 及 raw poll 锁存错误，健康队列不在 step 内偷读，再投递 FIFO/推进重复/timers/present。最小 wait 含输入/prefix deadline。enter_terminal_state 调 stop 后 close gfx；stop 关闭 fd/解绑/停回调但不 free input，调用方随后 destroy 不 double-free；platform_destroy 自动 destroy 仍附加的 input。无 input 的 GUI-1 顺序不变。
- [x] **6. GREEN：focused 加原 `CASE=platform`、`CASE=lvgl`；100 次 create/destroy 净资源回到基线。** 在真实 LVGL test 资源释放路径中重复确认，不以 fake 成功代替。
- [x] **7. 提交** `feat(gui): add bounded input lifecycle and platform integration`。
  已提交：`7ac69484 → b1b9ccaa`。

### Task 7: keypad、重复、快捷键与焦点注册

**Files:** Create `libgui/src/input_focus.c`、`hosttests/cases/test_gui_input_lvgl.c`；Modify `libgui/src/input_lvgl.c`；Extend `gui_input_private.h`、GUI native integration 编译及 CASE 注册。

**Interfaces:** 实现 Task 6 LVGL 生命周期/key hooks。私有 `int gui_input_focus_add(gui_input_t *,lv_obj_t *)`、`void gui_input_focus_remove(gui_input_t *,lv_obj_t *)`、`int gui_input_focus_set(gui_input_t *,lv_obj_t *)`；外 display/无注册对象 EINVAL，重复 add=0。对象存活/顺序记录由 focus 模块管理，删除事件撤销记录；hidden/disabled 的自身与祖先在每次导航和 step 检查。私有测试 `int gui_input_test_feed_keybytes(gui_input_t *,const uint8_t *,size_t)`，只接受 synthetic，不安装进 sysroot。

- [x] **1. 写真实 LVGL RED assertions。** textarea A↓B↓应为 `ab`；Enter↓换焦点/删除旧对象不增加任何 button click，旧 PRESSED 状态清除，新普通键首次输入保留；Tab/Shift-Tab 按注册顺序循环、跳过 ancestor hidden/disabled、空组无崩溃；点击空白保留焦点；恢复后回原顺序。
- [x] **2. 写确定时钟 assertions。** 首次 a 在 t=0 插一次；399 ms仍一次，400 ms两次，439 ms两次，440 ms三次；真实 UP 后稳定；Ctrl/Alt DOWN 取消、UP 不恢复；Shift/Caps 变化保留字符但 REPEAT modifiers 为当前 delivered snapshot；消费 Ctrl-Q 不插 q，UP不释放其他键；Space 在 textarea 重复、在按钮一次激活，Tab/Enter/Esc 不重复。

```c
/* test_repeat_deadlines；fixture先建离屏platform、synthetic input、
 * 已聚焦textarea，并用已有gui_test_set_now(0)固定初始时钟。 */
const uint8_t down[] = {0x1e};
assert_eq(0, gui_input_test_feed_keybytes(input, down, sizeof(down)));
gui_platform_step(platform, &delay);
assert_str_eq("a", lv_textarea_get_text(textarea));
gui_test_set_now(399);
gui_platform_step(platform, &delay);
assert_str_eq("a", lv_textarea_get_text(textarea));
gui_test_set_now(400);
gui_platform_step(platform, &delay);
assert_str_eq("aa", lv_textarea_get_text(textarea));
```

- [x] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl`，以真实 native LVGL 得到 RED，不 wrap 掉 keypad/group/event 处理。
- [x] **4. 实现 keypad/focus 与原生回调。** keypad=EVENT，显式 read、暂停其 timer；一个非修饰活动键。Tab 调显式导航，不送 textarea；按钮 Space 映射 Enter 动作，保留原生 physical key。group 默认不自动加对象，焦点 outline 可由像素断言观察。重复由 input monotonic deadline 触发，每 step最多一次，不补发；FIFO尚有事件时先返回delay=0推进剩余边沿，不在尚未消费的UP/Ctrl事件前插入软件重复。
- [x] **5. 完整实施 spec §6.2 取消协议。** `wait_release → 显式 RELEASED read → 清旧视觉 → 新 PRESSED`；回调中只标 pending，外层 read 返回后处理，不能递归 read。`lv_indev_reset` 不替代 keypad 重新就绪。屏幕动画/indev 暂停导致 read 未实际消费取消样本时，保留 pending，不投递新键；先在 fixture 关闭 screen transition 验证主路径，再测试 blocked read 不泄漏旧 Enter。
- [x] **6. GREEN：运行 input-lvgl、input-platform、input-decode；检查真实事件数、textarea 精确字符串与 outline 像素。** 全部隐藏/禁用、回调删除、重复途中焦点变化均要断言下一键不丢。
- [x] **7. 提交** `feat(gui): implement keypad navigation and safe focus cancellation`。
  已提交：`d9fc939d → 5dd8a84f → 00ab331f`。

### Task 8: pointer、光标、拖动与 hover wheel

**Files:** Extend `libgui/src/{input_lvgl,input_focus}.c`、`libgui/internal/gui_input_private.h`、`hosttests/cases/test_gui_input_lvgl.c`。

**Interfaces:** 实现 Task 6 pointer hook；私有 `int gui_input_test_feed_mouse(gui_input_t *,const mouse_event_t *,size_t count)`，synthetic-only。pointer state 与 keypad 独立，cursor 所有权归 input；无新样本 read_cb 返回当前状态和 enc_diff=0。

- [x] **1. 写 RED assertions。** 同 step 的左 down/up=一次 click；dx/dy 在 int16 极值下夹边正确；slider 拖动释放后不再移动；右/中只报告 native changed_buttons；光标热点/尺寸/命中透明；pointer 静止长按仍有 timer 进展。
- [x] **2. 补 hover wheel tests。** 未点击直接悬停 container 后 wheel=+1，scroll_y 增加32；-1返回；内层到边界剩余传播父层；无目标不动作，slider不被改值；拖动中 wheel 忽略；光标移动不夺 keyboard focus。
- [x] **3. 执行 `make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl` 得到 RED，然后实现。** 新 pointer indev 显式绑定本 display，保留 TIMER；FIFO 每条显式 read，read_cb 不私自 drain 后续 FIFO。12×18 箭头使用 system layer 的程序绘制对象、清除 CLICKABLE。wheel 不写 enc_diff，不用 last_pressed；公开 LVGL hit-test/parent/scroll API 实现排除 cursor 与 hidden/disabled，再夹滚动余量。
- [x] **4. GREEN：重跑三个 input 用户态 CASE 和既有 GUI-1 CASE。** 采样离屏 cursor/outline 像素，验证 destroy 后无 indev/timer/group/cursor 残留；mouse-only 功能不影响 keyboard-only。
- [x] **5. 提交** `feat(gui): add pointer cursor dragging and hover scrolling`。
  已提交：`7600955d → f276bae0`。

### Task 9: 完整 archive、sysroot 和 host 构建闭环

**Files:** Modify `libgui/Makefile`、`hosttests/Makefile`、`mk/components/run.mk`、`qemutests/gui_build_audit.py`；Extend相关 host runtime wrappers；仅在真实依赖需要时修改 `mk/components/sysroot.mk`。

**Interfaces:** libgui 生产源码清单显式包含 `platform.c tick.c input.c input_ps2.c input_lvgl.c input_focus.c`；install 增加且只增加公共 `gui_input.h`。host 的 staged UAPI symlink 增加 `uapi/mouse.h`，不把完整 kernel/include 或 OS01 libc/include 放在 native 系统头前。

- [x] **1. 添加 build audit RED 检查。** 发布 generation 必须包含 `gui_input.h`，不得包含 input private/PS2/frame 头；普通 user flags 不含 LVGL；archive 可解析公共输入符号；真实 native LVGL fixture 复用唯一 host archive，版本来自 manifest。
- [x] **2. 实现完整源码、host fixture 和包装接线。** input-platform 的 mock 与 input-lvgl 的 real LVGL 各自独立对象目录；新增 open/read/close/gfx bind/unbind/get_display_state 的 wrapper 只拦已登记测试 fd/handle，其他交还 `__real_*`。既有两个 GUI-1 binary 链接 platform 后新增依赖必须补齐，不能以 weak 永久空实现消除链接错误。
- [x] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=all`、`make PROFILE=x86_64-clang test-static`。随后 `make PROFILE=x86_64-clang sysroot`，扩展 `gui_build_audit.py::audit_sysroot_and_build` 的既有 C/C++ syntax consumer，新增真实目标 C 链接 probe：consumer仅含 `<gui/gui.h>`、main引用create/dispatch，CRT0、sigreturn、linker.ld及库顺序复用 `user/Makefile` 的实际规则，依次 `crt0.o sigreturn_trampoline.o consumer.o -lgui -lgfx -lc`。目标工具链使用profile值，对象与ELF写audit临时目录；既有audit仅语法编译，不得将它误报为链接验证。不能靠私有include路径才能编译。
- [x] **4. 验证增量。** 无修改重复构建不重编；touch gui_input.h 后相关 consumer/库重编；install 清除模拟 staging/gui 孤立文件；新 generation 不包含私有头。不得破坏保留的历史 generation。
- [x] **5. 提交** `build(gui): publish native input API and register complete host coverage`。
  已提交：`1cf5e8c3 → d2cef0b7 → aad8d328 → 5237d99f → 59cb159a → c2d9a729`。

### Task 10: 独立 guest 输入探针与可核验协议

**Files:** Create `user/test_gui_input/{app.mk,test_gui_input.c}`；Modify相关 build audit；rootfs 使用现有自动 discovery，不添加冗余手写程序清单。

**Interfaces:** `app.mk` 复用 test_lvgl 的私有 include 例外，`APP_LDLIBS := -lgui -lgfx`；仅探针允许 private focus/injection-state 查询。命令行 `--nonce <hex> --scenario <interaction|isolation|hold|fatal|overflow|owner>`；非法参数 exit2。真实输入均 DEVICES、require_mouse=true；禁止以 synthetic、控制文件或 stdin 文本替代物理键输入。所有场景均可由 shell 后台启动，探针不读共享 stdin，以便 runner 通过串口 shell 查询状态、写控制文件和 wait。

协议每行以 `[gui-input]` 开头，字段固定 `nonce=<...> seq=<递增值> phase=<...>`；READY 额外带 `pid width height` 与目标坐标；STATE 带 click_count/slider/scroll/focus/textarea 的**十六进制字节串**，以及最后一个原生 key 的 physical_key/modifiers/action、`sigint_seen`；EXIT 带场景断言结果。错误输出 ERROR+errno、非零退出。应用输出通过 fd1/2 写 PTY/serial 日志，必须 flush。

测试控制路径固定为 `/tmp/gui-input-<hexnonce>.ctl`。runner 在启动前创建空文件；通过串口 shell 写入 `nonce=<hex> ctl_seq=<递增值> command=<...>`。探针每至多10 ms重新打开并读取完整记录，同一 ctl_seq 只执行一次；命令只允许屏障、状态快照、测试生命周期动作，不生成按键/按钮/文本事件。ACK 必须回显 ctl_seq、probe pid、正常递增的协议 seq。可用命令为 `SNAPSHOT`、`PROGRESS`、`RELEASE_VIEW`、`RESUME_STEP`；未知命令/nonce错误/倒序编号报错。命令覆盖同一文件时 runner 必须先收到上一个ACK，避免截断/写入竞争；探针读到暂时空/不完整文件只等下一轮。全场景有界，退出清理控制文件，不新增FIFO、设备或产品UAPI。

- [x] **1. 为 protocol parser 添加 unittest fixture RED。** 只由 guest 生成且行首锚定的完整记录有效；错误 nonce、重复 seq、READY 前STATE、缺EXIT、textarea 非法hex均失败。parser 放 `gui_input_test.py`，可先写纯 parser，不拉 QEMU。
- [x] **2. 实现固定测试 UI。** button、slider、可滚容器、两个 textarea、disabled button；固定逻辑坐标但按真实屏幕范围布局并输出坐标。private focus_add 注册顺序明确，outline/cursor 颜色固定供截图采样。READY 只能在 acquire/bind、首次 step/present 成功后输出。
- [x] **3. 实现场景与观测。** interaction 用状态变化输出 STATE 并在 Ctrl-Q 返回0；isolation 不接受快捷键外的退出输入，记录待核验内容；hold不接受控制文件以外的生命周期命令。fatal在READY后等待真实PS/2 F12 DOWN再触发LVGL assertion。overflow从READY开始暂停dispatch，保持step/错误检查和控制文件PROGRESS处理；PROGRESS ACK额外带`raw_pump=0`。owner在静止首帧后接受RELEASE_VIEW：通过仅探针内部的platform私有头调用`gfx_release(platform->gfx)`，保持platform的acquired记录供后续step检测，输出RELEASED ACK并暂停dispatch/step，直到RESUME_STEP ACK后继续。这个测试动作不新增公开release接口，也不修改gfx独占规则。
- [x] **3a. 加入SIGINT观测。** 探针在READY前用现有`signal(SIGINT, handler)`安装仅将`static volatile int sigint_seen`置1的处理器；handler不调用日志/分配函数，主循环从该标志输出STATE。不得使用SIG_IGN掩盖TTY信号泄漏，不改变进程组。正常租约期间该值必须始终为0；异常值使场景失败。
- [x] **4. 运行** `python3 -m unittest qemutests.test_gui_runner`、`make PROFILE=x86_64-clang disk.img`；检查自动安装 `/bin/test_gui_input`。普通 test_lvgl --smoke 仍不打开输入设备。
- [x] **5. 提交** `test(gui): add guest input interaction probe and nonce protocol`。
  已提交：`6b75f0ae → e2693ece → 2d030262`。

### Task 11: QMP 真实交互矩阵与 runner 负契约

**Files:** Create/Extend `qemutests/gui_input_test.py`、`qemutests/test_gui_runner.py`；Modify `qemutests/gui_test.py::QmpClient/run_all`，保留原返回 outcomes schema 与 SMP matrix。

**Interfaces:** 在既有 QmpClient 增加 `input_events(self, events: list[dict]) -> None`，包装 `execute("input-send-event", {"events": events})`；`GuiInputScenarios` 消费既有 session、qmp、run_dir、smp，返回现有 case outcome dict。通过当前 `GuiCaseRunner.run_all` 接入；独立 helper 避免继续膨胀 run_all。

- [x] **1. 写 FakeQMP assertions。** key down/up payload 使用 `type=key,data={down, key:{type:qcode,data:<qcode>}}`；rel x/y 用 `type=rel,data={axis,value}`；left/right/middle/wheel-up/down 用 btn 的 down/up。wheel 不是 abs/rel 第三轴。鼠标起点从已知中心计算，按 READY 坐标移动。
- [x] **2. 写 runner RED negatives。** 只有命令回显、无换行prompt、跨块标记、旧 nonce、缺按键up、QMP error、截图不符、重复/乱序phase、silent timeout、一个 SMP 失败均必须最终 FAIL；补齐`sigint_seen=1`、probe PID变化、terminal重启、右修饰键被认成左键、modifier UP后位未清、Caps重复make再次翻转等失败fixture。FakeQMP仅验证编排，不被描述为真实输入证明。
- [x] **3. 实现 interaction。** 后台启动probe后按 READY→click→rapid click→drag→hover wheel→Tab/Shift-Tab→textarea `aA!`→编辑/多行→修饰键矩阵→租约期间Ctrl-C→repeat→Ctrl-Q 顺序，逐项等 STATE 并比对精确状态；不能用固定sleep代替应用证据。Ctrl-Q后用shell `wait <pid>`核验0退出。长按用有界时间观察count下限、释放后稳定；精确400/40边界由host fake-clock负责。
- [x] **3a. 明确修饰键物理矩阵。** 用QMP左右Shift、Ctrl、Alt分别down→普通a down/up→modifier up，逐事件断言原生physical identity与modifiers的对应单一L/R位。左右Shift时插入`A`；Ctrl/Alt时codepoint=0且textarea不变；UP后modifier位清零。Caps make/break→a为`A`；Caps+Shift→a为`a`；Caps第二次make/break恢复关闭；重复make不再次翻转。每项结束要求所有瞬态modifier位清零，不让前一项污染后一项。
- [x] **3b. 明确租约内Ctrl-C证据。** 启动probe前通过串口shell的`echo $$`与`/proc/<shellpid>/status`的PPid找到terminal，记录`/proc/<terminalpid>/status`的Pid/BirthId；使用独立控制文件SNAPSHOT确认probe初始`sigint_seen=0`。在租约ACTIVE时QMP发送Ctrl+C完整down/up，再SNAPSHOT：probe pid必须仍等于READY、协议seq连续递增、sigint_seen仍0、textarea无控制字符。随后必须继续完成点击和文本输入，再核对同一terminal的Pid/BirthId，禁止用退出后的Ctrl-C代替本项。缺失字段、terminal重启或检测到SIGINT均FAIL。
- [x] **3c. 设置有界场景预算。** 给新增场景独立截止时间，父session总预算取boot、已注册场景上限和cleanup的有限和，不机械沿用GUI-1总预算；timeout fixture同时覆盖子场景与总预算。预算不得因失败自动放大或无界重试。
- [x] **4. 执行** `make PROFILE=x86_64-clang test-harness` 后 `make PROFILE=x86_64-clang test-qemu SUITE=gui`。既有 GUI-1 七场景及新 input 场景均在 SMP1/2 独立运行；保留 QMP transcript、serial、关键PPM与result.json。
- [x] **5. 提交** `test(gui): verify physical input interactions through QMP`。
  已提交：`d0ffe93f → af42c90e → 512ff6cc → eb8c56dd → 73d45079`。

### Task 12: 所有权、溢出、异常退出与 shell 恢复

**Files:** Extend guest probe、`gui_input_test.py`、`test_gui_runner.py`；必要时扩展已有 `test_gui_fault` 的命令行场景，不新增产品测试设备或不受限注入 syscall。

**Interfaces:** 与Task 10协议和控制文件一致；后台probe不读共享stdin。shell使用serial启动/kill后台进程和写控制文件；物理keyboard仍经QMP。owner交接仅在probe自己的循环边界release；overflow使用PROGRESS屏障，ACK只证明guest已执行且未泵raw，不证明QMP事件已全部入队。

- [x] **1. 写fake runner RED scenarios。** missing acquired/READY、kill前未持输入、fatal返回0、屏幕恢复但PS2 canary失败、held key从未release、只出现canary命令回显、溢出未观察到EOVERFLOW、错误ctl_seq/ACK、RELEASED前启动第二owner、批次上游丢失而guest未溢出，全部失败。
- [x] **2. 实现 TTY 泄漏证明。** UI租约期间由QMP键入 `echo LEAK_<nonce> > /tmp/gui_input_leak_<nonce>` 再Enter；退出后通过串口shell检查该文件不存在，并用PS2再输入独立 `echo RECOVER_<newnonce>`，只接受新cursor后独占一行的实际输出及prompt。不能仅搜索字符串，因为命令回显包含它。每场景删除自己的临时文件，重用镜像用-snapshot。
- [x] **3. 实现 crash/held-key。** 正常Ctrl-Q、后台SIGKILL、真实LVGL fatal分别确认屏幕归还terminal；kill时按住Shift/左键，kill完成再发送break，之后PS2 canary大小写正确、无幽灵click。记录后台 `wait <pid>` 的状态；命令超时/进程未退出不能算恢复。先释放本场景所有物理按键，再以真实PS/2 Ctrl-C清理可能残留的未提交shell输入行，观察prompt后发送canary；不通过写TTY状态或清空键盘状态掩盖幽灵修饰键。
- [x] **4. 实现合法的静止owner交接。** 后台owner probe产生READY后不再dirty。先尝试第二gfx客户端acquire并断言EBUSY，保留现有排他协议。写RELEASE_VIEW，等待旧probe输出RELEASED且尚未继续step；再启动第二gfx hold客户端并确认acquired/截图。写RESUME_STEP，让旧probe在无新dirty/present条件下检测LOST_OWNERSHIP并非零退出，不能重新acquire或继续回调。旧probe关闭不应release第二view；确认第二hold仍在，再结束hold并验证terminal与PS2恢复。控制屏障最长5秒，无ACK或未取得新owner证据即FAIL。
- [x] **5. 实现真实队列溢出。** overflow probe从READY起不调用dispatch/raw read，仍执行step与每10ms控制文件检查。runner每批发送4组普通a make/break（8个QMP key事件），随后写唯一PROGRESS ctl_seq并等待ACK中的`raw_pump=0`或guest ERROR；最多64批、整个窗口最多40秒。批次间必须让guest运行，不以一个请求堆300字节，也不将QMP返回成功或累计发送字节视为入队数量。只有guest真实ERROR errno=EOVERFLOW、非零退出及恢复canary成功才PASS；没有溢出/没有ACK/超预算均FAIL，不增加生产统计或注入UAPI。mouse64项精确溢出继续由host验证。

  该分批约束依据锁定版本示例[QEMU v10.1.0 ps2.c](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/input/ps2.c)的16字节虚拟设备队列及满时丢弃行为，以及[input.c](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/ui/input.c)的批量事件发送流程。QMP成功与guest ring溢出不是同一证据；执行时记录本机QEMU版本，仍以guest错误与恢复为判据。
- [x] **6. GREEN：** `make PROFILE=x86_64-clang test-harness` 与 `make PROFILE=x86_64-clang test-qemu SUITE=gui`。SMP1/2 每种恢复路径都必须有真实PS2 canary；test runner 的串口救援只能标记失败并清理，不能转PASS。
- [x] **7. 提交** `test(gui): gate input isolation ownership loss and crash recovery`。
  已提交：`5e0ba9f4 → 45c0473b → 093a5fb4`。

### Task 13: 总回归、文档与交付证据

**Files:** Modify `docs/gui/gui.md`、`docs/driver/driver.md`、`docs/build/build.md`、`docs/roadmap.md`、`docs/changelog.md`；Create `docs/superpowers/reports/2026-10-20-gui-2-validation.md`（日期按实际执行日调整）。

**Interfaces:** 不新增产品接口。报告列出 commit/profile/工具链/LVGL版本、每个命令退出码和日志路径、SMP矩阵、失败或豁免。执行者更新本plan的checkbox与任务commit，不把未来测试写成已通过。

- [x] **1. 执行干净回归，逐条保存完整输出和退出码：**

```sh
make clean
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang test-harness
make PROFILE=x86_64-clang test-qemu SUITE=gui
make PROFILE=x86_64-clang test-qemu SUITE=gfx
make PROFILE=x86_64-clang test-qemu SUITE=resolution
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall
```

- [x] **2. 验证Task 9增量/sysroot证据，并再次读取suite实际result.json。** 不以终端尾部PASS代替逐SMP结果；不为无新失败的已通过suite重复跑多轮。若既有parked故障复现，按同环境baseline对照，报告具体限制，不能自行宣布门禁豁免。
- [x] **3. 更新文档。** 写明输入租约/errno/UAPI布局/恢复路径、native API与单活动键限制、focused CASE及QMP用例；roadmap仅在全部GUI-2完成条件确实达成时移入changelog，未完成则保留并注明具体缺口。
- [x] **4. 交付前核验** `git diff --check`、`git status --short`；无意外生成文件、子模块变更或私有头泄漏。遵循 verification-before-completion；若需要整体代码评审，使用 requesting-code-review 并修复实质性问题，不能把review建议直接当成测试结果。
- [x] **5. 提交** `docs(gui): record GUI-2 input and focus validation`，向用户报告最终行为、验证证据和实际限制；不要自动merge/push。
  已提交：`ed725324 → ac3f3eb6 → 3d6ab8b9`。

## Spec 覆盖自检

| Spec范围 | 实施与验证任务 |
|---|---|
| §1–3 单进程分层、边界、文件 | Tasks 1–9；不实施GUI-3/5 |
| §4.1 UAPI/包装/回滚 | Task 4；copyout代次回滚在Review Focus单列 |
| §4.2 租约/撤销/final-close | Tasks 3–4、12 |
| §4.3 整序列/epoch/隔离/锁/poll | Tasks 1–4、12 |
| §5 API/生命周期/FIFO/主循环 | Tasks 5–6、9–10 |
| §6 解码/修饰/重复/取消 | Tasks 2–3、5、7；真实LVGL取消测试不可mock |
| §7 pointer/wheel/focus | Tasks 7–8、11 |
| §8 资源/错误/发布/兼容 | Tasks 3–4、6、9、12 |
| §9 host/QEMU/SMP/runner/回归 | 每任务RED/GREEN，Tasks 10–13闭环 |
| §10–11 三阶段与两轮评审修订 | Task 3序列边界、Task 7无点击取消与Ctrl/Alt重复，最终Task 13 |

本plan只细化已评审spec的实现接线和测试入口，没有产品实现。用户评审本plan后，再由Luna按既定方式执行。

## Plan 子 agent 评审修订记录

| 轮次 | 结论 | 修订 |
|---|---|---|
| 第一轮 | REQUEST_CHANGES：2项P1、1项P2 | 使用显式release/双owner屏障验证静止失权；QMP小批次与guest进度ACK替代单请求大burst；补全租约内Ctrl-C、左右修饰键/Caps、terminal身份及runner负契约 |
| 第二轮 | APPROVE：无新增实质性阻塞 | 完整复审Revision 2，确认首轮三项已解决，新增控制文件协议与任务依赖/生命周期一致，可交给Luna实施；不代表产品实现或运行测试已完成 |
