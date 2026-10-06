# QEMU 屏幕分辨率动态切换系统设计规范 (v3)

## 1. 目标、范围与设计选择

在 OS01 的 x86_64 QEMU Standard VGA 上，提供 `/bin/setres` 查询和切换分辨率；无需重启，切换后交互终端保持同一 terminal、ash 与 PTY 会话。成功的含义是硬件读回、内核布局和可见画面一致，而非仅修改 `Pos`。

支持 `-vga std` 的 PCI VGA，默认 16 MiB，也允许能力探测确认的其他显存容量。`bochs-display`、secondary VGA、virtio-gpu、物理显卡不在本阶段支持范围。非 x86 后端返回 `-ENODEV`，原有 GOP 查询、console surrender 继续可用。不得改变 `boot_context` ABI。

本阶段提供模式枚举、事务性 SET、可查询的 generation、terminal 恢复和图形客户端恢复契约。desktop/LVGL 设置界面是后续工作；本阶段不承诺所有既有图形程序自动适配，但必须定义其旧 view 的确定性失效行为。不支持绕过内核直接改显示寄存器。

采用“关闭写入准入、排空已有写入、切换、重新开放”的协议。仅逐行检查 generation 存在 TOCTOU；全帧持有自旋锁会造成长时间关中断；仅用可睡眠 mutex 无法覆盖 IRQ console。准入短锁与写入租约同时覆盖这三类约束。generation 用于识别旧 view，不承担互斥功能。

## 2. 当前代码事实与容量模型

- `boot/uefi/main.c` 原样移交 GOP `FrameBufferSize`；OVMF QemuVideo 的长度是当前模式字节数按页对齐，不能当成完整显存容量。
- `x86_64_boot_memory()` 先建立初始 framebuffer 映射，`x86_64_boot_subsystems()` 和设备节点注册在其后。
- `/dev/fb` read 的旧 ABI 是 20 字节 `struct fb_info`；已有用户态 mmap 和 `FBIOSURRENDER`。
- terminal 使用 libgfx；当前 `fb_fd` 在 surrender 后关闭，空闲 `poll` 无超时；PTY master 仅有有限 ioctl，slave 的 `TIOCSWINSZ` 不发 `SIGWINCH`。

显示状态由 fb 层唯一管理：

| 字段 | 含义及不变量 |
|---|---|
| `vram_capacity` | PCI BAR0 与 BGA 容量核验后的可用物理字节数 |
| `fb_mapped_size` | 实际完成并验证的内核映射长度；不是计划映射长度 |
| `active_framebuffer_size` | 当前有效 `stride * height`，使用 uint64_t 运算 |
| `current_info` | width、height、stride、bpp、format 的一致快照 |
| `generation` | uint64_t，启动为 1；有效布局变化或回滚后的重绘失效递增 |
| `backend_ready` / `backend_failed` | 可切换能力与不可恢复硬件故障 |
| `transitioning` / `active_writers` | 写入准入状态与已获租约的写入数量 |
| `raw_mmap_seen` | 本次启动是否成功建立过原始 framebuffer 用户映射 |

`Pos` 是现有绘制代码的兼容镜像，不得成为第二个独立的模式来源。初始容量未知时保留 GOP 长度和映射；后端初始化完成后 `Pos.FB_length = fb_mapped_size`。SET 必须满足 `active_framebuffer_size <= min(vram_capacity, fb_mapped_size)`。初始化失败不得扩大 `Pos.FB_length` 或发布可切换状态。

### 2.1 启动顺序与映射

1. 在现有 `x86_64_boot_early()` 初始化 Pos 时调用 `fb_bootstrap_state()`，使用 spin_init 初始化 state lock，generation=1、active_writers=0、transitioning=false、backend_ready/failed=false、raw_mmap_seen=false，保存可信 GOP 元数据。frame_buffer_early_init 在首次 logo/glyph 写入前把实际映射地址与长度一致写入显示快照；尚未建立映射前拒绝 framebuffer writer，只允许 serial。保留 logo 和 frame_buffer_init 初始映射路径，重映射时同步快照地址。其他架构若提供 fb，也必须在首次 framebuffer 写入前完成对应 bootstrap。此入口仅初始化通用显示状态，不探测硬件。
2. BGA 通过 `PCI_DRIVER_DECLARE` 注册匹配表及 probe，由现有 phase-6 device-boot 在 pci_enumerate 后的 pci_bind_all 中传入 pdev。PMM/VMM 可用、AP/用户任务尚未启动；不另注册依赖隐含顺序的 BGA phase-6 扫描器，不重复枚举PCI，也不新增CF8/CFC实现。未匹配或可恢复probe失败不使device-boot致命失败。
3. 核验 BAR、BGA 和当前布局，扩展 `VIRT_FRAMEBUFFER_OFFSET` 映射，验证页表覆盖及物理地址，执行所需 TLB 刷新。
4. 一致发布容量、当前布局、generation 与 ready，重新开放写入准入。随后原设备注册路径注册 fb/gfx，禁止在那里重复探测。
5. 可睡眠 display_mutex 在bootstrap后、用户设备可用前完成 mutex_init，仅用于任务上下文；早期初始化不得调用需要 current 的加锁/等待操作。

启动probe关闭准入后仅在BSP检查active_writers=0，不进入运行时排空轮询：非零为probe EBUSY，重新开放可信GOP准入后拒绝后端。身份/能力/映射失败且PCI与BGA状态恢复验证成功，返回ENODEV或相应probe错误、backend_ready=false、恢复GOP准入及初始元数据；映射残留不对外扩大长度。PCI或BGA恢复验证失败则发布backend_failed并永久关闭framebuffer，只走serial；将该设备记为普通DEV_FAILED（EIO），不得使用会导致device-boot panic的DEVICE_UNSAFE，因为显示不可用不损害RAM/storage安全。

只有 BAR0 基址和映射长度均按 2 MiB 对齐时才复用大页映射；否则本后端返回 `-ENODEV` 并保留原 GOP 能力。本阶段不增加非对齐 aperture 支持。映射必须检查页表分配失败，不能直接依赖无返回值、未检查分配的 `vmm_map_page()` 来宣称成功；提供 fb 专用 checked 映射入口或复用已有 checked 页表分配逻辑。失败不得遗留对外可访问的半成品映射；可以保留不对外公布的已映射内核页，不释放共享页表。

### 2.2 身份、BAR sizing 与能力核验

候选设备须同时满足 Vendor/Device `1234:1111`、class `DISPLAY_VGA`、legacy VGA I/O 能力，且正常恢复后的 BAR0 基址与 GOP 的 `Pos.Phy_addr` 一致。绑定器逐个候选调用probe；GOP基址不匹配者返回ENODEV，不影响后续候选；最多接受一个与GOP一致的后端。无目标保留GOP能力，不能panic。

BAR sizing 在 BSP 启动阶段执行：先关闭 framebuffer 写入准入并排空写入；保存 PCI command、BAR 原值（包括 64-bit BAR 高半部），关闭该设备 memory/I/O decode，按 memory BAR 类型掩码探测，再在所有出口恢复 BAR 和 command，读回验证。探测期间不得 framebuffer 打印；错误日志在恢复后发出，恢复失败只走 serial。全操作经现有 PCI 配置层；BSP probe期间不会并行运行另一PCI配置事务，每次backend读写仍遵循现有irqsave配置锁。command只改低16位，使用32-bit配置写时status高16位写0，避免意外清除W1C状态；不得把读出的command/status原DWORD直接写回。拒绝 IO BAR、零容量、无效掩码、地址加法溢出或配置无法恢复。

恢复 decode 后，保存 DISPI index 和 ID；用 `arch_inw/arch_outw` 的 16-bit 访问进行 ID5 握手，必须读回 ID5，读取 `VIDEO_MEMORY_64K`（0xA），使用 uint64_t 计算容量。保存、临时设置、恢复 ENABLE 的 GETCAPS 位以读取最大宽高和 bpp；探测所有出口恢复原 ENABLE、ID 和 index，保证不关闭或清除原模式。能力不足或容量为零拒绝后端。

`vram_capacity = min(pci_bar_size, bga_vram_bytes)`，并核验初始有效 framebuffer 位于此范围。启动布局必须为 32bpp、零 offsets、virtual width 与 GOP width/stride 一致的 XRGB8888；不满足时禁用切换，不偷偷重设启动模式。

### 2.3 模式表

候选表：640×480、800×600、1024×768、1280×720、1280×800、1280×1024、1440×900、1600×900、1920×1080，均为 32bpp、stride=width×4。

按设备读出的最大宽高、32bpp 能力和 `min(vram_capacity, fb_mapped_size)` 过滤。使用 uint64_t 计算容量，最多 `FB_MAX_MODES=16` 项，顺序固定、不重复。启动模式可以不在候选表中；当前模式查询始终独立，CLI 必须单独输出它。不把硬编码的 QEMU 最大值当成能力查询。

## 3. 写入准入、锁与快照协议

### 3.1 锁职责

- `display_mutex`：串行化 SET 和运行时原始 mmap 准入，保护硬件事务。GET 当前状态在任务上下文也取此 mutex 得到事务完成后的快照。不在 IRQ、panic、早期 boot 使用。
- `display_state_lock`：irqsave 短自旋锁，保护状态快照、generation、transitioning、active_writers、raw_mmap_seen。只做字段检查、复制和计数；禁止用户拷贝、显存清屏、等待或日志。
- `Pos.lock`：保留现有 printk 的光标/缓冲区串行化职责；任务与IRQ字符路径统一使用irqsave/irqrestore获取/释放，替换当前color_printk的裸spin_lock，避免同CPU中断递归获取。console_putchar 也须串行化私有光标；console_surrender_fb、console_force_enable及fb_surrendered的发布同样按此锁同步。允许已有字符绘制路径，禁止在此锁下新增模式切换清屏或可睡眠操作。
- gfx table lock：只保护 view 槽位；不得同时持有 display_state_lock。先获取/释放 view 快照，再申请显示租约；CREATE_VIEW 必须将同一显示快照的布局与 generation 一起写入 view，写入后若发生切换，该 view 正常作为 stale 处理。

允许嵌套顺序为 `display_mutex -> Pos.lock -> display_state_lock`，也允许省略其中的锁；任何路径不得反序获取。SET 标记 transition 后必须释放 state lock 才等待排空，不能持有 Pos.lock 等待 writer；排空后才取 Pos.lock 发布控制台状态，绝不反向嵌套。持有Pos.lock或租约的writer不得申请display_mutex。surrender/force-enable只改变标志，不重新开放transition/backend_failed关闭的显示准入。

### 3.2 写入租约

所有内核 framebuffer writer（color_printk/putchark、console_putchar/scroll、gfx present、fb_write_row 的直接调用者和 panic framebuffer 路径）必须纳入准入。nested glyph/row helper 使用调用方租约，不得重复申请。

进入 writer：在 state lock 下，若 transitioning 或 backend_failed 则拒绝 framebuffer 写入，否则同时检查 view generation/矩形，复制布局并递增 active_writers。之后释放锁，整个绘制操作使用此快照；退出在短锁下减计数，所有成功/错误/容错拷贝出口恰好释放一次。

GFX_PRESENT 持有租约覆盖整帧（含逐行 staging 和容错用户拷贝），不持有自旋锁跨用户拷贝；请求结构先复制并校验。所有行使用同一快照的 stride 与地址。用户 fault 允许部分行已绘制，但必须释放租约，保持原有 partial-present 语义。模式切换绝不与这一帧交错。

transitioning 时 gfx 返回 `-EAGAIN`；stale generation 返回 `-ESTALE`；尺寸非法 `-EINVAL`；故障状态 `-EIO`。若 errno 目录缺少 ESTALE，须按仓库跨边界错误码规则同步发布，不用魔数。普通 console/printk 此时继续 serial，跳过 framebuffer，且不得改变屏幕光标。panic 不等待 mutex/transition，屏幕不可用时 serial 输出。

SET 持 mutex，在 state lock 下关闭准入，然后释放短锁，在任务上下文以调度器睡眠/让出 CPU 等待 active_writers=0。检查计数均在 state lock 下；使用有界轮询（每次最多等待 1ms，总截止 1s）避免依赖未提供的 wait-event 原语。排空超时恢复准入、硬件和 generation 均不变，返回 `-EBUSY`。最后一个 writer 不需要唤醒队列。有租约的 IRQ writer 不阻塞、不等待切换完成。

### 3.3 原始 mmap 与已有客户端

原始 framebuffer 用户写入无法遵守内核租约。为避免引入跨 VMA fork/unmap 生命周期重构，本阶段采用保守规则：任意一次成功 `/dev/fb` mmap 后，置 `raw_mmap_seen=true`，本次启动所有改变布局的 SET 均返回 `-EBUSY`，即使后来 munmap/进程退出也不解除；CLI 错误说明需重启才能恢复切换。同模式 SET 仍允许无操作成功。

mmap 在 display_mutex 下检查状态、按稳定映射长度建立 PTE、成功置sticky标记；与SET串行，无残留映射的失败不置位。失败须撤销本次已安装的全部用户framebuffer PTE并刷新相应TLB，保证没有用户可访问的残留映射；若不能可靠撤销，则仍置raw_mmap_seen=true并阻止后续改变布局的SET，同时报告失败，安全优先于“失败不置位”。不得释放设备物理页。映射权限、缓存属性、长度校验保持原约束。这是本阶段明确的兼容限制，后续才可采用活动 VMA 引用计数。

旧 gfx view 不自动更改尺寸。布局变化或回滚重绘失效后，所有旧 view 的 PRESENT 返回 ESTALE，即使 A→B→A 回到相同尺寸。libgfx 保留 errno，不缓存全局当前模式。terminal 按第 6 节恢复；desktop、LVGL 和其他既有应用遇EAGAIN保留view，以至少250ms间隔重试、不忙循环、不退出；仅ESTALE停止旧view写入并报告“显示模式已改变，请重新启动应用”，释放图形资源后退出。EIO报告设备故障后释放并退出，不将反复失败当作成功；由 terminal 的定期模式查询恢复 shell 画面。本阶段不增加 desktop/LVGL 自动 resize。

## 4. 硬件切换事务与失败语义

SET 请求在锁外容错复制，校验 bpp、白名单和容量；取 display_mutex 后重新确认 ready、当前状态与 raw_mmap_seen。请求有效且与当前模式相同：返回 0，不写寄存器、不清屏、不改 generation/光标。backend_failed 总是 EIO。

改变布局的流程：

1. 关闭 writer 准入并排空，保存完整有效旧布局（ENABLE、XRES、YRES、BPP、VIRT_WIDTH、X/Y_OFFSET、BANK，及当前 index）。
2. disable，写新 XRES/YRES/BPP，enable 使用 ENABLED|LFB_ENABLED|NOCLEARMEM；确认 QEMU enable 的 virtual width/offset 默认值，必要时显式设置 virtual width=width、offsets=0、bank=0。
3. 读回所有影响布局的寄存器，要求 enable 位成立、32bpp、宽高准确、virtual width=width、offsets=0、bank=0，实际 stride=width×4，并重新检查容量。VIRT_HEIGHT 是派生容量值，不作为可写恢复寄存器；要求足以容纳 height。
4. 硬件验证成功才清零新有效区域，使用 uint64_t 字节数。在准入关闭且无 writer 时执行，不持有 Pos/state 自旋锁。明确不提供无黑屏或显示垂直同步保证。
5. 取 Pos.lock 再取 state lock，发布布局、active_framebuffer_size、generation++ 和 Pos 镜像；调用只重置光标字段的 `console_notify_resize_locked()`，保持 console_fb_active 与 fb_surrendered 不变。释放两锁。
6. 恢复 DISPI index，重新开放准入，释放 mutex，返回 0。

任何硬件验证失败：用 NOCLEARMEM 恢复旧有效布局，enable 后恢复 virtual width/offset/bank，恢复原有效 enable flags 和 index，再完整读回校验；不得无条件宣称回滚成功。成功恢复则保留旧尺寸，generation++ 使所有旧 view 重新绘制，重置内核两套光标并清除旧有效区域，返回 `-EIO`。即使 SET 返回错误，客户端也应查询 generation；保证布局恢复，不承诺原像素内容保留。

回滚失败：发布 backend_failed，保持 framebuffer 写入准入关闭，fb/gfx 后续绘制与模式查询返回 EIO，serial 和 PTY 会话继续运行，SET 不再触碰硬件，需重启恢复。日志在短锁外且仅走 serial 安全路径；不得继续发布不可信 Pos 尺寸。FBIOSURRENDER 仍能执行。

## 5. UAPI 与错误契约

`kernel/include/uapi/fb.h` 是唯一共享定义；私有 driver/fb.h 引用它。以下 ioctl 号 v1/v2 尚未发布，本次采用 v2 的号；现有 FBIOSURRENDER 和 read ABI 保持原值及布局。

```c
#include <stdint.h>
#define FB_MAX_MODES 16
#define FB_FORMAT_RGB32 0 /* XRGB8888: 数值0x00RRGGBB，小端字节B,G,R,X */
struct fb_info {
    uint32_t width, height, stride, bpp, format;
} __attribute__((packed)); /* 20 bytes，旧read ABI不增加字段 */
struct fb_modes_req {
    uint32_t capacity, count, total;
    struct fb_info modes[FB_MAX_MODES];
}; /* 332 bytes，alignment=4 */
struct fb_set_mode_req {
    uint32_t width, height, bpp;
}; /* 12 bytes；bpp=0或32 */
struct fb_state {
    struct fb_info info;
    uint32_t reserved; /* 必须为0 */
    uint64_t generation;
}; /* 32 bytes，generation offset=24 */
#define FBIOSURRENDER     0x00004601
#define FBIOGET_MODES     0x00004602
#define FBIOSET_MODE      0x00004603
#define FBIOGET_CURR_MODE 0x00004604
#define FBIOGET_STATE     0x00004605
```

| 操作 | 用户指针方向/大小 | 行为 |
|---|---|---|
| FBIOSURRENDER | 不读取 arg，可为 NULL | 保持原语义；没有BGA也成功 |
| GET_MODES | 读取 capacity，写完整332字节 | 无BGA为ENODEV；capacity为0..16，超过16为EINVAL；count=min(capacity,total)，截断成功，剩余数组清零 |
| SET_MODE | 读12字节 | 无BGA为ENODEV；非法参数EINVAL；硬件失败EIO；原始映射/排空超时EBUSY |
| GET_CURR_MODE | 写20字节 | 无BGA仍返回可信GOP当前信息；故障为EIO |
| GET_STATE | 写32字节 | 当前info与generation同一快照；reserved=0；无BGA也可用；故障EIO |
| read(offset=0) | 原read语义 | 返回同一可信20字节info快照；保留部分读取行为，故障EIO |

未知 ioctl 返回 ENOTTY，不先检查 arg。已知携带指针的命令先按所需读/写方向调用 `syscall_check_user_range`，再用 `copy_from_user_ft` / `copy_to_user_ft`；无效/NULL/跨页 fault 返回 EFAULT。GET_MODES 只读取 capacity，内核输出整个结构从零初始化，不复制用户提供的 count/total/modes。纯输出 GET 不 copy_from_user。

请求在锁外复制；查询在锁内获得内核快照、解锁后写回。一次 GET 返回自身一致快照，不保证下一次调用仍同一 generation。copy-to-user 失败不泄露锁；所有 errno 遵循内核 -errno、libc -1/errno 的现有约定。结构大小/字段 offset 在内核、sysroot 用户构建和 hosttests 静态校验。fb_state 不使用 native pointer/long，不改变 boot_context。

## 6. terminal 恢复与 PTY 窗口契约

### 6.1 检测与生命周期

保留 fb_fd 至 terminal 退出；fork 的 ash 子进程关闭此描述符，父进程所有退出路径负责清理。每次主循环和 present 的 ESTALE 后查询 GET_STATE；空闲 poll 的 timeout 最大250ms，独立于dirty_pending与命令hold窗口，避免空闲黑屏。EAGAIN 不累计 fatal、不忙循环；模式恢复失败使用250ms重试间隔。正常可分配资源时空闲切换1s内完成查询与首帧。

比较 generation，不仅宽高；SET失败回滚、同尺寸ABA亦重建。正常非模式错误仍保留原fatal策略，但模式恢复错误不得杀死terminal/ash；backend_failed时进入serial-only会话，继续转发键盘/PTY输出，提示需重启。

### 6.2 资源事务

新增可host-test的 `term_core_resize(core, rows, cols)`：成功0，失败-1且旧对象完全不变。准备新 main/alt/dirty 数组，复制两屏左上重叠矩形、不做文本reflow，扩展区域空白；保留alt_active、parser与cursor_visible，当前/保存光标夹到新边界，重置scroll_pending并标全脏。rows/cols必须正数，乘法检查溢出。

恢复步骤：查询目标state；创建新gfx handle（旧handle暂不关闭）；再次GET_STATE验证generation与目标一致，否则释放新handle并重试。term_core_resize成功后，用已有 `term_render_init(&render, new_gfx, font, &core, fg, bg)` 显式替换renderer指针、重置cursor缓存，不发明无接口的term_render_resize。然后更新本地info/generation、替换gfx、关闭旧handle，全屏清背景和重绘。新present若ESTALE/EAGAIN则再恢复；只有实际present成功才清除模式恢复状态。

任何准备失败释放新资源，保持旧core/gfx用于继续接收PTY内容，暂停旧framebuffer present；定期重试，不退出会话。term_core变更后的再次切换允许继续裁剪恢复，不保证多次缩放能找回被裁掉的字符。若gfx view table已满，新view创建失败按上述重试，不提前销毁旧handle。

### 6.3 PTY窗口与SIGWINCH

本阶段新增PTY master的TIOCGWINSZ/TIOCSWINSZ，复用与slave一致的公共处理函数；安全拷贝规则相同。保存四个winsize字段，并使master/slave读取一致。更新时短锁保护PTY状态；释放锁后仅在四字段改变且pty->pgrp>0时向前台进程组发SIGWINCH；pgrp=0只更新，不伪造ash pid接收者。

terminal在初次创建和恢复成功时通过master设置rows/cols及xpixel/ypixel，检查返回值；winsize失败单独重试，不把已成功present的图形状态重新破坏。现有无job-control路径须在启动时把PTY前台pgrp设为ash实际进程组（复用TIOCSPGRP），不能假设已自动设置。PTY master还须支持本设置所需TIOCSPGRP/TIOCGPGRP，并保留现有TCGETS。信号发送遵守现有signal_pgrp，不持有PTY锁发信号。

## 7. CLI、集成与兼容修改范围

- setres支持 `-l`、`-h`、`W H`、`WxH`，只接受十进制正数，拒绝空字段、负数、零、溢出、字符后缀和多余参数。使用设备列表验证模式，不把16000当成解析溢出判断。帮助不打开设备；成功0，失败1，错误说明区别ENODEV/EIO/EBUSY。
- `-l`单次GET_MODES(capacity=16)，GET_STATE独立输出当前模式，即使不在白名单；可标CURRENT但不假定初态1024×768。输出generation用于诊断，正常CLI不提供直接端口访问。
- BGA源/公开头为 `kernel/driver/bga.c` / `kernel/include/driver/bga.h`；fb协调状态与租约；架构端口操作使用`arch_inw/arch_outw`，非x86stub在任何端口调用前返回ENODEV。硬件通过现有PCI_DRIVER_DECLARE绑定器probe，不新增kernel_main硬编码；fb_bootstrap仅接入已有早期显示初始化路径。
- kernel的install-headers自动发布uapi/fb.h；driver/fb.h、terminal、desktop、test_lvgl及其他重复定义处统一引用。保留旧gfx API结构体布局，view generation为内核私有字段。
- user/Makefile沿用现有自动发现/通用链接规则，不重复新增setres链接规则；rootfs注册 `/bin/setres=$(USER_ARTIFACT_DIR)/setres.elf:0755`。
- 修改Pos/gfx_view/pty等结构后必须make clean。更新相关hosttest mock与错误码镜像，并检查x86/aarch64构建隔离。文档描述接口，不把实现承诺误记为现有能力。

## 8. 测试与验收

### 8.1 Host测试

测试真实生产辅助逻辑，不复制实现：容量交集/对齐/溢出与模式表；CLI合法格式/uint32溢出/负数/尾随字符；UAPI大小/offset；GET_MODES的0/小容量/16/>16及输出清零；无效指针和fault清理；core resize分配失败旧状态不变、两屏裁剪/扩展、parser与光标保留、renderer重绑定。

模拟BGA寄存器后端覆盖完整读回、错误stride/offset、回滚成功与失败、NOCLEARMEM行为及probe所有出口恢复。并发host测试用可控barrier调度，证明writer进入与SET关闭准入原子、整帧期间SET不编程、EFAULT释放租约、超时重新开放、CREATE_VIEW快照一致、mmap/SET不交错。PTY验证master/slave窗口一致、真实前台组SIGWINCH、pgrp=0和同窗口不发信号。

### 8.2 QEMU测试接入

新增qemutests/test_resolution_switcher.py并注册根make `test-qemu SUITE=resolution`入口。使用PROFILE的print-run-paths、显式`-M q35 -vga std -smp 2`和普通interactive rootfs；不得带OS01_SYSTEST/KERNEL_SELFTEST污染此套件。启动等待实际shell-ready，不固定睡眠猜测。

guest测试辅助程序通过受控测试后端读取硬件寄存器并打印结构化断言（不新增普通用户可写端口UAPI）；使用QMP screendump验证宿主显示surface实际宽高，并验证已知边界像素/终端重绘。寄存器采样通过display_mutex与端口index恢复协议获得一致快照；若使用编译期开关测试后端，需单独记录其构建配置并确保生产构建不发布调试入口。普通surface/session用例使用生产配置，故障注入子集使用明确隔离的测试配置，两者均不得带OS01_SYSTEST或KERNEL_SELFTEST。

测试构建采用命令行 `FB_RESOLUTION_TEST=1`，须纳入profile配置指纹与产物隔离；生产配置为0且无控制节点、命令或注入分支。测试配置的 `/dev/fbtest` 仅供guest辅助程序使用，使用定宽结构与安全用户拷贝，最小操作为：读取寄存器/状态快照；为下一次改变布局的SET在“新模式读回”步骤一次性伪造失配；一次性同时伪造新模式失配和回滚读回失配；在受控guest writer成功获得租约后保持该租约，观察1s排空超时，再显式释放。测试租约绑定open文件，close/进程退出必须自动释放，不能永久卡住生产writer。故障仅针对该测试控制事务且消费一次，不更改其他设备/分配器全局行为；GET操作不消费注入。串行执行注入用例，回滚失败用例最后执行并重启后运行后续用例。

terminal新资源准备的分配失败通过测试构建专用控制，仅针对指定terminal PID的下一次resize准备，模拟gfx/core准备ENOMEM并随后正常重试；该控制不得影响PTY、ash或其他malloc。若采用用户程序内部hook，测试控制状态放在测试专用IPC对象，terminal只在FB_RESOLUTION_TEST构建时读取。原始mmap多页PTE安装中途失败及清理不可靠分支由host mock验证；QEMU只需验证正常mmap sticky行为。EFAULT、容量算法和精细并发交错以host测试覆盖，QEMU必须覆盖真实硬件surface、会话存活及受控事务故障结果。

验收必须覆盖：

1. 往返用例显式选择白名单内的800×600启动，动态保存初始模式；仍能枚举/切换1280×720，证明VRAM扩展有效；1280×720→640×480→保存初态，硬件、GET_STATE和QMP尺寸一致。白名单外初态单独用例只验证查询/CLI输出，结束时切到受支持模式或重启，不要求SET接受白名单外尺寸。
2. 同模式两次SET：generation不变、无清屏，旧view仍能present；A→B→A后旧view确定ESTALE并恢复。
3. 空闲terminal由独立guest进程触发切换，1s内出现正确新尺寸画面；切换前后terminal/ash PID及PTY保持不变，echo与Ctrl-C继续工作。
4. 实际前台测试程序记录SIGWINCH，master/slave读回winsize一致；检查像素字段，不只观察串口echo。
5. SMP并发present/SET与反复切换无跨布局写入、死锁、panic；EFAULT/准备内存不足/排空超时后可继续操作，EAGAIN期间desktop/LVGL不退出，generation不变的超时后旧view继续正常present。
6. 无BGA/不支持设备：SET/枚举ENODEV，旧read/surrender可用；只读查询不写硬件。原始mmap后SET为EBUSY，munmap后仍如此。
7. 注入读回失配与回滚失败：前者旧布局恢复且generation变化、terminal重绘；后者serial会话存活、fb/gfx为EIO，不再次写硬件。
8. CLI非法参数、ioctl跨页/只读输出指针、列表capacity边界均返回预期错误；current不在候选表仍独立显示。
9. desktop/LVGL旧view遇ESTALE按契约释放并退出，terminal恢复画面；普通x86构建不含测试寄存器接口。

运行make clean后完成相关test-host、test-static、resolution套件和现有gfx/terminal回归。系统调用suite与kernel selftests分开运行；若验证PTY/signal改动，syscall E2E必须使用 `make OS01_SYSTEST=1 test-syscall` 且不带KERNEL_SELFTEST。

## 9. 评审与后续阶段

此文件仅为设计规范。完成自审与独立子agent评审后，保留审查结果供用户检查；用户确认书面spec后才进入writing-plans和实现阶段。spec批准不能代替实际代码/硬件测试通过。
