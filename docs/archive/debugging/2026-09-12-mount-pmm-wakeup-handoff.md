# 交接：find_mount 损坏已定位修复；重复 systest 暴露调度丢唤醒

用户因额度要求暂停，交给下一位 coding agent。最后更新：2026-09-12。

## 先看工作区（重要）

所有本轮修改均在 `/home/aagu/OS01/.claude/worktrees/pmm-three-fixes`，不是主目录。
分支 `worktree-pmm-three-fixes`，开始时 HEAD `4468e75`，工作区干净。
修改尚未提交、未合入主目录。不要覆盖已有 PMM 三项修复及 find_mount 防御检查。
本工作树没有 `.codegraph/`，可以直接 rg；主目录有索引，应遵循 CodeGraph 指令。
主目录有其他未跟踪计划文件，不属于本任务。

用户提供的原交接：
`/home/aagu/.claude/projects/-home-aagu-OS01/memory/find-mount-pf-fork-exec-fix-completed.md`。
原文断言挂载损坏独立于 PMM，**硬件观察证明该断言错误**。

## 1. 已证实的挂载损坏根因

在 `/dev` mount 的 next 字段 `0xffff800000600028` 设置 GDB 硬件写断点，捕获：

```
Old value = 0
New value = 350
PC = e1000_xmit+311
rcx = ffff800000600000
r12 = 0x15e
stack: e1000_xmit -> etharp_output -> ip4_output_if_src
       -> udp_sendto_if_src -> dhcp_discover -> ... -> tcpip_thread
```

6 MiB 物理页同时归属 64 字节 boot slab 和 e1000 TX ring，网卡发送描述符覆盖 mount。
`0xb00015e` 是 TX 描述符长度/命令字段（350 字节），不是用户指针。
在 task_init、用户 fork/exec 之前挂载对象已经损坏。
证据：`/tmp/os01-mount-watch.log`、`/tmp/os01-mount-serial.log`。

GDB 确认 `PMMngr.pages_struct[0].phy_address == 0x200000`。
此前 alloc/free 已改为 RAM-relative bitmap，但初始化预留与转换宏仍用物理绝对页号：

- pmm_init 预留循环跳过 slot 0，且页数仍按绝对地址计算。
- Phy_to_2M_Page / Virt_To_2M_Page 未减 RAM 起点，ELF/VMM free 同样受影响。
- slab_init 元数据 descriptor 和两个 bitmap 更新仍用绝对页号。

已修改三个文件解决：
- `kernel/memory/pmm.c`：从 descriptor 0 开始，按 end_of_struct 与 lowest_ram 的差值向上取整预留；跳过 sparse slots，钳制 pages_size。
- `kernel/include/kernel/memory.h`：物理地址减 pages_struct[0].phy_address 再转换索引。
- `kernel/memory/slab.c`：使用 Phy_to_2M_Page；bitmap 使用 page - pages_struct。

未修改 struct/UAPI，但宏涉及消费者，已 make clean 并完整构建验证过 PMM 版本。
修复后 GDB：mount 仍在 6 MiB 且字段完整；e1000 RX/TX ring 分别在 20/22 MiB。
更详细说明：`docs/debugging/2026-09-12-vfs-mount-overwrite.md`。
既有限制：boot slab 放置仍假设元数据后连续 RAM，本轮未引入跨空洞布局。

## 2. PMM 回归测试及已完成验证

新增：
- `test/cases/test_pmm_boot_reservation.c`
- `test/mock/pmm_boot_include/kernel/arch/mmu.h`
- `test/mock/pmm_boot_include/kernel/percpu.h`
- `tests/pmm_boot_reservation_test.py`
- `mk/components/run.mk`：加入 make test 及 test-pmm-boot-reservation 目标。

测试链接真实 pmm_init、slab_init、alloc/free 和转换宏，只 mock 固件输入/direct-map/无用特权接口。
覆盖 RAM 起点 0、2 MiB、4 GiB × 元数据占一页/跨两页，共六例；耗尽分配验证不重复分配 kernel/slab；物理地址转 descriptor 后释放验证计数。
RED 顺序真实观察：slot0未预留 -> 修正后 descriptor错误 -> 修正后bitmap错误 -> 全修复通过。

**以下通过均在最后调度补丁之前，不能当作调度补丁的验证：**
- `make PROFILE=x86_64-clang test`：18 个宿主套件 + 六个新增场景通过。`/tmp/os01-mount-host.log`
- `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall`：268/268通过。`/tmp/os01-mount-syscall.log`
- clean build disk.img 成功。`/tmp/os01-mount-fixed-build.log`
- SMP4 重复测试不再出现 CORRUPT/PF，但挂起，见下一节；完整五轮尚未通过。

`tests/x86_64_systest_repeat.py` 增加检测 CORRUPT/PF-KRN 即失败，不能让 VFS guard 掩盖损坏。
五轮定义：单次 systest；字面 `for i in 1..3`（ash 实际只执行一次）；正确 `for i in 1 2 3`（三次）。

## 3. 验证期间发现独立调度丢唤醒（已定位，补丁刚写，未验证）

第一次 SMP4 停在 termios 子任务等待；第二次诊断运行完成三次 268/268 后 ash 等待不恢复。
GDB `/tmp/os01-mount-stall.log`：四 CPU 都在 idle，CR3 都是 init PGD 0x101000，挂载完整。

```
ash pid4 task=ffff800001e60000 state=1(RUNNING)
cpu=2 on_rq=0 on_cpu=0 in_schedule=1 blocker=0 waitpid=-1
child105 state=8(ZOMBIE) cpu3 on_rq=0 on_cpu=0 mm=0 cr3=101000
parent of child105 = ash
```

RUNNING ash 既不在 CPU 也不在队列，所有核心空闲，证实 stranded wakeup。
竞争时序：
1. schedule() 第2步看到当前任务 BLOCKED，出队后不再入队。
2. 远端 task_wake 设置 RUNNING，但看见 on_cpu=1，直接返回。
3. __switch_to 最后把 on_cpu 清零，未处理 pending wake，任务永远丢失。
旧 task_wake 的 state 写在 rq_lock 外，因此只在切出时随便多检查一次仍有竞争。

**刚加入的未验证候选补丁：**
- `kernel/sched/task.c`：task_wake 先锁队列、确认 cpu 未迁移，再写 RUNNING；新增 task_finish_switch(prev)，同一个 rq_lock 下检查 RUNNING && !on_rq && 非idle则入队并置 need_resched，最后 release on_cpu=0。
- `kernel/include/kernel/task.h`：新增函数声明，无结构修改。
- `kernel/arch/x86_64/switch.c`：架构保存结束后 prev!=next 时调用 helper；提前读取 PF_SELF_REAP，避免清 on_cpu 后普通 zombie 被父任务释放再读 flags 的 UAF。
- helper release on_cpu 后不能再解引用 prev；解锁只访问 rq。
- prev==next 自切换不要清 on_cpu/入队。

下一位请优先审查此并发修复（锁顺序、迁移、on_cpu 发布、self-reap 生命周期），再跑回归。
尚未专门增加 deterministic lost-wakeup 测试；已有重复 QEMU 场景在修改前确实失败。
不要把“已写补丁”描述为“已修复验证”。

## 4. 下一步命令与诊断工具

最后启动了构建（用户要求暂停时还在编译 BusyBox）：
`make PROFILE=x86_64-clang disk.img > /tmp/os01-mount-wake-build.log 2>&1`
原工具 session_id 49005（跨 agent 未必可用），先查进程/日志判断是否完成，勿重复并发构建。

在上述工作树执行：
```
make PROFILE=x86_64-clang test
make PROFILE=x86_64-clang SMP=4 test-syscall-repeat
make PROFILE=x86_64-clang SMP=2 test-syscall-repeat
make PROFILE=x86_64-clang SMP=1 test-syscall-repeat
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall
```
KERNEL_SELFTEST 不应与 systest 同时开启。测试会切换 inittab，若最后需要交互镜像再 make disk.img 并确认默认配置。
如 clean 后 firmware 获取因网络失败，复用主目录已有 firmware：
`OVMF_FIRMWARE_SOURCE=/home/aagu/OS01/build/x86_64-clang/firmware/OVMF.fd` 作为 make 参数。

可复用诊断：
- `/tmp/os01-mount-stall.py`：重复测试驱动的临时副本，QEMU Unix GDB socket `/tmp/os01-stall.sock`，超过30s无输出就抓状态。
- `/tmp/os01-mount-stall.gdb`：加载本工作树 ELF，输出任务状态/CPU栈/挂载/e1000。
- `/tmp/os01-mount-watch.gdb`：task_init 后 watch mount next 的原始脚本。
- `/tmp/os01-mount-repeat4.log`、`/tmp/os01-systest-repeat.log`：首次重复失败日志。

QEMU socket 需要 sandbox escalation（此前自动审核允许）；普通 headless 测试可直接运行。
不要用 -snapshot：临时 overlay 尝试写只读 /var/tmp；现有测试改用 /tmp 的磁盘镜像副本。
注入必须 -serial stdio，-serial file 不能输入。

GDB 任务字段偏移（当前 task struct 1352字节）：next8,state16,flags24,mm32,thread40,pid56,on_rq152(u8),cpu156,blocker160,waitpid192,parent232,in_schedule280(u32),on_cpu284(u32)。thread cr3=40,rsp=16。
e1000 结构 rx_descs32,tx_descs40,rx_phys48,tx_phys56。

最终需更新调试文档，说明哪些验证已通过，未自动提交/合并。用户明确要求本轮暂停交接，请勿后台继续扩大任务。
