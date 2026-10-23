# OS01 monitor 指标采集与算法设计

日期：2026-10-09；基于 master `456ddffb`。本文描述拟新增接口与结构，不代表代码已经实现。

## 1. 范围与现状

目标：用户程序 monitor 每秒显示 CPU 温度、整体及各核 CPU 利用率、物理内存利用率和进程列表。采集与计算独立于图形/终端界面；界面形式待确认。

当前源码事实：

- `kernel/fs/procfs.c` 已有 `/proc/meminfo` 与 `/proc/<pid>/status`。status 的 Name 固定为 task，没有进程运行时间。
- PIT、BSP LAPIC、AP LAPIC 当前配置为 100Hz。BSP 调用 `tick_handler()` 推进全局 jiffies；AP LAPIC 路径不调用该函数。
- `schedule()` 每次调用 `update_curr()`；vruntime 还受唤醒、fork、迁移影响，不能用于真实运行时间差分。
- PMM 管理 2MB 页；4KB 分配器将整页拆为 512 个槽，slot 0 用于池元数据。`free_4k_page()` 释放槽，但当前不归还空池的 2MB 页。
- 现有 `rdmsr()` 无异常恢复；procfs 每次 read 重新生成内容，VFS 当前没有通用的 per-file 快照读生命周期。

## 2. CPU 利用率：100Hz 采样记账

### 2.1 数据结构

在 `kernel/sched/accounting.c` 与 `kernel/include/sched/accounting.h` 新增统计，独立数组避免改变 percpu_t 的汇编布局：

```c
struct cpu_usage_account {
    spinlock_T lock;
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint64_t epoch;          /* 重置或重新上线时改变 */
    uint32_t tick_hz;
    uint32_t valid;          /* 已初始化且该核计时器确实启动 */
};
static struct cpu_usage_account usage[NR_CPUS];
```

task_t 新增下述字段，详见进程部分：

```c
uint64_t runtime_ticks;      /* 独立于 vruntime，使用原子访问 */
uint64_t birth_id;
char comm[64];
```

### 2.2 记账算法与接入点

每个有效的本地定时器中断调用一次 `sched_account_tick()`：

```text
取得本 CPU、current 与本 CPU idle 指针
若 scheduler_ok 或 usage.valid 未成立，或本 CPU idle 指针为空，返回
在本 CPU usage.lock 下：
    current == idle -> idle_ticks++
    否则            -> busy_ticks++
释放锁
若 current != idle，原子 runtime_ticks++
```

- BSP：在 `tick_handler()` 中接入，覆盖 PIT 回退与 BSP LAPIC 路径。
- AP：在 `lapic_timer_handler()` 的 AP 分支接入。
- BSP LAPIC 不能额外重复调用，否则 BSP 会计两次。
- 不在 schedule()、update_curr() 或负载均衡路径计账。
- 比较 idle 指针，不依赖 PID 或 TASK_RUNNING。内核线程也属于 busy；用户程序在 syscall 中执行也记给该 task。
- 初始化统计数组后才能启用采集。AP 的 `lapic_timer_start()` 返回值必须记录；计时器失败的核显示 unavailable，不能显示 0%。
- 第一版只声明 x86_64 已验证；aarch64 当前无完整 per-CPU tick 路径，未接入的核返回 unavailable。

采样含义：这是“定时器观察到非 idle task 的比例”。约 10ms 粒度，短任务可能漏采，关中断导致漏 tick；它不单独计量 IRQ/softirq 执行时间。idle 上执行的中断工作也不能据此完整统计。若未来要求精确耗时，再在切换边界与中断入口/出口做纳秒级记账。

### 2.3 用户态算法

连续两次有效快照，对于 CPU i：

```text
ΔB_i = busy_i(new) - busy_i(old)
ΔI_i = idle_i(new) - idle_i(old)
CPU_i% = 100 × ΔB_i / (ΔB_i + ΔI_i)
CPU_total% = 100 × ΣΔB_i / Σ(ΔB_i + ΔI_i)
```

只纳入两次均有效、epoch 相同且 tick_hz 相同的核。目前所有核为 100Hz；未来不同频率时须先将 ticks 除以各自 tick_hz 转为时间再求和。

例：双核各增加 100 ticks，其中核 0 busy=100、核 1 busy=0，整体为 50%；双核全忙为 100%。

首轮、计数回退、epoch 改变或分母为零：显示 `--` 并重新建立基线。不能把新上线 CPU 的历史累计值加入本周期差分。

整数显示以 10000 表示 100.00%，中间乘法用 `__uint128_t`，显示时四舍五入到一位小数。CPU 总利用率无需使用全局 jiffies 做分母；jiffies 只由 BSP 推进，不是所有核运行 ticks 的总和。

用户态 128 位除法会生成 `__udivti3` 调用；现有 root runtime 构建契约只接入 kernel，`user/Makefile` 不会自动链接该归档。因此 monitor 的实施前置项是新增 `user` runtime consumer：在 profile 显式声明用户目标 triple、CFLAGS、对象格式、machine 与 ABI，复用 `runtime/builtins/udivti3.c`，由 `mk/components/runtime.mk` 生成独立的 `build/<profile>/runtime/user/<provider>-<digest>/` 归档与 receipt（digest 包含 consumer、工具链、参数和源码）。不能直接复用 kernel variant 或依赖宿主隐式 runtime。第一版 user consumer 只支持 selfhosted；选择尚未支持的 provider 时明确拒绝构建。

根构建将 user runtime 作为 monitor 的前置依赖，并向 user 子构建传入归档；`user/monitor/app.mk` 通过应用 LDLIBS 在 monitor 对象之后链接该归档。必须用 profile 的真实用户编译器/链接器构建 monitor ELF，并检查无未解析的 runtime 符号；hosttests 通过不能替代这项验证。进程 CPU% 的 128 位乘除同样依赖此 consumer，除法前验证分母非零，结果超出输出类型范围则判无效。

### 2.4 并发

写端、读端均使用本核统计锁与 irqsave，保证 busy/idle 成对复制。读端逐核复制、立即解锁，不嵌套其他锁。初始化时给锁正确的 unlocked 值，不能仅依赖清零。

task runtime 使用原子 fetch-add/load；task_list_lock 保证 task 生命周期，不保证跨 CPU runtime 的非原子访问安全。热路径不做格式化、内存分配、温度读数或遍历任务表。

## 3. CPU 温度：内核硬件后端

### 3.1 范围与结构

定义 arch-neutral facade `kernel/include/arch/thermal.h`；x86 后端位于 `kernel/arch/x86_64/cpu/thermal.c`。弱默认返回 unsupported。使用 register_subsys 初始化，禁止硬编码到 kernel_main。

```c
enum thermal_state {
    THERMAL_OK, THERMAL_UNSUPPORTED, THERMAL_INVALID,
    THERMAL_READ_ERROR, THERMAL_STALE
};
struct thermal_sample {
    int32_t temp_mc;         /* 毫摄氏度；允许负数 */
    uint32_t state;
    uint32_t cpu_id;
    uint32_t source;         /* core_dts / package_dts */
    uint64_t sampled_ns;
};
```

温度数值仅在 state=OK 时有效；不使用 0 表示不存在。接口同时公开 source、采样时间与覆盖范围。

第一版明确显示 `CPU0 Core Temp`：在 BSP 上取得的 core DTS 温度，不冒充整包温度或全核最大值。若已验证 package DTS 后端，则显示 `CPU Package Temp`。多核最大温度是后续扩展：必须逐核执行采集，不能在 BSP 循环 rdmsr 就声称读到了全部核心。

### 3.2 Intel DTS 算法

1. CPUID 检查 GenuineIntel、支持的最大 leaf 与 leaf 6 的 DTS 能力。
2. 检查 family/model 白名单，确认该型号支持所用寄存器及字段。CPUID DTS 位本身不足以证明所有 MSR 都可读。
3. 在支持型号上读取 `IA32_TEMPERATURE_TARGET`（0x1A2），使用 bits 23:16 取得 TjMax；拒绝零或无效目标值，不硬编码 100°C。
4. 读取 `IA32_THERM_STATUS`（0x19C），检查 bit 31 的读数有效位。
5. 读取 bits 22:16 的 digital readout，表示距 TjMax 的温差：

```text
temperature_mC = (TjMax_C - digital_readout_C) × 1000
```

若经型号文档确认并启用 package DTS，检查 leaf 6 的 package thermal 能力，使用 `IA32_PACKAGE_THERM_STATUS`（0x1B1），按该型号定义解析，并保留 source 区别。

不套用 Intel 算法到 AMD；第一版 AMD/aarch64 返回 unsupported。也不凭空设定通用“危险温度”；报警应依赖有效的硬件阈值。

### 3.3 MSR 安全与采集调度

现有 rdmsr 会因不支持的 MSR 引发 #GP。新增 thermal 后端前需补齐并验证 `arch_rdmsr_safe(msr, &value)`：仅修复明确标记的 MSR 探测指令，#GP 返回错误；其余 #GP 继续遵循原异常处理。不能用全局开关吞掉异常。

第一版由固定到 BSP 的低频内核采集任务每秒更新缓存；“固定到 BSP”是实现前置要求，需新增/验证内核任务固定 CPU 的创建方式及迁移限制。计时器 IRQ 只做 CPU 使用率计账，不读 thermal MSR。用户态读取缓存，不能直接执行 rdmsr。

检测到 hypervisor 时第一版返回 unsupported；这样 QEMU 即使宣告部分 DTS 能力，也不会误报宿主温度。安全 MSR 访问仍是裸机后端的前置条件。

缓存状态、值与 sampled_ns 使用独立锁成组发布。超过 3 秒未更新变为 stale，界面可以保留上次值但必须显示过期标识。设备不支持、有效位清零和读取失败分别返回不同状态。

## 4. 内存利用率：PMM 占用与池内空闲

### 4.1 第一版展示口径

使用 PMM 管理的物理内存为分母，不是固件枚举的全部 RAM，也不是各进程虚拟地址空间总和。

在 `kernel/memory/pmm.c` 内新增 `pmm_get_stats()`，头置于 `kernel/include/memory/pmm.h`。锁留在分配器内部，不向 procfs 暴露静态 pmm_lock。

```c
struct pmm_stats {
    uint64_t managed_bytes;
    uint64_t reserved_bytes;  /* 此字段指 PMM 已占用，含池保留页 */
    uint64_t free_bytes;
};
```

在 pmm_lock 下汇总 Zone，锁外输出文本：

```text
T = Σzone.pages_length × 2097152
U = Σzone.page_using_count × 2097152
F = Σzone.page_free_count × 2097152
memory% = 100 × U / T
正常快照满足 T = U + F
```

保留现有 MemTotal/MemUsed/MemFree 的 kB 单位，内部一律 bytes。界面按 MiB 显示，标签注明“PMM 占用”。T=0 为 unavailable。

### 4.2 4KB 子页释放为何不一定使比例下降

2MB 池取得整页时 U 已增加 2MB。之后分配/释放池内 4KB 槽，U 不变。当前池不会因空闲而归还 PMM，因此程序退出后 MemUsed 未下降不一定是泄漏。

可选附加信息，在 subpage_lock 下汇总：

```text
pool_reserved = 池数 × 2MB
pool_allocated = Σpool.alloc_count × 4KB  （含 slot 0 元数据）
pool_reusable = Σ(512 - pool.alloc_count) × 4KB
```

pool_allocated 已包含在 U 中，禁止再次相加；pool_reusable 是池内可复用空间，不能标成可供任意 2MB 分配使用的 MemFree。slab 也可能保留整页，不能用上述子页统计冒充精确的“有效应用内存”。

主卡使用 T/U/F 同一 pmm_lock 快照。若单独展示子页池数据，它是另一次采样；需要联合一致性时按已有分配路径顺序 `subpage_lock -> pmm_lock` 复制，不能逆序。任何统计锁内禁止分配临时缓冲。

## 5. 进程列表与每进程 CPU%

### 5.1 task_t 字段生命周期

```c
uint64_t birth_id;           /* 全局原子分配的创建序号 */
uint64_t start_ns;           /* 创建时的单调时间 */
uint64_t runtime_ticks;      /* 原子累计，迁移不重置 */
char comm[64];               /* NUL 结尾，显示用 basename */
```

- spawn/fork：发布到任务表之前初始化 birth_id、start_ns；runtime_ticks=0。
- fork：复制 comm，不复制父进程 runtime、birth_id 或 start_ns。
- exec：成功提交时更新 comm；失败保持旧名称。birth_id、start_ns、runtime_ticks 保持，exec 不是新建进程。
- comm 更新与快照复制共同使用 task_list_lock。长度最多 63 字节；内核序列化时按 §6 的 Name 编码转义，用户态解码后再为显示转义控制字符。
- BusyBox 的 executable basename 可能只有 busybox；如需 ash 等 applet 名称，另增有长度限制的 argv[0] 显示字段，不能声称 comm 就是完整命令行。
- 内核线程初始化名称；默认列表只展示现有 procfs 范围内的用户任务，含僵尸。暂不新增内核线程开关或复杂线程聚合。

用户态记录：

```c
struct process_sample {
    uint64_t pid, ppid, birth_id;
    uint64_t sampled_ns, runtime_ticks;
    uint32_t last_cpu;       /* 上次/当前调度归属，非永久亲和性 */
    char state;             /* R / S / D / T / Z */
    char comm[64];
};
```

在 task_list_lock 下按 PID 找 task，复制身份、parent 的 PID 与 comm，原子读 runtime，并取得本条 sampled_ns；释放锁后才能格式化。不在解锁后保留 task 或 parent 指针。task_list_lock 保证任务生命周期以及新增 comm 的读写协议，不能保证所有状态字段同一时刻一致：现有 state/cpu 写侧使用运行队列锁，读取端须采用与写侧兼容的单字段原子访问或 READ_ONCE/WRITE_ONCE 协议（实施时审计并适配所有相关写点），允许 State 与 CPU 彼此异步；不为 monitor 嵌套运行队列锁。runtime 与 sampled_ns 为相邻采样而非原子二元组，tick 级偏差按 §5.3 处理。R 表示 runnable/running，不能据此认定正在占用 CPU。

### 5.2 枚举与消失处理

第一版复用 `/proc` 目录枚举及 `/proc/<pid>/status`，只匹配全十进制 PID，新增可选字段：

```text
Name:          monitor
Pid:           42
PPid:          3
State:         R
CPU:           0
BirthId:       105
StartNs:       18000000000
SampleNs:      23500000000
RuntimeTicks:  27
```

每个 PID 的 status 单独打开、读完、关闭；不同时保持所有 fd，当前 NOFILE=16。

procfs_readdir 当前按索引动态扫描：即使加 task_list_lock，每次扫描之间的增删仍可能导致遗漏或重复。因此需保护每次遍历的内存安全，用户态按 PID 去重，接受下一周期补齐；列表不承诺全局同一时刻。PID 消失或状态读取为空则跳过，不当作 monitor 整体失败。

如未来要求严格一致的任务列表，再设计 per-open 目录快照或带游标的批量导出；第一版不增加全局任务快照 syscall。

### 5.3 差分算法

以 `(pid, birth_id)` 为历史表键：

```text
ΔR = runtime_ticks(new) - runtime_ticks(old)
Δt = sampled_ns(new) - sampled_ns(old)
process_cpu% = 100 × ΔR × 1e9 / (tick_hz × Δt)
```

100% 表示一个逻辑 CPU 满载；双核系统一个满载单线程任务约为 100%，同时整体 CPU 约为 50%。使用每条记录采样时间避免扫描耗时改变分母，假定各核相同 tick_hz=100。

首见、birth_id 改变、Δt≤0、runtime 回退时显示 `--`，保留新基线。迁移不会清零 runtime。采样误差可造成略超 100%，原始数据保留诊断，当前单 task 显示可限制至 100%；未来多线程聚合不得限制到 100%。

完成本轮后移除未见记录；读取失败的旧行若保留，必须标为 stale，禁止继续显示旧 CPU% 为当前值。按 CPU% 降序，PID 为稳定次排序键。

本次需求只要求系统内存利用率，进程表第一版不承诺 RSS。未来 RSS 必须维护真实驻留页统计、处理 COW 共享；不能用 maps 地址跨度或 brk 差值代替物理内存占用。

## 6. procfs 输出与文件快照生命周期

拟新增 `/proc/stat`（CPU）、`/proc/thermal`（温度），复用 meminfo/status。采用 OS01 自有 key-value 格式并带 Version；不冒充 Linux `/proc/stat` 的固定列 ABI：

```text
Version: 1
SampleNs: 23500000000
TickHz: 100
cpu0: online=1 valid=1 epoch=1 busy=1200 idle=1150
cpu1: online=1 valid=1 epoch=1 busy=300 idle=2050
```

thermal 示例：

```text
Version: 1
State: unsupported
Reason: hypervisor
Source: none
SampleNs: 23500000000
```

state=ok 时另含 TempMilliC、CpuId 和 Source。用户解析按键，不依赖空格个数/字段顺序，忽略未知字段；必需字段缺失、已识别字段重复、数值溢出或非法值则判本文件样本无效。

`status` 的 Name 在内核输出前逐字节编码：ASCII 可打印字节（0x20–0x7e）除反斜杠外原样输出，反斜杠编码为 `\\`（输出字节 0x5c 0x5c），其他字节统一编码为 `\xHH`（输出字节 0x5c、0x78 和两位十六进制 ASCII；每个原始字节最多编码为 4 字节）。这些行内代码表示实际输出，不是 C 字符串字面量。因此 Name 永远只占一行，编码缓冲按最长 63×4 字节加终止符预留。用户态只接受这两种转义，拒绝不完整/未知转义、解码后的 NUL 或超过 63 字节的名称；解码后的控制字节在显示端再次转义，不直接送入终端。字符串中的冒号属于 Name 值，不解析为新键。

现有 procfs_read 每次重新生成内容，分段读取会拼接不同时间的数据。必须新增文件级快照生命周期：

1. file_t 新增通用 fs_private（不复用 dev_private，也不把快照挂在共享 vfs_node 上）。
2. 给 VFS 增加可选 per-file open/read/release 回调，默认沿用旧 node read；仅所需统计节点使用新回调。
3. open 时锁外预分配定长缓冲，按各数据源锁复制原始结构，解锁后格式化，成功后挂到该 file_t。
4. read 只按 offset 返回已生成文本，EOF 为稳定长度；dup/fork 共享同一个 file_t 与快照。
5. 最后 file_put 调用文件系统 release，释放快照。独立 open 得到新快照。
6. open 分配失败返回 ENOMEM；格式化超出容量返回 EOVERFLOW，不能静默截断成合法数据。

这一层是新增基础设施，不是目前已有能力。上线需检查所有 file_t 创建、最后释放及原有 devfs 生命周期，避免回调重复释放或在文件引用锁内执行释放。

每次 open 后文件文本固定，分段 read 不混合不同版本；CPU busy/idle、PMM T/U/F、thermal 缓存各自按数据源锁保证组内一致。status 的身份和 comm 遵循 task_list_lock 协议，State/CPU 等字段仅保证 §5.1 的单字段读取与弱一致性。各文件之间允许采样时间略有不同。monitor 使用文件/进程记录自带时间；不宣称温度、内存及全任务列表在同一瞬间采集。

## 7. 用户程序结构与循环

```text
user/monitor/app.mk
user/monitor/monitor.c       初始化、poll、退出与采样调度
user/monitor/collect.c/.h    procfs 解析、有效性与状态
user/monitor/model.c/.h      差分、历史表、排序、60点环形缓冲
user/monitor/render.c/.h     界面，消费模型，不读内核文件
```

deadline 使用 CLOCK_MONOTONIC，每秒采样，不以 wall clock 定时。poll 等待输入与下一 deadline；采样超时跳过已错过周期，使用真实 Δt，不忙循环补采。CPU/内存历史仅存有效样本；缺失是空点，禁止补零。

模型当轮分配失败（`-ENOMEM`）同样占用一个采样槽：CPU 和内存历史各追加一个
无效空点，避免时间轴压缩。上一轮已提交的基线和显示快照保持不变，下一次
成功采样仍从该基线按真实时间差分；失败轮的计数不提交。第二个数组分配失败
时必须释放第一个临时数组，不能泄漏或替换原有数组。

图形版遵循现有 libgfx 生命周期和 gfx_client_policy；EAGAIN 延迟重试，ESTALE 退出并恢复终端。程序退出关闭所有 fd、恢复 termios/终端。显示形式待用户确认。

## 8. 验证与实施依赖

- hosttests：差分分母、100Hz 换算、计数回退、CPU epoch、新 PID/exec、Name 含换行/反斜杠/冒号及非 ASCII 字节的编码往返、字段注入、非法转义与重复键拒绝、溢出与缺失字段。
- 目标构建：新增 user runtime consumer 后使用 x86_64-clang 用户目标构建 monitor ELF，验证 `__udivti3` 已定义且无未解析符号；检查 user/kernel variant 隔离及未支持 provider 的明确失败。
- 内核测试：CPU busy+idle 每次恰增一、AP 记账、task 初始化与迁移 runtime 连续；快照分段 read/dup/最后 close。
- QEMU：1/2/8 核、空闲和绑核 spin；双核一核满载对应整体约 50%，允许 tick 与后台任务误差。
- fork/exec/exit 压力中连续扫描，验证不访问已释放 task、僵尸仍可显示、NOFILE 不耗尽。
- 内存：2MB 分配释放改变 U；池内4KB分配释放改变 pool_allocated、可能不改变 U。
- 温度：模拟有效位、目标值、#GP错误；QEMU 显示 unsupported；真实 Intel 支持型号上与可信读数对照。QEMU 不能验证真实传感器温度。
- 修改 task_t/file_t/VFS结构后按 AGENTS.md 执行 make clean；运行适用的 host/static/QEMU/syscall 检查。

依赖顺序：统计结构与 tick 接入 → PMM 快照 → task 字段与 procfs 安全读取（含字段访问协议和 Name 编码）→ per-file 快照 → monitor 采集/差分 → 界面。user runtime consumer 与目标链接验证必须在 monitor 采集/差分交付前完成。thermal 安全 MSR 和 BSP 固定任务属于独立前置项；未具备时完整显示 unavailable，不使用假温度。
