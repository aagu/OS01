# OS01

OS01 是面向 x86_64 和 aarch64 的 UEFI hobby OS，两个架构共用 `kernel_main`，由 `arch/boot.h` 定义启动阶段、`arch/features.h` 声明能力。各 profile 支持的功能以构建文档为准。

<!-- CODEGRAPH_START -->
## CodeGraph

若仓库根目录存在 `.codegraph/`，定位或理解代码时先用 `codegraph_explore` / `codegraph_node`，再用文本搜索或直接读源码。无 MCP 工具时用 `codegraph explore "<符号或问题>"` / `codegraph node <符号或文件>`。若目录不存在，直接使用常规工具，不要自行建立索引。
<!-- CODEGRAPH_END -->

## Build and test

- `make run` / `make debug`：构建并在 QEMU 启动；依赖、profile、选项和完整目标见 [构建指南](docs/build/build.md) 与 [工具链](docs/build/toolchain.md)。
- `make test-host`：宿主测试；`make test-static`：静态审计；`make test-qemu SUITE=phase-0`：x86_64 QEMU 引导冒烟。
- syscall E2E 必须从顶层运行 `make OS01_SYSTEST=1 test-syscall`，不得同时设 `KERNEL_SELFTEST=1`；内核自测单独运行 `make KERNEL_SELFTEST=1 test-kernel-selftest`，避免干扰 systest 的 fork/exec/waitpid 用例。
- 新增宿主测试：在 `hosttests/cases/test_<name>.c` 写 `@os01-test` 元数据，按现有模式配置链接，再运行 `make regen-test-catalog`，将生成物随测试一并纳入变更。不要手写 `register_host_test` / `TEST_BINS`；详见 [测试规范](docs/testing/testing-policy.md#81-元数据扫描是唯一入口)。

## Git workflow

合并特性分支回 `master` 时优先 `git merge --ff`；用 `git merge-base --is-ancestor` 核实是否能快进，不要从历史合并提交推断策略。仅在需要或有意保留合并提交时使用 `--no-ff`。

## Directory organization

- 公共头放在 `kernel/include/<subsys>/`，与 `kernel/<subsys>/` 对应；用 `#include <subsys/foo.h>`，不要把公共头放在源文件旁。新增子系统前先确定源目录和头目录。
- `kernel/core/` 只放启动编排、致命路径和早期输出；其他代码归入对应子系统。
- 架构专属实现放 `kernel/arch/<arch>/`，专属头放 `kernel/include/arch/<arch>/`，架构中立接口放 `kernel/include/arch/`。`kernel/include/uapi/` 只放用户态 ABI，不放内核私有定义。
- 测试按层放置：`hosttests/`（宿主）、`qemutests/`（QEMU 集成）、`kernel/selftest/`（内核自测）、`runtime/selftest/`（运行时自测）。

## Critical gotchas

- 两套 UEFI 引导器与内核共用 `boot_context` ABI（`kernel/include/core/bootinfo.h`）；整数用固定宽度类型，不用 `unsigned long`（LLP64 与 LP64 宽度不同）。修改此 ABI 后，用 `make test-host` 运行包含 `hosttests/cases/test_bootinfo_abi.c` 的宿主测试；改动其他结构体或寄存器布局时，运行对应布局检查。
- x86_64 的 `set_intr_gate_raw` 只能接汇编桩；C 中断处理程序用 `DEFINE_INTR_STUB` + `REGISTER_INTR_HANDLER`。不要重载 GS 选择子。`set_tss64` 写入传入的 TSS，而不是 GDT；BSP 用 `TSS64_Table`，AP 用各自的 TSS，上下文切换通过 `cpu->tss_hw` 更新。
- `alloc_pages` 返回物理地址，解引用前先用 `Phy_To_Virt`。`vmm_unmap_4k_page` 必须在清除 PTE 前识别 `PAGE_COW` 并保留释放引用所需的物理地址。
- x86_64 的 `arch_do_signal_delivery` 只在返回用户态时交付信号（检查 `regs->cs & 3`）；TTY 调用的 `NULL` 路径只清理部分挂起信号，不向用户态交付。排查 Ctrl-C 时区分 `/dev/tty` 和 ash 所在的 PTY；`terminal.elf` 在 `exec` 后必须显式忽略 `SIGINT`。
- 新增或修改的内核运行日志用 `log_err` / `log_warn` / `log_info` / `log_debug` 或 `debug_<channel>()`，不用 `serial_printk` / `color_printk`；豁免以[日志规范](docs/log/log.md)为准。
- 新硬件接入走[子系统注册](docs/subsys/subsys.md)和对应驱动框架，不在 `kernel_main` 硬编码设备初始化。

## References

[架构与启动](docs/arch.md) · [跨边界符号与 ABI](docs/arch/cross-boundary-symbols.md) · [驱动](docs/driver/driver.md) · [文件系统](docs/fs/filesystem.md) · [调度](docs/sched/scheduler.md)
