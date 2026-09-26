# PS/2 鼠标驱动（/dev/mouse）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 接入 i8042 aux 鼠标，提供固定 8 字节事件的 `/dev/mouse`，并修复共用端口分流、设备读错误码及 poll 丢唤醒。

**Architecture:** `i8042.c` 独占 0x64/0x60，按 AUX 位将字节送给键盘或鼠标。`mouse_proto.c` 只解析硬件包；`mouse.c` 处理命令、IRQ、事件队列及 devfs。键盘和鼠标共享控制器锁，用户态只看到固定事件 ABI。

**Tech Stack:** freestanding C、GNU make、hosttests、QEMU。

**Spec:** `docs/superpowers/specs/2026-09-26-ps2-mouse-driver-design.md`。执行每个任务前读相关 spec 小节；若计划与 spec 冲突，先核实并修正计划。

## Global Constraints

- 代码实现前按 `superpowers:using-git-worktrees` 检查是否需要隔离；保留用户当前改动，不重置分支。每个任务完成并验证后单独提交。
- x86_64 的 `kernel/Makefile` 自动收集 `driver/*.c`；aarch64 使用显式源清单，不编译这些新驱动。新增公开头分别位于 `kernel/include/driver/` 与 `kernel/include/uapi/`。
- 改动任何共享结构后按 AGENTS.md 执行 `make clean`，再重建相关产物。常用根目标：`make PROFILE=x86_64-clang kernel.bin`、`test-host`、`disk.img`、`test-syscall-repeat`。
- 只有 `i8042.c` 访问 0x64/0x60。锁顺序为 `i8042 → keyboard/mouse 事件锁 → poll wait-queue 锁`；无持锁睡眠、长忙等或用户缓冲区复制。`mouse` 在 phase 6 optional，依赖 phase 5 keyboard 的真实成功状态。
- 启动期未安装 GS，计时用 `clocksource_cycles()`/`clocksource_freq_hz()`；频率为 0 时跳过 mouse。单次 IBF/OBF 等待上限 20 ms、单条设备命令含 RESEND 上限 60 ms、整个 mouse probe 上限 500 ms；每层 deadline 取最早者。
- 用户态事件按 spec §2 固定 8 字节；设备读取始终非阻塞。ring 满丢最旧完整事件。键盘已有行为与 TTY 输入须保留。

## Review Focus

1. `poll_scan` 后、任务挂 `pt.wq` 前设备到达：`poll(-1)` 不能永眠；有限超时节点必须在每条返回路径注销（Task 3）。
2. IRQ1/IRQ12 在不同 CPU 并发，或 0x20 命令响应前有旧 OBF 字节：status/data 不可错配，键盘扫描码与 AUX 应答不可串线（Task 4–5）。
3. aux probe 的 ACK/RESEND、超时和失败回滚：不得留下 IRQ12、错误 command-byte 位或 `/dev/mouse`（Task 7–8）。
4. 多读者与多个 poll 等待者：完整事件只被一个读者取走；唤醒时不能保存已释放的 `poll_wait_entry` 指针（Task 7–8）。
5. 9 位边界、滚轮 4 位符号和 Y 方向：无截断，QEMU 中向下为正（Task 6、9）。

---

### Task 1: 固定用户态事件 ABI

**Files:** Create `kernel/include/uapi/mouse.h`.

**Interfaces:** 定义 spec §2 的 `mouse_event_t`（buttons、wheel、dx、dy、reserved），编译期断言大小为 8；内核及 `mousetest` 均包含 `<uapi/mouse.h>`。`kernel/Makefile install-headers` 会复制整个 include 树。

- [ ] 写头及大小断言，不复制一份用户态镜像。
- [ ] 运行 `make PROFILE=x86_64-clang kernel.bin`，生成 sysroot 后检查 `build/x86_64-clang/sysroot-generations/*/usr/include/uapi/mouse.h` 存在。
- [ ] 提交该头。此任务只定义类型；行为测试留给使用它的任务。

### Task 2: 保留 FD_DEV/FD_VFS 负 errno

**Files:** Modify `kernel/fs/file.c`；extend `hosttests/cases/test_poll_requested.c` / `hosttests/Makefile` 的生产对象链接。

**Interfaces:** `fd_read` 尚未提交字节时原样返回 `vfs_read` 的负值；已经提交字节时返回短读字节数；0 仍表示 EOF。

- [ ] 在现有 host harness 中链接真实 `file.c` 的 `fd_read`，用 fake VFS read 分别返回 `-EAGAIN`、`-EINVAL`、0，以及“先成功读一块、后返回负值”。断言真实 `fd_read` 的返回值和 offset；若现有 harness 无法承载，新增专用 production-object case。不要测试一个未被 `fd_read` 调用的辅助函数。
- [ ] 跑该 case，确认错误码断言在修改前失败；随后在 `fd_read` 的 `n < 0` 分支保存/传播真实 errno，保持已有 copy-to-user 失败与短读处理。
- [ ] 跑该 case、`make PROFILE=x86_64-clang test-host`；提交代码与测试。

### Task 3: poll 睡眠握手

**Files:** Modify `kernel/fs/poll.c`；必要时修改 `kernel/sync/wait.c`、`kernel/include/sync/wait.h`；新增/扩展 production-object host case。

**Interfaces:** `do_poll_core` 在设备等待链已注册后，先把任务挂入 `pt.wq` 并置 `TASK_INTERRUPTIBLE`，再以 `poll_scan(..., NULL)` 重查，未就绪才调度。

- [ ] 写可控交错测试：wake 位于初扫与任务入队之间；wake 位于任务入队与 `schedule` 之间；信号、有限超时和正常就绪各自返回。断言任务节点最终脱链、状态恢复 `TASK_RUNNING`，有限超时注册表不留下 `pt.tmo`。先确认旧代码至少一个断言失败。
- [ ] 实现最小握手。关键顺序如下；如果提取 wait-queue arm/disarm helper，保持同一锁保护节点增删，并让其他 `wait_queue_sleep` 调用者行为不变：

  ```text
  scan 并登记 fd 等待链 → arm 当前任务到 pt.wq → scan(pt=NULL)
  ready / signal / deadline → disarm + 清理 fd 链与 timeout 节点 → 返回
  仍未就绪 → schedule → disarm（含中断状态恢复）→ 清理 fd 链 → 重扫/循环
  ```

  生产者在 arm 后、schedule 前 wake 时，任务已被置 `TASK_RUNNING`，不得重新进入不可唤醒睡眠。所有有限超时退出路径都须 `poll_tmo_unregister`，尤其重查后直接 ready 的路径。
- [ ] 跑新 case、现有 `test_poll_requested` / `test_wait_basic` / `test_sync_primitives` 和全量 `test-host`；提交。

### Task 4: i8042 端口所有权与命令字事务

**Files:** Create `kernel/driver/i8042.c`、`kernel/include/driver/i8042.h`；新增端口流 mock host case 并在 `hosttests/Makefile` 注册。

**Interfaces:** 向 Task 5/7 提供 `i8042_init(void)`、`i8042_set_kbd_consumer(void (*)(uint8_t))`、`i8042_set_aux_consumer(void (*)(uint8_t))`、`i8042_pump(void)`、`i8042_read_command_byte(uint8_t *)`、`i8042_write_command_byte(uint8_t)`、`i8042_write_controller_cmd(uint8_t)`、`i8042_write_aux_byte(uint8_t)`。写/事务函数返回 0 或负值；调用方必须检查。

- [ ] host mock 以带 AUX 标志的 FIFO 模拟 status/data、IBF、输出写序列及 cycle 时间。测试 K/A 交错、OBF 空、0x20 前预存 K/A、0x20 响应不进入键盘消费者、IBF/OBF 超时和 0xD4+数据配对。确认 RED。
- [ ] 实现唯一端口读写入口。`i8042_pump` 在锁内完成 status/data 配对及消费者分发；消费者不得再次访问 i8042。`i8042_read_command_byte` 必须在 AP 启动前以有界、关本地 IRQ 的同一锁事务先 drain 旧 OBF，再发 0x20 并收本次响应；超时不写回。`0x60` 写 command byte 的命令与数据、`0xD4` 与 aux 数据也须各自作为不可插入的事务。此处保留详细逻辑描述，因为普通 AUX 分流不能用于 0x20 响应。
- [ ] 跑新 case、`make PROFILE=x86_64-clang kernel.bin`；提交。

### Task 5: 键盘迁入 i8042

**Files:** Modify `kernel/driver/keyboard.c`、`kernel/include/driver/keyboard.h`；必要时修改 BSP GS 就绪标记所在文件；扩展 i8042 host case。

**Interfaces:** `int keyboard_init(void)`；wrapper 透传状态；IRQ1 和 `keyboard_poll` 调用 `i8042_pump`；键盘字节消费者复用现有扫描码、TTY、poll 逻辑。

- [ ] 测试键盘/AUX 交错不污染 TTY、保留 E0 与 raw scancode 顺序、早期唤醒不访问未安装的 GS、控制器事务或 IRQ1 注册失败时 `subsys_status("keyboard") != 1`。确认 RED。
- [ ] `keyboard_init` 先初始化 i8042 并登记键盘消费者，再经 i8042 API 修改 command byte；检查事务和 `register_irq(1)` 返回值，同时更新头声明和 wrapper。移除 `keyboard.c` 的 0x60/0x64 直接访问。
- [ ] `keyboard_wake_pollers` 在 BSP 安装 GS 前只唤醒等待者。可用 `num_cpus != 0` 作为 BSP 就绪门槛：`kernel_main` 在 `percpu_install_gs(0)` 后才写 `num_cpus`，AP 在开启本地 IRQ 前已有 GS；先核对这两个顺序，若不成立则选本 CPU 安全判定，避免单个全局“某 CPU 已装 GS”布尔值。
- [ ] 跑 host case、`make PROFILE=x86_64-clang kernel.bin`、正常 QEMU 键盘/TTY smoke 与一次 `test-syscall-repeat`；提交。

### Task 6: PS/2 包解析器

**Files:** Create `kernel/driver/mouse_proto.c`、`kernel/include/driver/mouse_proto.h`；新增 `hosttests/cases/test_mouse_proto.c` 并注册。

**Interfaces:** `mouse_proto_reset(state *, bool wheel_mode)` 与 `mouse_proto_feed(state *, uint8_t, mouse_event_t *)`；feed 返回完整事件/继续收集/丢弃无效包头。ACK/BAT/ID 不进入解析器，硬件模式在 reset 时固定。

- [ ] 写 RED 测试：3/4 字节包、按键、wheel ±1 与 -8 边界、X/Y ±255/-256、Y 取反、各轴溢出、reserved=0、无效包头及部分包 reset。数据字节的 bit3 可以为 0 或 1；不要把包内 bit3=0 判为失步。丢字节恢复仅验证可确认的包边界/reset 场景。
- [ ] 实现 9 位补码扩展与 4 位滚轮符号扩展；Y 取反先提升到 `int16_t`。只在等待包头时检查 bit3。通过 host case 后提交。

### Task 7: 鼠标命令探测与 IRQ12

**Files:** Create `kernel/driver/mouse.c`、`kernel/include/driver/mouse.h`；新增可控命令响应 host case。

**Interfaces:** `int mouse_init(void)`；`mouse_handler` 调用 `i8042_pump`。AUX 消费者在命令事务期间收 ACK/BAT/ID，启用上报后将字节送 Task 6 解析器。Task 8 依赖初始化状态。

- [ ] 测试 keyboard 未就绪、频率 0、无 AUX ACK、错误来源 ACK、RESEND 0/1/2/超限、BAT/ID 错误、滚轮 ID 00/03、明确拒绝后恢复、每层 deadline 与总 500 ms、IRQ12 注册失败及 F4 失败回滚。分别从原始 command-byte bit1=0 和 bit1=1 开始测试；失败后均须撤销 IRQ12、清 bit1、恢复原始 bit5 与全部键盘位、注销 AUX 消费者且不注册设备。若 F4 已发出但设备不再应答，F5 停止上报只能有界尝试，不能声称硬件已确认关闭。先确认 RED。
- [ ] 实现 spec §5 顺序。首次改 command byte 清 bit5 **且清 bit1**（`cmd & ~(0x20 | 0x02)`），记录原始 bit5 供回滚；不要把 `cmd & ~0x20` 误当成关闭 aux IRQ。先 `register_irq(12)`，再置 bit1、清旧 AUX、reset 包状态，最后 F4/ACK。命令等待短持锁轮询，不能持锁等待。失败按逆序撤销已完成步骤；在 `unregister_irq(12)` 前清 bit1，即使固件原本将它置 1 也不重开无处理器的 IRQ12，并注销 AUX 消费者以隔离晚到 ACK/包。键盘位始终保留。
- [ ] 跑新 case、`make PROFILE=x86_64-clang kernel.bin`、QEMU 启动日志；提交。

### Task 8: 完整事件 ring 与 devfs read/poll

**Files:** Modify `kernel/driver/mouse.c`、`kernel/include/driver/mouse.h`、`kernel/core/main.c`；扩展 mouse host case。

**Interfaces:** `mouse_devfs_read(vfs_node_t *, uint64_t, uint64_t, void *)`、`mouse_poll_dev(void *, uint32_t, poll_table_t *)`；main.c 在 `devfs_init()` 后仅当 `subsys_status("mouse")==1` 注册节点。

- [ ] 测试空队列 `-EAGAIN`、小缓冲区 `-EINVAL`、只返回完整 8 字节事件、多个读者各取唯一事件、ring 满丢旧并计数、多个 poll 等待者及“查空/注册/入队”交错。确认 RED。
- [ ] 一个 IRQ 安全事件锁保护 ring 与 poll 链；`mouse_poll_dev` 持该锁查空并调用本身不加锁的 `poll_wait`。生产者持相同锁入队并逐个摘链/调用 `wait_queue_wake_all`，沿用键盘的等待者生命周期协议；**不要**将 `poll_wait_entry` 或 `poll_wq` 指针复制到固定数组后解锁唤醒，也不要限制一次只唤醒八人。mouse IRQ 在 GS 安装前也可能触发，设置 `this_cpu()->need_resched` 须采用 Task 5 核实过的就绪门槛。read 在锁内复制完整事件到内核缓冲，用户复制由文件层完成。
- [ ] main.c 检查 `devfs_register_chrdev` 返回值。跑 host case、`make PROFILE=x86_64-clang kernel.bin`、QEMU 中 `/dev/mouse` 节点检查；提交。

### Task 9: mousetest 与镜像内 E2E

**Files:** Create `user/mousetest.c`；modify `mk/components/user.mk` 的 `USER_PROGRAMS`、`config/rootfs.mk` 的 `ROOTFS_FILES`。

**Interfaces:** 程序包含 `<uapi/mouse.h>`，使用 `read/poll/errno`；安装为 `/bin/mousetest`。

- [ ] 构建程序：先用小于 8 字节缓冲验证 `EINVAL`；清空已有事件后验证空读 `EAGAIN`；用 `poll(timeout=0)` 检查空队列，再以交互模式打印 buttons/dx/dy/wheel。交互期间 `poll` 返回可读后允许另一读者抢走事件，此时 `read` 的 `EAGAIN` 属正常重试。
- [ ] 运行 `make PROFILE=x86_64-clang disk.img`，确认镜像包含程序。在可见 QEMU 窗口验证左右/上下、三键和滚轮；若方向与 spec 不符，同时修正解析器及其测试并重跑。
- [ ] 提交程序与打包清单；记录手动验证的 QEMU 参数和输出。

### Task 10: 回归、无设备证据与文档

**Files:** Modify `docs/driver.md`、`docs/roadmap.md`；若确证 QEMU 不能提供“无鼠标但键盘正常”配置，修订 spec §8.3 的验收方法并记录限制。

- [ ] `make PROFILE=x86_64-clang test-host`、`make PROFILE=x86_64-clang test-static`、`make PROFILE=x86_64-clang test-syscall-repeat` 均通过；另在正常 QEMU 手动敲键盘并读 `/dev/keyboard`。
- [ ] 通过 mouse probe 的可控 host 测试证明“无响应鼠标”在 500 ms 内放弃、IRQ 与 command-byte 回滚且无节点注册。先核查本机 QEMU 的机器参数能否仅关闭 PS/2 鼠标而保留键盘；若可行，运行该无设备 QEMU 验证。`-M q35,i8042=off` 会同时禁用 PS/2 键盘，不能单独证明键盘保留；可作为控制器缺席的补充启动检查。若无可行配置，明确记录 QEMU 环境限制，并把键盘保留证据留在正常 QEMU 回归中，按上段修订 spec。
- [ ] 文档更新后运行 `git diff --check`，提交文档；完成一次全分支代码评审。aarch64 驱动不在本期构建范围，切勿把 x86 I/O 代码加入其显式源清单。
