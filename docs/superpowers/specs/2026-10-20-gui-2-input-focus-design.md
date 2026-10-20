# P3 GUI-2 输入与焦点设计规范

> **日期**：2026-10-20
>
> **状态**：Draft v2，子 agent 两轮评审后 APPROVE（2026-10-20），待用户评审；本文只定义设计，未实施
>
> **代码基准**：OS01 `d829b36f`；LVGL v9.5.0，版本仍以 `thirdpart/lvgl.manifest` 为准
>
> **任务来源**：[roadmap P3 GUI-2](../../roadmap.md#-p3-gui)
>
> **前置**：[GUI-1 spec](2026-10-19-gui-1-lvgl-platform-backend-design.md)、已落地的 libgui、PS/2 输入、poll、显示所有权协调器

## 1. 目标、范围与设计选择

让单进程全屏 libgui 应用能够通过真实 PS/2 鼠标、键盘操作 LVGL 控件：点击、拖动、滚动、ASCII 文本编辑、修饰键、按下/释放、重复、Tab/Shift-Tab 导航及应用快捷键。应用退出、异常终止或失去显示所有权后，物理键盘恢复给 console TTY，terminal/ash 保持可用。

本设计沿用 GUI-1 的单 UI 线程、单平台实例、应用持有主循环、公共接口隐藏 LVGL 类型、离屏验证与屏幕验收分离的约束。本文将“焦点”限定为进程内控件焦点；进程间路由、窗口焦点和输入抓取协议属于 GUI-5。

### 1.1 完成条件

1. `/dev/keyboard` Set 1 字节流及 `/dev/mouse` 的 8 字节事件经过 libgui 转换，进入绑定到平台 display 的 pointer/keypad indev。
2. 一个输入租约只对应一个显示 view、一个 keyboard open file description，以及至多一个 mouse open file description。租约期间其他读取者不能消耗这些设备的输入，物理键盘不再同步注入 console TTY。
3. 鼠标左键操作控件，指针可见；滚轮作用于指针所在的滚动区域；右/中键以原生事件提供给应用。控件焦点顺序、点击取焦、禁用/隐藏/删除行为均可验证。
4. 快速 make/break、跨 read 的 E0、多键组合及长按没有丢释放、重复提交或后台 shell 执行 GUI 输入的问题。
5. 无设备、失败回滚、队列溢出、显示失效和进程崩溃具有确定语义；宿主测试与 SMP 1/2 QEMU 验收分别提供证据。

### 1.2 不包含

- GUI-3 Material 桌面改写、完整原生控件 facade、窗口服务器、窗口化 terminal。
- 中文输入法、Unicode 键盘布局、剪贴板、组合键文本输入、USB HID、触摸及 aarch64 GUI。
- 通用 evdev、多 seat、热插拔、可配置键位/鼠标加速、系统级全局快捷键。
- 用 TTY 字节流代替扫描码输入；修改全局 termios 或关闭 ISIG 作为隔离手段。

### 1.3 方案取舍

| 方案 | 优点 | 代价与结论 |
|---|---|---|
| 仅在应用约定一个 raw 读取者 | 改动最少 | raw ring 仍可能被其他进程读走，且键盘仍注入 TTY，不能满足完成条件 |
| **显示 view 绑定输入租约（采用）** | 延用已有所有权与 final-close 回收；保留输入 payload ABI；可在 GUI-2 内形成闭环 | 需要少量 file-aware devfs 分发与内核输入路由，作为本任务第一阶段 |
| 提前建设 evdev/window server | 长期扩展性好 | 新增事件 ABI、IPC、权限与多进程生命周期，范围超过 GUI-2，留给 GUI-5 |

这里的租约保证路由和资源回收，不构成新的进程权限边界。现有 gfx acquire/原始设备访问权限仍沿用仓库协议；GUI-5 再统一收紧图形与输入旁路权限。

## 2. 当前实现与必须补齐的边界

以下结论以代码为准，不能仅据 GUI-1 旧 spec 推断：

| 当前实现 | 对 GUI-2 的约束 |
|---|---|
| `libgui/src/platform.c` 在首次 pending present 时才 acquire gfx；无输入时也不持续查询前台身份 | 输入绑定前必须 acquire；输入激活后每次 step 检查显示身份，静止画面也能停止投递 |
| `keyboard.c::kbd_consume()` 同时发布 raw ring 并调用 `translate_and_push()` | 显示独占不等于输入独占；必须在内核分流点抑制 TTY 路径 |
| keyboard ring 为全局 256 字节数组，满时丢新字节；读取无完整消费者锁 | GUI 模式必须加锁并报告失步，不能把丢 break 当作长按 |
| mouse ring 有锁，64 项，满时丢最旧事件；丢失计数尚未成为用户态状态 | 租约期间溢出必须显式失败，不能静默吞掉按钮边沿 |
| `mouse_event_t` 已归一化为 dx 向右、dy 向下 | 用户态不能再反转 dy；wheel 符号单独通过测试固定 |
| devfs 有 `ioctl_file`/`release_file`，read 与 poll 主要按 node 分发 | 增加可选 file-aware read/poll；保持旧设备回退路径 |
| `terminal.c` 即使不在图形前台也继续转发 `/dev/tty` 输入到 PTY | 仅暂停 terminal 绘图不能阻止 shell 接收 GUI 输入 |
| LVGL `lv_indev_data_t.enc_diff` 可用于 pointer，但 `indev_proc_pointer_diff()` 使用 `last_pressed` | 直接写 enc_diff 不能保证“悬停即滚动”，需要明确的 hover wheel 适配 |

代码依据：`kernel/driver/{keyboard,mouse,mouse_proto,fb_state,gfx}.c`、`kernel/fs/{devfs,file,poll}.c`、`kernel/include/fs/devfs.h`、`user/terminal/terminal.c`、`thirdpart/lvgl/src/indev/lv_indev.{c,h}`。三方源码不修改；实现时以仓库锁定版本验证这些调用语义。

## 3. 分层、文件与所有权

```text
i8042 → keyboard/mouse → input_route（输入租约与队列保护）
                         ├─ 无租约：现有 raw 设备 + console TTY
                         └─ 有租约：指定 raw files，禁止 keyboard → TTY
                                      ↓ poll/read
                         libgui input decode / routing
                         ├─ pointer indev + cursor
                         ├─ keypad indev + focus group
                         └─ native shortcut / auxiliary button callback
                                      ↓
                           gui_platform_step → render / present
```

拟新增/修改的文件边界：

| 路径 | 职责 |
|---|---|
| `kernel/driver/input_route.c`、`kernel/include/driver/input_route.h` | 租约、失步状态、切换边界、read/poll 授权；源/头对称 |
| `kernel/driver/{keyboard,mouse}.c` 及对应 driver 头 | 输入发布、物理按键跟踪、队列锁、file-aware 回调 |
| `kernel/driver/{gfx,fb_state}.c`、`kernel/include/uapi/gfx.h` | bind/unbind ioctl、显示切换时撤销租约 |
| `kernel/include/fs/devfs.h`、`kernel/fs/{devfs,poll}.c` | 可选 file-aware read/poll 分发；devfs VFS read_file 桥接 |
| `kernel/arch/x86_64/platform/boot.c` | 注册 keyboard/mouse 新 ops，不在 kernel_main 硬编码 |
| `libgfx/gfx.{h,c}` | 原生 bind/unbind 包装，不暴露内部 gfx fd |
| `libgui/include/gui/gui_input.h` | 无 LVGL 类型的输入 API；由 `gui.h` 聚合 |
| `libgui/src/{input,input_ps2,input_lvgl}.c` | 生命周期/设备、纯扫描码状态机、LVGL 与焦点适配 |
| `libgui/internal/gui_input_private.h` | 平台内部挂接、焦点桥接与确定性测试注入；不安装 |
| `user/test_gui_input/` | 独立交互验收程序；不把 GUI-1 smoke 改成依赖设备 |
| `hosttests/cases/test_input_route.c`、`test_gui_input*.c` | 内核路由、纯解析及真实 LVGL 集成测试 |
| `qemutests/gui_test.py`、`test_gui_runner.py` | 扩展已有 gui suite 的输入场景和 runner 负契约 |

UI 对象、indev、group、光标和所有回调仅在 UI 线程访问；IRQ 不调用 libgui/LVGL。一个 platform 至多附加一个 input；platform 销毁先销毁 input，再删除 display 和 lv_deinit。input 不持有 platform 的额外生命周期引用，应用不得在销毁平台后使用旧 input 指针。

## 4. 内核输入租约（本任务必需前置）

### 4.1 UAPI 与 libgfx 包装

保留 keyboard 字节流和 `mouse_event_t` 的已有布局。扩展 `uapi/gfx.h`：

```c
#define GFX_BIND_INPUT   0x470A
#define GFX_UNBIND_INPUT 0x470B

typedef struct {
    int32_t keyboard_fd; /* 必填，当前进程打开的 /dev/keyboard */
    int32_t mouse_fd;    /* /dev/mouse；-1 表示仅键盘 */
    uint32_t flags;      /* v1 必须为 0 */
    uint32_t reserved;   /* 必须为 0 */
    uint64_t generation; /* 输入为 0；成功输出租约 generation */
    uint32_t initial_modifiers; /* 输入为 0；输出 bit0=Caps Lock，其余为 0 */
    uint32_t reserved2;   /* 输入/输出均为 0 */
} gfx_bind_input_t;      /* sizeof == 32，增加 size/offsetof 静态断言 */

typedef struct {
    uint64_t generation;
    uint32_t initial_modifiers;
    uint32_t reserved;
} gfx_input_state_t;     /* libgfx 的原生结果结构，sizeof == 16 */

/* libgfx，0 成功，-1 并设置 errno；结构体由包装层构造。 */
int gfx_bind_input(gfx_handle_t *h, int keyboard_fd, int mouse_fd,
                   gfx_input_state_t *out_state);
int gfx_unbind_input(gfx_handle_t *h);
```

实施前再次核对 ioctl 编号无新增冲突。bind 必须以 fault-tolerant copy 验证参数，获取暂时的 file 引用、核对精确设备 ops、读权限、view 配置和当前显示 owner；不能用路径字符串或 PID 相等代替设备/文件身份验证。非 owner 返回 EACCES，错误 fd 返回 EBADF，错误设备/flags/reserved 返回 EINVAL，view 过期返回 ESTALE；已有租约或 files 已绑定返回 EBUSY。成功后释放暂时引用。

bind 不隐式 acquire，也不抢占已有输入租约。该 ioctl 是 INOUT：验证完整结构可读写，输入结果字段必须为 0；在路由切换的线性化点采样 Caps 状态，解锁后 copy_to_user，复制失败撤销此次绑定并返回 EFAULT，不能在 spinlock 内接触用户地址。libgfx 的 out_state 必填。unbind 只作用于本 view，未绑定时成功；已撤销记录允许清理，不能解除其他 view 的租约。无租约的旧 gfx 客户端行为保持兼容。

### 4.2 路由状态机

| 状态 | keyboard/mouse read | poll | keyboard → TTY |
|---|---|---|---|
| NORMAL | 沿用旧 ABI | 沿用旧 readiness | 正常 |
| ACTIVE | 仅绑定的 open file descriptions 可消费；空队列统一 EAGAIN | 对应队列有数据才 POLLIN | 禁止 |
| DESYNC | 绑定 files 返回 EOVERFLOW；其他 files 返回 EBUSY | 绑定 files 返回 POLLERR | 继续禁止，等待应用清理或 final-close |
| REVOKED（旧 files 的锁存状态） | 不再消费新数据；owner 丢失为 EACCES，模式改变为 ESTALE | POLLERR\|POLLHUP | 恢复 NORMAL，旧 files 不自动加入新会话 |

状态是租约实例的状态；撤销后系统路由可以恢复 NORMAL，同时旧 files 仍锁存 REVOKED，直到清理/关闭。不得通过清空全局指针让旧绑定 files 悄悄变成普通读者。

ACTIVE 时其他打开的 keyboard/mouse files 返回 EBUSY；其 poll 返回 POLLERR，避免永远睡眠或偷走数据。租约支持的唯一读取者指一个共享 open file description，dup/fork 的别名共享该身份；libgui 应用禁止另起读取线程或让子进程读取继承 fd。文件别名不提供进程间隔离。

切换到新 view、release、gfx 最终关闭、绑定 raw file 最终关闭、显示 generation 改变及后端失败均撤销整个租约。清理必须在 view/file 内存释放前完成，并唤醒相关 pollers。不能用 PID/整数 fd 作长期 cookie；使用受锁保护的 file/view 身份及租约 generation，防止 fd 复用和旧回调撤销新租约。长期绑定关系不持有会导致 final-close 无法触发的互相强引用。

DESYNC 不在 IRQ 内调用 gfx release 或拿睡眠 mutex。它使输入立即报错；libgui 下一次 dispatch/step 终止并关闭 gfx，触发既有 fallback 恢复。进程被杀时同样经 final-close 回收。本文不增加任意时限的内核租约看门狗。

### 4.3 队列、切换边界与并发

- bind 原子清除旧 raw 队列、建立新会话；libgui 在创建时初始化自己的解码状态。内核不能直接清除用户态对象；它不清空 console TTY/PTY 已有数据，不吞掉串口输入。bind 前已排队到 TTY 的字节属于旧会话，不能承诺追溯删除。
- **完整序列是 keyboard 路由单位**：在 raw ring/TTY 分流之前，由内核组帧器缓存完整普通 make/break、E0、PrintScreen 和 Pause 序列，更新物理键状态后，整段发布或整段丢弃。原始字节内容不变；raw read 仍可拆分完整入队的序列，但不得仅因队列剩余空间不足而发布半段。有效容量不足以容纳整段时，ACTIVE 进入 DESYNC，NORMAL 整段丢弃。TTY 的翻译器只接收完整序列，PrintScreen/Pause 不参与 Shift/Ctrl 状态更新。
- 组帧器在序列首字节记录路由 epoch。bind/unbind/撤销增加 epoch，清空旧消费者队列及旧 TTY 翻译前缀/瞬态修饰状态，**不重置物理组帧器**。跨 epoch 才完成的序列仍更新物理状态，但向新旧消费者均不发布；若它是 make，该键列入新消费者隔离集合直至 break。不能将 `E0` 前缀丢在旧会话、把 `1D` 尾字节当普通左 Ctrl 投给新会话。隔离键必须连前缀整段吞掉；只处理 break 的物理状态，不将其作为新会话孤立 UP 投递。
- 路由层跟踪物理 Set 1 按键状态，包括 E0 左右修饰键；bind/unbind 时仍按住的键隔离到对应 break，再接受下一次 make，避免启动按键、旧 Shift/Ctrl、长按尾部落入新消费者。鼠标在边界时仍按下的按钮同样隔离到释放；新租约初始逻辑按钮为 0。
- Caps Lock 是物理键盘的全局锁定状态，即使禁止 TTY 投递也持续跟踪。bind 在清队列、建立按住键隔离的同一临界区返回初始 Caps；切换之后的 Caps 边沿进入新队列，用户态从该快照继续解码，不能再查询当前状态覆盖它而双算排队边沿。TTY 恢复时采用此时的全局 Caps，其他修饰键按隔离规则等待新的 make。
- 每个原始输入发布和消费者出队在 IRQ-safe 锁下完成。keyboard 不再只靠 volatile 假定单消费者；读取到内核 bounce buffer，锁内不 copy 用户内存。
- 租约中的 keyboard 满、mouse 满均进入 DESYNC 并唤醒消费者，不再采用静默丢新/丢旧策略。NORMAL 保留兼容行为。用户态内部容量耗尽同样终止，不能覆盖按键/按钮边沿。
- 既有 i8042 demux 锁在最外层。允许 `i8042 → input_route → device queue/poll`；显示控制路径允许 `display_mutex → display_state_lock → input_route → device queue/poll`。禁止 input_route/device 回调再取 i8042、display_state_lock 或 display_mutex；IRQ 不访问 fb 状态。所有者校验与租约提交在同一显示状态临界区，不能先查 owner、解锁后再绑定。
- 撤销必须与设备出队/发布串行化。read 的消费线性化点为锁内出队；撤销前已复制给用户的字节无法收回。libgui 在投递前再核对 owner，且 REVOKED 后清空本地待投递事件；不声称屏幕切换与用户回调能做到跨 CPU 零延迟原子性。

`devfs_ops` 增加可选 `read_file`/`poll_file`，优先于 node 回调；devfs 的 VFS `read_file` 桥接与 `fd_poll(FD_DEV)` 传递真实 file。未提供新回调的所有设备保持原 fallback。file-aware poll 的等待注册与 readiness 检查必须共用队列锁，覆盖“检查空到挂 waiter”之间的 IRQ，不能引入 lost wakeup。struct 改动后执行 `make clean`。

## 5. libgui 原生接口与主循环

### 5.1 公共 API

`gui_input.h` 安装进 staging/gui，由 single-writer sysroot 发布；不能导出 `lv_*` 类型、私有桥接头或测试注入接口。

```c
#include <stdbool.h>
#include <stdint.h>
#include <poll.h>
#include <gui/gui_platform.h>

typedef struct gui_input gui_input_t;

typedef enum {
    GUI_INPUT_DEVICES = 0,
    GUI_INPUT_SYNTHETIC = 1
} gui_input_mode_t;

typedef struct {
    gui_input_mode_t mode;
    bool require_mouse; /* false：mouse 缺失时允许 keyboard-only */
} gui_input_config_t;

gui_input_t *gui_input_create(gui_platform_t *platform,
                              const gui_input_config_t *cfg);
void gui_input_destroy(gui_input_t *input); /* NULL 安全 */
/* 返回 0..2 个借用 fd；不允许调用方 read/close/更改 flags。 */
int gui_input_get_pollfds(gui_input_t *input, struct pollfd out[2]);
bool gui_input_has_mouse(const gui_input_t *input);
/* 仅泵输入，不渲染；0 成功，-1 锁存 errno，平台下次 step 终止。 */
int gui_input_dispatch(gui_input_t *input,
                       const struct pollfd *fds, unsigned count);
```

NULL config 等于 DEVICES、require_mouse=false。DEVICES 只允许屏幕平台，SYNTHETIC 只允许离屏平台，组合错误 EINVAL；后者不打开设备、不 acquire gfx，仅供仓库内部测试注入。每个平台最多一个 input，重复创建 EBUSY。键盘必需，打开失败透传 errno；鼠标仅 ENOENT/ENODEV 可按 require_mouse=false 降级，其他错误不得静默忽略。鼠标缺失时隐藏光标，但键盘与焦点可用。get_pollfds 顺序固定 keyboard、mouse，events=POLLIN、revents=0；keyboard-only 返回 1，synthetic 返回 0，终止对象返回 -1 并设置锁存 errno。

创建顺序：检查 platform 活跃 → 打开 O_RDONLY|O_NONBLOCK 设备 → 分配固定容量状态、indev/group/cursor → acquire view → bind input 并读取其初始 Caps 结果 → 挂接 platform。失败逆序回滚，保存最初 errno；若此次 acquire 成功而随后失败，release view，不能遗留仅显示独占的半初始化状态。已有 acquire 的平台失败时不擅自 release 它原有的显示所有权。

destroy 先停止回调及重复、重置 indev（取消未完成手势，不合成 click），删除 indev/cursor/group，unbind 并关闭 raw files，再从 platform 脱离。销毁 input 本身不 release 平台仍拥有的 gfx；平台销毁/终止态负责 release/close。回调不可同步销毁 platform/input 或递归 step；回调应设置应用退出/切换标志，循环边界再执行。

原生快捷键接口另加 `gui_input_set_key_handler(input, handler, userdata)`：handler 接收 `gui_key_event_t` 并返回 bool consumed。事件包含 `uint16_t physical_key`、`uint16_t modifiers`、`uint32_t codepoint`、`gui_key_action_t action`（DOWN/UP/REPEAT），枚举取值在原生头内固定；physical_key 使用普通 Set 1 code，E0 key 为 `0x100 | code`。修饰位区分 L/R Shift/Ctrl/Alt 及 Caps；codepoint 仅 ASCII 或 0，不借用 LVGL key 常量。右/中键通过只读 `gui_input_set_pointer_handler` 回调暴露坐标、完整 buttons、changed_buttons、wheel；左键和 wheel 的 LVGL 默认处理不可由该回调吞掉。完整控件事件 facade 留给 GUI-3。

### 5.2 poll 与步进契约

```text
create platform + build UI + create input
循环：
    gui_input_dispatch(input, NULL, 0)     # 非阻塞泵；失败时清理并非零退出
    gui_platform_step(platform, &delay)   # 消费输入、重复、布局、刷新、present
    若平台终止或应用请求退出：跳出
    n = gui_input_get_pollfds(input, fds)
    poll(fds, n, delay)
    EINTR：回到循环并处理退出标志
    其他失败：清理并非零退出
    gui_input_dispatch(input, fds, n)      # 失败时清理并非零退出
destroy input + platform
```

dispatch 校验 fd/count；NULL,0 合法，表示立即检查本对象设备，不能阻塞；SYNTHETIC 返回 0。每轮最多读 256 个 keyboard 字节和 64 个 mouse 事件，两类都给预算，不能因单设备洪水饿死另一设备或显示。mouse read 长度必须是 8 的倍数，非完整记录视为 EIO；keyboard 短读合法且保留前缀。EINTR 下一轮继续、EAGAIN 本轮结束；旧 NORMAL 的 keyboard 空读 0 不可被本轮当永久 EOF。

所有解码后事件进入有界 FIFO（512 项），按设备内顺序保留；跨设备没有硬件时间戳，固定每轮 keyboard 后 mouse，不声称总时间排序。dispatch 还受 FIFO 剩余容量约束，容量不足时停止 read，要求下一次 step 先消费，不能因为自己的泵预算触发假溢出；实际无法保存已读事件才终止为 EOVERFLOW。连续纯移动可合并，不能跨 buttons/wheel 边沿合并；同一轮 down/up 都必须送达 LVGL，不能只留下最终 released 快照。

dispatch 只读/入队，LVGL 调用和应用回调统一在 step 内执行。step 在输入投递前检查显示前台和租约错误，再按 FIFO 投递，之后推进 LVGL timers、最多一次 present。延迟取 GUI-1 等待时间、输入重复/长按所需时间与待处理 FIFO 的最小值；有余项返回 0，空闲沿用 100 ms 上限。indev 读取不得在回调内无限 drain；每 step 最多投递 256 项。

输入 POLLERR/HUP 时执行一次非阻塞 read 取得锁存原因；POLLNVAL 直接按 EBADF 处理，不能只凭 poll flags 丢失 EOVERFLOW/ESTALE 区别。永久 read 失败锁存；EACCES → LOST_OWNERSHIP、ESTALE → DISPLAY_CHANGED，其余永久错误（含 EOVERFLOW/EIO/EBADF/EBUSY）→ ERROR。输入错误进入现有平台终止清理路径，先停止输入，再释放显示。即使无 dirty frame，step 也检查 owner；不能只在 gfx_present 失败时才停止键盘重复。

## 6. 扫描码、键义与重复

### 6.1 Set 1 状态机

- 普通 make/break、E0 前缀可跨任意 read 分片；物理键身份独立于翻译后字符，UP 使用对应 DOWN 的身份与缓存键义。
- 分别跟踪左右 Shift/Ctrl/Alt；Caps 只在从 up 到 down 的真实边沿翻转，重复 make 不翻转。字母使用 Shift XOR Caps，数字/标点仅 Shift 影响；采用 US ASCII 布局。跨 epoch 的 Caps make 若完成于 bind 之后，不发布该旧序列，但须将 ACTIVE 标记为 DESYNC（EOVERFLOW），避免初始 Caps 快照与新会话键义分叉；清理重建后取得新快照。
- 正确吞掉 PrintScreen 的 `E0 2A E0 37` / `E0 B7 E0 AA` 假 Shift 序列；Pause 的 `E1 1D 45 E1 9D C5` 整序列忽略。二者 v1 无应用键义，但不得污染 Ctrl/Shift。
- 未知/损坏前缀恢复到边界，不生成字符；截断前缀 100 ms 后清除。组合序列解析表与转移测试完整列出，不把 fake Shift 当普通修饰键。
- 主键区可打印 ASCII；Enter、Esc、Tab、Backspace、E0 arrows/Home/End/Delete、KP Enter、KP slash 支持。PageUp/PageDown/Insert 和数字小键盘的 NumLock 布局 v1 不产生控件操作（可报告原始键身份），不得映射为不相干的方向键。F1..F12 可供应用快捷键使用，无默认控件动作。

### 6.2 投递、快捷键和重复策略

1. 更新物理键/修饰状态，再给原生 key handler。事件 modifiers 为该事件处理时刻的修饰状态快照；DOWN 的 codepoint 由该快照翻译，Ctrl/Alt 组合为 0；UP 的 codepoint 恒为 0，物理身份仍与 DOWN 相同。REPEAT 采用活动键 DOWN 缓存的字符与当前 modifiers，发生 Ctrl/Alt 取消后不再产生 REPEAT。未消费的 Ctrl/Alt 组合也不向 textarea 插入控制字节或普通字母。Ctrl-C 不是内核 SIGINT，不默认退出 GUI。
2. 被消费的 DOWN 连同后续 REPEAT/UP 归同一消费路径，不能 UP 意外释放其他控件。handler 可按 Ctrl-Q 设置退出标志；Q 本身在 textarea 中照常输入，不能作为全局退出键。
3. 普通键向 keypad indev 映射为 LVGL 特殊键或 ASCII。同一时刻仅一个非修饰键为 keypad 活动键：任意新非修饰键 DOWN（包括随后被应用消费的键）先取消旧活动键，再决定新键是否进入 keypad；旧键 UP 仅更新物理状态。释放新键后不恢复旧键的重复。取消不能仅调用 `lv_indev_reset()`，必须执行下述重新就绪协议。
4. 硬件 typematic 的重复 make 只保留物理按住状态，不再次插字。重复由 libgui 单调时钟产生：首次延迟 400 ms，后续 40 ms；只重复可打印字符、Backspace/Delete、arrows/Home/End。Tab、Enter、Esc、Caps、快捷键不重复执行动作；Space 在按钮上为激活键，不进入重复，在 textarea 中才按可打印字符重复。
5. keypad 使用 EVENT 模式，每个逻辑边沿显式调用 `lv_indev_read()`，不能依赖 `continue_reading` 自动排空（该模式会忽略它）。重复可打印/编辑键以完整的 RELEASED→PRESSED 对注入，最后一次保持按住直到真实 UP；无 Enter/Tab 合成点击。LVGL keypad 定时读取暂停，防止与软件重复叠加。pointer 的 long-press 时间处理单独保留，不能一并停掉。
6. 每 step 每活动键最多生成一个到期重复，下一期限从当前时刻向后计算，不补发调度停顿积累的整串重复。Shift/Caps 变化不更改已按下键缓存的字符；重新按键才采用新字符。Ctrl/Alt 的总状态从无到有时，先取消当前 keypad 活动键和重复，再报告 modifier DOWN；Ctrl/Alt 释放后不恢复旧键，必须等待新的非重复 DOWN。不能在按住 `a` 后按 Ctrl 仍持续向 textarea 插入 `a`。

按键切换、焦点变化、租约终止及输入失步都取消重复。当前焦点控件被删后，旧按键不能继续作用于自动选中的新控件；必须等待新的 DOWN。

**keypad 无点击取消与重新就绪协议**：锁定版本的 `lv_indev_reset()` 只设置 reset_query 并清理 pointer 字段，不清 `keypad.last_state/last_key`，不能当作完整 keypad release。适配层停止旧键重复，清空尚未投递的旧键事件，调用 `lv_indev_wait_release(keypad)`，让下一次 read_cb 提供一次 RELEASED，再显式 `lv_indev_read(keypad)`。`wait_until_release` 分支会先将 keypad.last_state 置为 RELEASED，从而跳过旧 Enter 的 RELEASED/CLICKED；此合成样本不生成原生 UP 回调。旧控件仍存活时，显式移除 `LV_STATE_PRESSED` 视觉状态；删除回调中只作失效标记，不再访问旧指针。完成这一读之后才投递新键 PRESSED，保证 A↓、B↓ 中 B 走真正的首次按下分支。`lv_indev_reset()` 可继续用于 pointer 手势清理，但不能替代本协议。

若失焦/删除发生在 LVGL 输入回调内，只记录待取消标志；外层 `lv_indev_read()` 返回后、投递下一个 FIFO 事件之前执行上述协议，不能递归调用 read 或在回调中销毁 indev。读取旧对象前用焦点注册记录的存活状态核验，不能靠缓存的对象地址假设对象仍存在。

## 7. 鼠标、光标与焦点

### 7.1 Pointer 与滚轮

- 初始坐标为视图中心。使用 int32/int64 中间值累加 dx/dy，再夹到 `[0,width-1] × [0,height-1]`；v1 不加速，1 计数=1 像素。固定 12×18 程序绘制箭头，热点 (0,0)，放在该 display 的 system layer，清除 CLICKABLE，不截获 hit-test。不新增图片/字体依赖。
- 左键对应 pointer PRESSED/RELEASED；移动和按下/释放按顺序送入 pointer indev，拖动、slider 和滚动手势使用 LVGL 现有机制。右/中键只报告原生回调，不模拟左键或焦点切换。
- pointer 保留 LVGL TIMER 模式以推进长按/拖动定时器；新 FIFO 事件通过显式 read 及时送达，定时器无新数据时仅返回当前 pointer 状态且 wheel=0。手动排空有界，不能反复返回旧 wheel。取消时用 reset，不补 click。
- 垂直滚轮正值定义为向下滚动内容位置（scroll_y 增大）；每刻度 32 px。v1 将当前 mouse_event wheel 原值按此符号使用，以 PS/2 原包 fixture + QMP wheel 实测作为门禁；若硬件与此方向不符，修正适配层映射并更新对应 fixture，不改变 mouse UAPI。基础三字节鼠标无滚轮，按 wheel=0 工作。
- 本版 `enc_diff=0`，不借 `last_pressed` 实现滚动。对当前指针位置命中对象（排除 cursor、hidden/disabled），从最近的可垂直滚动祖先开始，使用公开 `lv_obj_scroll_by(..., LV_ANIM_OFF)` 及滚动余量查询，按上下可用范围夹紧；到边界后将余量交给父滚动容器。无目标则无动作，不把滚轮变成修改 slider 值或键盘焦点。左键拖动期间忽略 wheel，避免手势冲突。

### 7.2 单默认焦点组

input 为 display 建一个私有默认 `lv_group_t`，keypad 显式绑定它，不依赖 LVGL 全局默认 group 自动收集对象。GUI-2 的交互验证程序通过内部桥接注册/移除控件，GUI-3 再将这部分包装到原生 widget facade；公共 sysroot 不先引入一个泄漏 LVGL 的 focus API。

内部接口：`gui_input_focus_add(input, lv_obj_t *)`、`gui_input_focus_remove(...)`、`gui_input_focus_set(...)`，位于私有头；只接受同 display 的对象，重复注册不增加项，注册顺序即 Tab 顺序。

| 动作 | 规定行为 |
|---|---|
| 首次建立焦点 | 第一个可聚焦控件；组为空则无焦点 |
| Tab / Shift-Tab | 在可聚焦项中前进/后退并循环；一个 make 只走一次；由适配层调用 group focus，Tab 不插入 textarea |
| 左键点击 | 可聚焦控件成为组焦点；空白/不可聚焦对象保留已有键盘焦点 |
| Enter | 按下/释放交给控件；按钮点击只计一次；textarea 使用其单行/多行配置 |
| Space | textarea 输入空格；按钮在 make/up 时激活一次，不触发长按连击；其他控件沿用明确的默认键义 |
| arrows/Home/End/Backspace/Delete | 路由给焦点控件；textarea 编辑、slider 方向操作；不额外改变 Tab 顺序 |
| Esc | 交给焦点控件取消操作，不默认结束进程 |
| hidden/disabled/删除 | 从导航候选中排除；若是当前焦点，选其后第一个可用项，找不到则无焦点；取消旧键/手势，不能解引用已删除对象 |

焦点组成员保留对象注册顺序；隐藏/禁用期间不占导航位置，恢复后回到原位置。移除/删除通过对象事件维护，祖先 hidden/disabled 同样排除；空组、全部禁用以及回调删控件都有测试。焦点视觉至少提供可观察的 outline，pointer hover 不自动夺取键盘焦点。

## 8. 错误、资源与构建约束

- 默认 keyboard-only 降级通过 `gui_input_has_mouse()` 可观测，require_mouse=true 时缺失即创建失败。运行中设备永久失败不自动重开；应用销毁重建，防止把不同输入会话拼接。
- 设备/状态对象的 malloc/free 与 fd 打开/关闭一一对应；FIFO 和解码状态采用固定容量，纯移动合并不增加长期堆占用；不为每个输入事件分配内存。
- input 的 LVGL 对象计入既有 4 MiB TLSF 池；可失败分配执行回滚，LVGL fatal assertion 沿用 GUI-1 `_exit(1)`，内核 final-close 必须恢复输入与画面。不可在 fatal handler 中依赖 LVGL 清理。
- `libgui/Makefile` 显式追加平台源码、安装 `gui_input.h`，保持配置唯一源、私有头不发布、staging/gui 清理和 manifest；公开应用仍链接 `-lgui -lgfx`。修改 kernel struct 后 clean rebuild；旧无输入 `test_lvgl --smoke` 必须继续成功。
- 不修改旧 desktop/tetris 的交互架构；它们未持租约时仍走兼容路径，竞争 GUI 租约时不得偷走 raw 事件。内核新增/修改运行日志统一 `log_*`；输入日志默认不逐键输出用户文本。

## 9. 验证与交付门禁

### 9.1 宿主测试

| 层次 | 必测内容 |
|---|---|
| 路由/ABI | bind 参数、用户地址 fault、非 owner、错误设备与权限、重复绑定、dup/fork/最终关闭、fd 重用；size/offset；绑定期间旁路读 EBUSY，poll 无 lost wakeup |
| 切换/失步 | owner/generation 切换撤销；无脏帧也停止输入；keyboard/mouse 溢出锁存 EOVERFLOW；held key/button 隔离；bind/unbind/撤销落在 E0/PrintScreen/Pause 每个分片点，右 Ctrl 不变幽灵左 Ctrl、fake Shift 不泄漏；Caps 初始同步与跨 epoch make 的失步处理；串口不被禁用；租约与 file 生命周期锁顺序 |
| Set 1 纯解析 | 每个合法序列的所有分片边界；普通/E0 make+break；左右修饰键、Shift XOR Caps、typematic、损坏/截断前缀、PrintScreen/Pause；多键切换与未知键 |
| 确定时钟 | 399/400/439/440 ms 重复边界、UP 停止、无补发风暴、Tab/Enter/快捷键不重复；按住 a 后 Ctrl/Alt DOWN 立即取消重复，释放修饰键不恢复；Shift/Caps 保留缓存字符且事件 modifiers 为当前快照；失焦/删除/终止后不重复 |
| 真实 LVGL | 用锁定版本而非只靠 mock：快速 click down/up、slider 拖动、hover wheel/嵌套边界、焦点顺序和 outline、textarea 内容、Space/Enter、隐藏/禁用/删除/空组、消费快捷键不插字；A↓B↓ 保留 B 首次输入、Enter↓后换焦点/删除无幽灵 click 与残留 pressed 状态 |
| 生命周期 | 每个打开/分配/bind 阶段故障回滚；100 轮 create/destroy 的 fd/mmap/池占用回到稳定基线；offscreen 不访问设备；公共 sysroot include 隔离 |

host 的 synthetic 注入仅在私有测试桥接提供，走相同解码/队列/适配路径。fake 时钟替代 sleep；像素证明 outline/cursor，控件状态证明 click/drag/text，不能只断言“回调被调用”。内核并发关键线性化用可控 interleaving fixture，另用 QEMU 验证真实 SMP；no-op mock spinlock 不充当并发正确性证据。

### 9.2 QEMU 输入验收

扩展现有 `make PROFILE=x86_64-clang test-qemu SUITE=gui`，保留 GUI-1 原用例，并在独立 SMP=1、SMP=2 启动内运行输入子集。物理输入经 QMP `input-send-event` 的键 make/break、相对位移、按钮与 wheel 注入；串口只用于启动命令和采集证据，不能替代键盘扫描码。

`/bin/test_gui_input` 建立固定布局：button、slider、scroll container、两个 textarea、一个 disabled button；UI/租约均就绪后输出带 run nonce 的 READY（不含可由命令回显伪造的最终成功句）。布局输出尺寸/目标坐标，runner 不假设固定显示分辨率。每次操作需获得应用状态断言，关键时点另取 QMP screenshot。

| 场景 | 验收依据 |
|---|---|
| click / rapid click | 左键按下释放在一个 step 前到达仍只产生一次 click；右/中不误触 |
| drag | slider 从低值拖至高值，释放后移动不再拖动；屏幕边缘夹紧 |
| wheel | 未点击直接悬停滚动；上下方向正确，嵌套容器边界传播；无鼠标路径在 host 覆盖 |
| focus / text | Tab 跳过 disabled，Shift-Tab 返回；点击 textarea 后输入 `aA!`、编辑/删除及多行 Enter，内容精确断言 |
| modifiers / repeat | 左右 Shift/Ctrl/Alt、Caps 交接；长按字符出现受控重复，释放后值稳定，Tab 不连跳 |
| shortcut / TTY 隔离 | Ctrl-C 不杀 terminal/GUI，不产生 TTY SIGINT；Ctrl-Q 正常退出；GUI 中输入带 nonce 的 shell 命令再 Enter，退出后确认后台 shell 未执行且命令未留在 PTY |
| owner / desync | 静止画面被其他 gfx owner 接管后不得继续投递；host 强制 ring 溢出证明错误契约，真实队列路径由 QEMU 补受控 burst 验证，不依赖自然时序制造溢出 |
| crash / held keys | 正常退出、SIGKILL、LVGL fatal 后 terminal surface 恢复；杀进程时按住 Shift/左键，释放后 shell 输入正常且无幽灵 click/修饰键 |
| recovery | 每种退出后通过真实 PS/2 输入运行带新 nonce 的 echo，收到实际输出与 shell prompt；仅串口 echo 不足以证明键盘恢复 |

runner 对阶段标记使用发送后的输出 cursor 和 nonce，要求 guest 退出码；重复/乱序/缺失标记、仅命令回显、无换行 prompt、静默超时、QMP 失败和截图不符都必须失败并保留日志。SMP 任一失败不得合并为 PASS。串口救援通道可以终止卡住的测试，但不能据此报告输入恢复成功。

### 9.3 构建与回归入口

实施阶段至少执行并保存结果：

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

syscall E2E 不带 KERNEL_SELFTEST=1；内核自测如需执行，单独运行。新增宿主二进制/runner 测试接入现有 test bucket，不创建平行测试别名；构建手册同步 gui suite 扩展内容。干净与增量构建、公共 sysroot 外部小程序编译都验证；任何环境豁免记录具体失败、baseline 对照和补验范围，不能把豁免写成已通过。

## 10. 交付分期与评审检查

1. **输入路由前置**：file-aware devfs、显示绑定 UAPI、锁与 final-close、溢出/边界隔离，独立宿主契约先通过。
2. **libgui 输入后端**：纯 Set 1、原生事件与生命周期、pointer/keypad/group、重复和错误收敛，真实 LVGL 离屏测试通过。
3. **端到端闭环**：独立 test_gui_input、QMP 操作、SMP 1/2、TTY 隔离与异常恢复，现有 GUI-1/gfx/分辨率/syscall 回归通过。

三阶段是同一 GUI-2 的交付分解，不把第一阶段产生的内核缺口留作未完成依赖。完整控件 facade、Material 桌面和进程间焦点另行设计。

本文供评审时重点确认：显示绑定的租约是否符合当前全屏模型；keyboard-only 默认降级；US ASCII/单活动 keypad 键限制；Ctrl-C 的 GUI 语义；hover wheel 的明确目标；退出/崩溃后的输入与显示双恢复。确认本文后再写实施 plan，本次不修改产品代码。

## 11. 子 agent 评审修订记录

| 轮次 | 结论 | 修订 |
|---|---|---|
| 第一轮 | REQUEST_CHANGES：2 项 P1、1 项 P2 | 补齐 keypad wait-release 重新就绪协议；内核完整 Set 1 序列路由与 epoch 边界；Ctrl/Alt 取消活动键重复及原生事件快照语义；三项均追加宿主/真实 LVGL 验收要求 |
| 第二轮 | APPROVE：无新增实质性阻塞 | 完整复审 Draft v2，确认首轮三项已解决，具备编写实施 plan 的条件；该结论不代表产品实现或运行时测试已完成 |
