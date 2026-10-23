# 54c 用户只读页异常：提前发布 ZOMBIE 导致退出状态 0

2026-10-23，基于 `c4a9e07` 调查 roadmap parked 项。

## 现象与区分

前次 repeat 验证的一轮日志同时出现：

```text
Killing task 290 (user fault at RIP=..., signal=11)
[FAIL] 54c_fault_write_ro: ... status=0x0
```

故不能直接归因于只读页没有触发异常，也不能从 status=0 单独断言子进程
确实正常退出。先给原 case 增加 waitpid 的 PID 返回值和 errno 观测。
50 次最小单 case 循环未复现，但完整 suite 的较大地址空间扩大了窗口。

## 根因

`kernel/arch/x86_64/intr/trap.c::kill_current_user_task()` 原来先写
`task->state = TASK_ZOMBIE`，再切换到任务内核栈并调用 `do_exit(sig)`。

#PF 使用 `set_trap_gate(14, 0, page_fault)`：0x8F trap gate 不清 IF，IST=0
运行在任务自己的内核栈。teardown 可以被定时器抢占。

1. helper 将仍运行、尚未清理的任务提前标为 ZOMBIE。
2. 定时器调度将其切出；调度器不会重新入队 ZOMBIE，switch 发布 on_cpu=0。
3. 父进程的 `do_waitpid()` 见到 ZOMBIE + on_cpu=0，合法地按现有协议回收它。
4. `do_exit()` 尚未写入 `exit_code=SIGSEGV`，父读到默认 0，并回收了原本
   还应供退出清理使用的任务/内核栈。清理因此也可能被放弃。

`on_cpu` 检查只能保证任务当时不在 CPU 上，不能补救错误提前发布的 ZOMBIE。
`do_exit()` 本身已要求清理、exit_code 写入完成后才发布最终状态；helper
破坏了这个已有生命周期契约。

## 验证根因的实验

仅针对运行在自身栈的 SIGSEGV，在 helper 原状态赋值之后临时插入一次
`schedule()`，模拟定时器切出。这不是最终生产改动。

- 原赋值保留：probe 显示 state=8、on_cpu=1；父进程得到
  `pid=5 waited=5 errno=0 status=0x0`。因此排除 waitpid 返回错误/状态写回失败。
  证据：`.../systest-repeat/20261023T015833Z-e66725d47c544a999649a97d9e206dc9/`。
- 仅删除原赋值，保留相同调度探针：state=1（RUNNING），返回
  `pid=5 waited=5 errno=0 status=0xb`。证据：
  `.../systest-repeat/20261023T015928Z-85115e08d742462da822bd894a223279/`。
- 然后彻底移除探针，恢复原内核；使用持久的用户态压力回归：**6 PASS / 2 FAIL**，
  两次失败均为正确 waited PID、errno=0、status=0。证据：
  `.../systest-repeat/20261023T020039Z-1276980a8ae2448884b46aa6b1c33bb7/`。

上述路径前缀均为工作树内 `build/x86_64-clang/logs/tests/`。

## 最终修复与回归

生产修复只删除异常 helper 的提前 ZOMBIE 发布并说明原因。由 `do_exit()`
在清理和退出码都就绪后统一发布；它此前保存 CR3 到 init_mm 的逻辑也保证
清理阶段抢占后恢复不会重载已释放的用户 PGD。没有修改页权限、UAPI 或
waitpid 的退出码编码。

现有 `54c_user_fault_write_ro` case 扩展为 8 次 fork/只读页写入/waitpid：
每次要求返回目标 PID，且 WIFSIGNALED + SIGSEGV。子进程持有 128 MiB 的
惰性只读 VMA，以让退出遍历 32,768 个地址、扩大定时器抢占窗口；只有首个
页被 demand fault，不分配 128 MiB 物理内存。父进程状态断言失败会打印
PID、waited、errno、status，防止再把“未写状态”误认成“正常退出”。

修复后同一持久回归通过；所有临时 schedule/日志 probe 均已移除。
无结构变化，无需清理旧对象以修正结构布局。

## 完整验证

```sh
QEMU_SMP=1 make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall CASE=54c_user_fault_write_ro
QEMU_SMP=2 make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall CASE=54c_user_fault_write_ro
QEMU_SMP=4 make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall CASE=54c_user_fault_write_ro
QEMU_SMP=8 make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall CASE=54c_user_fault_write_ro
make PROFILE=x86_64-clang test-syscall-repeat
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
```

- 1/2/4/8 CPU 单 case：四个运行均 PASS，共 32/32 次目标 PID + SIGSEGV 断言。
  systest 证据从 `20261023T020446Z-b7da04e14b294bb9829bfbf9cf8ede7a` 到
  `20261023T020518Z-97f7127d39ee4c198eb2c5d92fd8f9c0`。
- normal init → terminal/ash 的完整五轮：**每轮 349 passed, 0 failed**，包含
  额外 40 次只读页退出检查（替代旧版每轮一次，故断言总数由 342 增至 349）。
  证据：`systest-repeat/20261023T020354Z-edb5ac87dda74f74bb89b1d5f49b034e`。
- 完整 host 和静态检查 exit 0。
- 独立代码评审无待修正项。
- 没有与 KERNEL_SELFTEST=1 合跑 syscall suite。
