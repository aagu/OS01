# 调度器源码导读（task.c 2254 行，读不懂时从这里进）

> `docs/scheduler.md` 是**参考手册**（结构体、语义、开关）；本文件是**阅读路线**——按什么顺序读、每段在做什么、哪些可以跳过。

## task.c 全景（kernel/sched/task.c）

```
1-100     头文件（含 memory/fs 依赖）+ 常量 + 静态声明
100-135   sched_notify_remote / update_curr（vruntime 每 tick +1）+ rbtree 按 deadline 排序
135-230   enqueue_task / dequeue_task + pick_eevdf(): rbtree 选下一个任务 O(log n)
230-430   唤醒路径 + waitpid blocker callback（L357 注释：SMP race window 修复）
432-530   sched_balance(): 跨 CPU 均衡（pull / steal）
531-700   schedule() 主入口 ←←← 建议第一站（L695 switch_to）
700-940   上下文切换细节 + idle
940-1100  do_waitpid（L983）+ blocker_wait 阻塞框架
1100-1870 exit/exec/spawn 生命周期（含 do_waitpid blocker 闭环）
1871-2000 do_fork（L1871）+ kernel_thread（L2008+）
2000-2254 create_kthread / task_init + kthread 启动序列
```

## 推荐阅读顺序（3 遍法）

**第 1 遍·控制流**（只看主路径，跳过所有错误分支）：
1. `schedule()`（L531）— 整个调度器唯一的"总调度室"：拿锁 → update_curr → dequeue → reap zombie → `sched_balance()` → `pick_eevdf()` → `switch_to`
2. `pick_eevdf()`（L181）— 红黑树怎么选：eligible（vruntime ≥ 左子树最小）里 deadline 最早者
3. `ret_from_intr`（arch/x86_64/entry.S）— 抢衬怎么触发：硬 irq 返回前查 `%gs:8`（need_resched）

读完这 3 个点，你就能回答"两个任务怎么切换的"。

**第 2 遍·生命周期**：fork（COW 路径，配合 docs/cow-mmap.md）→ exec（elf.c 联动）→ exit → waitpid（blocker 框架）。注意 `PF_KTHREAD`/`PF_PROCESS` 的分叉点——kthread 没有 mm。

**第 3 遍·细节**：sched_balance 的 pull/steal 条件、deferred_free kthread（异步 reaper）、`in_schedule` 重入保护。

## 已知坑（评审报告 + AGENTS.md 摘录，读代码时对照）

- `get_current_task()` 是 `RSP & ~(STACK_SIZE-1)`，不是 `& ~STACK_SIZE`
- exec() 重置全部 sighand（含 SIG_IGN）——terminal.elf 必须 `signal(SIGINT, SIG_IGN)` 的根因
- 失去唤醒窗口（dequeue 与 on_cpu=0 之间）已由 `514e062` 修复，读 dequeue/wake_up 代码时注意成对出现内存屏障
- 改任何 task_struct 字段后 **必须 `make clean`**（Makefile 无头文件依赖）

## 自测问题

1. 为什么 `pick_eevdf` 要先判 eligibility 再比 deadline？退化为纯 fair scheduler 的条件是什么？
2. zombie 为什么不能在 `exit()` 里直接 reap，而要等 parent waitpid / 异步 reaper？
3. `sched_balance` 在 `schedule()` 里哪个位置被调用？为什么不能在 pick 之后？
