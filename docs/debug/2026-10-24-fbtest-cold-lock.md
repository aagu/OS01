# parked：fb_test 文件回调冷调用锁初始化（2026-10-24）

## 结论与范围

`fb_test_release_file()` 与 `fb_test_ioctl_file()` 原先都能直接获取
`g_test_lock`，没有先调用 `fb_test_lock_ensure()`。
正常启动经 `fb_bootstrap_state()` → `fb_test_reset()` 已初始化该锁，
设备注册也会 ensure，因此本次没有复现正常启动中的死锁。

缺口存在于模块入口独立冷调用：BSS-zero 在内核锁约定中不是可直接使用
的 unlocked 状态。现有宿主 shadow 却以零表示 unlocked，且原 fixture
每次先调用 `setup_env()` → reset，无法发现此初始化顺序问题。

修复在两个文件回调通过有效 file/private-state 检查后，各增加一次
`fb_test_lock_ensure()`，总计 2 行内核修改。空指针分支保持不触锁，
正常启动和重复回调不会重复 spin_init。仅测试配置涉及该模块，无 ABI
或结构体变化；既有首次 ensure 在 BSP 串行前置的约定保持不变，不将
这次防护描述为并发首次初始化机制。

## 可复现回归

在 `hosttests/cases/test_fb_resolution_hooks.c` 首个场景中，从尚未执行
bootstrap/reset/init 的父进程派生三个独立子进程，分别测试：

1. open 后，首先执行 register ARM ioctl；
2. open 后，首先执行 terminal ARM ioctl；
3. open 后，不执行 ioctl，直接 release。

前两者重复 ARM，验证 EBUSY 不变，最后 release。每个子进程检查
spin_init 仅一次、每次取锁前已初始化，并验证 per-file state 释放。
子进程彼此不共享初始化进展；父进程不预热模块。宿主 fork/waitpid
通过显式 host ABI 别名调用，避免引入 OS01 的 inline syscall wrapper。

`fb_resolution_runtime.h` 增加可选的 init/acquire 弱观察钩子；只有
冷调用场景启用观察，不修改既有宿主锁的状态值或真实互斥行为。
`fb_test.o` 与对应测试对象显式依赖该头，保证增量构建重新编译观察点。

先写回归，保留旧内核实现：三个子场景均失败，分别观察到 3、3、1 次
未初始化取锁；共 136 条断言中 3 条失败。补上两个 ensure 后，
136/136 断言全部通过。其余 register/terminal ARM、故障消费、writer
lease 和快照回归仍运行，不以 mock 替代生产回调。

## 验证

- `make PROFILE=x86_64-clang test-resolution-host RES_CASE=hooks`：
  13 个场景、136 条断言及 profile contract 检查通过。
- `make PROFILE=x86_64-clang test-resolution-host`：完整九组通过。
- `make PROFILE=x86_64-clang test-host`：退出 0，129 个宿主二进制、
  PMM reservation、kernel-selftest runner（43 项）和 user build（7 项）通过。
- 测试配置真实 guest：根 Make 构建 `FB_RESOLUTION_TEST=1 image`，
  解析 `print-run-paths` 并准备该 profile 的固件；TCG/q35/std VGA、
  512 MiB、SMP 1/2 各 13 项协议检查通过（总计 26 项）。使用 `/tmp`
  qcow2 overlay，不写基线镜像；验证 ARM、重复 EBUSY、零 token EINVAL、
  原目标保留和消费后 re-arm。该检查验证正常启动后的兼容性，不作为
  冷调用复现证据。
- `git diff --check` 通过。

红/绿和完整分辨率日志：
`/tmp/os01-parked-fbtest-cold-{red,green,resolution}.log`。
完整 host 日志：`/tmp/os01-parked-fbtest-cold-host.log`。
QEMU 脚本与日志：`/tmp/os01-parked-fbtest-cold-qemu.py`、
`/tmp/os01-parked-fbtest-cold-qemu-{result,smp1,smp2}.log`。
首次 QEMU 启动在 guest 执行前因 profile 固件缺失退出，补运行仓库的
固件目标后重试通过；没有将该启动失败计为通过。

独立的 token 事务身份、测试 sysroot 私有头发布以及 guest ash/test_lvgl
SIGSEGV 项不在本次关闭范围内。
