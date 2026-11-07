# ARCH-6 — Task 9 根因与修复记录（2026-11-07）

Task 9 已完成；用户限定止于 Task 9，Task 10 及后续任务未推进。工作区为 `.worktrees/arch6-fs-concurrency-blockcache`，分支为 `feature/arch6-fs-concurrency-blockcache`，接手基点为 `12be81ad`。Tasks 1–8 的提交与结论保持原记录。

## 根因：用户态压测屏障与结果协议错误

本报告取代原交接中“预先存在的 VFS rename 可见性竞态”的结论。原 A/B 在两个内核版本复用了同一份有缺陷的压力程序，不能据此认定 VFS 有问题。

1. 父进程每收到一个 worker 事件，就向全部 worker 发放 release，而没有收齐当前 FS/阶段的四个唯一事件。快 worker 会在邻居尚未 rename 时 cross-open，也会在邻居 cross-read 前删除文件。ENOENT 与后续 readdir 可见并不矛盾：这些操作没有正确的先后关系。
2. worker 发送 `0` 表示成功，父进程却以 `if (!ok)` 判失败；即使文件操作成功，也无法得到可靠的最终 PASS。
3. shared 阶段只覆盖 ext2；失败后仍推进阶段；父进程可能因 SIGPIPE 退出，未完整终止、回收其他 worker。

新增宿主回归直接编译并运行生产压力程序，使用 Linux 的真实 fork/pipe 和临时目录，只替换测试根路径，并延迟 worker4 的 rename 或注入 create 失败。修复前可稳定观察 cross-open ENOENT、SIGPIPE 与错误成功状态，无 OS01 VFS 参与；修复后全部通过。宿主测试证明协议缺陷，真实三种 OS01 文件系统覆盖由四核 QEMU 验收提供。

## 实现

- 使用 16 字节事件记录（worker/fs/phase/result），父进程按当前阶段收齐四个唯一成功事件后才释放；明确 rename → cross-read → delete 顺序。
- 三个文件系统均执行 files 与 shared 的创建、读取校验和删除屏障；父进程确认目录为空并完成清理后才汇总 PASS。保留每文件 32×512 字节签名、邻居读取及一次 open，无重试或 WARN 降级。
- 事件和 release 读取共用 120 秒绝对预算；子进程失败、EOF、超时、部分 pipe/fork 失败均进入关闭 FD、终止并 waitpid 回收的路径。SIGPIPE 转入 EPIPE 清理路径。
- parser 核验单次实际四核记录、12 个唯一 worker PASS、其后的唯一父 PASS；任意 FAIL、panic、缺项、重复、越界结果均拒绝。runner 使用标准 ProcessSession 串口归档，并观察最终结果后一秒的日志。
- 接好独立镜像/用户变体、inittab、互斥检查、rootfs 过滤、suite 分派、四核 gate、catalog 与 nightly CI。CI 显式设置 `ARCH6_FSSTRESS_TEST=1 SMP=4`，通过 `env QEMU_SMP=4` 隔离核数设置。
- 移除 DIRCHK/CROSSDBG、cross-open 重试和 tmpfs 临时探针；内核文件最终无修改。

## 验证

| 验证 | 结果 |
| --- | --- |
| `python3 -m unittest qemutests.test_arch6_fsstress` | 28 项通过；包含真实宿主协议、失败注入、parser、runner、变体与 CI 参数回归 |
| Task9 与 `qemutests.test_ci_workflow` 联合运行 | 45 项通过 |
| `make ARCH6_FSSTRESS_TEST=1 SMP=4 test-qemu SUITE=arch6-fsstress` | 连续两次通过；均为实际四核、12 个唯一 worker PASS、一个父 PASS，无 FAIL/panic |
| `make test-qemu SUITE=phase-0` | 通过；另查正常 rootfs manifest 未包含两套专用压力程序 |
| `make test-harness` 首轮 | 36 模块中 35 通过；唯一失败为新增 catalog 未接入 CI，补入口后 CI 模块与 Task9 联合定向复验通过；未重复整轮 |
| 独立子 agent 复审 | 首轮 request_changes（CI 缺入口）；修复后 approve，无剩余 Critical / Important / Minor |

两次正式归档位于 `build/x86_64-clang/logs/tests/arch6-fsstress/`：

- `20261107T082142Z-c2c8ce97f94640febd465ebfe321e4f1/`
- `20261107T082226Z-8e92f177a42d487d8d5e99f4edbc4cc2/`

每份 `result.json` 均为 PASS、cpu_count=4，argv 含 `-smp 4`、`-serial stdio`；`stdout.log` 保存完整串口证据。开发红绿日志位于本计划的 git-ignored 台账工作区 `debug-task9/`，不作为可随提交传播的证据。

## 后续边界

本次只闭环 Task 9。Task 10 的 roadmap/changelog/FS/build 文档、全分支终验与整合均未执行。保留台账和开发证据，供用户另行授权继续。catalog 的通用 matrix prepare/run 留空，使用登记的精确四核 make 入口和 nightly 命令；不借用默认单核 prepare。
