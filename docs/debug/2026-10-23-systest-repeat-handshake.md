# x86 systest-repeat stage 0 停滞：runner 遗漏 COW TTY 回复

2026-10-23，基于已提交 mutex 修复的 `a4a5b6d` 调查。

## 复现与根因

正常内核、normal init、terminal/ash 路径运行：

```sh
make PROFILE=x86_64-clang test-syscall-repeat
```

旧 runner 在 SMP=2 发出首条 `systest` 后停在 `[COW TTY READY 0]`，180 秒
截止到期，没有任何 systest RESULT 或 shell 完成标记。证据目录：
`build/x86_64-clang/logs/tests/systest-repeat/20261022T161822Z-72908141c75b47328cb62cc44777d48e/`。
目录名为 UTC，本地已是 10 月 23 日。

`user/systest.c::test_cow_kernel_outputs()` 为验证内核写入 COW 用户缓冲区，
在四种内存区域分别打开 TTY、fork，子进程打印 READY 后执行四字节 read，
要求读到 `COW!`。单次 systest 驱动 `qemutests/run_test.py` 已按四个 region
提供该回复。repeat runner 却只发送 shell 命令，然后等 `__REPEAT_DONE_N__`，
完全没有服务 READY 输入请求。

等待关系因此为：子进程等输入 → systest 父进程 waitpid → ash 等 systest
退出 → runner 等 ash 输出完成标记。网络定时器在此期间继续输出日志，
并不证明 DHCP 是阻塞原因。

调查时还考虑过 terminal 与 `/dev/tty0` 竞争输入的可能性；仅补 runner 握手后
真实 terminal 路径即完成多次全套测试，因此没有据此修改 user/TTY 代码。

构建注意：保持 KERNEL_SELFTEST 未设置。当前 `kernel/Makefile` 用 `ifdef`
检查它，设置为 `0` 仍会编译内核自测；最初一轮这种错误配置没有进入 ash，
已排除在 stage0 复现及修复验证之外。

## 修复与回归

`wait_stage()` 在每个 stage 内等待 READY 或完成/损坏标记；每个合法 READY
只回复 `b"COW!"`，不附加换行。ProcessSession 每次成功等待都会消费游标，
所以 helper 累积所有返回片段，供原有严格 RESULT 数量和失败数检查使用。
若只保留最后一个片段，三轮循环 stage 会丢掉前两轮结果。

三个 stage 的命令及 1/1/3 预期保持不变，仍从真实 terminal/ash 启动。每次
运行仍复制私有磁盘，保持输入镜像 hash、损坏标记检查、观察窗口和同一个
总截止；没有通过改测试子集或放宽失败判定获得 PASS。

新增回归通过生产 `run_repeat()` 驱动游标语义一致的 FakeSession：

- 全部五轮的 READY/RESULT 按单字节到达，必须收到恰好 20 个四字节回复；
- 前一轮 RESULT 与后一轮 READY 同片段，不能丢失结果；
- boot 窗口的陈旧 READY 不得重放；
- 完成握手后结果仍失败时必须 FAIL；READY 后无输出仍须 TIMEOUT。

该正向回归在旧 runner 上因未回复 READY 而 TIMEOUT，修复后通过。
独立评审还使用真实 ProcessSession + Python 子进程，逐字节发送 12 个 READY
和三轮 RESULT，并验证每次精确四字节输入；结果通过，无待修正评审项。

## 验证与限制

- repeat runner 单测：27/27，通过。
- `make PROFILE=x86_64-clang test-harness`：523 tests，OK（skipped=1）。
- 真实 QEMU 修复后四次运行均完成全部 20 次 COW 握手及三个 stage，原始
  stage0 停滞 0/4；其中三次完整五轮全部 PASS：
  - SMP=2，直接 runner（诊断时 35 秒截止）：
    `20261022T162014Z-6ca29fa87424482ca7bcb871de9736b8`。
  - SMP=2，原始根 make 目标、180 秒截止：
    `20261022T162151Z-7bb591bb332947d4ba3549379e4e42f1`。
  - SMP=4，180 秒截止：
    `20261022T162221Z-cc08d5e19a054c2a97a1333a18479ca6`。
  - 以上三次每轮均为 `342 passed, 0 failed`。
- 另一次 SMP=2 运行完成握手但 FAIL：
  `20261022T162108Z-7823f02f2a8447c892e47f8ee10b7262`。第三轮的
  `54c_user_fault_write_ro` 实际报 `status=0x0`，该轮 341 passed / 1 failed；
  RESULT 又被内核 fork 日志交织打断，runner 因匹配到的结果数量不足而拒绝。
  没有把这次运行报告为 PASS，也没有放松匹配来绕过 guest 失败。已另列 parked。

原 M3 x86 五轮门禁现在能走完并在实际错误时失败；旧 stage0/DHCP 豁免原因
已解除，不表示其他独立失败已经修复。

后续闭环（2026-10-23）：独立 54c 失败已确认是异常 helper 提前发布 ZOMBIE，
导致父进程在 `do_exit` 完成前回收任务，已修复并通过 1/2/4/8 CPU 回归。
详见 [退出发布竞态调查](2026-10-23-user-fault-zombie-publication.md)。本文件保留
当时的失败数据，不将历史失败改写为 PASS。
