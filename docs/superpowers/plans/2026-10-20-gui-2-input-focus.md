# GUI-2 Input and Focus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为单进程全屏 libgui 应用交付独占物理输入、LVGL 控件交互、确定的焦点导航及退出后终端恢复。

**Architecture:** 在 raw keyboard/mouse 与 TTY 分流之前建立与显示 view 绑定的输入租约。libgui 负责非阻塞泵、Set 1 解码、pointer/keypad 和进程内焦点，应用继续拥有 poll 主循环。内核队列失步显式失败；显示与输入生命周期一起收敛到 GUI-1 既有终止状态。

**Tech Stack:** C11、OS01 devfs/poll、PS/2 Set 1、libgfx、libgui、锁定的 LVGL v9.5.0、原生 hosttests、Python unittest、QEMU/QMP。

**Spec:** [2026-10-20-gui-2-input-focus-design.md](../specs/2026-10-20-gui-2-input-focus-design.md)，Draft v2，子 agent 两轮评审后 APPROVE。本文以已提交该 spec 的 `8a1bcf9` 为基准；此前产品代码基准为 `d829b36f`。

**执行交接：** 用户指定交给 **Luna** 执行。优先在 Luna 会话使用 `superpowers:executing-plans`，按下面的依赖顺序逐任务实施；本次仅编写 plan，不启动执行、不切模型、不创建新 chat。不要因默认 header 的推荐项再次询问模型选择或擅自派发实现子 agent。

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

- [ ] **1. 写 RED fixture。** `test_file_identity_reaches_device`：同 node 的两个 file 到达不同 mock 标识；`test_legacy_fallback`：无新 hook 时原 read/poll 各调用一次；`test_invalid_node`：poll=POLLNVAL；`test_file_read_uses_bounce`：hook 接收内核 buffer，uaccess 仍由 fd_read 完成。沿用 `test_gfx_file_lifecycle.c` 的真实 file/devfs 编译隔离，禁止把整个行为 mock 掉。
- [ ] **2. 接入 `CASE=devfs-input` 后执行** `make PROFILE=x86_64-clang test-gui-host CASE=devfs-input`，确认因新分发缺失失败。
- [ ] **3. 实现上述分发。** 保留原 devfs read/write/ioctl/mmap；read_file 错误不无条件降级到 node read，避免 EACCES 被绕过；poll_file 返回 0 也不能回退到 always-ready。先检查 node/type/ops，再调用设备。
- [ ] **4. 执行 `make clean`，重跑 focused 与 `make PROFILE=x86_64-clang test-host`。** 新断言及旧 gfx/file/poll/PTY 全通过；struct 布局一致。
- [ ] **5. 提交** `refactor(devfs): support file-aware input reads and polling`。

### Task 2: 内核完整 Set 1 组帧与边界测试

**Files:** Create `kernel/driver/input_ps2.c`、`kernel/include/driver/input_ps2.h`、`hosttests/cases/test_input_ps2_frame.c`；Modify x86 source lists、hosttests/focused 注册。

**Interfaces:** 私有头定义可栈分配 `input_ps2_frame_state_t`（最多 6 字节）、`input_ps2_frame_t`（`bytes[6]`、length、physical_key、make/break、special、start_epoch）。`void input_ps2_frame_reset(input_ps2_frame_state_t *)`；`int input_ps2_frame_feed(input_ps2_frame_state_t *,uint8_t byte,uint64_t epoch,input_ps2_frame_t *out)`：1=完整，0=等待，负 errno=损坏。普通 physical_key=code，E0=`0x100|code`，PrintScreen/Pause 标为 special，不伪装 Shift/Ctrl。

- [ ] **1. 写表驱动断言。** `{E0,1D}` 只在第二字节输出右 Ctrl；`{E0,2A,E0,37}`、`{E0,B7,E0,AA}` 各一次 special；Pause 六字节一次 special；每个分片点更改传入 epoch，输出 start_epoch 必须仍为首字节 epoch。损坏 special 不能输出假 Shift；普通 `1E/9E` 输出 A make/break。
- [ ] **2. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-frame`，确认缺组帧实现的 RED。
- [ ] **3. 实现纯组帧器及 x86 构建接线。** 不持锁、不做 IO、不改物理位图；遇损坏返回错误并恢复到确定边界，不能把 special 已消费字节重新当独立修饰键。开始 epoch 从首字节缓存，不从完成时读取。
- [ ] **4. 重跑 focused 并检查构建源码清单。** 用 `make -n PROFILE=x86_64-clang kernel.bin` 确认仅一次编译新文件；spec 的 NORMAL/ACTIVE 错误策略由下一任务决定，不在纯组帧器吞错。
- [ ] **5. 提交** `feat(input): frame complete PS2 scan sequences before routing`。

### Task 3: 输入 endpoint、租约核心、设备路由与 final-close

**Files:** Create `kernel/driver/input_route.c`、`kernel/include/driver/input_route.h`、`hosttests/cases/test_input_route.c`、所需 `hosttests/mock/input_route_test_runtime.{c,h}`；Modify `keyboard.c/mouse.c` 与其头、`kernel/arch/x86_64/platform/boot.c`、构建/测试注册。

**Interfaces:** 以 `input_route.h` 统一以下内核私有契约：

```c
typedef enum { INPUT_DEVICE_KEYBOARD, INPUT_DEVICE_MOUSE } input_device_t;
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

- [ ] **1. 写 RED 路由 fixture。** 绑定前 NORMAL，绑定后 owner file 可读、其他 EBUSY/POLLERR；空 ACTIVE 为 EAGAIN；dup 的两个引用只在最终 close 撤销；关闭任一绑定 raw file 撤销整租约；fd 地址复用不解除新租约；keyboard 整序列空间不足及 mouse 第 65 项均为 EOVERFLOW/POLLERR。
- [ ] **2. 补边界和可控 interleaving 断言。** 每个 E0/PrintScreen/Pause 分片处 bind/unbind/撤销：不发布半序列，物理状态继续更新、跨 epoch make 隔离到 break；按住 Shift/按钮后绑定不产生新 DOWN；Caps typematic 不翻转、初始快照一致；read/poll 注册边界注入一次发布不得 lost wakeup。fixture 记录实际锁持有与顺序，不能以 no-op spinlock 证明并发。
- [ ] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-route`，确认在租约缺失时失败。
- [ ] **4. 实现 route 并接入驱动。** keyboard init 在安装 consumer/启用 IRQ 前调用一次 route_init。kbd_consume 只把硬件 byte 交 route；route 使用 Task 2 组帧、维护物理状态，并向 NORMAL 整序列发布或 ACTIVE 队列发布。mouse 解包后先 route，再决定 NORMAL/ACTIVE，不能先拿 event_lock 再拿 route_lock。
- [ ] **5. 完成 NORMAL bridge 与 ops 身份。** 将 boot 中 static keyboard_ops/mouse_ops 移为对应 driver 中导出的 `const struct devfs_ops keyboard_ops/mouse_ops`，boot 只注册；open/read_file/poll_file/release_file 接 route。保留旧 node 回调只作兼容桥接，真实 FD_DEV 使用 file-aware 路径。NORMAL read/poll 的授权检查和出队/挂 waiter 必须在同一 route 临界区，不能授权后解锁再读旧 ring。
- [ ] **6. 实现 TTY 边界。** 内核 Caps 只有 route 的物理状态为权威；NORMAL translator 使用 route 提供的逻辑 modifier/Caps 快照，不再次翻转 Caps。bind/unbind 重置 TTY 前缀/瞬态修饰状态，但不清物理组帧器；跨 epoch Caps make 完成时 ACTIVE 进入 DESYNC。held 键必须整序列吞掉，mouse 按钮隔离不能丢正常位移。保持串口 TTY 路径原样。
- [ ] **7. 重跑 route、frame、devfs 三个 CASE 与 `make PROFILE=x86_64-clang test-host`。** 更新原 `test_i8042_demux`/`test_mouse_init` 的链接依赖，使其链接真实 route/frame 或只在不测路由的 fixture 提供明确 shim；不能删除原 PS/2 断言。
- [ ] **8. 提交** `feat(input): enforce exclusive input leases and boundary isolation`。

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

- [ ] **1. 写 RED 参数与 errno 断言。** size=32，offset 为 `0,4,8,12,16,24,28`；只读/坏用户地址 EFAULT；坏 fd EBADF；非读 fd/错误 ops/非零 flags 或 reserved/输入结果非零 EINVAL；非 owner EACCES；旧 view ESTALE；重复 bind EBUSY。libgfx 成功结果/NULL out_state/errno 透传都覆盖。

```c
/* test_gfx_input_uapi_layout，使用真实UAPI头，不复制结构体。 */
assert_eq(32, sizeof(gfx_bind_input_t));
assert_eq(16, offsetof(gfx_bind_input_t, generation));
assert_eq(24, offsetof(gfx_bind_input_t, initial_modifiers));
assert_eq(28, offsetof(gfx_bind_input_t, reserved2));
```

- [ ] **2. 写 owner 生命周期断言。** acquire 新 view、release、detach、replace_fallback 真正改变 owner、分辨率 generation 更新、backend failure 各撤销；只改变 fallback 而当前 owner 未变不无故撤销。正常解绑只清本 view。`test_copyout_rollback_does_not_revoke_new_generation`：注入旧 copyout 失败与撤销/重新绑定交错，新租约仍可读。
- [ ] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-route`，确认 UAPI/owner 缺口产生 RED。
- [ ] **4. 实现 ioctl 与包装。** 复用 gfx 现有 uaccess、`files_get_file`、`file_put` 和精确 ops 核验；所有失败路径释放暂时引用。bind 不 acquire、不永久引用 raw files。将撤销钩子放在 fb_state 的实际 owner/mode 状态提交点，先撤销输入再让旧 view 被 free；IRQ 不拿 display 锁。
- [ ] **5. 验证** route focused、既有 `make PROFILE=x86_64-clang test-display-owner` 与 `make PROFILE=x86_64-clang test-host`。检查 NORMAL 无租约客户端行为不变、无锁倒序和引用环。
- [ ] **6. 提交** `feat(gfx): bind input leases to display ownership`。

### Task 5: 原生输入头与纯用户态键盘解码

**Files:** Create `libgui/include/gui/gui_input.h`、`libgui/internal/gui_input_ps2.h`、`libgui/src/input_ps2.c`、`hosttests/cases/test_gui_input_decode.c`；Modify `gui.h`、hosttests/focused 构建。

**Interfaces:** public create/destroy/get_pollfds/has_mouse/dispatch 与 config 照 spec §5.1；补全原生类型：DOWN=0、UP=1、REPEAT=2；modifier 位依次 LSHIFT=1<<0、RSHIFT=1<<1、LCTRL=1<<2、RCTRL=1<<3、LALT=1<<4、RALT=1<<5、CAPS=1<<6。`gui_key_event_t` 字段照 spec，`bool (*gui_key_handler_t)(const gui_key_event_t *,void *)`；pointer event 字段为 `int32_t x,y; uint8_t buttons,changed_buttons; int8_t wheel;`，handler 返回 void。setter 返回 void，NULL handler 为取消注册，NULL input 设置 EINVAL。公共头含 extern C，不含 LVGL。

私有解码器类型 `gui_ps2_state_t` 可栈分配，保存 prefix、按下位图、各 DOWN 字符、modifiers；`void gui_ps2_init(gui_ps2_state_t *,uint16_t initial_modifiers)`；`int gui_ps2_feed(gui_ps2_state_t *,uint8_t,uint64_t now_ms,gui_key_event_t *out)`（1 边沿/0 无事件/-1 errno）；`void gui_ps2_expire(gui_ps2_state_t *,uint64_t now_ms)`。不直接复用内核私有头/对象，也不执行 LVGL。

- [ ] **1. 写 RED 表驱动 tests。** a↓=97、Shift+a=65、Caps+a=65、Caps+Shift+a=97、Shift+1=33；左右 Ctrl/Alt physical 身份；UP codepoint=0；重复 make 无事件；PrintScreen/Pause 不污染修饰；E0 每个分片点；99 ms 保留前缀、100 ms 到期清除；未知码无字符。KP slash/Enter 支持，PageUp/PageDown/Insert/NumLock 数字区仅报告物理事件而不冒充文本/方向。
- [ ] **2. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-decode`，确认 decoder RED。
- [ ] **3. 实现解码与公共声明。** DOWN 翻译、UP 身份、Caps 边沿按 spec；完整按键重复策略留给 Task 7，解码器只报告硬件真实边沿。调用 expire 的截止时间纳入后续 input 最小等待时间，不创建线程或信号定时器。
- [ ] **4. 重跑 focused；分别用 C/C++ consumer 仅包含 `gui/gui.h` 做语法编译。** 不添加 thirdpart/LVGL include 路径，也应成功。此阶段可以仅编译不链接 create/dispatch，未实现函数不能先以假成功 stub 投入产品。
- [ ] **5. 提交** `feat(gui): define native input events and PS2 decoding`。

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

- [ ] **1. 写 RED 生命周期 fixture。** 每个 open/分配/acquire/bind/LVGL create 阶段注入失败，校验首 errno、fd/lease/映射净计数；mouse 仅 ENOENT/ENODEV 可降级；二次 create EBUSY 不影响第一次；无新 acquire 的失败不 release 原 owner；offscreen synthetic 从不碰设备。
- [ ] **2. 写泵与平台断言。** mouse 读 7 字节→EIO、keyboard 短读拼接、EINTR/EAGAIN/旧空0、POLLERR 取 EOVERFLOW、HUP 取 ESTALE、NVAL→EBADF；512 FIFO 容量不足停止 read，down/up 不合并；每种设备预算、256 投递预算、余项 delay=0。`test_static_view_lost_owner` 无 flush/present 时仍停止投递；所有回调仅在 step，dispatch 不调用 LVGL。
- [ ] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-platform`，确认缺生命周期/FIFO 的 RED。
- [ ] **4. 实现 create/destroy/dispatch/get_pollfds。** config、顺序、借用 fd、Caps 初值照 spec。每个 read 上限还受 FIFO 剩余容量约束，keyboard 与 mouse 每轮均有配额；解析状态在 input 对象内，失败锁存。get_pollfds 不泄漏关闭权，synthetic=0、keyboard-only=1、完整=2。
- [ ] **5. 接入 platform step/terminal/destroy。** step 先检查 gfx state 及 raw poll 锁存错误，健康队列不在 step 内偷读，再投递 FIFO/推进重复/timers/present。最小 wait 含输入/prefix deadline。enter_terminal_state 调 stop 后 close gfx；stop 关闭 fd/解绑/停回调但不 free input，调用方随后 destroy 不 double-free；platform_destroy 自动 destroy 仍附加的 input。无 input 的 GUI-1 顺序不变。
- [ ] **6. GREEN：focused 加原 `CASE=platform`、`CASE=lvgl`；100 次 create/destroy 净资源回到基线。** 在真实 LVGL test 资源释放路径中重复确认，不以 fake 成功代替。
- [ ] **7. 提交** `feat(gui): add bounded input lifecycle and platform integration`。

### Task 7: keypad、重复、快捷键与焦点注册

**Files:** Create `libgui/src/input_focus.c`、`hosttests/cases/test_gui_input_lvgl.c`；Modify `libgui/src/input_lvgl.c`；Extend `gui_input_private.h`、GUI native integration 编译及 CASE 注册。

**Interfaces:** 实现 Task 6 LVGL 生命周期/key hooks。私有 `int gui_input_focus_add(gui_input_t *,lv_obj_t *)`、`void gui_input_focus_remove(gui_input_t *,lv_obj_t *)`、`int gui_input_focus_set(gui_input_t *,lv_obj_t *)`；外 display/无注册对象 EINVAL，重复 add=0。对象存活/顺序记录由 focus 模块管理，删除事件撤销记录；hidden/disabled 的自身与祖先在每次导航和 step 检查。私有测试 `int gui_input_test_feed_keybytes(gui_input_t *,const uint8_t *,size_t)`，只接受 synthetic，不安装进 sysroot。

- [ ] **1. 写真实 LVGL RED assertions。** textarea A↓B↓应为 `ab`；Enter↓换焦点/删除旧对象不增加任何 button click，旧 PRESSED 状态清除，新普通键首次输入保留；Tab/Shift-Tab 按注册顺序循环、跳过 ancestor hidden/disabled、空组无崩溃；点击空白保留焦点；恢复后回原顺序。
- [ ] **2. 写确定时钟 assertions。** 首次 a 在 t=0 插一次；399 ms仍一次，400 ms两次，439 ms两次，440 ms三次；真实 UP 后稳定；Ctrl/Alt DOWN 取消、UP 不恢复；Shift/Caps 变化保留字符但 REPEAT modifiers 为当前 delivered snapshot；消费 Ctrl-Q 不插 q，UP不释放其他键；Space 在 textarea 重复、在按钮一次激活，Tab/Enter/Esc 不重复。

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

- [ ] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl`，以真实 native LVGL 得到 RED，不 wrap 掉 keypad/group/event 处理。
- [ ] **4. 实现 keypad/focus 与原生回调。** keypad=EVENT，显式 read、暂停其 timer；一个非修饰活动键。Tab 调显式导航，不送 textarea；按钮 Space 映射 Enter 动作，保留原生 physical key。group 默认不自动加对象，焦点 outline 可由像素断言观察。重复由 input monotonic deadline 触发，每 step最多一次，不补发；FIFO尚有事件时先返回delay=0推进剩余边沿，不在尚未消费的UP/Ctrl事件前插入软件重复。
- [ ] **5. 完整实施 spec §6.2 取消协议。** `wait_release → 显式 RELEASED read → 清旧视觉 → 新 PRESSED`；回调中只标 pending，外层 read 返回后处理，不能递归 read。`lv_indev_reset` 不替代 keypad 重新就绪。屏幕动画/indev 暂停导致 read 未实际消费取消样本时，保留 pending，不投递新键；先在 fixture 关闭 screen transition 验证主路径，再测试 blocked read 不泄漏旧 Enter。
- [ ] **6. GREEN：运行 input-lvgl、input-platform、input-decode；检查真实事件数、textarea 精确字符串与 outline 像素。** 全部隐藏/禁用、回调删除、重复途中焦点变化均要断言下一键不丢。
- [ ] **7. 提交** `feat(gui): implement keypad navigation and safe focus cancellation`。

### Task 8: pointer、光标、拖动与 hover wheel

**Files:** Extend `libgui/src/{input_lvgl,input_focus}.c`、`libgui/internal/gui_input_private.h`、`hosttests/cases/test_gui_input_lvgl.c`。

**Interfaces:** 实现 Task 6 pointer hook；私有 `int gui_input_test_feed_mouse(gui_input_t *,const mouse_event_t *,size_t count)`，synthetic-only。pointer state 与 keypad 独立，cursor 所有权归 input；无新样本 read_cb 返回当前状态和 enc_diff=0。

- [ ] **1. 写 RED assertions。** 同 step 的左 down/up=一次 click；dx/dy 在 int16 极值下夹边正确；slider 拖动释放后不再移动；右/中只报告 native changed_buttons；光标热点/尺寸/命中透明；pointer 静止长按仍有 timer 进展。
- [ ] **2. 补 hover wheel tests。** 未点击直接悬停 container 后 wheel=+1，scroll_y 增加32；-1返回；内层到边界剩余传播父层；无目标不动作，slider不被改值；拖动中 wheel 忽略；光标移动不夺 keyboard focus。
- [ ] **3. 执行 `make PROFILE=x86_64-clang test-gui-host CASE=input-lvgl` 得到 RED，然后实现。** 新 pointer indev 显式绑定本 display，保留 TIMER；FIFO 每条显式 read，read_cb 不私自 drain 后续 FIFO。12×18 箭头使用 system layer 的程序绘制对象、清除 CLICKABLE。wheel 不写 enc_diff，不用 last_pressed；公开 LVGL hit-test/parent/scroll API 实现排除 cursor 与 hidden/disabled，再夹滚动余量。
- [ ] **4. GREEN：重跑三个 input 用户态 CASE 和既有 GUI-1 CASE。** 采样离屏 cursor/outline 像素，验证 destroy 后无 indev/timer/group/cursor 残留；mouse-only 功能不影响 keyboard-only。
- [ ] **5. 提交** `feat(gui): add pointer cursor dragging and hover scrolling`。

### Task 9: 完整 archive、sysroot 和 host 构建闭环

**Files:** Modify `libgui/Makefile`、`hosttests/Makefile`、`mk/components/run.mk`、`qemutests/gui_build_audit.py`；Extend相关 host runtime wrappers；仅在真实依赖需要时修改 `mk/components/sysroot.mk`。

**Interfaces:** libgui 生产源码清单显式包含 `platform.c tick.c input.c input_ps2.c input_lvgl.c input_focus.c`；install 增加且只增加公共 `gui_input.h`。host 的 staged UAPI symlink 增加 `uapi/mouse.h`，不把完整 kernel/include 或 OS01 libc/include 放在 native 系统头前。

- [ ] **1. 添加 build audit RED 检查。** 发布 generation 必须包含 `gui_input.h`，不得包含 input private/PS2/frame 头；普通 user flags 不含 LVGL；archive 可解析公共输入符号；真实 native LVGL fixture 复用唯一 host archive，版本来自 manifest。
- [ ] **2. 实现完整源码、host fixture 和包装接线。** input-platform 的 mock 与 input-lvgl 的 real LVGL 各自独立对象目录；新增 open/read/close/gfx bind/unbind/get_display_state 的 wrapper 只拦已登记测试 fd/handle，其他交还 `__real_*`。既有两个 GUI-1 binary 链接 platform 后新增依赖必须补齐，不能以 weak 永久空实现消除链接错误。
- [ ] **3. 执行** `make PROFILE=x86_64-clang test-gui-host CASE=all`、`make PROFILE=x86_64-clang test-static`。随后 `make PROFILE=x86_64-clang sysroot`，扩展 `gui_build_audit.py::audit_sysroot_and_build` 的既有 C/C++ syntax consumer，新增真实目标 C 链接 probe：consumer仅含 `<gui/gui.h>`、main引用create/dispatch，CRT0、sigreturn、linker.ld及库顺序复用 `user/Makefile` 的实际规则，依次 `crt0.o sigreturn_trampoline.o consumer.o -lgui -lgfx -lc`。目标工具链使用profile值，对象与ELF写audit临时目录；既有audit仅语法编译，不得将它误报为链接验证。不能靠私有include路径才能编译。
- [ ] **4. 验证增量。** 无修改重复构建不重编；touch gui_input.h 后相关 consumer/库重编；install 清除模拟 staging/gui 孤立文件；新 generation 不包含私有头。不得破坏保留的历史 generation。
- [ ] **5. 提交** `build(gui): publish native input API and register complete host coverage`。

### Task 10: 独立 guest 输入探针与可核验协议

**Files:** Create `user/test_gui_input/{app.mk,test_gui_input.c}`；Modify相关 build audit；rootfs 使用现有自动 discovery，不添加冗余手写程序清单。

**Interfaces:** `app.mk` 复用 test_lvgl 的私有 include 例外，`APP_LDLIBS := -lgui -lgfx`；仅探针允许 private focus/injection-state 查询。命令行 `--nonce <hex> --scenario <interaction|isolation|hold|fatal|overflow|owner>`；非法参数 exit2。真实输入均 DEVICES、require_mouse=true；禁止以 synthetic 或 stdin 文本替代物理键输入。

协议每行以 `[gui-input]` 开头，字段固定 `nonce=<...> seq=<递增值> phase=<...>`；READY 额外带 `pid width height` 与目标坐标；STATE 带 click_count/slider/scroll/focus/textarea 的**十六进制字节串**；EXIT 带场景断言结果。错误输出 ERROR+errno、非零退出。应用输出通过 fd1/2 写 PTY/serial 日志，必须 flush。

- [ ] **1. 为 protocol parser 添加 unittest fixture RED。** 只由 guest 生成且行首锚定的完整记录有效；错误 nonce、重复 seq、READY 前STATE、缺EXIT、textarea 非法hex均失败。parser 放 `gui_input_test.py`，可先写纯 parser，不拉 QEMU。
- [ ] **2. 实现固定测试 UI。** button、slider、可滚容器、两个 textarea、disabled button；固定逻辑坐标但按真实屏幕范围布局并输出坐标。private focus_add 注册顺序明确，outline/cursor 颜色固定供截图采样。READY 只能在 acquire/bind、首次 step/present 成功后输出。
- [ ] **3. 实现场景与观测。** interaction 用状态变化输出 STATE 并在 Ctrl-Q 返回0；isolation 不接受快捷键外的退出输入，记录待核验内容；hold 不读 stdin，以便后台运行；fatal 在READY后等待真实PS/2 F12 DOWN再触发LVGL assertion，runner可先截图确认持有画面与输入；overflow 在输出 READY 后有界暂停 dispatch 但保留 step/错误检查，接收真实 burst 后报告 EOVERFLOW。owner 场景保持静止 UI，等待被抢占。
- [ ] **4. 运行** `python3 -m unittest qemutests.test_gui_runner`、`make PROFILE=x86_64-clang disk.img`；检查自动安装 `/bin/test_gui_input`。普通 test_lvgl --smoke 仍不打开输入设备。
- [ ] **5. 提交** `test(gui): add guest input interaction probe and nonce protocol`。

### Task 11: QMP 真实交互矩阵与 runner 负契约

**Files:** Create/Extend `qemutests/gui_input_test.py`、`qemutests/test_gui_runner.py`；Modify `qemutests/gui_test.py::QmpClient/run_all`，保留原返回 outcomes schema 与 SMP matrix。

**Interfaces:** 在既有 QmpClient 增加 `input_events(self, events: list[dict]) -> None`，包装 `execute("input-send-event", {"events": events})`；`GuiInputScenarios` 消费既有 session、qmp、run_dir、smp，返回现有 case outcome dict。通过当前 `GuiCaseRunner.run_all` 接入；独立 helper 避免继续膨胀 run_all。

- [ ] **1. 写 FakeQMP assertions。** key down/up payload 使用 `type=key,data={down, key:{type:qcode,data:<qcode>}}`；rel x/y 用 `type=rel,data={axis,value}`；left/right/middle/wheel-up/down 用 btn 的 down/up。wheel 不是 abs/rel 第三轴。鼠标起点从已知中心计算，按 READY 坐标移动。
- [ ] **2. 写 runner RED negatives。** 只有命令回显、无换行prompt、跨块标记、旧 nonce、缺按键up、QMP error、截图不符、重复/乱序phase、silent timeout、一个 SMP 失败均必须最终 FAIL。FakeQMP 仅验证编排，不被描述为真实输入证明。
- [ ] **3. 实现 interaction。** 按 READY→click→rapid click→drag→hover wheel→Tab/Shift-Tab→textarea `aA!`→编辑/多行→repeat→Ctrl-Q 顺序，逐项等 STATE 并比对精确状态；不能用固定 sleep 代替应用证据。长按用有界时间观察 count 下限、释放后稳定；精确400/40边界由 host fake-clock 负责，避免让宿主负载决定 QEMU 是否通过。
- [ ] **4. 执行** `make PROFILE=x86_64-clang test-harness` 后 `make PROFILE=x86_64-clang test-qemu SUITE=gui`。既有 GUI-1 七场景及新 input 场景均在 SMP1/2 独立运行；保留 QMP transcript、serial、关键PPM与result.json。
- [ ] **5. 提交** `test(gui): verify physical input interactions through QMP`。

### Task 12: 所有权、溢出、异常退出与 shell 恢复

**Files:** Extend guest probe、`gui_input_test.py`、`test_gui_runner.py`；必要时扩展已有 `test_gui_fault` 的命令行场景，不新增产品测试设备或不受限注入 syscall。

**Interfaces:** 与 Task 10 协议一致；后台 probe 用 `--scenario hold/owner/overflow`，不读 shell 的 stdin。shell 使用 serial 启动/kill 后台进程；物理 keyboard 仍经 QMP，不能用串口结果冒充键盘恢复。

- [ ] **1. 写 fake runner RED scenarios。** missing acquired/READY、kill前未持输入、fatal返回0、屏幕恢复但PS2 canary失败、held key从未release、只出现canary命令回显、溢出未观察到EOVERFLOW，全部失败。
- [ ] **2. 实现 TTY 泄漏证明。** UI租约期间由QMP键入 `echo LEAK_<nonce> > /tmp/gui_input_leak_<nonce>` 再Enter；退出后通过串口shell检查该文件不存在，并用PS2再输入独立 `echo RECOVER_<newnonce>`，只接受新cursor后独占一行的实际输出及prompt。不能仅搜索字符串，因为命令回显包含它。每场景删除自己的临时文件，重用镜像用-snapshot。
- [ ] **3. 实现 crash/held-key。** 正常Ctrl-Q、后台SIGKILL、真实LVGL fatal分别确认屏幕归还terminal；kill时按住Shift/左键，kill完成再发送break，之后PS2 canary大小写正确、无幽灵click。记录后台 `wait <pid>` 的状态；命令超时/进程未退出不能算恢复。先释放本场景所有物理按键，再以真实PS/2 Ctrl-C清理可能残留的未提交shell输入行，观察prompt后发送canary；不通过写TTY状态或清空键盘状态掩盖幽灵修饰键。
- [ ] **4. 实现静止owner抢占。** 后台owner probe产生READY后不再dirty；另一gfx测试程序acquire，probe应在step检测LOST_OWNERSHIP并退出，后续键不再更改其状态。然后释放新owner并验证terminal与PS2恢复。
- [ ] **5. 实现确定性真实队列溢出。** probe暂停dispatch有明确有界窗口并输出READY；runner用一个QMP请求发送超容量普通make/break burst（keyboard有效容量255，至少300字节），确保pause期间确实不调用dispatch。guest观测ERROR errno=EOVERFLOW后清理并非零退出；若无法保证pause窗口，改为guest通过stdin控制的有界屏障并等待ACK，不通过增加sleep或弱化错误断言掩盖。mouse 64项溢出及全部边界仍由host精确验证，QEMU无需依赖鼠标硬件包合并规律制造特定项数。
- [ ] **6. GREEN：** `make PROFILE=x86_64-clang test-harness` 与 `make PROFILE=x86_64-clang test-qemu SUITE=gui`。SMP1/2 每种恢复路径都必须有真实PS2 canary；test runner 的串口救援只能标记失败并清理，不能转PASS。
- [ ] **7. 提交** `test(gui): gate input isolation ownership loss and crash recovery`。

### Task 13: 总回归、文档与交付证据

**Files:** Modify `docs/gui/gui.md`、`docs/driver/driver.md`、`docs/build/build.md`、`docs/roadmap.md`、`docs/changelog.md`；Create `docs/superpowers/reports/2026-10-20-gui-2-validation.md`（日期按实际执行日调整）。

**Interfaces:** 不新增产品接口。报告列出 commit/profile/工具链/LVGL版本、每个命令退出码和日志路径、SMP矩阵、失败或豁免。执行者更新本plan的checkbox与任务commit，不把未来测试写成已通过。

- [ ] **1. 执行干净回归，逐条保存完整输出和退出码：**

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

- [ ] **2. 验证Task 9增量/sysroot证据，并再次读取suite实际result.json。** 不以终端尾部PASS代替逐SMP结果；不为无新失败的已通过suite重复跑多轮。若既有parked故障复现，按同环境baseline对照，报告具体限制，不能自行宣布门禁豁免。
- [ ] **3. 更新文档。** 写明输入租约/errno/UAPI布局/恢复路径、native API与单活动键限制、focused CASE及QMP用例；roadmap仅在全部GUI-2完成条件确实达成时移入changelog，未完成则保留并注明具体缺口。
- [ ] **4. 交付前核验** `git diff --check`、`git status --short`；无意外生成文件、子模块变更或私有头泄漏。遵循 verification-before-completion；若需要整体代码评审，使用 requesting-code-review 并修复实质性问题，不能把review建议直接当成测试结果。
- [ ] **5. 提交** `docs(gui): record GUI-2 input and focus validation`，向用户报告最终行为、验证证据和实际限制；不要自动merge/push。

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
