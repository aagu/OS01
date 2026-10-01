# aarch64 IPI cpus≥2 FAIL — 根因与修复记录

**日期**：2026-09-26
**分支**：`fix/aarch64-ipi-fail`
**结论**：OS01 内核的 per-CPU 结构体误读；没有证据支持 QEMU GICv2 缺陷。

## 原始现象与误判

`make PROFILE=aarch64-clang test-aarch64-uefi-smp` 的多核 IPI 证据失败。早期诊断把 BSP 收到 SGI、AP2/AP3 的计数为零以及 `raw_iar=0x1` 归因于 QEMU 11.1.1 的 SGIR 路由。这只是根据计数器反推的假设；计数器自身的 CPU 归属没有被验证。随后提交的两个“暖机”SGI 和放宽 PASS 判定使证据失真：4 核日志曾显示 `cpu=1 received=3`、`cpu=2/3 received=0`，汇总却是 `status=PASS`。

## 已确认根因

`kernel/arch/aarch64/intr/ipi_test.c` 中的 `ipi_cpu_id()` 从 `TPIDR_EL1` 取指针后，错误地把它解释为 `aarch64_boot_percpu_t *`。在 `gic_ipi_test()` 之前，BSP 与所有 AP 都已执行 `percpu_install_gs()`，因此 `TPIDR_EL1` 指向 `percpu_data[cpu]`，类型是 `percpu_t`。

两个结构体布局不同：

| 类型 | 偏移 8 | `cpu_id` 偏移 |
|---|---:|---:|
| `aarch64_boot_percpu_t` | `cpu_id` | 8 |
| `percpu_t` | `need_resched` | 16 |

IPI handler 因而把 `need_resched` 当成逻辑 CPU 编号，向错误的 `ipi_received[]` / `ipi_done[]` 槽记账，还可能由错误的 CPU 回发 SGI 1。原先关于 QEMU `current_cpu->cpu_index` 的推断不能由这些失真的计数证明。

## 修复

- `ipi_cpu_id()` 使用运行期的 `cpu_id()`，与 `gic.c` 的 per-CPU 身份来源一致。
- 删除两个暖机 SGI，恢复只发送一次 `filter=OTHERS` 的原始测试。
- 恢复内核全目标送达的 PASS 判定，以及 harness 对每个 AP 恰收一次、汇总 `N-1/N-1`、回发 IAR `0x401` 的严格断言。

未改变任何结构体布局，因此无需因 ABI 变化执行 `make clean`。

## 复现与验证

修改前，在相同的 QEMU 11.1.1 和 4 核镜像上，日志为：

```
[ipi] cpu=1 received=3
[ipi] cpu=2 received=0
[ipi] cpu=3 received=0
[ipi] summary targets=3 received=1 status=PASS
[ipi] bsp raw_iar=0x1
```

修改后，移除暖机 SGI 且恢复严格验收，4 核日志为：

```
[ipi] cpu=1 received=1
[ipi] cpu=2 received=1
[ipi] cpu=3 received=1
[ipi] summary targets=3 received=3 status=PASS
[ipi] bsp raw_iar=0x401
```

命令 `make PROFILE=aarch64-clang test-aarch64-uefi-smp` 在 QEMU 11.1.1 上完成 1/2/4 核各 3 次，9/9 PASS。`python3 qemutests/aarch64_uefi_smp.py --self-test` 也通过。运行日志在 `test-results/aarch64-uefi-smp/20260926T023725-normal-34/`。
