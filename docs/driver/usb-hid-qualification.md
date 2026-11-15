# USB Boot HID 集成复验与实机记录

本文档记录 USB Boot Protocol HID（键盘/鼠标）在 `feat/usb-hid-input` 分支（基于提交 `80b17c05`）上的 Task 7 回归验证结果、产物与日志路径，以及实机覆盖边界。

## 1. 实机与硬件覆盖声明（QEMU-only）

- **当前环境为纯 QEMU 虚拟化环境，物理实机未验证。**
- 以下实机项目在本次验证中**均未执行**（缺真实硬件）：
  - 真实 xHCI 控制器 BDF、固件版本、外设 `VID:PID`、链路速度与物理拓扑；
  - **高速 Hub（HS Hub）+ 低速/全速 HID 的 Split Transaction / Transaction Translator（TT）路径**；
  - 实机物理热拔出、断线后 PS/2 回退、以及同 Hub 挂载真实 USB 大容量存储设备的并发读写。
- **不以 QEMU FS Hub 结果替代实机**：QEMU `SUITE=usb-hid` 与 `SUITE=usb-boot` 中的 Hub 拓扑仅覆盖虚拟化全速/高速设备模型，无法替代真实 HS Hub + LS/FS Split/TT 的硬件行为验证。

## 2. 回归验证命令与退出码汇总

所有命令均在 `/home/aagu/OS01/.worktrees/usb-hid-input` 根目录独立执行：

| # | 命令 | 退出码 | 结果 | 关键日志 / 产物路径 |
|---|------|--------|------|---------------------|
| 1 | `make PROFILE=x86_64-clang test-host` | `0` | PASS | `build/x86_64-clang/logs/tests/hosttests/`（含 `61` + `7` + `2` + `12` 项 Python/宿主测试） |
| 2 | `make PROFILE=x86_64-clang test-static` | `0` | PASS | `build/x86_64-clang/logs/tests/{validate-kernel,runtime-link-order,kernel-runtime-link,syscall-boundary,kernel-layout,kernel-canary-contract,header-object-audit,stack-frame-audit,driver-model-boundary,gui-build-audit,intr-dispatch-audit,arch10-capacity-audit,assertion-policy,user-canary}/` |
| 3 | `make PROFILE=x86_64-clang test-harness` | `2` | FAIL（4/65 模块） | 见第 3 节详情 |
| 4 | `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | `0` | PASS | `build/x86_64-clang/logs/tests/phase-0/20261115T115507Z-a1795e35603440e78185a6092c6c39dc` |
| 5 | `make PROFILE=x86_64-clang test-qemu SUITE=gui` | `0` | PASS（SMP=1, 2） | `build/x86_64-clang/logs/tests/gui/20261115T115519Z-d0d96f09a42244d68ca98e9b25434817` |
| 6 | `make PROFILE=x86_64-clang test-qemu SUITE=usb-boot` | `0` | PASS（6/6 子用例） | `build/x86_64-clang/test-results/usb-boot/`（见第 4 节） |
| 7 | `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | `0` | PASS（9/9 轮次） | `build/aarch64-clang/logs/tests/aarch64-uefi-smp/`（见第 5 节） |

> 注：`SUITE=usb-hid`（Task 6 专项 E2E，`direct`/`hub` × `SMP=1`/`2`）最近一次产物位于 `build/x86_64-clang/test-results/usb-hid/`（`direct-smp1-20261115T113940-513325`、`direct-smp2-20261115T113953-515200`、`hub-smp1-20261115T114006-517074`、`hub-smp2-20261115T114020-518950`）。

## 3. `test-harness` 失败明细（exit 2）

65 个 unittest 模块中 61 个通过（含 `qemutests.test_usb_hid_runner` 14 项和 `qemutests.test_usb_boot_runner`），4 个模块失败：

1. `qemutests.test_dispatch_generated`（`test_flavors_and_images_match_bridge`）：`AssertionError: 22 != 20`，新增 `usb-hid` flavor 后测试中的硬编码数量 `20` 未同步更新。
2. `qemutests.test_gui_runner`（`test_the_real_qmp_client_writes_the_transcript`、`test_the_transcript_is_not_truncated_when_it_is_reopened`）：`TypeError: _RecordingQmp.execute() got an unexpected keyword argument 'deadline'`，`qmp.input_events` 传递 `deadline=deadline` 后 mock `_RecordingQmp.execute` 签名未适配。
3. `qemutests.test_host_build_contract`（`test_inventory_unique`）：`AssertionError: 'boot' not found in {'gui', ...}`（`test_boot_devpath` 使用了未在 `VALID_GROUPS` 列出的 `'boot'` 组）。
4. `qemutests.test_scenario_catalog`（`test_all_host_tests_mapped`）：`AssertionError: 'test_usb_hid_report' not found in ...`，新增宿主测试 `test_usb_hid_report` 尚未映射到 `config/test-scenarios.json`。

## 4. `SUITE=usb-boot` 子用例明细（exit 0）

共享固件与镜像哈希：`firmware_sha256=f21ceda677a84b9e53a44226f59936fbfa4b2624d0d5dbdc8bfb6712bbcad926`、`disk_sha256=c5c3300364a66eeb5fa3961113268b6ead067f96575bc993dac90ffad4eb2c07`。

| 子用例 | 结果 | `log_dir` |
|--------|------|-----------|
| `readonly` | PASS | `build/x86_64-clang/test-results/usb-boot/20261115T115541-583930` |
| `dual` | PASS | `build/x86_64-clang/test-results/usb-boot/20261115T115549-585806` |
| `ahci` | PASS | `build/x86_64-clang/test-results/usb-boot/20261115T115559-588131` |
| `persistence` | PASS | `build/x86_64-clang/test-results/usb-boot/20261115T115606-590008` |
| `superspeed` | PASS | `build/x86_64-clang/test-results/usb-boot/20261115T115619-591898` |
| `hub` | PASS | `build/x86_64-clang/test-results/usb-boot/20261115T115627-593778` |

## 5. `aarch64-clang` SMP 交叉构建与启动验证（exit 0）

`make PROFILE=aarch64-clang test-aarch64 MODE=smp` 全部 9 轮（`cpus=1,2,4` × `run=1,2,3`）均返回 `PASS`（`timeout=false, complete=true, returncode=0`），确认新增 x86_64 USB HID 私有实现未污染 ARM 构建或启动流程：

- `cpus=1`: `20261115T115847Z-19713ccbd5474d60a024253fde073c3d`、`20261115T115852Z-01b470f8abc74b2683a31f387bd3f3f9`、`20261115T115856Z-9c5de62041f64829ad8f71a967cd140b`
- `cpus=2`: `20261115T115901Z-329232d81fb7401caf5ae5e58ccf25fc`、`20261115T115906Z-660a26f3bb5540238bee2c57b0e9ddd6`、`20261115T115911Z-73ee3fddfe7245b2b2534095e9eb21ff`
- `cpus=4`: `20261115T115916Z-daab1c737242416e9fbcf7c84b10d864`、`20261115T115921Z-4a9d88e14cda418c878298fbd281cf5b`、`20261115T115927Z-a0eae8fef53b451985e598376663c69d`
