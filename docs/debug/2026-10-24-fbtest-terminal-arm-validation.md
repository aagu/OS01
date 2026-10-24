# parked：fb_test terminal ARM 校验对称性（2026-10-24）

## 根因与修复

测试专用 `/dev/fbtest` 的 register ARM 已拒绝零 token，并在对应槽
仍有 pending 请求时返回 `-EBUSY`。但 `fb_test_arm_terminal()` 缺少
这两项检查：零 token 会成功挂起 ENOMEM；重复 ARM 会无条件覆盖原目标。
这与 `uapi/fb_test.h` 的“ARM 命令 token 非零”契约及另一类 ARM 的
单槽行为不一致，可能使测试控制事务失去原先的目标。

修复仅在 `kernel/driver/fb_test.c` 增加 7 行：

- 取锁前拒绝零 token，返回 `-EINVAL`。
- 在现有 `g_test_lock` 下检查 terminal pending 槽；已占用时释放锁并
  返回 `-EBUSY`，不写目标、token 或消费计数。

UAPI 仅补充同类 pending 槽不可覆盖的注释，未改变命令号、字段或布局。
查询和 CONSUME 命令仍允许零 token；消费后允许重新 ARM，累计 consumed
计数继续保留。`target_pid=0` 仍取 opener PID。

`fb_test.c` 只在 `FB_RESOLUTION_TEST=1` 构建。此次不改生产配置行为，
也不处理同组中另外的 token 事务身份、首次锁初始化或私有头发布条目。

## 回归与红绿证据

在 `hosttests/cases/test_fb_resolution_hooks.c` 新增两个测试，调用真实
`fb_test_ops` 和消费函数，分别覆盖：

1. 空槽零 token、已有 pending 时零 token：均返回 EINVAL，status 和
   消费目标不变。
2. 同一请求重复 ARM、另一 fd/owner 为不同目标 ARM：均返回 EBUSY；
   关闭被拒绝的 owner 文件不影响原远程目标；非目标不能消费，原目标
   仅消费一次；消费后重新 ARM，累计计数递增；默认目标仍为 opener。

旧实现下共 124 条断言中 13 条失败；修复后 124/124 通过。

## 最终验证

- `make PROFILE=x86_64-clang test-resolution-host RES_CASE=hooks`：
  hooks 12 个测试、124 条断言及 profile contract 检查通过。
- `make PROFILE=x86_64-clang test-resolution-host`：完整九组通过。
- 完整 `make PROFILE=x86_64-clang test-host` 重试通过：129 个宿主
  二进制及后续 PMM reservation、kernel-selftest runner（43 项）、
  user build（7 项）检查通过。首轮 PTY fixture 失败见下节，未隐去。
- 真实 guest 协议验证：先由根 Make 构建
  `make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 image`；用同参数
  `print-run-paths` 解析固件和独立的
  `image/resolution-test/disk.img`。QEMU TCG/q35/std VGA，512 MiB，
  SMP 1/2；每次使用 `/tmp` qcow2 overlay，基线镜像不写入。
- 每核配置 13 项、总计 26 项全部通过：初始状态、零 token 的 errno
  与无副作用、有效 ARM、同目标及不同目标 EBUSY、原请求保留、非目标
  消费 0、原目标消费 1、消费计数、消费后再 ARM/消费。每条同时检查
  helper 输出和 shell 返回值，等待实际 shell-ready 与独立完成标记。
- `git diff --check` 通过。

脚本：`/tmp/os01-parked-terminal-arm-qemu.py`。该聚焦 QEMU 验证不改变
显示模式，不代替完整 resolution QEMU suite 或 terminal resize 恢复验收。

日志：`/tmp/os01-parked-terminal-arm-{red,green,resolution,host,build}.log`、
`/tmp/os01-parked-terminal-arm-qemu-{result,smp1,smp2}.log`。

## 首轮失败与未关闭问题

### QEMU 临时文件路径

全局 `-snapshot` 首次尝试在 `/var/tmp/vl.*` 创建临时文件，因只读文件系统
启动失败；显式 TMPDIR 仍未解决。后改用 `/tmp` 显式 qcow2 overlay，
正常启动。没有放宽文件系统权限，也没有写基线镜像。

### PTY 并发 resize fixture

首轮完整 host 失败于 `test_pty_winsize::test_concurrent_resize_consistency`，
`ctx.torn_count=1`，其他宿主二进制通过。归档：
`build/x86_64-clang/logs/tests/hosttests/20261024T032447Z-40162dfc20154f1291a9f934ea83392a/`。

检查发现该 fixture 未先设置初始窗口：reader 只接受 A=`25,80,640,480`
或 B=`50,160,1280,960`，而 `pty_alloc()` 的合法默认值为 `25,80,0,0`。
在 `/tmp` 副本中强制 reader 先完成再启动 writer，真实 PTY 对象返回
`OBSERVED_INITIAL_WINSIZE=25,80,0,0` 并使同一断言失败。
这证明 fixture 有确定性误报路径；首轮未记录实际窗口四元组，因此不能
断言首轮失败已被这个原因完全解释。本次未修改 PTY fixture，重试完整
host 通过。已新增 parked 项跟踪。

诊断脚本和证据：`/tmp/os01-parked-terminal-arm-pty-fixture-check.py`、
`/tmp/os01-parked-terminal-arm-pty-fixture-evidence.log`、
`/tmp/os01-parked-terminal-arm-host-attempt1.log`。

### guest ash SIGSEGV

首次能启动的 SMP 1 协议验证中，重复 ARM 已打印
`ERROR=arm_terminal errno=16`，但随后 ash（task 3）在用户态
`RIP=0x40DE24`、signal 11 终止，未打印该命令的 shell 完成标记。
terminal 退出后被 init respawn。该轮因此失败，没有算作通过。

后续在实际 shell-ready 后等待 0.5 秒、命令间隔 0.3 秒，重新启动
SMP 1/2 两轮，均 13/13 通过。不能据此声称修复或定位了 SIGSEGV，也不能
确定其与本修复或其他 guest 故障的关系；已独立登记 parked。
失败日志：`/tmp/os01-parked-terminal-arm-qemu-smp1-attempt1.log` 和
`/tmp/os01-parked-terminal-arm-qemu-attempt1-result.log`。
