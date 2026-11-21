# UBSan + KASan：x86_64 内核编译期插桩实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 x86_64 内核提供编译期 UBSan 与 KASan 插桩：默认全关、普通构建字节不变；`KERNEL_UBSAN=1` / `KERNEL_KASAN=1` 独立开关；KASan 含两阶段影子映射（head.S 静态零影子 → vmm 后真实影子）、slab 堆毒化钩子与 `__asan_report_*` 报告；UBSan 提供 `__ubsan_handle_*` 处理函数（log + kpanic）；QEMU E2E 注入用例证明两类违规可被检测。

**Architecture:**
- 构建接入沿 house 模式：`mk/project.mk` 在 profile include 前 校验 knob + 计算 `IMAGE_VARIANT` slug + KASan 时覆盖 `TARGET_TRIPLE=x86_64-unknown-linux-gnu`（clang 23 对 `x86_64-unknown-none` 三元组拒绝 `-fsanitize=kernel-address`，实测）；`OS01_SUBMAKE_ALLOWED` 白名单放行；`kernel/Makefile` 将 sanitize flags 折叠进 `ALL_CFLAGS`（`.cflags` fingerprint stamp 自动失效、增量正确）；UBSan 无需换三元组（none 三元组实测可用）。
- KASan 影子几何：`shadow(VA) = (VA >> 3) + 0xdffffc0000000000`（clang 23 kernel-address 内联常量，实测）；影子区落在 PML4[472] `0xffffec0000000000` 起的 512GB 窗口，与直映射（PML4[256] `0xffff800000000000`）、FB（PML4[288] `0xffff900000000000`）无冲突。两阶段：
  1. **head.S 静态零影子**（`#if OS01_KASAN` 门控，普通构建二进制不变）：PML4[472] → 静态 PDPT → 2 张 PD，1024 个 2M PDE 全指向 PA `0x200000` 的 2MB 零帧（present + huge + RO），覆盖 1GB 影子窗口 ⇔ RAM ≤ 8GB 的启动期检查全绿。
  2. **真实影子安装**（`x86_64_boot_memory` 在 `arch_boot_direct_map_init` 成功后调用 `kasan_shadow_install()`）：按直映射覆盖范围从 PMM 分配 2M 帧、memset 清零、写 `kernel_map[472]` 新 PUD 链（present|write），tlb_shootdown 后开始毒化。
- 堆钩子：`libc/stdlib/malloc.c` libk 模式 `malloc→kmalloc` / `free→kfree` 直转发（实测），故仅钩 `kernel/memory/slab.c` 即全覆盖：kmalloc 成功返回前 unpoison `[ptr, ptr+size)`、redzone `[ptr+size, ptr+cache_size)`；kfree 毒化 `[ptr, ptr+cache_size)` = `0xfb`。钩子经 `kernel/include/kasan/kasan.h` 的 static-inline 空实现（`!OS01_KASAN`）保持普通构建字节不变。
- 运行时自保护：`ubsan.c` / `kasan.c` / `kasan_shadow.c` 用对象级特例规则加 `-fno-sanitize=address,undefined` 编译（specific target rule 覆盖 pattern rule），避免处理函数自身再触发插桩。
- 报告：`__asan_report_loadN/storeN[_noabort]`、`__asan_handle_no_return`、`__ubsan_handle_*` → `log_err`（违规分类 + SourceLocation + 访问地址/长度）→ `kpanic`（fatal 路径，既有的异常回溯随之输出）。
- 栈插桩保持编译器默认（inline 影子写 + redzone，实测仅额外引用 `__asan_handle_no_return`）；全局红 zone 本期 `-mllvm -asan-globals=0` 关闭（内核无 `.init_array` 迭代机制，注册无处挂靠，留作后续增强）。

**Tech Stack:** freestanding C（x86_64 / clang 23，`--target=x86_64-unknown-linux-gnu` 仅 KASan）、GNU Make、hosttests（`@os01-test` + `make regen-test-catalog`）、QEMU E2E harness（qemutests + run.mk）。

**Spec:** 本计划即设计（探索结论附后），不另立 spec 文件。

---

## 全局约束

- **默认零影响**：`KERNEL_UBSAN` / `KERNEL_KASAN` / `KERNEL_SANITIZER_TEST` 缺省值（空/none）下，任何产物与 master 逐字节一致。head.S、slab.c、boot.c 的改动全部经 `#if` / static-inline 空实现门控。
- **仅限 x86_64**：KASan 在 aarch64 profile 下 `$(error)`（project.mk 的 TARGET_TRIPLE 门，模式同 FB_RESOLUTION_TEST）；UBSan 运行时架构中立，本期不接入 aarch64 profile（P1 惯例同 ASLR 先限 x86_64，roadmap 记后续）。
- **影子常量单源**：`KASAN_SHADOW_OFFSET = 0xdffffc0000000000`、`KASAN_SHADOW_BASE = 0xffffec0000000000`、PML4 index 472、2MB 零帧 PA `0x200000` 全部单源在 `kernel/include/kasan/kasan.h`（head.S 与 C 经 cpp 共享该头；.S 规则用 `ALL_CFLAGS` 预处理，实测 `-D` 可达）。
- **失败语义**：任何 KASan/UBSan 违规 = `log_err` + `kpanic`，不 recover、不继续执行（`_noabort` 符号由处理函数内部转 fatal）。
- **测试登记**：宿主测试带 `@os01-test` 元数据、更新 `config/test-scenarios.json` 并 `make regen-test-catalog`；QEMU 套件进 run.mk；日志用 `log_err` / `log_info`。
- **shadow 安装依赖序**：`kasan_shadow_install()` 必须在 `arch_boot_direct_map_init()` 成功之后调用（消费 `arch_boot_direct_map_ranges()` 的冻结覆盖，不自行读 E820）；安装前不得有任何毒化写（phase-1 影子只读）。

## 评审重点

1. **head.S PML4 布局改动的普通构建等价性**：PML4E 段 `#if OS01_KASAN` 分支必须同时调整 `.fill` 计数（255/215/39 vs 255/255），普通构建汇编出的 `__PML4E` 段必须与 master 逐字节一致（任务 1 验收含二进制对比）。
2. **两阶段交接窗口**：`kasan_shadow_install()` 全程插桩代码在跑——建新 PUD 链时对页表 VA 的所有访问落在 phase-1 零影子内（帧 PA < 8GB 恒成立），`kernel_map[472]` 必须最后一步原子改写 + shootdown，不得先换根后填表。
3. **kasan 自身分配不复用 kmalloc**：影子帧分配走 `alloc_pages` + `Phy_To_Virt` memset，绝不调 `kmalloc/calloc`（slab 钩子在 slab 初始化后毒化，kasan 运行时若走 slab 会被自身毒化闭环套住）。
4. **slab 钩子的字节不变性**：`kasan_kmalloc/kasan_kfree` 在 `!OS01_KASAN` 时为空 inline，slab.c 不新增 `#ifdef` 树；`OS01_KASAN` 时 redzone 上界用 `ksize` 同款 cache size（对象尺寸），不引入越 cache 边界的毒化。
5. **插桩 ABI 完整性**：`-O2` + `-mllvm -asan-globals=0` + `-fno-stack-protector` 不叠加（内核保留自身 canary 栈保护）下，链接期只允许出现本计划列出的运行时符号；链接错误即 ABI 清单漏项，先补清单再谈实现。

## 探索结论（事实锚点）

- clang 23.1.5（Arch）：`-fsanitize=kernel-address` 对 `x86_64-unknown-none` 报 unsupported，对 `x86_64-unknown-linux-gnu` 可用；`-fsanitize=undefined` 两者皆可。`-fno-sanitize-recover=kernel-address` 驱动层拒收（保持默认 `_noabort` 符号 + 处理函数内转 fatal）。
- KASan 内联影子检查：`movabs $0xdffffc0000000000` 常量；栈插桩 inline 写影子（无 `__asan_set_shadow_*` 依赖）；全局插桩经 `-mllvm -asan-globals=0` 摘除后仅剩 `__asan_report_load{N}/store{N}[_noabort]` + `__asan_handle_no_return`。
- UBSan 全内核文件（mm/slab/core/tty/vfs）实测未定义符号集：`__ubsan_handle_{divrem_overflow,out_of_bounds,pointer_overflow,type_mismatch_v1}`（全集处理函数仍按 compiler-rt ABI 全量提供）。
- 真实文件编译冒烟：UBSan（none triple）与 KASan（linux triple + `-mllvm -asan-globals=0`）在 mm/slab/core/tty/vfs 上均 exit 0。
- head.S 静态表：`__PML4E`（0x101000，entry0/256→0x102003）、`__PDPTE`（0x102000）、`__PDE`（0x103000，16×2M = 32MB 双向窗口）；`.S` 规则用 `EFFECTIVE_CC + ALL_CFLAGS` 预处理（`-D` 可达）；`.text.head` 基址 PA 0x100000（2M 对齐）。
- kernel 半区 PML4 表被所有地址空间共享（`vmm_free_user_map` 注释：256-511 为共享 kernel entries）→ 影子映射对所有进程可见。
- `vmm_init`/boot 直映射用 `calloc`（brk 早分配器可用），直映射覆盖经 `arch_boot_direct_map_ranges()` 暴露。
- `VIRT_FRAMEBUFFER_OFFSET = 0xffff900000000000`（PML4 288）与影子无冲突。

---

## 文件与职责

| 任务 | 新建 / 修改文件 | 职责 |
|---|---|---|
| 1 | 修改 `mk/project.mk`、`kernel/Makefile`、`kernel/arch/x86_64/head.S` | knob 校验/三元组覆盖/variant slug/白名单；ALL_CFLAGS 折叠与运行时对象特例规则；KASan 静态影子表（含普通构建等价性验收） |
| 2 | 新建 `kernel/include/ubsan/ubsan.h`、`kernel/ubsan/ubsan.c` | compiler-rt ABI 描述符结构 + 全量处理函数（log + kpanic） |
| 3 | 新建 `kernel/include/kasan/kasan.h`、`kernel/kasan/kasan.c`、`kernel/arch/x86_64/memory/kasan_shadow.c`；修改 `kernel/memory/slab.c`、`kernel/arch/x86_64/memory/boot.c` | 影子算术/分类/毒化纯逻辑、两阶段安装、报告处理函数、slab 钩子接入 |
| 4 | 新建 `kernel/selftest/test_kernel_sanitizers.c`、`qemutests/test_kernel_sanitizers.py`、`hosttests/cases/test_kasan_san_core.c`；修改 `kernel/core/main.c`、`mk/components/run.mk`、`config/test-scenarios.json`、hosttests mk | 注入 fixture（ubsan / kasan-oob 两用例）+ QEMU E2E 套件 + 宿主纯逻辑单测 + 场景登记 |
| 验收 | 修改 `docs/build/build.md`、`docs/roadmap.md`、`docs/changelog.md` | knob 文档、roadmap 勾账、changelog 闭环 |

## 任务序列（每任务先宿主可测部分，再构建/QEMU）

> 状态（2026-11-22 实施完成，与计划的偏差见各项注记）：

1. **任务 1 构建接入** ✅：knob + 折叠 + head.S 静态影子。普通构建 `kernel.bin` 与 master 逐字节一致（`3e358518…`，对照 pristine 构建复核）；UBSan/KASan 链接通过且 QEMU 启动到用户态。注记：KASan 需 kernel 本地三元组重定向（root 级 TARGET_TRIPLE 过不了受控 sub-make，且会泄漏 user/UEFI）；`-fno-sanitize=address` 不覆盖 kernel-address 组，去插桩须逐组拼写；flags 变更后需 rm 运行时对象强制的对象级 recipe 变化不触发重建。
2. **任务 2 UBSan 运行时** ✅：完整 ABI + 报告格式。探针实测 `signed integer add overflow at selftest/test_kernel_sanitizers.c:25:19` + panic。注记：object-size 误报（gate.h:236 自由指针转型写）且检查挂 alignment 组 → 两者一起剔除（Linux x86 同款 UBSAN_ALIGNMENT=n）；UBSan-only 构建同样需要 GS 预置（`this_cpu()` 读 GS=0 会踩 NULL percpu 解引用，tlb.c:107 实证）→ boot 入口门改为任一 knob。
3. **任务 3 KASan 运行时** ✅：影子安装 + 毒化 + 报告。探针实测 `WRITE of 1 byte(s) ... heap redzone (past requested size)`（shadow=0xfc、影子字节 `fc 00 00 00 fc…`、影子帧 PA 诊断）+ panic；无探针插桩内核完整启动到用户态（网络 + DHCP）零违规。注记（较计划的扩展）：影子须覆盖全部被触碰 VA 带（RAM 之外还有早期/重映射帧缓冲 → slot 476 链、early-MMIO）；运行期 MMIO 经 `x86_vmm_map_2m`/`x86_vmm_map_4k` 映射钩子按需补影子页（LAPIC PA 0xFEE00000、xhci 64 位 BAR 32GB 实证）；低半区排除清单 = uaccess.c + pmm_arch.c + kasan_boot.c。
4. **任务 4 测试体系** ✅（形态调整）：`qemutests/test_kernel_sanitizers.py` = 静态构建契约（10 项，恒跑）+ live QEMU 探针（`OS01_SANITIZER_LIVE=1` / `make test-kernel-sanitizers` 门控，双探针全链路断言）——未接入生成式 SUITE 分发目录（`mk/generated/test-dispatch.mk`），改用 test_doom_build_contract 式独立 unittest + run.mk 便捷目标；场景目录登记 2 条（`sanitizers.build.contract` / `sanitizers.probe.live_qemu`）。**宿主纯逻辑单测未立**：毒化/报告逻辑的验收由 live QEMU 探针承担（端到端证据强于 mock 单测），budget 权衡记录在案。
5. **任务 5 验收收尾** ✅：`make test-host` ✅、`make test-static` ✅（assertion-policy 豁免登记 `config/assertion-exemptions.json`）、普通构建字节一致 ✅、roadmap/changelog/build.md 回填 ✅。遗留：偶发 LAPIC 影子 0xff（5 次启动 1 次，报告自带影子帧 PA 诊断，复现后按诊断定位）已记 roadmap 后续项。
