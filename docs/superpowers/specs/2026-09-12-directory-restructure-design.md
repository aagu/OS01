# 目录结构重构设计（directory restructure）

日期：2026-09-12
状态：待复审
范围：kernel / test / tests / runtime/tests 的目录冗余与命名治理

## 1. 背景与目标

当前代码库有几处目录「功能相近但命名撞车」和「功能冗杂/命名冗余」，新增代码时难以判断该放哪里：

| 问题组 | 现象 | 根因 |
|---|---|---|
| A 测试目录 | `test/`（宿主 C 单元）、`tests/`（Python QEMU 集成）、`kernel/test/`（内核内自测）、`runtime/tests/`（runtime 内测）四个测试位置只用单复数区分 | 各时期独立演进，无统一命名 |
| B 核心目录 | `kernel/kernel/`（源）与 `kernel/include/kernel/`（头）名字里嵌两次 `kernel`；头目录是 ~85 个文件的大杂烩 | 子系统约定两种并存：`block/driver/fs/net` 有对称 `include/<subsys>/`，`memory/sched/sync/time/intr/tty/percpu/subsys` 的头却全平铺在 `include/kernel/` |
| C 次要冗余 | `kernel/include/device/` 只有 2 个头无对应源目录；`percpu/subsys/block` 单文件源目录；`test/include/` 镜像树与真头不同步；`tests/` 混入游离 C 文件 | 历史遗留 |

目标：**让目录结构自解释**——目录名说明职责、源目录与头目录一一对称、测试目录说明「在哪跑/测什么」。

## 2. 已确认决策

1. **范围**：全面整改（含组 C 次要问题），分阶段提交，每阶段可独立编译验证。
2. **`kernel/kernel/`** → 重命名 `kernel/core/`，且把 `random / hang / font / logo / log` 拆出 core。
3. **`kernel/include/kernel/`** → 按子系统拆分为对称的 `include/<subsys>/`（与源目录一一对应）。
4. **测试目录** → 按用途重命名：`test/`→`hosttests/`、`tests/`→`qemutests/`、`kernel/test/`→`kernel/selftest/`、`runtime/tests/`→`runtime/selftest/`。

## 3. 目标目录树

```
kernel/
├── arch/{x86_64,aarch64}/        # 不变
├── block/       blockdev.c        # 保留（见 §5 备注）
├── core/                          # ← 原 kernel/kernel/
│   ├── main.c  printk.c  panic.c  # 核心
│   ├── kallsyms.c  kallsyms.S     # 二段链接构建产物
│   └── (hang.c 并入 panic.c)
├── driver/      ahci e1000 fb font.psf logo pci pit rtc serial virtio-net
├── fs/          devfs elf ext2 fat file poll procfs select tmpfs vfs
├── intr/        apic/ pic/ dispatch irq softirq
├── log/         log.c              # ← 从 kernel/kernel/ 拆出
├── memory/      dump pmm pmm_arch slab tlb uaccess vma vmm
├── net/         net socket sys_arch
├── percpu/      percpu.c
├── random/      random.c           # ← 从 kernel/kernel/ 拆出
├── sched/       task arch_kernel_thread_entry
├── subsys/      subsys.c
├── sync/        completion futex mutex rwlock seqlock wait
├── time/        clocksource tick timer
├── tty/         canon console pty tty
├── selftest/                       # ← 原 kernel/test/
│   ├── selftest.c  symlink_selftest.c  test_*.c
└── include/                        # 不变：单一 -I 根
    ├── arch/        19 facade + x86_64/ + aarch64/    # ← 原 include/kernel/arch/，整体上移
    ├── core/        assert bootinfo debug panic printk selftest smp trace (hang 并入 panic)
    ├── driver/      ahci e1000 fb font keyboard logo pci pit rtc serial virtio-net
    ├── fs/          devfs elf fat file poll select vfs
    ├── intr/        apic interrupt ipi pic softirq
    ├── log/         log
    ├── memory/      memory memory_map pmm slab uaccess vma vmm
    ├── net/         net socket lwipopts arch/…
    ├── percpu/      percpu
    ├── random/      random
    ├── sched/       task
    ├── subsys/      subsys
    ├── sync/        completion futex mutex rwlock seqlock wait
    ├── time/        clockevent clocksource timer
    ├── tty/         canon console pty tty
    ├── uapi/        syscall futex time stat sockaddr   # 用户态 ABI，不动
    ├── errno.h  kernel.h                               # 顶层伞头，不动
```

（pty 已确认归 `tty/`，原 `driver/pty.c` 一并迁移。）

宿主测试（整改后）：
```
hosttests/            # ← 原 test/
├── Makefile  cases/  include/test_framework.h  mock/
qemutests/            # ← 原 tests/
│   ├── run_test.py  *_test.py  build_contract.sh  arch_runner/*.c
runtime/selftest/     # ← 原 runtime/tests/
```

### 3.1 组织规范（新增/迁移代码必须遵守）

这些规则同步写入 `AGENTS.md`「Directory organization」节，作为迁移后的长期约定：

1. **源目录 ↔ 头目录一一对称**：`kernel/<subsys>/*.c` 的公开头放 `kernel/include/<subsys>/*.h`；头统一在 `kernel/include/` 单一根解析（`-Iinclude`），`#include <subsys/foo.h>`。禁止头散落源目录旁。
2. **`kernel/core/` 最小化**：只容纳启动序列 + 致命路径 + 内核早期输出（`main`/`printk`/`panic`/`kallsyms`）；`random`/`log`/`font`/`logo`/`pty` 等必须下沉到对应子系统。
3. **架构分层**：per-arch 实现 `kernel/arch/<arch>/`；arch-neutral facade 头 + per-arch 头统一放 `kernel/include/arch/`（facade `arch/*.h` + `arch/<arch>/*.h`），与源目录 `kernel/arch/<arch>/` 对称。
4. **测试目录按用途命名**：`hosttests/`（宿主 C 单元）、`qemutests/`（QEMU Python 集成）、`kernel/selftest/`（内核内自测）、`runtime/selftest/`（runtime 内测）。
5. **`kernel/include/uapi/` 为用户态 ABI**：只放 syscall 号与跨边界结构，变动需评估 ABI。

后续新增子系统必须先确定「源目录 + 头目录」成对后才落文件；已有的旧路径在 P1–P6 逐一收敛到本节。

## 4. 完整文件迁移映射

### 4.1 核心源拆分（原 `kernel/kernel/` → 各归属）

| 原文件 | 目标 | 理由 |
|---|---|---|
| `main.c` | `core/main.c` | 启动序列，纯核心 |
| `printk.c` | `core/printk.c` | 内核早期输出，核心（aarch64 已有 printk_stub.c 替代，佐证其可替换性） |
| `panic.c` | `core/panic.c` | 致命路径，核心 |
| `hang.c` | **并入 `core/panic.c`** | halt 死循环，仅 panic/sched 调用；折叠避免 1 函数目录（已确认） |
| `log.c` | `log/log.c` | 分级通道 + per-arch `_log_*_impl`（aarch64 已有 `arch/aarch64/log_impl.c`），已成独立子系统 |
| `random.c` | `random/random.c` | /dev/random 熵源 + arch/random.h 配合，独立子系统 |
| `font.psf` + `font.h` | `driver/font.psf` + `include/driver/font.h` | 帧缓冲字形资产，与 fb.c 同域 |
| `logo.c` + `logo.h` | `driver/logo.c` + `include/driver/logo.h` | 启动 logo 画到 fb |
| `trace.c` + `trace.h` | `core/trace.c` + `include/core/trace.h` | 调试追踪，core-adjacent，保留 |
| `kallsyms.c` + `kallsyms.S` | `core/`（保持） | 二段链接构建产物，Makefile 特殊处理 |

### 4.2 头文件按子系统拆分（原 `kernel/include/kernel/*.h` → `include/<subsys>/`）

规则：**头跟随「拥有实现的源目录」**。归并后：

| 目标 `include/<subsys>/` | 头文件 |
|---|---|
| `core/` | assert, bootinfo, debug, panic, printk, smp, trace, selftest（hang 并入 panic） |
| `memory/` | memory, memory_map, pmm, slab, uaccess, vma, vmm |
| `sched/` | task |
| `sync/` | completion, futex, mutex, rwlock, seqlock, wait |
| `time/` | clockevent, clocksource, timer（timer.h 自 device/） |
| `intr/` | apic, interrupt, ipi, pic（pic.h 自 device/）, softirq |
| `tty/` | canon, console, pty, tty |
| `percpu/` | percpu |
| `subsys/` | subsys |
| `random/` | random |
| `log/` | log |
| `driver/` | fb, logo（+ 已有 ahci/e1000/keyboard/pci/pit/rtc/serial/virtio-net） |
| `fs/` | file, poll, select（+ 已有 devfs/elf/fat/vfs） |

现有 `include/{block,driver,fs,net,uapi}/` 保持，只把 `include/kernel/` 内对应的 fs/driver 头并入，并从 `include/kernel/` 移除。

此外，`kernel/include/kernel/arch/`（19 个 arch-neutral facade：atomic/barrier/cache/cpu/cpuid/elf/gate/io/irq/mmu/msr/percpu/random/regs/rtc/segment/spinlock/subsys/thread + `x86_64/` + `aarch64/`，共 **46 个头**）**整体上移**为 `kernel/include/arch/`，对应源目录 `kernel/arch/<arch>/`。涉及 **201 条** `#include <kernel/arch/...>` → `#include <arch/...>` 改写，纳入 P4。

### 4.3 测试目录重命名

| 原 | 新 | 类型 |
|---|---|---|
| `test/` | `hosttests/` | 宿主 C 单元 |
| `tests/` | `qemutests/` | Python QEMU 集成/架构 |
| `kernel/test/` | `kernel/selftest/` | 内核内自测 |
| `runtime/tests/` | `runtime/selftest/` | runtime 内建自测 |
| `tests/pmm_arch_test_runner.c` | `qemutests/arch_runner/pmm_arch_test_runner.c` | 游离 C 归位 |
| `test-results/` | 加入 `.gitignore` | 构建产物 |

**测试脚本内部硬编码路径**（目录移动后失效，P1 一并改写，非仅改入口调用点）：

- `qemutests/runtime_audit_test.py` 定位 `ROOT / "tests/runtime_audit.py"`（含 docstring）→ 改 `qemutests/runtime_audit.py`
- `qemutests/kernel_canary_contract_test.py` 引用 `"tests/kernel_canary_contract.mk"` → `qemutests/...`
- `qemutests/pmm_arch_test.py` docstring 引 `tests/aarch64_ram_test.py`、`tests/pmm_arch_test_runner.c`、`tests/aarch64_uefi_smp.py`
- `qemutests/build_contract.sh` 含 `test/build`、`host-test` 产物路径、`test_poll_requested.elf` 期望（host-test 构建产物若非改名则保持 `host-test`）
- `mk/components/run.mk` 全部 `tests/*.py|.sh` 调用点 + `host-test` 目标

## 5. 已拍板的边界决策

| 决策 | 结论 |
|---|---|
| `hang.c` | 并入 `core/panic.c`（`hang.h` 声明并入 `panic.h`） |
| `pty.c/pty.h` | 迁 `tty/`（原 `driver/pty.c` → `tty/pty.c`，头 `include/tty/pty.h`） |
| `percpu/` `subsys/` `block/` | 全部保留独立目录，不合并 |
| `include/device/` | `pic.h`→`intr/`，`timer.h`→`time/`，删 `device/` 层 |
| 历史文档 | `docs/superpowers/specs|plans/` 归档不溯及改；`AGENTS.md` + 顶层活文档（boot/architecture/driver/interrupt/smp/scheduler/signal/cow-mmap/debug/build-run-debug/roadmap/gui）同步更新 |

## 6. 构建系统改动

- **`kernel/Makefile`**：
  - wildcard 列表 `$(wildcard kernel/*.c)` → `$(wildcard core/*.c)`，新增 `random/*.c log/*.c`，`test/*.c` → `selftest/*.c`。
  - 特殊规则：`$(BUILD_DIR)/kernel/font.o: kernel/font.psf` → `driver/font.psf`；`$(BUILD_DIR)/kernel/kallsyms.o: kernel/kallsyms.c` → `core/...`；`kallsyms` 目标路径 `kernel/kallsyms.c` → `core/kallsyms.c`。
  - `-I$(KERNEL_HEADERS)`（=`-Iinclude`）**不变**——仍是唯一 `#include` 根，`#include <core/printk.h>` 之类在 `-Iinclude` 下解析。
  - `install-headers` 复制 `include/.` 不变（目录结构整体迁移，天然同步）。
- **`test/Makefile`（→`hosttests/Makefile`）**：`TESTS_DIR`/`TEST_SRC` 相对路径、`kernel/include/kernel/*` 引用、`runtime/tests` 引用更新。
- **`mk/components/run.mk`**：`tests/*.py`、`tests/build_contract.sh` → `qemutests/*`；`host-test` 目标 `-C test` → `-C hosttests`（若有）。
- **UEFI 链（`mk/components/uefi.mk` + `boot/uefi/`）**：P4 移除 `kernel/include/kernel/{bootinfo.h,arch/**}` 后，以下旧路径引用必须同步改写，否则 UEFI 子构建因 prerequisite/头文件缺失失败：
  - `mk/components/uefi.mk`：`UEFI_BOOT_INPUTS` 的 `kernel/include/kernel/bootinfo.h` → `kernel/include/core/bootinfo.h`；`kernel/include/kernel/arch/aarch64/handoff_layout.h` → `kernel/include/arch/aarch64/handoff_layout.h`
  - `boot/uefi/Makefile:107-108`：`$(ROOT)/kernel/include/kernel/bootinfo.h` → `$(ROOT)/kernel/include/core/bootinfo.h`；`$(ROOT)/kernel/include/kernel/arch/aarch64/handoff_layout.h` → `$(ROOT)/kernel/include/arch/aarch64/handoff_layout.h`
  - `boot/uefi/arch/arch.h:21`：`../../../kernel/include/kernel/bootinfo.h` → `../../../kernel/include/core/bootinfo.h`
  - `boot/uefi/arch/x86_64/boot.c:12`：`../../../../kernel/include/kernel/arch/x86_64/bootinfo_x86.h` → `../../../../kernel/include/arch/x86_64/bootinfo_x86.h`
  - `boot/uefi/arch/aarch64/loader.h:8`：`../../../../kernel/include/kernel/arch/aarch64/handoff_layout.h` → `../../../../kernel/include/arch/aarch64/handoff_layout.h`
- **`mk/components/kernel.mk` / `mk/profiles/*.mk`**：注释中的 `test/Makefile` 路径更新。
- **`.gitignore`**：`kernel/kernel/kallsyms` → `kernel/core/kallsyms`；新增 `test-results/`。

## 7. 分阶段实施计划

每阶段一个 commit，独立可编译/可测；rename 阶段以「`kernel.bin` 字节相同」作为正确性证据（沿用 roadmap 已验证的做法）。

| 阶段 | 内容 | 验证 |
|---|---|---|
| P1 | 测试目录重命名（4 处）+ 脚本内部硬编码路径改写（§4.3）+ `.gitignore` + 游离 C 归位 + 文档 | `make test`、`make KERNEL_SELFTEST=1`、`python3 qemutests/run_test.py` 及受影响脚本（runtime_audit_test.py / kernel_canary_contract_test.py / build_contract.sh）；`rg` 旧目录名零残留 |
| P2 | `kernel/kernel/` → `core/`（纯改名，不拆功能） | `kernel.bin` 字节相同 |
| P3 | 核心拆分（random→random/、log→log/、font+logo→driver/、hang→panic） | 编译通过 + `make` 全量 + `make KERNEL_SELFTEST=1` |
| P4 | 头文件按子系统拆分（~90 头 + 46 arch 头移动 + ~411 `kernel/` + 201 `kernel/arch/` `#include` 更新 + `hosttests` 影子同步 + UEFI 链 `boot/uefi/*` 与 `uefi.mk` bootinfo/handoff 路径） | `kernel.bin` 字节相同（注意 `__FILE__` 泄漏，见 §8）；x86_64 + aarch64 UEFI 构建通过（`make PROFILE=aarch64-clang` 产出 BOOTAA64.EFI 及 x86_64 BOOTX64.EFI） |
| P5 | 单文件目录/device 归并、pty 移位、`hosttests` 镜像树审计删冗余 | 编译 + 测试回归 |
| P6 | `AGENTS.md` + 活文档路径全局更新 | grep 无残留旧路径 |

（P2/P4 的「字节相同」对 rename-only 阶段强制；P3/P5 属语义移动，字节允许变化，以测试为准。）

## 8. 风险与豁免

1. **`__FILE__` 泄漏**：若 `log_err`/`assert`/`color_printk` 把 `__FILE__` 编译进字符串，纯 rename 后 `kernel.bin` 也会变。处理：优先用 `-ffile-prefix-map=. <root>` 归一，或接受「重新拍摄基线」再走字节相同验证。
2. **`#include` 顺序敏感**：`kernel/include` 内的头相互 include（如 `tty.h`→`canon.h`），拆分后需逐文件核对 include 归属，避免漏改导致「旧路径仍可编译」但「文档指向失效」。
3. **孤行子目录构建产物**：`build/` 下旧路径的 `.o` 在 rename 后残留，需 `make clean`（或依赖 `.d` 自动失效）。
4. **豁免**：`docs/superpowers/specs/`、`plans/` 历史归档不溯及改；`libc/` 布局（musl 风格）不属本次范围。

## 9. 成功标准

- `grep -rn 'kernel/kernel' kernel/ mk/ Makefile .gitignore AGENTS.md docs/`（活文档）无残留。
- `#include <kernel/...>`（含 `<kernel/arch/...>`）在 `kernel/` 无残留；arch 头改 `<arch/...>`，其余改 `<subsys/...>`。
- `grep -rn 'kernel/include/kernel/' kernel/ mk/ boot/ Makefile` 无残留（含 `uefi.mk` + `boot/uefi/*` 的 bootinfo/handoff）。
- 测试脚本内部无残留旧路径：`rg 'tests/runtime_audit\.py|tests/kernel_canary_contract|test/build|\btests/|\btest/' qemutests/ mk/` 零命中（build 产物路径除外）。
- `find . -name test -o -name tests` 仅剩语义明确的新名；四个测试入口名互不混淆。
- 全量 `make PROFILE=x86_64-clang`、`make KERNEL_SELFTEST=1`、`make OS01_SYSTEST=1 test-syscall`、`make PROFILE=aarch64-clang` 通过。