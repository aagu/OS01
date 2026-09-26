# PS/2 鼠标驱动设计（/dev/mouse）

日期：2026-09-26
状态：待评审
路线图位置：P3 GUI 第一项（`docs/roadmap.md`）

## 1. 背景与目标

GUI 栈（fb + fb mmap + terminal 双缓冲 + Tetris）已落地，输入侧只有键盘。
本任务补上 PS/2 鼠标（i8042 aux 口，IRQ12），暴露为 `/dev/mouse` 字符设备，
为后续 GUI 光标 / Window Server 提供输入源。

**成功标准**：

- QEMU（i8042 默认带 PS/2 鼠标）中，用户态 `read(/dev/mouse)` 能拿到
  归一化的移动/按键事件；`poll(2)` 语义正确。
- 键盘路径（IRQ1 / `keyboard_poll` / TTY）行为不变，`systest-repeat` 回归通过。
- 无鼠标环境下启动不挂起、不多花可感知时间，`/dev/mouse` 不存在。

**Non-goals**（本期不做）：光标渲染、compositor、USB 鼠标、
绝对定位设备（tablet）、加速曲线/坐标累加（内核只报相对位移，
累计坐标留给用户态）。

## 2. 用户态 ABI

`/dev/mouse` 是 OS01 自有的归一化事件流，不声明兼容 Linux
`/dev/input/mice`。事件固定为 **8 字节**，不随硬件 3/4 字节包模式改变。
结构放入 `kernel/include/uapi/mouse.h` 并安装到用户态 sysroot，
以编译期断言确认 `sizeof(mouse_event_t) == 8`：

```c
typedef struct {
    uint8_t buttons;   /* bit0=左 bit1=右 bit2=中，其余为 0 */
    int8_t wheel;      /* 基础鼠标恒为 0 */
    int16_t dx;        /* 相对位移，正值向右 */
    int16_t dy;        /* 相对位移，正值向下 */
    uint16_t reserved; /* 写 0，用户态忽略 */
} mouse_event_t;
```

- 用户态只按固定 8 字节事件切包。`dx/dy` 使用 `int16_t` 保留硬件
  9 位有符号位移；滚轮模式（设备 ID `0x03`）将第四字节低 4 位按
  4 位补码符号扩展，基础鼠标的 wheel 为 0。不支持额外按键模式。
- 设备始终非阻塞，`O_NONBLOCK` 不改变语义：无完整事件时返回
  `-EAGAIN`；`poll(2)` 等待后重试。并发读者取走事件后，已返回可读的
  fd 仍可能得到 `-EAGAIN`。绝不以返回 0 表示暂时无数据。
- `size < sizeof(mouse_event_t)` 返回 `-EINVAL`；其余读取只返回整数个
  完整事件。`size == 0` 沿用现有 `fd_read` 行为，不在本任务修改。
- 所有打开此设备的 fd 共用队列；ring 满时丢最旧的完整事件并统计丢弃数。

现有 `kernel/fs/file.c` 的 FD_DEV/FD_VFS 读路径把底层所有负返回值
折叠成 `-1`。为使用户态确实收到 `EAGAIN`/`EINVAL`，本任务须在尚未
提交任何字节时原样传播底层负 errno；已有部分读取时仍返回已提交字节数。
对其他字符设备的错误码行为增加回归测试。

## 3. 架构

`kernel/driver/mouse.c` 负责鼠标协议、事件队列及 devfs 回调。
`kernel/driver/i8042.c` 提供键盘和鼠标共用的 0x64/0x60 访问与来源分流；
`keyboard.c` 不再直接读取 0x60。公开头置于 `kernel/include/driver/`，
保持源/头目录对称。

```
IRQ1 / IRQ12 / 启动期轮询 ──► i8042 单一读入口（锁 + 来源分流）
                               ├── 键盘字节 ──► 扫描码 / TTY
                               └── aux 字节 ──► 命令应答或包解析
                                                  └──► 事件 ring ──► read / poll
```

### 初始化顺序

keyboard 保持 `SUBSYS_PHASE_5`；mouse 注册为 `SUBSYS_PHASE_6`、
`SUBSYS_FLAG_OPTIONAL`，故键盘初始化先于鼠标，不依赖链接顺序。
`mouse_init` 仍检查 `subsys_status("keyboard") == 1`，失败时跳过鼠标。
`keyboard_init` 在首次使用端口前调用 `i8042_init()` 初始化共用锁与
分流状态，并经 i8042 API 完成原有 `keyboard_enable_irq()` 的 command
byte 读改写。现有 `keyboard_init(void)` 与 `_keyboard_init_wrapper()`
会无条件报告成功；本任务须让键盘初始化返回状态，检查 controller
事务及 `register_irq(1)` 的返回值，由 wrapper 透传失败，确保上述
`subsys_status` 真正表示键盘可用。它不能依赖 devfs、调度器或 per-CPU
GS base。

## 4. i8042 分流（本任务的关键正确性修复）

现状：`keyboard_handler`（keyboard.c:232）和 `keyboard_poll()`
（keyboard.c:288）都无条件读空 0x60，不检查 0x64 status 寄存器。
鼠标字节会被当成扫描码消费/污染 TTY。

**规则：0x64 status 与对应 0x60 数据必须由同一受锁保护的入口读取。**
IRQ1、IRQ12 和 `keyboard_poll()` 都调用该入口；IRQ 号只是唤醒来源，
实际归属取决于读取时 status bit5（AUX）。output-buffer-full 为 0 时不读
0x60。遇到 aux 字节时 `keyboard_poll()` 也经此入口把它交给鼠标路径，
不得 `continue` 自旋或将字节留在队首阻塞后续键盘输入。

共用 `spin_lock_irqsave` 锁覆盖 status/data 配对读取和按来源分发的顺序，
阻止不同 CPU 上的 IRQ1/IRQ12 抢读或重排同一设备的字节。键盘和鼠标
处理函数接收已分流字节，不再碰 i8042 端口或重复获取该锁。明确与各自
ring/poll 锁的获取顺序；等待、睡眠、用户缓冲区复制均在 i8042 锁外。
命令事务期间，aux 字节送入应答状态机，非 aux 字节照常送键盘；应答
必须同时匹配 AUX 来源和预期值，ACK/BAT/ID 不进入普通包解析器。
控制器自身 `0x20` 读 command byte 是单独的 i8042 API 事务：其响应
不带 AUX 标记，不能按键盘扫描码分流。phase 5 的
`keyboard_enable_irq()` 与 phase 6 的 mouse_init 均使用此 API，
keyboard.c 不保留任何直接读取 0x60 的代码。事务在 AP 启动前、持
i8042 锁并关闭本地 IRQ 的短超时临界区内完成。发 `0x20` 前先按
来源 drain/分流所有预存 OBF 字节（mouse 尚未就绪时丢弃旧 AUX 字节），
确认 OBF 为空才发命令；只接受本次发命令后的新响应。整个清理与应答
均受同一个总 deadline 约束，超时不写回 command byte。读到响应后
才读改写 command byte；结束后恢复普通分流。hosttest 覆盖预存键盘
及 AUX 字节。鼠标设备命令的等待仍按上一段的短持锁轮询进行。

## 5. 初始化序列（mouse_init）

所有命令/参数字节经 `0x64 ← 0xD4`、`0x60 ← byte` 发往 aux 口，
写入前等待 input-buffer-empty；每个字节单独等待 AUX 来源 ACK `0xFA`。
`0xFE` RESEND 最多重试两次，其余错误应答或超时使该事务失败。
轮询应答时反复短暂获取 i8042 锁、读取并分流，然后释放锁；不得持锁
忙等。此时 per-CPU GS base 尚未安装，**不得调用**
`clocksource_read_ns()`（它访问 `this_cpu()`）。使用 phase 4 已校准的
`clocksource_cycles()` 与 `clocksource_freq_hz()` 计算各步骤及整个探测
过程的 deadline；频率为 0 时跳过可选鼠标初始化。实现计划须给出毫秒
上限，并用 QEMU 无鼠标场景验证启动延迟。

1. 初始化解析器、事件 ring 和 poll 锁，确认 keyboard 已完成。
   `0xA8` enable aux port；`0x20` 读、`0x60` 写 command byte，
   清 bit5（aux disable），保持 bit1（aux IRQ）关闭，保留所有键盘位。
2. `0xFF` reset → AUX ACK、BAT `0xAA`、基础设备 ID `0x00`；
   随后 `0xF6` set defaults 并验证 ACK。
3. 滚轮探测：`0xF3,200`、`0xF3,100`、`0xF3,50`，每个命令
   和参数均验证 ACK；再以 `0xF2` Get Device ID 读取 ID。
   `0x03` 使用 4 字节硬件包；`0x00` 使用 3 字节包。
   探测命令明确拒绝但基础鼠标仍响应时，用 `0xF6` 恢复默认值并退回
   3 字节模式；超时、异常应答或恢复失败则放弃设备。
4. `register_irq(12, …, IRQF_TRIGGER_EDGE, …)` 并检查返回值；
   再置 command byte bit1 使能 aux IRQ。清理残留 AUX 字节、复位包
   状态机，然后 `0xF4` enable data reporting 并验证 ACK。此后
   AUX 字节才进入普通包解析器，避免首包丢在无处理器窗口。

失败时清除命令状态，撤销本驱动开启的 aux IRQ；若 IRQ12 已注册则
`unregister_irq(12)`。只回滚本驱动改动的 command-byte 位，保留键盘
IRQ 配置；不能让残留 ACK 污染后续包。`mouse_init` 返回非零，
`/dev/mouse` 不注册。

## 6. 数据路径

- **包组装**：状态机按已确定的硬件 3/4 字节包长收集。只在等待包头
  时接受 byte0 bit3=1；命令事务结束、失步或错误时清空部分包。
  bit3 只能提供有限同步能力，测试丢字节后的恢复，不宣称任意丢字节
  都能立即准确重同步。ACK/BAT/ID 不进入解析器。
- **归一化**：byte0 bit4/bit5 与低 8 位组成 X/Y 的 9 位补码。
  在 `int16_t` 范围内对 Y 取反，输出“向下为正”，在 QEMU 手动
  验证方向。bit6/bit7 对应轴溢出时将该轴置 0，另一轴和按键保留。
  符号、溢出和滚轮处理提取为纯函数供 hosttest。
- **ring**：固定容量的完整事件数组。一个独立的 IRQ 安全锁同时保护
  head/tail、丢旧事件、多读者消费和 poll 等待链；不照搬 keyboard 的
  无锁单消费者假设。
  锁内复制到内核 bounce buffer，锁外由现有文件层复制到用户缓冲区。
- **poll**：在同一事件锁下检查队列：非空 → `POLLIN|POLLRDNORM`；
  空且请求读 → 调用 `poll_wait` 登记等待链（它本身不加锁）。生产者
  在此锁下入队并摘取/唤醒等待者，保证检查空与登记之间不会丢唤醒；
  `poll_table_cleanup` 也通过此锁删除节点。覆盖跨 CPU 注册/唤醒交错
  测试；仅在 per-CPU GS base 已可用时访问 `this_cpu()->need_resched`。
  同时修正键盘 `keyboard_wake_pollers()` 的现有无条件 `this_cpu()`，
  在启动早期只做等待队列唤醒，GS 安装后才设置 `need_resched`。

现有 `do_poll_core()` 在 `poll_scan` 注册设备等待链后，才调用
`wait_queue_sleep(&pt->wq)` 将任务挂入总等待队列；两步之间发生设备
wake 时，即使鼠标端正确加锁，`poll(-1)` 仍可能永久睡眠。本任务须修复
`kernel/fs/poll.c` 的通用睡眠握手：先将当前任务挂入 `pt->wq` 并置为
可唤醒状态，再用不注册新等待链的扫描重查 fd 就绪，确认仍未就绪才
调用 `schedule()`；若重查就绪则从等待队列移除并恢复运行状态。
生产者在挂队列之后发出的 wake 即使先于 `schedule()` 也不能丢失。
该修复须适用于无限和有限 timeout，并保持超时、信号与 cleanup 路径。

## 7. DevFS 注册

`devfs_register_chrdev("mouse", NULL, &mouse_ops)`，ops 含
`read = mouse_devfs_read`、`poll = mouse_poll_dev`。注册点与
`/dev/keyboard` 一致——`main.c` 中 devfs 建好之后（keyboard 现在就在
main.c:157 注册），不做 devfs.c 内建注册。`mouse_init` 在 devfs 挂载前
运行，因此 main.c 检查 `subsys_status("mouse") == 1`，仅成功时注册，
并检查 `devfs_register_chrdev` 的返回值。

## 8. 测试策略（TDD）

1. **hosttest（先 RED）**：纯解析器覆盖基础包、4 字节滚轮、
   9 位位移边界、Y 取反、溢出、失步及部分包重置。可控端口流模拟
   覆盖键盘/aux 交错、ACK 来源、RESEND、超时与失败回滚；验证 AUX
   字节不进入 TTY。覆盖多读者读队列与 `poll` 唤醒交错。
2. **mousetest（QEMU 手动 E2E）**：`user/mousetest.c` 从共享 UAPI
   头读取固定事件，验证空读 `EAGAIN`、小缓冲区 `EINVAL`、poll 等待、
   读后清空，再在可见 QEMU 窗口验证移动、三键、滚轮和 Y 方向。
   加入 `USER_PROGRAMS` 与 `ROOTFS_FILES`，安装为 `/bin/mousetest`。
3. **回归及无设备**：运行键盘输入测试与 `systest-repeat`。用明确
   禁用 PS/2 鼠标的 QEMU 配置验证总探测时间、`/dev/mouse` 缺席和
   `/dev/keyboard` 正常；记录 QEMU 参数、启动时间和日志。
4. **poll 竞态**：用可控同步点确定性地触发“设备 wake 位于
   `poll_scan` 与任务入 `pt->wq` 之间”及“入队后、`schedule()` 前
   wake”两种交错，确认 `poll(-1)` 均不挂死，有限 timeout 仍正确。

## 9. 改动文件清单

| 文件 | 改动 |
|------|------|
| `kernel/driver/i8042.c`、`kernel/include/driver/i8042.h` | 控制器锁、端口访问与来源分流 |
| `kernel/driver/mouse.c` | 新增：驱动主体 |
| `kernel/include/driver/mouse.h` | 新增：对外接口 |
| `kernel/include/uapi/mouse.h` | 固定 8 字节用户态事件 ABI |
| `kernel/driver/keyboard.c` | IRQ handler + keyboard_poll 改走统一分流入口 |
| `kernel/fs/file.c` | FD_DEV/FD_VFS 读错误码传播，保留短读语义 |
| `kernel/fs/poll.c`（必要时 `kernel/sync/wait.c`） | 修复 poll 总等待队列的睡眠握手 |
| `kernel/core/main.c` | 成功时注册 `/dev/mouse` |
| `hosttests/` | 解析器、分流与失败路径测试 |
| `user/mousetest.c`、`mk/components/user.mk`、`config/rootfs.mk` | 构建并打包 E2E 程序 |
| `docs/driver.md`、`docs/roadmap.md` | 实现完成后更新文档与路线图 |

x86_64 的 `kernel/Makefile` 已自动收集 `driver/*.c`，linker script 已
收集 `SUBSYS_INITCALL`，无需添加新的链接条目。实施时核对 UAPI 头
是否进入用户态 sysroot，并更新必要的安装清单。
