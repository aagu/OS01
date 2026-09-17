# aarch64 GICv2 Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 aarch64 中断路径从"entry.S 零保存 + time.c 硬编码 PPI 30"泛化为通用 GICv2 框架：driver（hw 访问/分类/enable/handler 表）+ 全量 save/restore + pt_regs_t + 通用 dispatch + PL011 SPI 33 通路 + SGI/IPI 跨核，SMP 1/2/4 ×3 全绿。

**Architecture:** 三层：`gic_driver.c`（纯逻辑、指针式 MMIO、hosttest 可编译）→ `gic.c` 生产 wrapper（挂 DTB 基址，`gic_init/gic_cpu_init` 签名不变）→ `trap.c::el1_irq` + `entry.S::el1_irq_entry`（31 GPR + sp_el0/elr/spsr 全量保存）。handler 表镜像 x86_64 `(nr, param, regs)` 签名但不编 kernel/intr。测试四层：hosttest mock MMIO（Task 1，含 dispatch-CPUID case）、QEMU serial 断言（--expect-gic 扩展）、破坏性 clobber 探针（shim 保链接的 RED，Task 2.2）、SPI socket 注入 harness（Task 2.3a RED / 2.3b GREEN）。IPI 计数走项目既有原子/屏障 API（arch_atomic_fetch_add + stlr/ldar release/acquire）。

**Tech Stack:** freestanding C（clang -target aarch64-none-elf）、AArch64 手写汇编（entry.S）、GNU Make profile 构建（mk/profiles/aarch64-clang.mk）、host clang 单元测试（hosttests/）、Python QEMU harness（qemutests/）。

**Spec:** docs/superpowers/specs/2026-09-17-aarch64-gic-phase1-design.md（v4，R3 修订版）

**R1 修订落点索引（9 条全落地；其中 R1-5/R1-9 在 R2 收口）：**
R1-1→Task 2.2 过渡 shim；R1-2→Task 1.2 `gic_clear_pending` + Task 2.2 探针 enable/route/拆除；
R1-3→Task 1.2 `gic_driver_set_unexpected` 定义进 API 块 + hosttest 回调用例；R1-4→Task 1.1
dispatch 用例传 NULL；R1-5→Task 2.3 拆 2.3a/2.3b + harness 明确支持 `--diagnostic-dtb`
（R2-1 修复其 --self-test fixture 后收口）；R1-6→Task 3.2 stlr/ldar +
arch_atomic_fetch_add 精确内存序；R1-7→删除全部 x86 build 验证，边界核查改为源级 diff；
R1-8→全部行数引用重核（entry.S 116 / main.c 264 / dtb.c 83 / trap.c 20 / pl011.c 135 /
harness 578 / hosttests TEST_BINS 62-82 等）；R1-9→hosttest dispatch-CPUID case +
per-CPU IAR trace + AP1 回发 SGI 与 `[ipi] bsp raw_iar=0x401` E2E 断言
（R2-6 改 per-CPU 槽后收口）。

**R2 修订落点索引（7 条全落地）：**
R2-1→Task 2.3a `--self-test` fixture：注入窗口用例输入只含 armed 行；
R2-2→Task 1.1 INTID 33 的 IPRIORITYR/ITARGETSR 断言 +8→+32（(33/4)*4=32，保留 >>8）；
R2-3→Task 1.2 wrapper 的 intids marker 后立即换行（独占一行），harness 全行 regex 不变；
R2-4→Task 2.2 clobber 探针重写为**确定性 trampoline**：自发 SGI 2 触发真实 IRQ，
RED 判据 = AAPCS64 实参寄存器 x0-x2 架构性必然被销毁（与编译器无关）；
asm 输出经 `mov %0,...` 显式回写（不再依赖写死 x9）；
R2-5→Task 2.2 unexpected 探针改观察递送：probe 注入的 unexpected 回调对 intid 40
计数并 release 置 flag，轮询到 flag 且 count==1 才打 survived；
R2-6→Task 1.2 IAR trace 改 per-CPU 槽（`gic_driver_set_cpu_index` 注入 TPIDR 实现，
`trace_iar[cpu]`），`gic_dbg_last_iar()` 读本 CPU 槽——跨核 ack 不再互相覆写；
R2-7→spec §7.5 引用改 aarch64_percpu.h:93-115。

**R3 修订落点索引（1 条全落地）：**
R3-1→Task 3.2 `gic_ipi_test()` 在两次 `gic_register_handler()` 成功后、首次
`gic_send_sgi()` 前调 `arch_publish_handler_table()`（新加 wrapper，aarch64=
`dsb ishst` + "memory" clobber，发布 handler 表给被 SGI 唤醒的 AP）；
IPI harness 断言 = 严格恰一次（received==1 / summary targets=received /
raw_iar==0x401 恰一条），不允许多次确认——多次会 FAIL（不假阳、不假阴）。

**R4 修订落点索引（4 条全落地）：**
R4-1→Task 3.2 给出 wrapper 完整 header 代码（static inline + dsb ishst），Task
步骤加精确调用行（arch_publish_handler_table(); 在 register 成功与 send_sgi
之间）；
R4-2→wrapper 改放 `kernel/include/arch/aarch64/gic_pub.h`（**仅 aarch64 头**），
不进 `kernel/include/arch/barrier.h`（后者被 e1000.c:9 include，改它破坏
"x86 编译输入逐字节不变"边界）；
R4-3→spec §7.5 + 头文件注释：明确 dsb ishst = "完成 + 指令边界"（vs dmb ishst
仅排序），并说明这是有意的较强选择；
R4-4→spec 版本引用 v3/R2→v4/R3；cntp 描述纠正（实际在 smp_boot_aps() 后注册，
但 AP CNTP disabled ⇒ 仅 BSP 接收，跨核问题不存在）。

## Global Constraints

- **Worktree**：全部工作在 `feat/aarch64-gic` worktree（/home/aagu/aarch64-gic）完成，不碰 master。
- **不动 x86_64**：`kernel/arch/x86_64/`、`kernel/include/arch/x86_64/`、`kernel/intr/` 零改动。
  **边界核查方式（R1-7）：只做源级验证**——每个功能 commit 前跑
  `git diff --stat <分支起点> -- kernel/arch/x86_64 kernel/include/arch/x86_64 kernel/intr`
  必须为空。源级零改动 ⇒ x86 构建输入逐字节不变 ⇒ 构建产物不可能回归。
  **本 Phase 不跑任何 x86 build 作验证**（spec §6.2）。
- **不编 kernel core**：kernel/Makefile:42-43 白名单不动；新文件只放 `kernel/arch/aarch64/`（wildcard 自动收编，kernel/Makefile:70-71）；新头文件只放 `kernel/include/arch/aarch64/`。
- **探针门控**：所有测试/探针内核代码 `#if OS01_SELFTEST`（main.c:18/224 既有模式）；唯一生产行为变化 = AP 尾循环开 DAIF.I 收 IPI（Task 3.2，commit message 显式声明）。
- **跨核发布屏障（R3-1）**：新建 `kernel/include/arch/aarch64/gic_pub.h`（**仅 aarch64 头**，
  不进 `kernel/include/arch/barrier.h`——后者被 `kernel/driver/e1000.c:9` include，
  改它会破坏 x86 编译输入零变更边界，R4-2 强制 aarch64 专属），声明并定义
  `arch_publish_handler_table()` = aarch64 `__asm__ __volatile__("dsb ishst" ::: "memory")`。
  在"修改全局 handler 表之后、向 AP 发送 SGI 之前"调用。Task 3.2 的 `gic_ipi_test()`
  是唯一当前调用点。spec §7.5 + §8 R3-1。
- **hw 层零依赖**：`gic_driver.c` 不 include boot_log/dtb/smp，不打日志，只返回错误码 + `gic_driver_set_unexpected` 回调注入（R1-3）。`struct pt_regs` 用前向声明（facade 按 `__aarch64__` 分发，host 编译会 #error，kernel/include/arch/regs.h:29-30）；**hosttest 内不得定义 pt_regs 对象**（不完整类型不可定义对象，R1-4）——dispatch 用例传 NULL。
- **链接安全（R1-1）**：dispatch 切换期间旧入口符号 `el1_irq_dispatch`（entry.S:78 bl 的目标）必须始终可解析——RED 阶段保留 shim，entry.S 替换与 shim 删除在同一变更内。
- **ISR 顺序契约不变**（phase1 spec §2.3）：TVAL 重装仍在 handler 最前；EOI 统一移交 dispatch（handler 返回后执行）。
- **构建/测试入口**（全部真实 target，从 repo 根）：
  - hosttest：`make -C hosttests PROFILE=aarch64-clang OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver`（root `test` 被 rootfs capability 门挡，run.mk:258）
  - 全量回归：`make PROFILE=aarch64-clang test-aarch64-uefi-smp`（run.mk:161-172，KERNEL_SELFTEST=1 构建 + qemutests/aarch64_uefi_smp.py --cpus 1 2 4 --repeat 3）
  - SPI 注入：`make PROFILE=aarch64-clang test-aarch64-gic-spi`（Task 2.3b 新增）
- **commit 尾注**：`Co-Authored-By: Claude Code <noreply@anthropic.com>`；commit 划分 = 1 docs + 3 功能（GIC driver = Task 1.1+1.2 / entry.S+dispatch+SPI = Task 2.1+2.2+2.3a+2.3b / SGI = Task 3.1+3.2），RED 测试与 GREEN 实现同 commit 落地（commit 时全绿）。
- 每个 RED 步骤必须**先跑出预期失败并留存输出**再写实现（superpowers:test-driven-development）。RED 意外通过 = 测试自身缺陷，停下调查修正，不允许带着"意外绿"继续。

---

### Task 0: spec/plan 自纳入（文档 commit，放最前）

**Files:**
- Create: `docs/superpowers/specs/2026-09-17-aarch64-gic-phase1-design.md`（本 plan 的 Spec，v2 已存在）
- Create: `docs/superpowers/plans/2026-09-17-aarch64-gic-phase1-plan.md`（本文件）

**Interfaces:**
- Consumes: 无
- Produces: 两份文档（后续 commit 引用其路径）

- [ ] 确认两份文档都在 worktree 内且 git 可见
  ```sh
  cd /home/aagu/aarch64-gic && git status --short docs/superpowers/
  # 预期：?? docs/superpowers/plans/2026-09-17-aarch64-gic-phase1-plan.md
  #       ?? docs/superpowers/specs/2026-09-17-aarch64-gic-phase1-design.md
  ```
- [ ] commit
  ```sh
  git add docs/superpowers/specs/2026-09-17-aarch64-gic-phase1-design.md \
          docs/superpowers/plans/2026-09-17-aarch64-gic-phase1-plan.md
  git commit -m "docs(superpowers): aarch64 GICv2 Phase 1 spec + implementation plan (R1 修订版)

  - spec v2: GICv2 硬件模型 / 现状审计(真实 line no., R1-8 重核) /
    pt_regs_t 精确布局(272B) / dispatch 流程(shim 保链接, R1-1) /
    x86_64 范式对照 / G1-G6 / non-goals
  - plan: 8 Task RED/GREEN（hosttest mock-MMIO 含 dispatch-CPUID case +
    QEMU --expect-gic + SPI socket 注入 2.3a/2.3b 拆分 + SGI/IPI
    release-acquire 协议）
  - R1 评审 9 条(3 blocker/4 major/2 minor)全数落地，索引见 plan 头部
  - 纠正三处未验证路径: mk/components/aarch64.mk 与 mk/qemu.mk 不存在
    (真身在 image.mk:96-164 / run.mk), thirdpart/aarch64/ 不存在
    (GIC 常量真身在 kernel/arch/aarch64/reg.h)

  Co-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 1.1: RED — hosttest GICv2 driver（mock MMIO）

**Files:**
- Create: `hosttests/cases/test_gic_driver.c`
- Modify: `hosttests/Makefile`（TEST_BINS 追加 + 三条规则 + PHONY；TEST_BINS 列表在 62-82 行，追加在 `$(TEST_BLD)/test_lwip_rand.elf`（line 82）之后；规则追加在文件尾部 test_lwip_rand 规则块之后）

**Interfaces:**
- Consumes（尚不存在——这正是 RED 的来源）: `kernel/include/arch/aarch64/gic.h` 全部 API（见 Task 1.2 Produces）
- Produces: 可重复的失败命令 `make -C hosttests ... test_gic_driver`；测试夹具（mock MMIO 数组 + 断言集），Task 1.2 完成后原样转绿

**mock 设计**（写进测试文件头注释）：
```c
/* mock MMIO：两个普通数组，把地址交给 gic_dev_init。寄存器偏移沿用
 * kernel/arch/aarch64/reg.h 的值（host 侧在测试里重定义同值宏，不 include
 * reg.h——它带 aarch64 target 专用 inline asm 访问器）。 */
static uint32_t gicd_mock[0x400];        /* 覆盖到 SGIR 0xF00（索引 0x3C0） */
static uint32_t gicc_mock[0x20];         /* 覆盖到 GICC_AHPPIR 0x28 */
```

- [ ] 写 `hosttests/cases/test_gic_driver.c` 骨架（真实代码）：
  ```c
  /* hosttests/cases/test_gic_driver.c — GICv2 driver 单元测试（spec §4.1/G1）。
   *
   * 编译【真实生产文件】kernel/arch/aarch64/gic_driver.c（host clang，无修改），
   * MMIO 是两个 mock 数组。覆盖: init(TYPER/IIDR)、分类、enable/disable、
   * priority/targets、handler 注册表、unexpected 回调、SGIR 编码、
   * set/clear pending、IAR/EOIR 往返(含 CPUID 位)、dispatch 三分支
   * (spurious/unexpected/命中) + dispatch-CPUID case (R1-9)。
   *
   * R1-4: dispatch 用例一律传 NULL regs——gic.h 只前向声明 struct pt_regs，
   * 本测试绝不定义 pt_regs 对象（不完整类型不可定义对象）。 */
  #include <test_framework.h>
  #include <stdint.h>
  #include <stdbool.h>
  #include <arch/aarch64/gic.h>

  /* 与 kernel/arch/aarch64/reg.h 同值（reg.h 带 target asm，host 不能 include） */
  #define M_GICD_CTLR       0x000
  #define M_GICD_TYPER      0x004
  #define M_GICD_IIDR       0x008
  #define M_GICD_IGROUPR    0x080
  #define M_GICD_ISENABLER  0x100
  #define M_GICD_ICENABLER  0x180
  #define M_GICD_ISPENDR    0x200
  #define M_GICD_ICPENDR    0x280
  #define M_GICD_IPRIORITYR 0x400
  #define M_GICD_ITARGETSR  0x800
  #define M_GICD_SGIR       0xF00
  #define M_GICC_CTLR       0x000
  #define M_GICC_PMR        0x004
  #define M_GICC_IAR        0x00C
  #define M_GICC_EOIR       0x010

  static uint32_t gicd_mock[0x400];
  static uint32_t gicc_mock[0x20];
  static struct gic_dev dev;

  static void mock_reset(uint32_t typer_itlines)
  {
      for (unsigned i = 0; i < 0x400; ++i) gicd_mock[i] = 0;
      for (unsigned i = 0; i < 0x20; ++i) gicc_mock[i] = 0;
      gicd_mock[M_GICD_TYPER / 4] = typer_itlines & 0x1f;
      gicd_mock[M_GICD_IIDR / 4] = 0x0200143b;   /* QEMU virt GICv2 IIDR 非零 */
  }
  static uint32_t rd(const uint32_t *m, uint32_t off) { return m[off / 4]; }

  /* handler 注册表探针（regs 恒为 NULL，handler 不解引用） */
  static uint32_t hit_intid; static uint64_t hit_param; static uint32_t hit_calls;
  static void probe_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
  { (void)regs; hit_intid = intid; hit_param = param; ++hit_calls; }

  /* unexpected 回调探针（R1-3 的 hosttest 侧覆盖） */
  static uint32_t unexpected_hits; static uint32_t unexpected_last;
  static void probe_unexpected(uint32_t intid)
  { ++unexpected_hits; unexpected_last = intid; }

  /* per-CPU IAR trace 的 cpu-index 探针（R2-6 的 hosttest 侧覆盖） */
  static uint32_t mock_cpu_index; static uint32_t mock_cpu_index_calls;
  static uint32_t probe_cpu_index(void) { ++mock_cpu_index_calls; return mock_cpu_index; }
  ```
- [ ] 追加测试主体（各 suite 真实断言）：
  ```c
  static void suite_init(void)
  {
      TEST_SUITE("gic_dev_init");
      mock_reset(2);                                   /* ITLinesNumber=2 → 96 intids */
      assert_eq(0, gic_dev_init(&dev, gicd_mock, gicc_mock));
      assert_eq(96, (int)dev.nr_intids);
      mock_reset(2); gicd_mock[M_GICD_IIDR / 4] = 0;   /* IIDR=0 → 拒绝 */
      assert_true(gic_dev_init(&dev, gicd_mock, gicc_mock) != 0);
      assert_true(gic_dev_init(&dev, 0, gicc_mock) != 0);      /* NULL gicd */
      assert_true(gic_dev_init(&dev, gicd_mock, 0) != 0);      /* NULL gicc */
  }

  static void suite_classify(void)
  {
      TEST_SUITE("gic_irq_type");
      assert_eq(GIC_IRQ_SGI, gic_irq_type(0));
      assert_eq(GIC_IRQ_SGI, gic_irq_type(15));
      assert_eq(GIC_IRQ_PPI, gic_irq_type(16));
      assert_eq(GIC_IRQ_PPI, gic_irq_type(30));        /* CNTP */
      assert_eq(GIC_IRQ_SPI, gic_irq_type(32));
      assert_eq(GIC_IRQ_SPI, gic_irq_type(33));        /* PL011 RX */
      assert_eq(GIC_IRQ_SPI, gic_irq_type(1019));
      assert_eq(GIC_IRQ_INVALID, gic_irq_type(1020));
      assert_eq(GIC_IRQ_INVALID, gic_irq_type(1023));
      assert_eq(GIC_IRQ_INVALID, gic_irq_type(4000));
  }

  static void suite_enable(void)
  {
      TEST_SUITE("enable/disable/priority/targets");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      /* SPI 33: ISENABLER[1] bit1, 优先级字节 33, 目标 byte → cpu0。
       * R2-2: byte 寄存器地址 = BASE + (33/4)*4 = BASE + 32; 33%4=1 → >>8。 */
      assert_eq(0, gic_irq_config(&dev, 33, true, 0x00, 0x01));
      assert_eq((uint32_t)(1u << 1), rd(gicd_mock, M_GICD_ISENABLER + 4));
      assert_eq(0x00u, (rd(gicd_mock, M_GICD_IPRIORITYR + 32) >> 8) & 0xff);
      assert_eq(0x01u, rd(gicd_mock, M_GICD_ITARGETSR + 32) & 0xff);
      assert_eq(0u, rd(gicd_mock, M_GICD_IGROUPR + 4) & (1u << 1));   /* 组0 */
      /* disable 走 ICENABLER */
      assert_eq(0, gic_irq_disable(&dev, 33));
      assert_eq((uint32_t)(1u << 1), rd(gicd_mock, M_GICD_ICENABLER + 4));
      /* 越界拒绝 */
      assert_true(gic_irq_enable(&dev, 96) != 0);      /* == nr_intids */
      assert_true(gic_irq_enable(&dev, 2000) != 0);
  }

  static void suite_registry(void)
  {
      TEST_SUITE("handler registry");
      gic_unregister_handler(35);                       /* 未注册 → 容错 */
      assert_eq(0, gic_register_handler(35, probe_handler, 0x1234, "t35"));
      assert_true(gic_register_handler(35, probe_handler, 0, "dup") != 0);
      assert_true(gic_register_handler(96, probe_handler, 0, "oor") != 0);
      uint64_t param = 0;
      assert_true((uintptr_t)gic_get_handler(35, &param) == (uintptr_t)probe_handler);
      assert_eq(0x1234, (int)param);
      assert_true(gic_get_handler(36, &param) == 0);
      assert_eq(0, gic_unregister_handler(35));
      assert_true(gic_get_handler(35, &param) == 0);
  }

  static void suite_ack_eoi(void)
  {
      TEST_SUITE("IAR/EOIR roundtrip + per-CPU trace");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      uint32_t iar = (2u << 10) | 35u;                  /* SGI 风格: CPUID=2 */
      gicc_mock[M_GICC_IAR / 4] = iar;
      assert_eq((int)iar, (int)gic_ack(&dev));          /* 原始值, 含 CPUID */
      assert_eq((int)iar, (int)dev.dbg_last_iar);       /* 单槽 trace（无 hook 时） */
      gic_eoi(&dev, iar);
      assert_eq((int)iar, (int)rd(gicc_mock, M_GICC_EOIR));  /* 原样写回 — D7 修复 */
      /* per-CPU trace (R2-6): 注入 cpu-index hook 后, ack 写本 CPU 槽,
       * 单槽字段照写（向后兼容）, 其他核槽不受影响。 */
      gic_driver_set_cpu_index(probe_cpu_index);
      mock_cpu_index = 3; gicc_mock[M_GICC_IAR / 4] = (3u << 10) | 7u;
      assert_eq(0xC07, (int)gic_ack(&dev));
      assert_eq(0xC07, (int)gic_driver_trace_get(3));   /* 本 CPU 槽 */
      assert_eq(0, (int)gic_driver_trace_get(0));       /* 其他槽仍为 0 */
      mock_cpu_index = 0; gicc_mock[M_GICC_IAR / 4] = 99u;
      assert_eq(99, (int)gic_ack(&dev));
      assert_eq(99, (int)gic_driver_trace_get(0));
      assert_eq(0xC07, (int)gic_driver_trace_get(3));   /* 旧槽不被跨"核"覆写 */
      assert_true(mock_cpu_index_calls >= 2);
      gic_driver_set_cpu_index(NULL);                   /* 清除 → 回退单槽路径 */
      gicc_mock[M_GICC_IAR / 4] = 55u;
      assert_eq(55, (int)gic_ack(&dev));
      assert_eq(55, (int)dev.dbg_last_iar);
      gicc_mock[M_GICC_IAR / 4] = 1023;                 /* spurious */
      assert_eq(1023, (int)gic_ack(&dev));
  }

  static void suite_dispatch(void)
  {
      TEST_SUITE("gic_dev_dispatch (NULL regs)");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      hit_calls = 0; unexpected_hits = 0;
      assert_eq(0, gic_register_handler(35, probe_handler, 0x5678, "d35"));
      gicc_mock[M_GICC_IAR / 4] = 35;
      gic_dev_dispatch(&dev, NULL);
      assert_eq(1, (int)hit_calls);
      assert_eq(35, (int)hit_intid);
      assert_eq(0x5678, (int)hit_param);
      assert_eq(35, (int)rd(gicc_mock, M_GICC_EOIR));   /* handler 后 EOI */
      /* spurious: 不写 EOIR、不调 handler、不触发 unexpected */
      gicc_mock[M_GICC_IAR / 4] = 1023; gicc_mock[M_GICC_EOIR / 4] = 0;
      gic_dev_dispatch(&dev, NULL);
      assert_eq(1, (int)hit_calls);
      assert_eq(0, (int)rd(gicc_mock, M_GICC_EOIR));
      assert_eq(0, (int)unexpected_hits);
      /* unexpected: 无 handler → unexpected_cb(intid) + 仍 EOI（防 GIC 锁死,
       * 对齐 time.c:105-115 语义）。R1-3: 回调可注入/可清除 */
      gic_driver_set_unexpected(probe_unexpected);
      gicc_mock[M_GICC_IAR / 4] = 60;
      gic_dev_dispatch(&dev, NULL);
      assert_eq(1, (int)hit_calls);
      assert_eq(60, (int)rd(gicc_mock, M_GICC_EOIR));
      assert_eq(1, (int)unexpected_hits);
      assert_eq(60, (int)unexpected_last);
      gic_driver_set_unexpected(NULL);                  /* 清除后不再触发 */
      gicc_mock[M_GICC_IAR / 4] = 61;
      gic_dev_dispatch(&dev, NULL);
      assert_eq(1, (int)unexpected_hits);
      assert_eq(61, (int)rd(gicc_mock, M_GICC_EOIR));
      /* dispatch-CPUID case (R1-9): CPUID=3 的 SGI 7 走真实 dispatch 路径,
       * handler 只见低 10 位, EOIR mock 必须等于完整 0xC07 */
      assert_eq(0, gic_register_handler(7, probe_handler, 0, "d7"));
      gicc_mock[M_GICC_IAR / 4] = (3u << 10) | 7u;
      gic_dev_dispatch(&dev, NULL);
      assert_eq(7, (int)hit_intid);
      assert_eq(0xC07, (int)rd(gicc_mock, M_GICC_EOIR));
      gic_unregister_handler(35);
      gic_unregister_handler(7);
  }

  static void suite_sgi(void)
  {
      TEST_SUITE("SGIR + set/clear pending");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      gic_send_sgi(&dev, 5, 0x03, GICD_SGIR_FILTER_LIST);
      assert_eq((int)(((0u << 24) | (3u << 16) | 5u)), (int)rd(gicd_mock, M_GICD_SGIR));
      gic_send_sgi(&dev, 0, 0, GICD_SGIR_FILTER_OTHERS);
      assert_eq((int)(1u << 24), (int)rd(gicd_mock, M_GICD_SGIR));
      gic_send_sgi(&dev, 1, 0x01, GICD_SGIR_FILTER_LIST);   /* AP1→BSP 回发编码 */
      assert_eq((int)((1u << 16) | 1u), (int)rd(gicd_mock, M_GICD_SGIR));
      gic_set_pending(&dev, 40);                        /* ISPENDR 测试注入 (R1-2) */
      assert_eq((uint32_t)(1u << 8), rd(gicd_mock, M_GICD_ISPENDR + 4));
      gic_clear_pending(&dev, 40);                      /* ICPENDR 测试拆除 (R1-2) */
      assert_eq((uint32_t)(1u << 8), rd(gicd_mock, M_GICD_ICPENDR + 4));
  }

  static void suite_cpu_iface(void)
  {
      TEST_SUITE("cpu interface enable");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      gic_dev_cpu_enable(&dev);
      assert_eq(0xffu, rd(gicc_mock, M_GICC_PMR));
      assert_eq(1u, rd(gicc_mock, M_GICC_CTLR));
  }

  int main(void)
  {
      suite_init(); suite_classify(); suite_enable(); suite_registry();
      suite_ack_eoi(); suite_dispatch(); suite_sgi(); suite_cpu_iface();
      printf("\n%s: %d total, %d passed, %d failed\n", "test_gic_driver",
             __test_stats.total, __test_stats.passed, __test_stats.failed);
      return __test_stats.failed ? 1 : 0;
  }
  ```
  （`__test_stats` 由 test_framework.h 提供，hosttests/include/test_framework.h:15-16。）
- [ ] 改 `hosttests/Makefile`：
  - TEST_BINS 列表（`$(TEST_BLD)/test_lwip_rand.elf`，line 82 之后）追加：
    ```make
    $(TEST_BLD)/test_gic_driver.elf \
    ```
  - `.PHONY` 行（`test_lwip_rand` 旁）追加 `test_gic_driver`。
  - 文件尾部追加规则（镜像 test_lwip_rand 模式，hosttests/Makefile 尾部 LWIP 块）：
    ```make
    # GICv2 driver hosttest: host-compile the PRODUCTION
    # kernel/arch/aarch64/gic_driver.c against mock-MMIO arrays (spec §4.1).
    # gic_driver.c is dependency-free (no arch asm, no UART logging); the
    # pt_regs type is forward-declared in <arch/aarch64/gic.h> and the test
    # never defines a pt_regs object (R1-4: dispatch cases pass NULL).
    GIC_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

    $(TEST_BLD)/gic_driver_production.o: $(TESTS_DIR)/kernel/arch/aarch64/gic_driver.c \
            $(TESTS_DIR)/kernel/include/arch/aarch64/gic.h
    	@mkdir -p $(TEST_BLD)
    	$(HOST_CC) $(GIC_HOST_CFLAGS) -c $< -o $@

    $(TEST_BLD)/test_gic_driver.elf: $(TEST_BLD)/test_gic_driver.o $(TEST_BLD)/gic_driver_production.o
    	$(HOST_CC) -o $@ $^

    test_gic_driver: $(TEST_BLD)/test_gic_driver.elf
    	$<
    ```
- [ ] **跑 RED**（从 repo 根 /home/aagu/aarch64-gic）：
  ```sh
  make -C hosttests PROFILE=aarch64-clang \
       OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver 2>&1 | tail -5
  ```
  预期失败（二选一，取决于 make 先撞哪个 prerequisite）：
  ```
  make: *** No rule to make target 'build/aarch64-clang/host-test/gic_driver_production.o',
        needed by 'build/aarch64-clang/host-test/test_gic_driver.elf'.  Stop.
  ```
  或编译 test_gic_driver.c 时：
  ```
  hosttests/cases/test_gic_driver.c:9:10: fatal error: 'arch/aarch64/gic.h' file not found
  ```
  留存输出（RED 证据）。**不 commit**（与 Task 1.2 同 commit）。

---

### Task 1.2: GREEN — gic.c 泛化真实现

**Files:**
- Create: `kernel/include/arch/aarch64/gic.h`（公共 API 头）
- Create: `kernel/arch/aarch64/gic_driver.c`（纯逻辑 driver，host 可编译）
- Modify: `kernel/arch/aarch64/gic.c`（整文件重写为生产 wrapper，`gic_init/gic_cpu_init` 对外签名不变——main.c:247 与 smp.c:208 的调用点零改动）
- Modify: `kernel/include/arch/aarch64/smp.h:9-10`（核对 `gic_init/gic_cpu_init` 声明与 gic.h 一致后改为 include `<arch/aarch64/gic.h>`，避免重复声明漂移）

**Interfaces:**
- Consumes: reg.h 的寄存器偏移常量（kernel/arch/aarch64/reg.h:27-65，全部已存在，含 GICD_ICENABLER/ISPENDR/ICPENDR/ITARGETSR/SGIR）；`dtb_gicd_base()/dtb_gicc_base()/dtb_cntp_ppi()`（kernel/arch/aarch64/dtb.c:16-19）
- Produces（后续 Task 依赖的精确签名——**唯一 API 源是 gic.h，R1-3**）：
  ```c
  /* kernel/include/arch/aarch64/gic.h */
  #ifndef OS01_AARCH64_GIC_H
  #define OS01_AARCH64_GIC_H
  #include <stdint.h>
  #include <stdbool.h>

  #define GIC_INTID_SPURIOUS   1023u
  #define GIC_INTID_SPI_FIRST  32u
  #define GIC_INTID_SPI_LAST   1019u
  #define GIC_HANDLER_MAX      1020u          /* 表容量上限（TYPER 再大也截断） */
  #define GICD_SGIR_FILTER_LIST   0u
  #define GICD_SGIR_FILTER_OTHERS 1u
  #define GICD_SGIR_FILTER_SELF   2u

  struct pt_regs;                              /* 前向声明：host 编译不进 facade */
  typedef void (*gic_handler_fn)(uint32_t intid, uint64_t param, struct pt_regs *regs);

  enum gic_irq_type { GIC_IRQ_INVALID = 0, GIC_IRQ_SGI, GIC_IRQ_PPI, GIC_IRQ_SPI };

  struct gic_dev {
      volatile uint32_t *gicd;                 /* Device-nGnRnE, identity 映射 */
      volatile uint32_t *gicc;
      uint32_t nr_intids;                      /* (TYPER.ITLinesNumber+1)*32, cap 1020 */
      volatile uint32_t dbg_last_iar;          /* 单槽 trace（hosttest 默认路径）;
                                                  内核配 set_cpu_index 后另写 per-CPU 槽 */
  };

  /* hw 层 —— 全部经 dev 指针；返回 0 成功 / 非 0 失败；不打印 */
  int  gic_dev_init(struct gic_dev *dev, volatile uint32_t *gicd, volatile uint32_t *gicc);
  void gic_dev_dist_enable(struct gic_dev *dev);    /* GICD_CTLR = 1 */
  void gic_dev_dist_disable(struct gic_dev *dev);
  void gic_dev_cpu_enable(struct gic_dev *dev);     /* PMR=0xff + GICC_CTLR=1（屏障在 wrapper）*/
  enum gic_irq_type gic_irq_type(uint32_t intid);
  int  gic_irq_config(struct gic_dev *dev, uint32_t intid, bool enable,
                      uint8_t prio, uint8_t targets); /* IGROUPR0+ISENABLER/ICENABLER
                                                       + IPRIORITYR + ITARGETSR(仅 SPI)
                                                       —— SPI 递送先决条件 (spec §2.2 R1-2) */
  int  gic_irq_enable(struct gic_dev *dev, uint32_t intid);
  int  gic_irq_disable(struct gic_dev *dev, uint32_t intid);
  uint32_t gic_ack(struct gic_dev *dev);            /* 原始 IAR（含 CPUID 位）+ 记录 dbg_last_iar */
  void gic_eoi(struct gic_dev *dev, uint32_t iar);  /* 原样写回完整 IAR (spec §2.3/D7) */
  void gic_send_sgi(struct gic_dev *dev, uint32_t sgi, uint8_t targets, uint8_t filter);
  void gic_set_pending(struct gic_dev *dev, uint32_t intid);   /* ISPENDR — 测试注入 */
  void gic_clear_pending(struct gic_dev *dev, uint32_t intid); /* ICPENDR — 测试拆除 (R1-2) */

  /* unexpected 回调注入（R1-3：声明在此，定义在 gic_driver.c） */
  void gic_driver_set_unexpected(void (*cb)(uint32_t intid));

  /* per-CPU IAR trace（R2-6：消除共享单字段的跨核覆写竞态）。
   * wrapper 注入"读本核逻辑 CPU 号"的实现（TPIDR_EL1 槽）；hosttest 注入
   * mock。gic_ack 在 hook 就位时把原始 IAR 写 trace_iar[cpu]（单槽字段照写）；
   * gic_driver_trace_get(cpu) 读指定槽。 */
  #define GIC_TRACE_CPUS 8
  void gic_driver_set_cpu_index(uint32_t (*fn)(void));
  uint32_t gic_driver_trace_get(uint32_t cpu);

  /* handler 注册表 —— 模块级全局（Phase 1 非 per-CPU, spec §4.1） */
  int  gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param,
                            const char *name);       /* 0 / -1 越界 / -2 已注册 */
  int  gic_unregister_handler(uint32_t intid);       /* 0 / -1 越界 / -3 未注册 */
  gic_handler_fn gic_get_handler(uint32_t intid, uint64_t *param_out);

  /* 通用 dispatch（trap.c 的 el1_irq 是它的三行壳） */
  void gic_dev_dispatch(struct gic_dev *dev, struct pt_regs *regs);

  /* 生产 wrapper（gic.c）—— 基址来自 DTB；gic_init/gic_cpu_init 兼容旧调用点 */
  struct gic_dev *gic_dev_current(void);
  int  gic_irq_configure(uint32_t intid, bool enable, uint8_t prio, uint8_t targets);
  void gic_force_pending(uint32_t intid);            /* 探针注入 wrapper */
  void gic_clear_pending_irq(uint32_t intid);        /* 探针拆除 wrapper (R1-2) */
  uint32_t gic_dbg_last_iar(void);                   /* R1-9: handler 上下文读取无竞态 */
  void gic_init(void);
  void gic_cpu_init(void);
  #endif
  ```

步骤：

- [ ] 写 `kernel/include/arch/aarch64/gic.h`：内容即上面的 Produces（签名逐字保持）。
- [ ] 写 `kernel/arch/aarch64/gic_driver.c` 骨架（真实代码核心；MMIO 访问一律 `dev->gicd[off/4]` 形式，**不用** reg.h:70-88 的硬编码访问器）：
  ```c
  /* GICv2 driver core — 纯逻辑, 无 UART/DTB/asm 依赖（hosttest 可编译, spec §4.1）。
   * 寄存器偏移沿用 kernel/arch/aarch64/reg.h 的值, 此处自带同值定义
   * （reg.h 的 inline 访问器是 target 专用）。 */
  #include <stdint.h>
  #include <stdbool.h>
  #include <arch/aarch64/gic.h>

  #define GICD_CTLR       0x000u
  #define GICD_TYPER      0x004u
  #define GICD_IIDR       0x008u
  #define GICD_IGROUPR    0x080u
  #define GICD_ISENABLER  0x100u
  #define GICD_ICENABLER  0x180u
  #define GICD_ISPENDR    0x200u
  #define GICD_ICPENDR    0x280u
  #define GICD_IPRIORITYR 0x400u
  #define GICD_ITARGETSR  0x800u
  #define GICD_SGIR       0xF00u
  #define GICC_CTLR       0x000u
  #define GICC_PMR        0x004u
  #define GICC_IAR        0x00Cu
  #define GICC_EOIR       0x010u

  static inline void w32(volatile uint32_t *m, uint32_t off, uint32_t v)
  { m[off / 4u] = v; }
  static inline uint32_t r32(volatile uint32_t *m, uint32_t off)
  { return m[off / 4u]; }

  int gic_dev_init(struct gic_dev *dev, volatile uint32_t *gicd, volatile uint32_t *gicc)
  {
      if (!dev || !gicd || !gicc) return -1;
      dev->gicd = gicd; dev->gicc = gicc; dev->dbg_last_iar = 0;
      if (r32(gicd, GICD_IIDR) == 0) return -1;         /* 镜像 gic.c:30-33 的门 */
      uint32_t lines = (r32(gicd, GICD_TYPER) & 0x1fu) + 1u;
      dev->nr_intids = lines * 32u;
      if (dev->nr_intids > GIC_HANDLER_MAX) dev->nr_intids = GIC_HANDLER_MAX;
      return 0;
  }

  enum gic_irq_type gic_irq_type(uint32_t intid)
  {
      if (intid <= 15u)  return GIC_IRQ_SGI;
      if (intid <= 31u)  return GIC_IRQ_PPI;
      if (intid <= GIC_INTID_SPI_LAST) return GIC_IRQ_SPI;
      return GIC_IRQ_INVALID;
  }
  ```
- [ ] 实现 enable/config 族（真实代码）：
  ```c
  static int intid_ok(const struct gic_dev *dev, uint32_t intid)
  {
      return gic_irq_type(intid) != GIC_IRQ_INVALID && intid < dev->nr_intids;
  }

  int gic_irq_config(struct gic_dev *dev, uint32_t intid, bool enable,
                     uint8_t prio, uint8_t targets)
  {
      if (!dev || !intid_ok(dev, intid)) return -1;
      volatile uint32_t *g = dev->gicd;
      /* 组 0（secure；QEMU 无 EL3, 沿用 gic.c:16 的清位做法） */
      uint32_t grp_off = GICD_IGROUPR + (intid / 32u) * 4u;
      w32(g, grp_off, r32(g, grp_off) & ~(1u << (intid % 32u)));
      /* 优先级字节（SGI/PPI 的 banked 域同样可写） */
      uint32_t pr_off = GICD_IPRIORITYR + (intid / 4u) * 4u;
      uint32_t shift = (intid % 4u) * 8u;
      w32(g, pr_off, (r32(g, pr_off) & ~(0xffu << shift)) | ((uint32_t)prio << shift));
      /* SPI 路由；SGI/PPI 的 ITARGETSR 只读 banked, 不写。
       * R1-2: SPI 递送先决条件 = enable(ITARGETSR 后的 ISENABLER) + route。 */
      if (gic_irq_type(intid) == GIC_IRQ_SPI) {
          uint32_t tg_off = GICD_ITARGETSR + (intid / 4u) * 4u;
          w32(g, tg_off, (r32(g, tg_off) & ~(0xffu << shift))
                         | ((uint32_t)targets << shift));
      }
      if (enable) w32(g, GICD_ISENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
      else        w32(g, GICD_ICENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
      return 0;
  }

  int gic_irq_enable(struct gic_dev *dev, uint32_t intid)
  { return gic_irq_config(dev, intid, true, 0x00, 0x01); }

  int gic_irq_disable(struct gic_dev *dev, uint32_t intid)
  {
      if (!dev || !intid_ok(dev, intid)) return -1;
      w32(dev->gicd, GICD_ICENABLER + (intid / 32u) * 4u, 1u << (intid % 32u));
      return 0;
  }

  void gic_dev_cpu_enable(struct gic_dev *dev)
  {
      w32(dev->gicc, GICC_PMR, 0xffu);
      w32(dev->gicc, GICC_CTLR, 1u);
  }
  ```
  注意：`dsb sy; isb` 屏障**留在生产 wrapper** `gic_cpu_init()`（gic.c:24 原位）——
  driver 内的 inline asm 会让 host 编译失败（R1-7 之外的既有约束：hw 层零依赖）。
  hosttest 的 `suite_cpu_iface` 只断言 PMR/CTLR 两个 mock 值，与 Task 1.1 一致。
- [ ] 实现 ack/eoi/sgi/pending（真实代码）：
  ```c
  /* per-CPU IAR trace (R2-6)：跨核 ack 只写本 CPU 槽——BSP 在 handler 上下文
   * 读到的必是本核最近一次 ack（同核 dispatch 串行 + IRQ masked，无嵌套），
   * AP 的 ack 写各自槽，互不覆写。hosttest 用 mock hook 覆盖。 */
  static uint32_t (*cpu_index_fn)(void);
  static volatile uint32_t trace_iar[GIC_TRACE_CPUS];

  void gic_driver_set_cpu_index(uint32_t (*fn)(void))
  { cpu_index_fn = fn; }

  uint32_t gic_driver_trace_get(uint32_t cpu)
  { return cpu < GIC_TRACE_CPUS ? trace_iar[cpu] : 0u; }

  uint32_t gic_ack(struct gic_dev *dev)
  {
      uint32_t iar = r32(dev->gicc, GICC_IAR);
      dev->dbg_last_iar = iar;                /* 单槽 trace（hosttest 默认路径） */
      if (cpu_index_fn) {
          uint32_t c = cpu_index_fn();
          if (c < GIC_TRACE_CPUS) trace_iar[c] = iar;
      }
      return iar;
  }

  void gic_eoi(struct gic_dev *dev, uint32_t iar)
  { w32(dev->gicc, GICC_EOIR, iar); }        /* 完整 IAR 原样写回 — D7 修复 */

  void gic_send_sgi(struct gic_dev *dev, uint32_t sgi, uint8_t targets, uint8_t filter)
  {
      w32(dev->gicd, GICD_SGIR,
          ((uint32_t)(filter & 3u) << 24) | ((uint32_t)targets << 16) | (sgi & 0xfu));
  }

  void gic_set_pending(struct gic_dev *dev, uint32_t intid)
  { w32(dev->gicd, GICD_ISPENDR + (intid / 32u) * 4u, 1u << (intid % 32u)); }

  void gic_clear_pending(struct gic_dev *dev, uint32_t intid)
  { w32(dev->gicd, GICD_ICPENDR + (intid / 32u) * 4u, 1u << (intid % 32u)); }
  ```
- [ ] 实现 handler 表 + unexpected 回调 + dispatch（真实代码）：
  ```c
  struct gic_handler_slot {
      gic_handler_fn fn;
      uint64_t param;
      const char *name;
  };
  static struct gic_handler_slot handlers[GIC_HANDLER_MAX];

  static void (*unexpected_cb)(uint32_t) = 0;

  void gic_driver_set_unexpected(void (*cb)(uint32_t intid))   /* R1-3: 定义在此 */
  { unexpected_cb = cb; }

  int gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param,
                           const char *name)
  {
      if (gic_irq_type(intid) == GIC_IRQ_INVALID || intid >= GIC_HANDLER_MAX || !fn)
          return -1;
      if (handlers[intid].fn) return -2;
      handlers[intid].fn = fn; handlers[intid].param = param; handlers[intid].name = name;
      return 0;
  }

  int gic_unregister_handler(uint32_t intid)
  {
      if (gic_irq_type(intid) == GIC_IRQ_INVALID || intid >= GIC_HANDLER_MAX) return -1;
      if (!handlers[intid].fn) return -3;
      handlers[intid].fn = 0; return 0;
  }

  gic_handler_fn gic_get_handler(uint32_t intid, uint64_t *param_out)
  {
      if (gic_irq_type(intid) == GIC_IRQ_INVALID || intid >= GIC_HANDLER_MAX)
          return 0;
      if (param_out) *param_out = handlers[intid].param;
      return handlers[intid].fn;
  }

  void gic_dev_dispatch(struct gic_dev *dev, struct pt_regs *regs)
  {
      uint32_t iar = gic_ack(dev);
      uint32_t intid = iar & 0x3ffu;
      if (intid == GIC_INTID_SPURIOUS) return;          /* 绝不写 EOIR */
      uint64_t param = 0;
      gic_handler_fn fn = gic_get_handler(intid, &param);
      if (fn) fn(intid, param, regs);                   /* 设备清源在 EOI 前 */
      else if (unexpected_cb) unexpected_cb(intid);     /* 对齐 time.c:105-115 语义 */
      gic_eoi(dev, iar);                                /* 完整 IAR 回写 (D7) */
  }
  ```
- [ ] 重写 `kernel/arch/aarch64/gic.c` 为生产 wrapper（真实代码全文）：
  ```c
  /* 生产 wrapper：driver 核心(gic_driver.c) + DTB 基址 + PL011 日志。
   * gic_init/gic_cpu_init 对外签名不变（main.c:247 / smp.c:208 调用点零改动）。 */
  #include <stdint.h>
  #include <arch/aarch64/boot_log.h>
  #include <arch/aarch64/dtb.h>
  #include <arch/aarch64/smp.h>
  #include <arch/aarch64/gic.h>
  #include "aarch64_percpu.h"

  static struct gic_dev g_gic;
  struct gic_dev *gic_dev_current(void) { return &g_gic; }

  static void log_unexpected(uint32_t intid)            /* driver 回调 → PL011 */
  {
      kputs("[gic] unexpected IRQ intid=");
      kputu(intid);
      kputs("\n");
  }

  static uint32_t k_cpu_index(void)                      /* R2-6: TPIDR 槽读本核号 */
  {
      uint64_t slot;
      __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(slot));
      return ((volatile aarch64_boot_percpu_t *)(uintptr_t)slot)->cpu_id;
  }

  void gic_cpu_init(void)                                /* 每核各跑一次（banked） */
  {
      gic_dev_cpu_enable(&g_gic);
      /* banked SGI/PPI 白名单：SGI 0（IPI 主载荷）+ SGI 1（R1-9 回发确认）
       * + SGI 2（clobber 探针, R2-4）+ CNTP PPI（dtb）。
       * R5: 白名单外的 banked enable 位保持复位 0。 */
      (void)gic_irq_config(&g_gic, 0, true, 0x00, 0x00);
      (void)gic_irq_config(&g_gic, 1, true, 0x00, 0x00);
      (void)gic_irq_config(&g_gic, 2, true, 0x00, 0x00);
      uint32_t cntp = dtb_cntp_ppi();
      (void)gic_irq_config(&g_gic, cntp, true, 0x00, 0x00);
      __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");   /* 屏障在 wrapper (hw 层零依赖) */
  }

  void gic_init(void)
  {
      if (gic_dev_init(&g_gic, (volatile uint32_t *)dtb_gicd_base(),
                       (volatile uint32_t *)dtb_gicc_base()) != 0) {
          log_err("[gic] FATAL: GICD IIDR=0\n");
          for (;;) __asm__ __volatile__("wfi" ::: "memory");
      }
      gic_driver_set_unexpected(log_unexpected);        /* R1-3 */
      gic_driver_set_cpu_index(k_cpu_index);            /* R2-6: per-CPU IAR trace */
      gic_dev_dist_enable(&g_gic);
      gic_cpu_init();
      /* R2-3: intids marker 独占一行（harness 的 --expect-gic 全行 regex
       * 要求 `^\[gic\] GICv2 driver: intids=\d+$`），CPU interface 另起一行。 */
      log_info("[gic] GICv2 driver: intids=");
      kputu(g_gic.nr_intids);
      log_info("\n[gic] CPU interface @ 0x");
      kputx(dtb_gicc_base());
      log_info("\n");
  }

  int gic_irq_configure(uint32_t intid, bool enable, uint8_t prio, uint8_t targets)
  { return gic_irq_config(&g_gic, intid, enable, prio, targets); }

  void gic_force_pending(uint32_t intid)
  { gic_set_pending(&g_gic, intid); }

  void gic_clear_pending_irq(uint32_t intid)
  { gic_clear_pending(&g_gic, intid); }

  uint32_t gic_dbg_last_iar(void)
  { return gic_driver_trace_get(k_cpu_index()); }       /* R2-6: 本 CPU 槽 */
  ```
  与旧行为的差异（有意）：gic_cpu_init 现在还 banked 使能 SGI 0/1/2（Task 2.2 探针与
  Task 3 IPI 的前置，无害——SGI 只有软件写 SGIR 才会来）；打印新增
  `[gic] GICv2 driver: intids=N` **独占一行**（harness --expect-gic 的 marker，
  spec §7.2；CPU interface 地址另起一行，R2-3）。
- [ ] **跑 GREEN（hosttest）**：
  ```sh
  make -C hosttests PROFILE=aarch64-clang \
       OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver
  ```
  预期：全部 suite 通过，exit 0，末行 `test_gic_driver: N total, N passed, 0 failed`。
  （Task 1.1 的 RED 在此刻转绿——同一份测试文件零改动。）
- [ ] **跑内核冒烟**（确认 wrapper 无回归；不需要手动拼 QEMU 命令，直接用标准回归——
  它内部完成 KERNEL_SELFTEST=1 构建与 DTB 生成）：
  ```sh
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：9/9 case PASS（`[gic] GICv2 driver: intids=` 新行出现且不影响既有断言——
  kernel_failure 只匹配 `[smp|spinlock]` 前缀的 FATAL/PANIC/FAIL/DEGRADED，
  aarch64_uefi_smp.py:230-236，新行不误伤）。此刻 time.c 仍在用 reg.h 访问器
  gicc_read32/gicc_write32（旧 dispatch 路径）——没冲突，Task 2.2 才切。
- [ ] commit（含 Task 1.1 的测试）：
  ```sh
  git add hosttests/cases/test_gic_driver.c hosttests/Makefile \
          kernel/include/arch/aarch64/gic.h kernel/arch/aarch64/gic_driver.c \
          kernel/arch/aarch64/gic.c kernel/include/arch/aarch64/smp.h
  git commit -m "feat(aarch64): GICv2 driver 泛化 + mock-MMIO hosttest

  - gic_driver.c: 指针式 MMIO(struct gic_dev) + SGI/PPI/SPI/invalid 分类
    + enable/disable/priority/targets + handler 注册表 + gic_dev_dispatch
    (IAR→查表→handler→EOIR 原样回写完整 IAR, 修 D7 CPUID 位丢失)
    + gic_driver_set_unexpected 回调注入 + SGIR 编码
    + ISPENDR/ICPENDR 注入与拆除 + per-CPU IAR trace
    (set_cpu_index/trace_get, R2-6 消除跨核覆写竞态);
    零依赖可 host 编译(屏障留 wrapper)
  - gic.c 重写为生产 wrapper: DTB 基址 + PL011 日志 + unexpected 回调注入
    + TPIDR cpu-index hook; gic_init/gic_cpu_init 签名不变(main.c/smp.c 零改动);
    gic_cpu_init 额外 banked 使能 SGI 0/1/2(Task 2.2 探针/Task 3 前置);
    intids marker 独占一行(R2-3)
  - hosttests/test_gic_driver: mock MMIO 覆盖 init(TYPER/IIDR)/分类/
    enable(ICENABLER)/prio/targets(偏移 +32=BASE+(33/4)*4, R2-2)/注册表/
    unexpected 回调注入清除/SGIR 三种 filter/set+clear pending/EOIR 往返/
    dispatch 三分支 + dispatch-CPUID case(IAR=0xC07 → EOIR mock==0xC07, R1-9)
    + per-CPU trace 槽隔离(mock cpu-index hook, R2-6)

  Co-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

**验证命令（本 Task 全量）**：
```sh
make -C hosttests PROFILE=aarch64-clang OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver
make PROFILE=aarch64-clang test-aarch64-uefi-smp
```

---

### Task 2.1: RED — QEMU 中断冒烟扩展（--expect-gic + 破坏性探针断言）

**Files:**
- Modify: `qemutests/aarch64_uefi_smp.py`（578 行；新增 `--expect-gic` flag；断言函数 + self_test fixtures；改动点：argparse ~550 行区、self_test() ~99-227 区、passed() ~263-348 区）

**Interfaces:**
- Consumes: 既有 `passed()/run_case()/main()` 结构（aarch64_uefi_smp.py:263/442/539）
- Produces: `--expect-gic` flag（Task 2.2 把它加进 run.mk:167 的标准 target；Task 3.1 在同一 flag 下追加 IPI 断言）；断言的 marker 行（kernel 侧由 Task 2.2 产出）：
  - `[gic] GICv2 driver: intids=` 前缀行恰一条
  - `[gic] dispatch ready` 恰一条
  - `[gic-probe] save-restore OK` 恰一条，且 `[gic-probe] ...FAIL` 行出现即拒绝
  - `[gic-probe] unexpected intid=40 survived` 恰一条
- **本 Task 不改任何 kernel 侧文件**——RED 就是对未改内核跑新断言。

- [ ] 在 `hard_kernel_failure`（~line 261）之后新增断言函数（真实代码）：
  ```python
  def gic_evidence_ok(text: str, cpus: int) -> bool:
      """--expect-gic: GICv2 框架证据（spec §7.2）。
      marker 恰一条（多打/漏打都拒）；clobber FAIL 行出现即拒；
      unexpected 探针超时行出现即拒。"""
      text = text.replace("\r", "")
      if re.search(r"^\[gic-probe\][^\n]*\bFAIL\b", text, re.MULTILINE):
          return False
      if re.search(r"^\[gic-probe\][^\n]*TIMEOUT", text, re.MULTILINE):
          return False
      checks = [
          (r"^\[gic\] GICv2 driver: intids=\d+$", 1),
          (r"^\[gic\] dispatch ready$", 1),
          (r"^\[gic-probe\] save-restore OK$", 1),
          (r"^\[gic-probe\] unexpected intid=40 survived$", 1),
      ]
      for pattern, want in checks:
          found = re.findall(pattern, text, re.MULTILINE)
          if len(found) != want:
              print(f"FAIL: gic evidence {pattern!r} found {len(found)}, want {want}")
              return False
      return True
  ```
  （`cpus` 参数在本 Task 暂未使用——Task 3.1 在同一函数追加 IPI per-cpu 断言时消费它；
  先带参数定形，避免 Task 3.1 再改调用链。）
- [ ] `passed()` 签名加 `expect_gic: bool = False`（镜像 expect_selftest，line 263），函数体在 `ram_summary_ok` 检查后追加：
  ```python
      if expect_gic and not gic_evidence_ok(text, cpus):
          return False
  ```
- [ ] `acceptance_evidence()`（line 382-384）透传（no-ack case 不要求 gic 证据）：
  ```python
  def acceptance_evidence(args, text, cpus):
      expect_selftest = getattr(args, "expect_selftest", False)
      expect_gic = getattr(args, "expect_gic", False)
      if args.expect_no_ack is not None:
          return degraded_passed(text, expect_selftest=expect_selftest)
      return passed(text, cpus, expect_selftest=expect_selftest, expect_gic=expect_gic)
  ```
- [ ] `main()` argparse 追加（--expect-selftest 旁，~line 550）：
  ```python
  parser.add_argument("--expect-gic", action="store_true",
                      help="Require GICv2 framework markers: driver init, "
                           "dispatch ready, save-restore probe OK, "
                           "unexpected-intid survival")
  ```
- [ ] `self_test()` 追加 fixtures（真实代码；放在 expect_selftest 块之后）：
  ```python
      # --expect-gic: 四条 marker 恰一条; FAIL/TIMEOUT 行拒; 漏任一拒。
      gic_markers = [
          "[gic] GICv2 driver: intids=96\n",
          "[gic] dispatch ready\n",
          "[gic-probe] save-restore OK\n",
          "[gic-probe] unexpected intid=40 survived\n",
      ]
      gic_log = current_log_for_2_cpus + "".join(gic_markers)
      assert passed(gic_log, cpus=2, expect_gic=True), "all gic markers must pass"
      for marker in gic_markers:
          assert not passed(current_log_for_2_cpus + marker, cpus=2, expect_gic=True), \
              f"missing {marker.strip()} must reject"
      assert not passed(gic_log.replace("save-restore OK", "save-restore FAIL"),
                        cpus=2, expect_gic=True), "clobber FAIL must reject"
      assert not passed(gic_log.replace("intid=40 survived", "intid=40 TIMEOUT"),
                        cpus=2, expect_gic=True), "probe TIMEOUT must reject"
      assert not passed(gic_log + "[gic] dispatch ready\n", cpus=2, expect_gic=True), \
          "duplicate marker must reject"
      assert passed(gic_log, cpus=2), "expect_gic default-off keeps legacy behavior"
  ```
- [ ] **跑 harness 自测（应绿）**：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test
  ```
  预期输出：`aarch64_uefi_smp: self-test passed`。
- [ ] **跑 RED（对未改内核）**：
  ```sh
  make PROFILE=aarch64-clang aarch64-uefi   # 或确认 build/aarch64-clang/image/ 已是最新
  python3 qemutests/aarch64_uefi_smp.py --cpus 2 --repeat 1 --timeout 90 \
    --expect-selftest --expect-gic --diagnostic-dtb=auto \
    --firmware build/aarch64-clang/image/QEMU_EFI.fd \
    --image build/aarch64-clang/image/aarch64-uefi.img \
    --qemu qemu-system-aarch64 \
    --log-dir /tmp/gic-phase1-red-$$
  ```
  预期：**exit 1**，case 输出 `"result": "FAIL"`；stdout.log 里没有
  `[gic] dispatch ready` / `[gic-probe] ...` 行。此刻 Task 1.2 已并入
  （`[gic] GICv2 driver: intids=` 行应已存在），失败点应落在 dispatch/probe 三条
  marker——把 `grep -ac 'gic' /tmp/gic-phase1-red-*/cpus-2-run-1.stdout.log` 的结果
  记进 RED 证据。**不 commit**（与 Task 2.2/2.3 同 commit）。

---

### Task 2.2: GREEN — entry.S 全量 save/restore + pt_regs_t + 通用 dispatch（trap.c）

**Files:**
- Modify: `kernel/include/arch/aarch64/regs.h:19-28`（pt_regs_t 加 x30 + Section 2 偏移常量）
- Modify: `kernel/arch/aarch64/entry.S`（EL1h IRQ 槽 61-79 改 `b el1_irq_entry`；表后新增 el1_irq_entry；头注释更新）
- Modify: `kernel/arch/aarch64/trap.c`（el1_irq 三行壳 + arch_install_exception_vectors 保持 no-op）
- Modify: `kernel/arch/aarch64/time.c`（删硬编码比较；tick 改注册 handler；**RED 阶段保留 `el1_irq_dispatch` 过渡 shim**，GREEN 同一变更内删除；`g_ticks` 保持 static——探针已不依赖它，v2 的导出要求撤销）
- Create: `kernel/arch/aarch64/irq_probe.c`（clobber 探针 + unexpected-intid 探针，`#if OS01_SELFTEST`）
- Modify: `kernel/arch/aarch64/main.c`（SELFTEST 块里调探针；注册 tick handler 后打 `[gic] dispatch ready`）
- Modify: `mk/components/run.mk:167`（test-aarch64-uefi-smp 的 python 参数追加 `--expect-gic`）

**Interfaces:**
- Consumes: Task 1.2 的 `gic_dev_current()/gic_dev_dispatch()/gic_register_handler()/gic_irq_configure()/gic_force_pending()/gic_clear_pending_irq()`；`arch_local_irq_enable/disable`（arch/irq.h:88-96）
- Produces（Task 2.3b/3.2 依赖）：
  ```c
  /* trap.c —— entry.S 的 el1_irq_entry bl 到这里 */
  void el1_irq(struct pt_regs *regs);
  /* gic.c（Task 1.2 已产出，此处被 shim/dispatch 消费） */
  struct gic_dev *gic_dev_current(void);
  /* time.c —— harness 经 [tick] 行观察（g_ticks 保持 static, 无导出） */
  bool arch_tick_start(void);            /* 内部注册 cntp handler（签名不变） */
  /* irq_probe.c（OS01_SELFTEST 门控） */
  void gic_clobber_probe(void);          /* 自发 SGI 2 确定性 trampoline (R2-4) */
  void gic_unexpected_probe(void);       /* SPI 40 enable→注入→观察恰一次→拆除 (R1-2/R2-5) */
  ```

步骤（**shim 保链接**：先让新 dispatch 在旧入口下可运行并跑出探针 RED，再换 entry.S 转绿，R1-1）：

- [ ] 改 `kernel/include/arch/aarch64/regs.h`：pt_regs_t 按 spec §5.2 精确布局重写（x0..x30, sp_el0, elr_el1, spsr_el1；sizeof==272），并加 `_Static_assert(sizeof(pt_regs_t) == 34 * 8, "pt_regs layout");`；Section 2（`#endif /* !__ASSEMBLER__ */` 之后）追加：
  ```c
  // ── pt_regs_t 偏移（entry.S 与 C 共享；字段序 = 入栈序, spec §5.2）──
  #define PT_REGS_X0         (0  * 8)
  #define PT_REGS_X2         (2  * 8)
  #define PT_REGS_X4         (4  * 8)
  #define PT_REGS_X6         (6  * 8)
  #define PT_REGS_X8         (8  * 8)
  #define PT_REGS_X10        (10 * 8)
  #define PT_REGS_X12        (12 * 8)
  #define PT_REGS_X14        (14 * 8)
  #define PT_REGS_X16        (16 * 8)
  #define PT_REGS_X18        (18 * 8)
  #define PT_REGS_X20        (20 * 8)
  #define PT_REGS_X22        (22 * 8)
  #define PT_REGS_X24        (24 * 8)
  #define PT_REGS_X26        (26 * 8)
  #define PT_REGS_X28        (28 * 8)
  #define PT_REGS_X30        (30 * 8)
  #define PT_REGS_SP_EL0     (31 * 8)
  #define PT_REGS_ELR_EL1    (32 * 8)
  #define PT_REGS_SPSR_EL1   (33 * 8)
  #define PT_REGS_SIZE       (34 * 8)
  ```
  entry.S 需要能 include 它：在文件头加 `#include <arch/aarch64/regs.h>`
  （kernel/Makefile .S 规则走 C 预处理器，kernel/Makefile:222-224，-Iinclude 已有，
  kernel/Makefile:92）。regs.h 的 C 段被 `#ifndef __ASSEMBLER__` 挡住，.S 只见
  Section 2 宏——安全。
- [ ] 写 `kernel/arch/aarch64/irq_probe.c`（真实代码全文；`#if OS01_SELFTEST` 门控）：
  ```c
  /* 破坏性探针（spec §7.3）——证明 entry.S save/restore 真实生效。
   *
   * clobber 探针 = 确定性 trampoline（R2-4）：
   *   - 触发：asm 内直写 GICD_SGIR(filter=SELF, sgi=2) 自发一次【真实 IRQ】
   *     （SGI 2 已由 gic_cpu_init banked 使能；探针运行时 IRQ 已 unmask）。
   *   - 判据的确定性来源：dispatch 链以 fn(intid, param, regs) 调用 handler，
   *     AAPCS64 规定前三个实参必须经 x0/x1/x2 传递——旧路径（槽 6:
   *     bl el1_irq_dispatch; eret，零保存）下 x0-x2 的哨兵【架构性必然】
   *     被销毁，与编译器寄存器分配无关；x3-x5/x18 是加宽覆盖。新路径
   *     （el1_irq_entry）先保存 x0-x30 再 bl，eret 前恢复——全部存活。
   *   - 可靠性：探针自身的轮询状态（flag 地址/SGIR 地址/deadline）全部驻
   *     内存（adrp 重取），IRQ 在 RED 路径上 clobber 掉任何 caller-saved
   *     寄存器都不影响探针循环；结果经 mov %0 显式回写输出操作数
   *     （0=OK / 1=clobber / 2=超时）。
   *
   * unexpected 探针（R1-2 + R2-5）：SPI 递送需 enable+route（spec §2.2）；
   * 递送观察不再依赖 g_ticks（CNTP tick 自己会来，构不成证据），而是探针
   * 注入的 unexpected 回调对 intid 40 计数并 release 置 flag——轮询到
   * flag 且 count==1 才打 survived。回调打印与 wrapper 的 log_unexpected
   * 同文案，替换后日志不变。 */
  #if OS01_SELFTEST
  #include <stdint.h>
  #include <stdbool.h>
  #include <arch/cpu.h>
  #include <arch/aarch64/boot_log.h>
  #include <arch/aarch64/gic.h>

  #define PROBE_SGI_ID           2u      /* clobber 探针专用自发 SGI */
  #define PROBE_UNEXPECTED_INTID 40u

  static volatile uint32_t probe_sgi_seen;         /* stlr 置位 / ldar 轮询 */
  static volatile uint32_t probe_unexpected_flag;
  static volatile uint32_t probe_unexpected_count;
  static volatile uint64_t probe_sgir_addr;        /* C 预计算, asm 经 adrp 重取 */
  static volatile uint64_t probe_deadline;

  static inline void probe_flag_release(volatile uint32_t *p, uint32_t v)
  {
      __asm__ __volatile__("stlr %w0, [%1]" :: "r"(v), "r"(p) : "memory");
  }
  static inline uint32_t probe_flag_acquire(const volatile uint32_t *p)
  {
      uint32_t v;
      __asm__ __volatile__("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
      return v;
  }

  static void probe_sgi_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
  {
      (void)intid; (void)param; (void)regs;
      probe_flag_release(&probe_sgi_seen, 1);
  }

  static void probe_unexpected_cb(uint32_t intid)
  {
      kputs("[gic] unexpected IRQ intid=");        /* 与 log_unexpected 同文案 */
      kputu(intid);
      kputs("\n");
      if (intid == PROBE_UNEXPECTED_INTID) {
          probe_unexpected_count += 1;
          probe_flag_release(&probe_unexpected_flag, 1);
      }
  }

  void gic_clobber_probe(void)
  {
      int rc = gic_register_handler(PROBE_SGI_ID, probe_sgi_handler, 0, "probe-sgi");
      if (rc != 0 && rc != -2) {                   /* -2 = 已注册, 重复探针沿用 */
          kputs("[gic-probe] save-restore FAIL regs=handler-register\n");
          return;
      }
      probe_sgi_seen = 0;
      probe_sgir_addr = (uint64_t)(uintptr_t)gic_dev_current()->gicd + 0xF00u;
      probe_deadline  = arch_cycle_counter() + arch_cycle_freq() * 2;
      uint64_t bad = 2;
      __asm__ __volatile__(
          "mov  x0, #0x1111\n\t"
          "mov  x1, #0x2222\n\t"
          "mov  x2, #0x3333\n\t"
          "mov  x3, #0x4444\n\t"
          "mov  x4, #0x5555\n\t"
          "mov  x5, #0x6666\n\t"
          "mov  x18, #0x7777\n\t"
          "dsb  sy\n\t"
          "adrp x9, probe_sgir_addr\n\t"
          "ldr  x9, [x9, :lo12:probe_sgir_addr]\n\t"
          "mov  x10, #2\n\t"
          "movk x10, #2, lsl #24\n\t"              /* 0x02000002: SELF + SGI 2 */
          "str  w10, [x9]\n\t"                     /* ← 自发 IRQ, 立即可入 */
          "1: adrp x9, probe_sgi_seen\n\t"
          "add  x9, x9, :lo12:probe_sgi_seen\n\t"
          "ldar w11, [x9]\n\t"
          "cbnz w11, 4f\n\t"
          "adrp x10, probe_deadline\n\t"
          "ldr  x10, [x10, :lo12:probe_deadline]\n\t"
          "mrs  x11, cntvct_el0\n\t"
          "cmp  x11, x10\n\t"
          "b.lo 1b\n\t"
          "mov  %0, #2\n\t"                        /* 超时: IRQ 未递送/未处理 */
          "b    3f\n"
          "4: cmp  x0, #0x1111\n\t b.ne 2f\n\t"
          "cmp  x1, #0x2222\n\t b.ne 2f\n\t"
          "cmp  x2, #0x3333\n\t b.ne 2f\n\t"
          "cmp  x3, #0x4444\n\t b.ne 2f\n\t"
          "cmp  x4, #0x5555\n\t b.ne 2f\n\t"
          "cmp  x5, #0x6666\n\t b.ne 2f\n\t"
          "cmp  x18, #0x7777\n\t b.ne 2f\n\t"
          "mov  %0, #0\n\t"
          "b    3f\n"
          "2: mov  %0, #1\n"
          "3:"
          : "=r"(bad)
          :
          : "x0", "x1", "x2", "x3", "x4", "x5", "x9", "x10", "x11", "x18",
            "cc", "memory");
      if (bad == 0)      kputs("[gic-probe] save-restore OK\n");
      else if (bad == 1) kputs("[gic-probe] save-restore FAIL regs=x0-x5,x18\n");
      else               kputs("[gic-probe] save-restore TIMEOUT\n");
  }

  void gic_unexpected_probe(void)
  {
      probe_unexpected_count = 0;
      probe_unexpected_flag = 0;
      gic_driver_set_unexpected(probe_unexpected_cb);   /* R2-5: 观察递送本身 */
      if (gic_irq_configure(PROBE_UNEXPECTED_INTID, true, 0x00, 0x01) != 0) {
          kputs("[gic-probe] unexpected intid=40 TIMEOUT\n");
          return;
      }
      gic_force_pending(PROBE_UNEXPECTED_INTID);
      uint64_t deadline = arch_cycle_counter() + arch_cycle_freq() * 2;
      while (probe_flag_acquire(&probe_unexpected_flag) == 0) {  /* 等递送证据 */
          if ((uint64_t)arch_cycle_counter() > deadline) {
              kputs("[gic-probe] unexpected intid=40 TIMEOUT\n");
              gic_irq_configure(PROBE_UNEXPECTED_INTID, false, 0x00, 0x01);
              gic_clear_pending_irq(PROBE_UNEXPECTED_INTID);
              return;
          }
          arch_cpu_pause();
      }
      gic_irq_configure(PROBE_UNEXPECTED_INTID, false, 0x00, 0x01);   /* 拆除 */
      gic_clear_pending_irq(PROBE_UNEXPECTED_INTID);
      if (probe_unexpected_count == 1)
          kputs("[gic-probe] unexpected intid=40 survived\n");       /* 恰一次 */
      else
          kputs("[gic-probe] unexpected intid=40 FAIL count!=1\n");
  }
  #endif
  ```
- [ ] 改 `kernel/arch/aarch64/time.c`：
  - `static volatile uint64_t g_ticks`（line 39）**保持 static 不动**（探针已改用自发
    SGI/递送观察，不再依赖它；v2 的"去 static 导出"要求撤销，避免死接口）。
  - 删除 `el1_irq_dispatch()` 的硬编码比较逻辑（96-133），新增注册式 handler +
    **过渡 shim**（真实代码）：
    ```c
    #include <arch/aarch64/gic.h>
    #include <arch/aarch64/dtb.h>

    /* 注册进 GIC handler 表的 tick ISR。顺序契约（phase1 spec §2.3）：
     * TVAL 重装仍在最前（避免丢 tick）；EOI 统一移交 dispatch 在返回后执行
     * （只延长该 INTID 的 active 窗口，同优先级不嵌套本来就掩着，行为等价）。 */
    static void cntp_tick_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
    {
        (void)intid; (void)param; (void)regs;
        cntp_tval_el0_write(g_period);
        uint64_t t = g_ticks + 1;
        g_ticks = t;
        if ((t % TICKS_PER_SECOND) == 0) {
            kputs("[tick] ");
            kputu(t / TICKS_PER_SECOND);
            kputs("\n");
        }
    }

    /* R1-1 过渡 shim：旧 entry.S:78（槽 6）仍 bl el1_irq_dispatch。在 entry.S
     * 替换为 b el1_irq_entry 的同一变更内删除本函数——不允许存在
     * "符号已删、入口未换"的链接断裂中间态。regs 传 NULL：旧入口本就
     * 不保存任何寄存器，语义等价。 */
    int el1_irq_dispatch(void)
    {
        gic_dev_dispatch(gic_dev_current(), (struct pt_regs *)0);
        return 1;
    }
    ```
  - `arch_tick_start()` 尾部（return true 前）注册：
    ```c
        if (gic_register_handler(dtb_cntp_ppi(), cntp_tick_handler, 0,
                                 "cntp-tick") != 0)
            return false;
    ```
- [ ] 改 `kernel/arch/aarch64/trap.c`（真实代码核心；此刻尚无调用者——entry.S 仍走 shim，但符号独立存在无链接问题）：
  ```c
  #include <stdint.h>
  #include <arch/regs.h>
  #include <arch/aarch64/gic.h>
  #include <arch/aarch64/boot_log.h>

  /* entry.S el1_irq_entry 的 C 落点：全量保存的 pt_regs + driver dispatch。 */
  void el1_irq(struct pt_regs *regs)
  {
      gic_dev_dispatch(gic_dev_current(), regs);
  }

  void arch_install_exception_vectors(void) { /* 仍 no-op: VBAR 在 head.S/main.c */ }
  ```
- [ ] **此刻先不改 entry.S**——把 main.c 探针接上并构建，跑出 clobber RED：
  - main.c 在 `arch_local_irq_enable()`（main.c:261）与最终 halt 循环（263）之间插入：
    ```c
    #if OS01_SELFTEST
        kputs("[gic] dispatch ready\n");
        gic_clobber_probe();
        gic_unexpected_probe();
    #endif
    ```
    （`[gic] dispatch ready` 打点放这里：VBAR 已装(188-189)、handler 已注册
    （arch_tick_start 内）、dispatch 链闭合。探针在 dispatch ready 之后串行执行。）
  - 构建 + 跑（构建与 DTB 由标准回归完成；只跑 1 case 快速抓 RED）：
    ```sh
    make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi
    python3 qemutests/aarch64_uefi_smp.py --cpus 1 --repeat 1 --timeout 90 \
      --expect-selftest --diagnostic-dtb=auto \
      --firmware build/aarch64-clang/image/QEMU_EFI.fd \
      --image build/aarch64-clang/image/aarch64-uefi.img \
      --qemu qemu-system-aarch64 --log-dir /tmp/gic-clobber-red-$$ ; \
    grep -a "gic" /tmp/gic-clobber-red-*/cpus-1-run-1.stdout.log
    ```
    **预期（RED 证据，旧 entry.S 零保存 + shim 保链接）**：
    `[gic] GICv2 driver: intids=...`（独占一行，R2-3）/ `[gic] dispatch ready` /
    `[gic-probe] save-restore FAIL regs=x0-x5,x18` /
    `[gic-probe] unexpected intid=40 survived`（unexpected 探针走 shim 的
    dispatch，观察递送后应已绿）。
    **save-restore FAIL 是确定性判据**（R2-4）：dispatch 链以
    `fn(intid, param, regs)` 调 handler，AAPCS64 强制实参走 x0/x1/x2——
    旧路径零保存下 x0-x2 哨兵必然被销毁，与编译器寄存器分配无关。
    若日志出现 `save-restore OK` 或 `TIMEOUT` 而非 `FAIL`，说明探针自身
    缺陷（触发/轮询路径坏了），停下修复重跑——不允许跳过。
- [ ] **改 entry.S 并同变更删 shim**（关键 GREEN 步骤，R1-1）：
  - 文件头（line 33 后）加 `#include <arch/aarch64/regs.h>`。
  - EL1h IRQ 槽（61-79）替换为：
    ```asm
    /* ───── IRQ, current SP_ELx (this is what kernel uses at EL1h) ─────
     * 槽内只放一条跳转（≤0x80 字节约束）；save/restore 在表后的
     * el1_irq_entry（spec §4.2/§5）。全量 31 GPR + sp_el0 + elr + spsr，
     * 布局 = pt_regs_t（kernel/include/arch/aarch64/regs.h）。 */
    .balign 0x80
    6:
        b   el1_irq_entry
    ```
  - 表尾（16 号槽之后、文件末尾）追加（真实代码全文）：
    ```asm
    /* ──────────────────────────────────────────────────────────────
     * el1_irq_entry — EL1h IRQ 通用入口（spec §4.2）
     * 入栈序 = pt_regs_t 字段序 = regs.h PT_REGS_* 偏移。
     * ────────────────────────────────────────────────────────────── */
    .balign 0x80
    .globl el1_irq_entry
    el1_irq_entry:
        sub     sp, sp, #PT_REGS_SIZE
        stp     x0, x1,   [sp, #PT_REGS_X0]
        stp     x2, x3,   [sp, #PT_REGS_X2]
        stp     x4, x5,   [sp, #PT_REGS_X4]
        stp     x6, x7,   [sp, #PT_REGS_X6]
        stp     x8, x9,   [sp, #PT_REGS_X8]
        stp     x10, x11, [sp, #PT_REGS_X10]
        stp     x12, x13, [sp, #PT_REGS_X12]
        stp     x14, x15, [sp, #PT_REGS_X14]
        stp     x16, x17, [sp, #PT_REGS_X16]
        stp     x18, x19, [sp, #PT_REGS_X18]
        stp     x20, x21, [sp, #PT_REGS_X20]
        stp     x22, x23, [sp, #PT_REGS_X22]
        stp     x24, x25, [sp, #PT_REGS_X24]
        stp     x26, x27, [sp, #PT_REGS_X26]
        stp     x28, x29, [sp, #PT_REGS_X28]
        str     x30,      [sp, #PT_REGS_X30]
        mrs     x1, spsr_el1
        str     x1,      [sp, #PT_REGS_SPSR_EL1]
        mrs     x1, elr_el1
        str     x1,      [sp, #PT_REGS_ELR_EL1]
        mrs     x1, sp_el0
        str     x1,      [sp, #PT_REGS_SP_EL0]
        mov     x0, sp
        bl      el1_irq
        ldr     x1,      [sp, #PT_REGS_SP_EL0]
        msr     sp_el0, x1
        ldr     x1,      [sp, #PT_REGS_ELR_EL1]
        msr     elr_el1, x1
        ldr     x1,      [sp, #PT_REGS_SPSR_EL1]
        msr     spsr_el1, x1
        ldr     x30,     [sp, #PT_REGS_X30]
        ldp     x28, x29, [sp, #PT_REGS_X28]
        ldp     x26, x27, [sp, #PT_REGS_X26]
        ldp     x24, x25, [sp, #PT_REGS_X24]
        ldp     x22, x23, [sp, #PT_REGS_X22]
        ldp     x20, x21, [sp, #PT_REGS_X20]
        ldp     x18, x19, [sp, #PT_REGS_X18]
        ldp     x16, x17, [sp, #PT_REGS_X16]
        ldp     x14, x15, [sp, #PT_REGS_X14]
        ldp     x12, x13, [sp, #PT_REGS_X12]
        ldp     x10, x11, [sp, #PT_REGS_X10]
        ldp     x8, x9,   [sp, #PT_REGS_X8]
        ldp     x6, x7,   [sp, #PT_REGS_X6]
        ldp     x4, x5,   [sp, #PT_REGS_X4]
        ldp     x2, x3,   [sp, #PT_REGS_X2]
        ldp     x0, x1,   [sp, #PT_REGS_X0]
        add     sp, sp, #PT_REGS_SIZE
        eret
    ```
  - 同一变更内：**删除 time.c 的 `el1_irq_dispatch` shim**（entry.S 已无引用；
    `grep -rn el1_irq_dispatch kernel/` 为空作为自查）。
  - 同步更新 entry.S 头注释（1-33：删除"Task 3 wires up … dispatches directly"
    的过时描述，改为指向 el1_irq_entry/el1_irq/gic_dev_dispatch）。
- [ ] 改 `mk/components/run.mk`：`test-aarch64-uefi-smp` 的 python 参数行（167 附近）
  在 `--expect-selftest` 后追加 `--expect-gic \`（标准回归从此携带 GIC 断言）。
- [ ] **跑 GREEN（clobber 转绿 + 全量）**：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test && \
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：self-test passed；9/9 case PASS。抽查任一 stdout.log：
  ```sh
  grep -a "gic-probe\|dispatch ready\|GICv2 driver" \
    test-results/aarch64-uefi-smp/*/cpus-2-run-1.stdout.log
  # 预期四行 marker 全在, 且是 OK/survived 不是 FAIL/TIMEOUT
  ```
- [ ] x86 边界自查（源级，R1-7；**不跑任何 x86 build**）：
  ```sh
  git diff --stat master -- kernel/arch/x86_64 kernel/include/arch/x86_64 kernel/intr
  # 预期: 空
  ```
- [ ] **不单独 commit**——与 Task 2.3a/2.3b 合并为"entry.S+dispatch+SPI"功能 commit。

**验证命令（本 Task 全量）**：
```sh
python3 qemutests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang test-aarch64-uefi-smp
```

---

### Task 2.3a: RED — SPI 注入 harness 完整交付（不含任何内核改动）

**Files:**
- Create: `qemutests/aarch64_gic_spi.py`（完整可运行，无占位代码；**本 Task 不改任何内核文件、不加 make target**——R1-5）

**Interfaces:**
- Consumes: `aarch64_uefi_smp.generate_diagnostic_dtb`（同目录 import，aarch64_uefi_smp.py:406-439）
- Produces: 可重复的失败命令（对现状内核 `--diagnostic-dtb auto` 运行 → 等 armed 行超时 → FAIL 即 RED 证据）；harness 的 CLI 契约（Task 2.3b 的 make target 依赖）：`--firmware/--image/--qemu/--log-dir/--cpus/--timeout/--diagnostic-dtb <auto|PATH>/--self-test`

- [ ] 写 `qemutests/aarch64_gic_spi.py`（真实代码全文）：
  ```python
  #!/usr/bin/env python3
  """PL011 RX → GIC SPI 通路注入测试（spec §7.4）。

  -chardev socket 起 QEMU serial：读端扫 '[gic] spi-test armed intid=<N>'，
  注入 1 字节触发 RX IRQ，断言 '[gic-spi] intid=<N> handled count=1'。
  DTB 机制与 SMP 套件一致：--diagnostic-dtb auto 逐 case 生成（复用
  aarch64_uefi_smp.generate_diagnostic_dtb，同目录 import）。UEFI 固件自身
  的输出也走这条 PL011，读端只做行扫描不受影响。"""

  import argparse
  import os
  import re
  import select
  import socket
  import subprocess
  import sys
  import time
  from pathlib import Path

  ARMED_RE = re.compile(r"^\[gic\] spi-test armed intid=(\d+)$", re.MULTILINE)
  HANDLED_RE = re.compile(r"^\[gic-spi\] intid=(\d+) handled count=(\d+)$", re.MULTILINE)


  def spi_verdict(text: str, injected: bool):
      """状态机：返回 (verdict, inject_now)。
      verdict: 'pass' / 'fail' / None（继续等）；inject_now 仅在 armed 且未注入时为真。
      PL011 输出是 LF+CR，统一去 \\r 后匹配（镜像 SMP harness 的做法）。"""
      text = text.replace("\r", "")
      armed = ARMED_RE.search(text)
      handled = HANDLED_RE.search(text)
      if armed and handled:
          if int(handled.group(1)) != int(armed.group(1)):
              return "fail", False             # handled 了错误的 intid
          if int(handled.group(2)) >= 1:
              return "pass", False
      if armed and not injected:
          return None, True                    # armed 未注入 → 注入窗口
      return None, False


  def self_test() -> None:
      armed_only = "[gic] spi-test armed intid=33\n"
      ok = armed_only + "[gic-spi] intid=33 handled count=1\n"
      assert spi_verdict(ok, injected=True)[0] == "pass"
      assert spi_verdict(ok, injected=False) == ("pass", False)  # 已 handled, 注入与否不再影响
      assert spi_verdict(armed_only, injected=False) == (None, True)   # 注入窗口
      assert spi_verdict(armed_only, injected=True) == (None, False)   # 已注入未到 → 等
      wrong = armed_only + "[gic-spi] intid=40 handled count=1\n"
      assert spi_verdict(wrong, injected=True)[0] == "fail"       # 错误 intid
      assert spi_verdict("", injected=False) == (None, False)     # 什么都没有 → 等
      assert spi_verdict(ok.replace("\n", "\n\r"), injected=True)[0] == "pass"  # LF+CR


  def qemu_command(args, dtb: str, sock: str) -> list:
      return [args.qemu, "-M", "virt,gic-version=2,acpi=off", "-cpu", "cortex-a53",
              "-smp", str(args.cpus), "-m", "512",
              "-drive", "if=pflash,format=raw,file=" + args.firmware,
              "-drive", "if=none,file=" + args.image +
                        ",format=raw,readonly=on,id=disk",
              "-device", "virtio-blk-device,drive=disk",
              "-chardev", "socket,id=ser0,path=" + sock + ",server=on,wait=off",
              "-serial", "chardev:ser0", "-display", "none",
              "-no-reboot", "-no-shutdown", "-dtb", dtb]


  def main() -> int:
      parser = argparse.ArgumentParser()
      parser.add_argument("--self-test", action="store_true")
      parser.add_argument("--firmware")
      parser.add_argument("--image")
      parser.add_argument("--qemu")
      parser.add_argument("--log-dir")
      parser.add_argument("--cpus", type=int, default=2)
      parser.add_argument("--timeout", type=float, default=90.0)
      parser.add_argument("--diagnostic-dtb", metavar="PATH_OR_AUTO",
                          help="'auto' 在 log-dir 生成 QEMU DTB（生产固件不透出 "
                               "DTB 时必需）；或给一个现成 DTB 路径")
      args = parser.parse_args()
      if args.self_test:
          self_test()
          print("aarch64_gic_spi: self-test passed")
          return 0
      if not all((args.firmware, args.image, args.qemu, args.log_dir)):
          parser.error("--firmware, --image, --qemu, and --log-dir are required "
                       "outside --self-test")
      if not args.diagnostic_dtb:
          parser.error("--diagnostic-dtb is required (use 'auto' or an explicit path)")
      Path(args.log_dir).mkdir(parents=True, exist_ok=True)
      if args.diagnostic_dtb == "auto":
          sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
          from aarch64_uefi_smp import generate_diagnostic_dtb
          dtb = generate_diagnostic_dtb(args.qemu, args.log_dir, args.cpus)
      else:
          dtb = args.diagnostic_dtb

      sock = os.path.join(args.log_dir, "pl011.sock")
      proc = subprocess.Popen(qemu_command(args, dtb, sock),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
      log = bytearray()
      verdict = None
      try:
          client = None
          injected = False
          deadline = time.monotonic() + args.timeout
          while time.monotonic() < deadline and proc.poll() is None:
              if client is None:
                  try:
                      client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                      client.connect(sock)
                      client.setblocking(False)
                  except OSError:
                      try:
                          client.close()
                      except OSError:
                          pass
                      client = None
                      time.sleep(0.2)
                      continue
              readable, _, _ = select.select([client], [], [], 0.2)
              if not readable:
                  continue
              try:
                  chunk = client.recv(4096)
              except BlockingIOError:
                  continue
              if not chunk:
                  break
              log.extend(chunk)
              verdict, inject_now = spi_verdict(log.decode("utf-8", "replace"),
                                                injected)
              if inject_now and not injected:
                  client.send(b"G")            # 注入 1 字节 → PL011 RX IRQ
                  injected = True
              if verdict is not None:
                  break
      finally:
          (Path(args.log_dir) / "serial.log").write_bytes(log)
          proc.terminate()
          try:
              proc.wait(timeout=2)
          except subprocess.TimeoutExpired:
              proc.kill()
              proc.wait()
      result = "PASS" if verdict == "pass" else "FAIL"
      print('{"event": "spi-case", "cpus": %d, "result": "%s", '
            '"stdout": "%s"}' % (args.cpus, result,
                                 Path(args.log_dir) / "serial.log"))
      return 0 if verdict == "pass" else 1


  if __name__ == "__main__":
      sys.exit(main())
  ```
- [ ] **跑 harness 自测（应绿）**：
  ```sh
  python3 qemutests/aarch64_gic_spi.py --self-test
  ```
  预期输出：`aarch64_gic_spi: self-test passed`。
- [ ] **跑 RED（对未改内核——Task 2.2 后、2.3b 前的镜像）**：
  ```sh
  make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi
  python3 qemutests/aarch64_gic_spi.py --diagnostic-dtb auto \
    --firmware build/aarch64-clang/image/QEMU_EFI.fd \
    --image build/aarch64-clang/image/aarch64-uefi.img \
    --qemu qemu-system-aarch64 \
    --log-dir /tmp/gic-spi-red-$$ ; \
  grep -ac "spi-test armed" /tmp/gic-spi-red-*/serial.log
  ```
  预期：**exit 1**（`"result": "FAIL"`，armed 行等待超时）；grep 计数 = 0
  （serial.log 里无任何 spi-test armed 行——RED 证据留存）。
  **不 commit**（与 Task 2.2/2.3b 同 commit）。

---

### Task 2.3b: GREEN — DTB 解析 + PL011 RX + SPI handler 走 handler 表

**Files:**
- Modify: `kernel/arch/aarch64/dtb_parse.c:122-127`（pl011 节点补 `interrupts` 解析 → `pl011_spi`）
- Modify: `kernel/include/arch/aarch64/dtb.h:16-20`（struct aarch64_platform_info 加 `uint32_t pl011_spi;`）+ 访问器声明
- Modify: `kernel/arch/aarch64/dtb.c`（新增 `uint32_t dtb_pl011_spi(void)`，实现放 dtb_pl011_base 旁 ~line 18）
- Modify: `kernel/arch/aarch64/pl011.c`（新增 RX 中断三函数，文件尾部 kputx 之后）
- Create: `kernel/arch/aarch64/spi_test.c`（`#if OS01_SELFTEST`）
- Modify: `kernel/arch/aarch64/main.c`（SELFTEST 区在 arch_tick_start 成功后、irq_enable 前调 `gic_spi_test_init()`）
- Modify: `mk/components/run.mk`（新增 `test-aarch64-gic-spi` target，放 test-aarch64-uefi-smp-no-ack 块之后 ~line 195；传 `--diagnostic-dtb=auto`，Task 2.3a 的 harness 已支持）

**Interfaces:**
- Consumes: Task 1.2 `gic_register_handler/gic_irq_configure`；Task 2.2 `el1_irq` dispatch 链；Task 2.3a harness CLI；`kputs/kputu`（pl011.c:88/99）
- Produces:
  ```c
  /* dtb.c */
  uint32_t dtb_pl011_spi(void);            /* QEMU virt: 33 */
  /* pl011.c */
  uint32_t pl011_dr_read(void);            /* 读 DR（清 RX） */
  void pl011_irq_rx_enable(void);          /* IMSC = RXIM(bit4)，TX 保持掩死 */
  void pl011_clear_ints(void);             /* ICR = 0x7FF */
  /* spi_test.c（OS01_SELFTEST） */
  void gic_spi_test_init(void);            /* 注册 handler + 使能 SPI + 打 armed 行 */
  ```

- [ ] dtb 三件套：
  - dtb.h `struct aarch64_platform_info` 加字段 `uint32_t pl011_spi;`（紧跟 cntp_ppi 后），声明 `uint32_t dtb_pl011_spi(void);`
  - dtb.c 加实现（dtb_pl011_base 之后）：
    ```c
    uint32_t dtb_pl011_spi(void) { return platform.pl011_spi; }
    ```
  - dtb_parse.c pl011 分支（122-127）追加校验+解析（真实代码）：
    ```c
    if (contains(n->compatible, "arm,pl011")) {
        /* interrupts = <GIC_SPI(0) nr IRQ_TYPE_LEVEL_HIGH(4)>，3 个 be32 word */
        if (s->uart || (n->status.data && !exact(n->status, "okay") && !exact(n->status, "ok")) ||
            !device_reg(n, 0, 0x09000000) ||
            n->interrupts.len != 12 || be32(n->interrupts.data) != 0 ||
            be32(n->interrupts.data + 4) == 0 || be32(n->interrupts.data + 8) != 4)
            return -4;
        s->uart = true;
        s->info.pl011_base = 0x09000000;
        s->info.pl011_spi = 32u + be32(n->interrupts.data + 4);   /* QEMU virt: 33 */
    }
    ```
- [ ] pl011.c 追加（真实代码）：
  ```c
  /* ── RX 中断通路（GIC Phase 1, spec §7.4）── 只开 RXIM；输出继续轮询。 */
  uint32_t pl011_dr_read(void)
  {
      return pl011_r32(PL011_DR);          /* 读 DR 顺带清 RX 状态 */
  }

  void pl011_irq_rx_enable(void)
  {
      pl011_w32(PL011_IMSC, 1U << 4);      /* RXIM only; TX 中断保持掩死 */
  }

  void pl011_clear_ints(void)
  {
      pl011_w32(PL011_ICR, 0x7FF);
  }
  ```
- [ ] 写 `kernel/arch/aarch64/spi_test.c`（真实代码核心）：
  ```c
  /* SPI 通路测试（spec §7.4）：PL011 RX → GIC SPI(dtb) → handler 表。
   * harness(qemutests/aarch64_gic_spi.py, Task 2.3a) 在看到 armed 行后
   * 向 PL011 注入 1 字节并断言 handled 行。level 触发契约: handler 内先清
   * 设备源（读 DR + ICR），EOI 由 dispatch 在返回后统一做（spec §2.3）。 */
  #if OS01_SELFTEST
  #include <stdint.h>
  #include <stdbool.h>
  #include <arch/aarch64/gic.h>
  #include <arch/aarch64/dtb.h>
  #include <arch/aarch64/boot_log.h>

  uint32_t pl011_dr_read(void);
  void pl011_irq_rx_enable(void);
  void pl011_clear_ints(void);

  static volatile uint32_t g_spi_count;

  static void pl011_rx_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
  {
      (void)param; (void)regs;
      (void)pl011_dr_read();               /* 清设备源 #1 */
      pl011_clear_ints();                  /* 清设备源 #2 */
      uint32_t n = g_spi_count + 1;
      g_spi_count = n;
      kputs("[gic-spi] intid=");
      kputu(intid);
      kputs(" handled count=");
      kputu(n);
      kputs("\n");
  }

  void gic_spi_test_init(void)
  {
      uint32_t intid = dtb_pl011_spi();    /* 33, 来自 DTB 不硬编码 */
      if (gic_register_handler(intid, pl011_rx_handler, 0, "pl011-rx") != 0 ||
          gic_irq_configure(intid, true, 0x00, 0x01) != 0) {   /* 路由 BSP(bit0), R1-2 */
          log_err("[gic] spi-test arm FAIL\n");
          return;
      }
      pl011_irq_rx_enable();
      kputs("[gic] spi-test armed intid=");
      kputu(intid);
      kputs("\n");
  }
  #endif
  ```
- [ ] main.c：在 `arch_tick_start()` 成功后（line 260 `[IRQ] enabled` 打印之前）插入：
  ```c
  #if OS01_SELFTEST
      gic_spi_test_init();
  #endif
  ```
  （顺序：handler 注册与 SPI 使能必须在 `arch_local_irq_enable()` 之前完成，
  避免使能瞬间未注册的 pending SPI 走 unexpected 路径。）
- [ ] `mk/components/run.mk` 新增 target（no-ack 块后，真实代码；`--diagnostic-dtb=auto`
  是 Task 2.3a harness 的已支持参数）：
  ```make
  # PL011 RX → GIC SPI 注入测试（spec §7.4）。复用 SMP 套件的固件/镜像/DTB 机制。
  .PHONY: test-aarch64-gic-spi
  test-aarch64-gic-spi:
  	$(call require_aarch64_uefi)
  	$(call require_capability,uefi)
  	$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi
  	python3 qemutests/aarch64_gic_spi.py \
  	  --diagnostic-dtb=auto \
  	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
  	  --image "$(AARCH64_UEFI_DISK)" \
  	  --qemu "$(AARCH64_QEMU)" \
  	  --log-dir "$(OS01_ROOT)/test-results/aarch64-gic-spi/$$(date -u +%Y%m%dT%H%M%S)-$$$$"
  ```
- [ ] **跑 GREEN**：
  ```sh
  make PROFILE=aarch64-clang test-aarch64-gic-spi
  ```
  预期：`{"event": "spi-case", ... "result": "PASS"}`，exit 0；
  `test-results/aarch64-gic-spi/*/serial.log` 里依次出现
  `[gic] spi-test armed intid=33` → `[gic-spi] intid=33 handled count=1`。
- [ ] **全量回归**（Task 2.1 + 2.2 + 2.3a + 2.3b 合并验证）：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test && \
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：9/9 PASS（SPI armed 行是新增输出，不与既有断言冲突——kernel_failure
  只认 `[smp|spinlock]` 前缀；但 `[gic] spi-test arm FAIL` 走 `[gic]` 前缀不会被
  它拦住，在 `gic_evidence_ok` 里补一条 `^\[gic\][^\n]*\bFAIL\b` 拒绝规则——做）。
- [ ] commit（Task 2.1 + 2.2 + 2.3a + 2.3b 合一）：
  ```sh
  git add qemutests/aarch64_uefi_smp.py qemutests/aarch64_gic_spi.py \
          kernel/include/arch/aarch64/regs.h kernel/arch/aarch64/entry.S \
          kernel/arch/aarch64/trap.c kernel/arch/aarch64/time.c \
          kernel/arch/aarch64/irq_probe.c kernel/arch/aarch64/main.c \
          kernel/arch/aarch64/dtb_parse.c kernel/arch/aarch64/dtb.c \
          kernel/include/arch/aarch64/dtb.h kernel/arch/aarch64/pl011.c \
          kernel/arch/aarch64/spi_test.c mk/components/run.mk
  git commit -m "feat(aarch64): entry.S 全量 save/restore + pt_regs_t + 通用 IRQ dispatch + PL011 SPI 通路

  - regs.h: pt_regs_t 补 x30 → x0..x30+sp_el0+elr+spsr (272B, 16对齐),
    Section 2 PT_REGS_* 偏移与 entry.S 共享, _Static_assert 锁布局
  - entry.S: EL1h IRQ 槽只放 b el1_irq_entry; 表后新增全量 save/restore
    (15对 stp + str x30 + 3 个系统状态) → bl el1_irq → 对称恢复 → eret;
    过渡 shim el1_irq_dispatch 在同一变更内删除(全程无链接断裂中间态, R1-1)
  - trap.c: el1_irq = gic_dev_dispatch 三行壳; time.c 删硬编码 intid
    比较, tick 改注册 handler (TVAL 重装仍最先, EOI 移交 dispatch)
  - 破坏性探针(OS01_SELFTEST): clobber 确定性 trampoline——asm 直写 SGIR
    自发 SGI 2 真实 IRQ, 哨兵 x0-x5+x18, 判据 = AAPCS64 实参寄存器
    x0-x2 架构性必然被销毁(R2-4); unexpected(SPI 40 enable+route→ISPENDR
    注入→回调观察递送恰一次→disable+ICPENDR 拆除, R1-2/R2-5);
    --expect-gic 进标准回归
  - SPI: dtb_parse 补 pl011 interrupts 解析(33); pl011 只开 RXIM;
    gic_spi_test_init 注册 handler; harness aarch64_gic_spi.py
    (Task 2.3a 完整交付: --self-test/--diagnostic-dtb auto/socket chardev
    注入 1 字节) 先对现状内核 RED, Task 2.3b 转 GREEN
  - EOIR 修复落地: dispatch 写回完整 IAR(D7)

  Co-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

**验证命令（Task 2.3a + 2.3b 全量）**：
```sh
python3 qemutests/aarch64_gic_spi.py --self-test
make PROFILE=aarch64-clang test-aarch64-gic-spi
python3 qemutests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang test-aarch64-uefi-smp
```

---

### Task 3.1: RED — SMP IPI 跨核 SGI 测试（harness 断言先行）

**Files:**
- Modify: `qemutests/aarch64_uefi_smp.py`（--expect-gic 追加 IPI 断言 + self_test fixtures）

**Interfaces:**
- Consumes: Task 2.1 的 `gic_evidence_ok(text, cpus)/passed(expect_gic)`
- Produces: 断言的 marker 行（kernel 侧由 Task 3.2 产出）：
  - `[ipi] send sgi=0 filter=others` 恰一条
  - `[ipi] cpu=<n> received=1` 对 1..cpus-1 每核恰一条（received!=1 拒）
  - `[ipi] summary targets=<cpus-1> received=<cpus-1> status=PASS` 恰一条（cpus=1 时 targets=0 received=0、无 per-cpu 行）
  - `[ipi] bsp raw_iar=0x401` **cpus≥2 恰一条、cpus=1 必须无**（R1-9 CPUID E2E）

- [ ] `gic_evidence_ok` 升级为最终形态（真实代码；`cpus` 形参在本 Task 开始消费，四条基础 checks 扩为六条并追加 per-cpu/raw_iar 校验）：
  ```python
  def gic_evidence_ok(text: str, cpus: int) -> bool:
      """--expect-gic: GICv2 框架证据（spec §7.2）。
      marker 恰一条（多打/漏打都拒）；clobber FAIL/TIMEOUT 行出现即拒；
      IPI per-cpu/summary/raw_iar 按 cpus 校验（Task 3.1 追加）。"""
      text = text.replace("\r", "")
      if re.search(r"^\[gic-probe\][^\n]*\bFAIL\b", text, re.MULTILINE):
          return False
      if re.search(r"^\[gic-probe\][^\n]*TIMEOUT", text, re.MULTILINE):
          return False
      checks = [
          (r"^\[gic\] GICv2 driver: intids=\d+$", 1),
          (r"^\[gic\] dispatch ready$", 1),
          (r"^\[gic-probe\] save-restore OK$", 1),
          (r"^\[gic-probe\] unexpected intid=40 survived$", 1),
          (r"^\[ipi\] send sgi=0 filter=others$", 1),
          (r"^\[ipi\] summary targets=(\d+) received=(\d+) status=PASS$", 1),
      ]
      for pattern, want in checks:
          found = re.findall(pattern, text, re.MULTILINE)
          if len(found) != want:
              print(f"FAIL: gic evidence {pattern!r} found {len(found)}, want {want}")
              return False
      # 每个非 BSP 核恰一行 received=1（cpus=1 时无此行）
      ipi_cpus = re.findall(r"^\[ipi\] cpu=(\d+) received=(\d+)$", text, re.MULTILINE)
      if len(ipi_cpus) != cpus - 1 or {int(c) for c, _ in ipi_cpus} != set(range(1, cpus)):
          print(f"FAIL: ipi per-cpu lines {ipi_cpus}, want cpus 1..{cpus - 1}")
          return False
      for _, k in ipi_cpus:
          if int(k) != 1:                        # 多发=风暴, 漏发=丢 IPI
              return False
      m = re.search(r"^\[ipi\] summary targets=(\d+) received=(\d+) status=PASS$",
                    text, re.MULTILINE)
      if not m or tuple(map(int, m.groups())) != (cpus - 1, cpus - 1):
          return False
      # R1-9 CPUID E2E: cpus>=2 恰一条 0x401 (CPUID=1|SGI 1); cpus==1 必须无
      raw_iar = re.findall(r"^\[ipi\] bsp raw_iar=0x401$", text, re.MULTILINE)
      if len(raw_iar) != (1 if cpus >= 2 else 0):
          print(f"FAIL: ipi bsp raw_iar lines {len(raw_iar)}, cpus={cpus}")
          return False
      return True
  ```
- [ ] `self_test()` 追加 fixtures（真实代码）：
  ```python
      # --expect-gic 的 IPI 断言（Task 3）：cpus=2 与 cpus=1 两形态 + 各类破坏。
      def with_ipi(log, cpus_n):
          lines = ["[ipi] send sgi=0 filter=others\n"]
          lines += [f"[ipi] cpu={c} received=1\n" for c in range(1, cpus_n)]
          lines += [f"[ipi] summary targets={cpus_n - 1} received={cpus_n - 1}"
                      " status=PASS\n"]
          if cpus_n >= 2:
              lines += ["[ipi] bsp raw_iar=0x401\n"]
          return log + "".join(lines)
      g2 = with_ipi(gic_log, 2)
      assert passed(g2, cpus=2, expect_gic=True), "ipi complete (2 cpus) must pass"
      g1 = with_ipi(gic_log, 1)
      assert passed(g1, cpus=1, expect_gic=True), "ipi targets=0 (1 cpu) must pass"
      assert not passed(with_ipi(gic_log, 2).replace("[ipi] cpu=1 received=1\n", ""),
                        cpus=2, expect_gic=True), "missing cpu=1 line must reject"
      assert not passed(gic_log + "[ipi] summary targets=1 received=1 status=PASS\n",
                        cpus=2, expect_gic=True), "summary without send/per-cpu must reject"
      assert not passed(with_ipi(gic_log, 2).replace("cpu=1 received=1",
                                                     "cpu=1 received=2"),
                        cpus=2, expect_gic=True), "received=2 must reject"
      assert not passed(with_ipi(gic_log, 2).replace("targets=1 received=1",
                                                     "targets=1 received=0"),
                        cpus=2, expect_gic=True), "received mismatch must reject"
      assert not passed(with_ipi(gic_log, 2).replace("[ipi] bsp raw_iar=0x401\n", ""),
                        cpus=2, expect_gic=True), "missing raw_iar (2 cpus) must reject"
      assert not passed(g1 + "[ipi] bsp raw_iar=0x401\n", cpus=1, expect_gic=True), \
          "raw_iar present with 1 cpu must reject"
  ```
- [ ] **跑 harness 自测（应绿）**：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test
  ```
- [ ] **跑 RED**（对 Task 2.3b 后的内核——无 IPI marker）：
  ```sh
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：**9/9 FAIL**（exit 1），stdout.log 无任何 `[ipi]` 行（grep 留存）。
  这一步同时证明 Task 2.x 的改动没顺手把 IPI 做了。
  **不 commit**（与 Task 3.2 同 commit）。

---

### Task 3.2: GREEN — SGI send + handler，SMP 1/2/4 ×3 全绿

**Files:**
- Create: `kernel/include/arch/aarch64/gic_pub.h`（R3-1/R4-1/R4-2：仅 aarch64 头，
  不进 `kernel/include/arch/barrier.h`；声明+定义 `arch_publish_handler_table()` = aarch64
  `dsb ishst`；见下方完整代码）
- Modify: `kernel/arch/aarch64/smp.c:204-226`（secondary_idle 尾循环前开 DAIF.I）
- Create: `kernel/arch/aarch64/ipi_test.c`（`#if OS01_SELFTEST`；include `<arch/aarch64/gic_pub.h>`）
- Modify: `kernel/arch/aarch64/main.c`（SELFTEST 区在 gic_unexpected_probe 之后、halt 循环前调 `gic_ipi_test(dtb_cpu_count())`）

**Interfaces:**
- Consumes: Task 1.2 `gic_send_sgi/gic_register_handler/gic_dev_current/gic_dbg_last_iar/GICD_SGIR_FILTER_LIST/GICD_SGIR_FILTER_OTHERS`（`gic_dbg_last_iar` 读 per-CPU trace 槽，R2-6）；Task 2.2 `el1_irq` dispatch 链；`arch_atomic_fetch_add`（kernel/include/arch/atomic.h:47-58，aarch64 段）；stlr/ldar release/acquire 模式先例（kernel/arch/aarch64/aarch64_percpu.h:93-115，R2-7）；`aarch64_boot_percpu_t`（aarch64_percpu.h:21-22，cpu_id 在槽 offset 8）；TPIDR_EL1（BSP head.S:312-323 / AP head.S:650 都已设）；`arch_cycle_counter/arch_cycle_freq`（cpu.h:78-86）
- Produces:
  ```c
  /* ipi_test.c（OS01_SELFTEST） */
  void gic_ipi_test(uint32_t cpu_count);   /* 发 SGI 0 (others) → acquire 轮询 → summary →
                                              等 AP1 回发 SGI 1 → 打 bsp raw_iar 行 (R1-9) */
  ```

- [ ] **新建** `kernel/include/arch/aarch64/gic_pub.h`（R3-1/R4-1/R4-2 完整代码，
  **仅 aarch64 头**——不进 `kernel/include/arch/barrier.h`，那个 shared header 被
  `kernel/driver/e1000.c:9` include，改它会破坏"x86 编译输入逐字节不变"边界）：
  ```c
  /* kernel/include/arch/aarch64/gic_pub.h — GICv2 跨核发布原语（spec §7.5 R3-1）。
   *
   * 作用：把对全局 `handlers[]` 表的写发布给 Inner-Shareable 域（AP），
   *       保证后续 GICD_SGIR 写触发的 SGI 在 AP 端看到的是已发布过的 handler。
   *
   * 屏障选择：aarch64 `dsb ishst`，与 dmb ishst 的差别在于——
   *   - dmb ishst 仅排序（ordering-only）：保证此屏障前的 store 在屏障后的
   *     store 之前被观察到，但不阻塞屏障后指令的执行。
   *   - dsb ishst 完成 + 指令边界（completion + instruction boundary）：
   *     屏障前的所有 store 必须完成（cache/TLB/write buffer drain 到
   *     Inner-Shareable 域的可见点）才执行屏障后的指令；屏障后指令也
   *     不会重排到屏障前。本 Phase 选 dsb 是有意的较强选择：SGIR 写后
   *     立即发 IRQ，必须保证 handler 表已全局可见。Linux GICv2 选
   *     dmb(ishst) 是因为紧接 dsb sy 做隐含 ordering；OS01 此处直接
   *     dsb ishst 省去再配对屏障的复杂度。
   *
   * 该 wrapper 不进 gic_register_handler()（避免无谓开销），由调用方按需
   * 显式调——Task 3.2 的 gic_ipi_test() 是唯一当前调用点（cntp / pl011_rx
   * / probe_sgi 的注册要么仅 BSP 接收、要么本核自触发，spec §7.5 + §8 R11）。 */
  #ifndef _ARCH_AARCH64_GIC_PUB_H
  #define _ARCH_AARCH64_GIC_PUB_H

  #include <stdint.h>

  #ifdef __cplusplus
  extern "C" {
  #endif

  /* 发布内核 handler 表给 Inner-Shareable 域。SGIR 写之前必须调。 */
  static inline void arch_publish_handler_table(void)
  {
      /* dsb ishst = Data Synchronization Barrier, Inner-Shareable,
       * Store-only：屏障前的所有 store 必须完成（对其他核可见）才执行
       * 屏障后的指令；屏障后指令不重排到屏障前。memory clobber 强制
       * 编译器不把对 handlers[] 的写挪到屏障后。 */
      __asm__ __volatile__("dsb ishst" ::: "memory");
  }

  #ifdef __cplusplus
  }
  #endif

  #endif /* _ARCH_AARCH64_GIC_PUB_H */
  ```
  说明：① header 是 aarch64 专属，仅被 aarch64 .c/.S 引用（kernel/Makefile:42-43
  白名单），不存在被 x86 或 host 编译路径引用的可能；② x86 不需要此原语
  （x86 强内存模型，Device MMIO 写自然带发布语义）；③ hosttests 用 aarch64
  target 编译（PROFILE=aarch64-clang），与内核同走 `__aarch64__` 分支，
  wrapper 静态 inline 自然可用。

- [ ] 改 `secondary_idle`（smp.c:219-225 区；真实 diff）：
  ```c
      /* Includes a late AP: go=2 persists even if the BSP already resumed
       * ticks. APs keep their CNTP disabled; the only enabled banked lines
       * are SGI 0/1/2 (IPI + probe) and — for the BSP — the CNTP PPI.
       * Unmask DAIF.I
       * so the AP can take SGIs through el1_irq (GIC Phase 1, spec §7.5). */
      cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));
      arch_local_irq_enable();
      __asm__ __volatile__("isb" ::: "memory");
      for (;;) arch_cpu_halt();
  ```
  （`arch_local_irq_enable` 来自 <arch/irq.h>，smp.c 需补 `#include <arch/irq.h>`。
  这是**唯一的生产行为变化**，commit message 已声明。）
- [ ] 写 `kernel/arch/aarch64/ipi_test.c`（真实代码核心；**内存序协议精确到指令，R1-6**）：
  ```c
  /* SGI/IPI 跨核测试（spec §7.5）。
   *
   * 内存序契约（R1-6 + R3-1，与 aarch64_percpu.h:93-113 的 boot_online/bench_done
   * 完全同模式）：
   *   AP  : arch_atomic_fetch_add(&ipi_received[cpu], 1)   ← ldxr/stxr 原子计数
   *         ipi_flag_release(&ipi_done[cpu], 1)            ← stlr (RELEASE store)：
   *                                                             计数先于标志可见
   *   BSP : ipi_flag_acquire(&ipi_done[cpu]) == 1           ← ldar (acquire load)：
   *                                                             见标志后读计数必得递增
   * 超时兜底 cntvct deadline（不依赖 IRQ，phase1 spec §2.1 既有方法）。
   * AP handler 不打印（多核并发写 PL011 会绞线, spec §8 R4）。
   *
   * R1-9 回发确认（R2-6 收口）：AP1 收到 SGI 0 后回发 SGI 1
   * (filter=LIST targets=0x01) 给 BSP；BSP 的 SGI-1 handler 在 handler 上下文
   * 读 gic_dbg_last_iar()——per-CPU trace 槽只被本核 ack 写入（同核 dispatch
   * 串行 + IRQ masked 无嵌套，跨核 ack 写各自槽），因此读到的必是本 SGI 的
   * 原始 IAR，无跨核覆写竞态。期望 0x401 = CPUID(1)<<10 | SGI 1——非零
   * CPUID 真实穿越 ack 路径。 */
  #if OS01_SELFTEST
  #include <stdint.h>
  #include <stdbool.h>
  #include <arch/aarch64/gic_pub.h>  /* R3-1/R4-1: arch_publish_handler_table() */
  #include <arch/irq.h>
  #include <arch/atomic.h>
  #include <arch/cpu.h>
  #include <arch/aarch64/gic.h>
  #include <arch/aarch64/dtb.h>
  #include <arch/aarch64/boot_log.h>
  #include "aarch64_percpu.h"

  #define IPI_SGI_ID       0u
  #define IPI_REPLY_SGI    1u
  #define IPI_WAIT_SECONDS 2u

  static volatile uint64_t ipi_received[AARCH64_BOOT_MAX_CPUS];  /* fetch_add 原子递增 */
  static volatile uint32_t ipi_done[AARCH64_BOOT_MAX_CPUS];      /* stlr 置 1 / ldar 读 */
  static volatile uint32_t bsp_reply_flag;                       /* stlr 置 1 / ldar 读 */
  static volatile uint32_t bsp_raw_iar;                          /* SGI-1 handler 捕获 */

  static inline void ipi_flag_release(volatile uint32_t *p, uint32_t v)
  {
      __asm__ __volatile__("stlr %w0, [%1]" :: "r"(v), "r"(p) : "memory");
  }
  static inline uint32_t ipi_flag_acquire(const volatile uint32_t *p)
  {
      uint32_t v;
      __asm__ __volatile__("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
      return v;
  }

  static uint32_t ipi_cpu_id(void)
  {
      uint64_t slot;
      __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(slot));
      return ((volatile aarch64_boot_percpu_t *)(uintptr_t)slot)->cpu_id;
  }

  static void ipi_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
  {
      (void)intid; (void)param; (void)regs;
      uint32_t cpu = ipi_cpu_id();
      if (cpu >= AARCH64_BOOT_MAX_CPUS) return;
      arch_atomic_fetch_add(&ipi_received[cpu], 1);   /* 原子计数 (atomic.h:47-58) */
      ipi_flag_release(&ipi_done[cpu], 1);            /* RELEASE：计数先于标志 */
      if (cpu == 1)                                   /* AP1 回发 SGI 1 → BSP (R1-9) */
          gic_send_sgi(gic_dev_current(), IPI_REPLY_SGI, 0x01, GICD_SGIR_FILTER_LIST);
  }

  static void bsp_reply_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
  {
      (void)intid; (void)param; (void)regs;
      bsp_raw_iar = gic_dbg_last_iar();               /* 本 CPU trace 槽 (R2-6):
                                                         必为本 SGI 的原始 IAR */
      ipi_flag_release(&bsp_reply_flag, 1);
  }

  void gic_ipi_test(uint32_t cpu_count)
  {
      if (cpu_count > AARCH64_BOOT_MAX_CPUS) cpu_count = AARCH64_BOOT_MAX_CPUS;
      for (uint32_t i = 0; i < AARCH64_BOOT_MAX_CPUS; ++i) {
          ipi_received[i] = 0; ipi_done[i] = 0;
      }
      bsp_reply_flag = 0; bsp_raw_iar = 0;
      if (gic_register_handler(IPI_SGI_ID, ipi_handler, 0, "ipi0") != 0 ||
          gic_register_handler(IPI_REPLY_SGI, bsp_reply_handler, 0, "ipi1") != 0) {
          log_err("[ipi] register FAIL\n");
          return;
      }
      /* R3-1：AP 已 secondary_idle 开 DAIF.I 等 SGI；BSP 写 handlers[] 是
       * 普通内存写，Device-nGnRnE 的 GICD_SGIR 写可不与之排序（普通内存→
       * Device 不强制 ordering）。AP 收到 SGI 时 handler 指针/param 可能
       * 尚未可见 → 走 unexpected_cb 路径、不置 ipi_done、致 BSP 超时。
       * 修法：发布屏障 dsb ishst（Inner-Shareable store-only，对所有
       * SGI 唤醒的核可见），调用经 arch 包装 arch_publish_handler_table()
       * 强制未来调用方不遗漏（见 spec §7.5 + §8 R3-1）。注：cntp / pl011
       * / probe-sgi 的注册在 SMP 启动前或本核自触发，**不需要**本屏障——
       * R3 评审要求只在 Task 3.2 路径里加，不进 gic_register_handler。 */
      arch_publish_handler_table();
      kputs("[ipi] send sgi=0 filter=others\n");
      gic_send_sgi(gic_dev_current(), IPI_SGI_ID, 0, GICD_SGIR_FILTER_OTHERS);

      uint64_t deadline = arch_cycle_counter() + arch_cycle_freq() * IPI_WAIT_SECONDS;
      uint32_t done = 0;
      for (;;) {
          done = 0;
          for (uint32_t i = 1; i < cpu_count; ++i)
              if (ipi_flag_acquire(&ipi_done[i]) != 0) ++done;    /* ACQUIRE */
          if (done + 1 >= cpu_count) break;
          if ((uint64_t)arch_cycle_counter() > deadline) break;
          arch_cpu_pause();
      }
      for (uint32_t i = 1; i < cpu_count; ++i) {
          kputs("[ipi] cpu=");
          kputu(i);
          kputs(" received=");
          kputu(ipi_received[i]);
          kputs("\n");
      }
      kputs("[ipi] summary targets=");
      kputu(cpu_count - 1);
      kputs(" received=");
      kputu(done);
      kputs(done + 1 >= cpu_count ? " status=PASS\n" : " status=FAIL\n");

      if (cpu_count >= 2) {                           /* R1-9 回发确认 */
          uint64_t reply_deadline = arch_cycle_counter()
                                  + arch_cycle_freq() * IPI_WAIT_SECONDS;
          while (ipi_flag_acquire(&bsp_reply_flag) == 0) {
              if ((uint64_t)arch_cycle_counter() > reply_deadline) break;
              arch_cpu_pause();
          }
          kputs("[ipi] bsp raw_iar=0x");
          kputx(bsp_raw_iar);
          kputs("\n");
      }
  }
  #endif
  ```
  注意：`gic_cpu_init`（Task 1.2）已为每核 banked 使能 SGI 0/1/2 的
  ISENABLER0 位（0=IPI 主载荷、1=回发确认、2=clobber 探针）——AP 无需再配；
  handler 表是 BSP 注册的全局单份、AP 同核映射可见，**但跨核内存序需 R3-1
  显式屏障**（见 gic_ipi_test() 内 `arch_publish_handler_table()` 调用；
  spec §7.5 + §8 R3-1）。**注：cntp handler 在 `smp_boot_aps()` 之后注册
  （main.c:249 arch_tick_start）**，但 AP 的 CNTP 已在 smp.c:209/224 关掉——
  **CNTP 仅 BSP 接收**，因此无跨核可见性问题；同理 pl011_rx_handler 是
  SPI→BSP 本核、probe_sgi_handler 是 BSP 自触发 SGI 2——三处注册均**不需**
  `arch_publish_handler_table()`。本 Task 的 IPI 是唯一跨核 SGI 路径。
- [ ] main.c 在 `gic_unexpected_probe()` 之后追加（同一 SELFTEST 块内）：
  ```c
      gic_ipi_test(dtb_cpu_count());
  ```
- [ ] **跑 GREEN**：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test && \
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：self-test passed；**9/9 case PASS**（cpus=1：targets=0 received=0 PASS、
  无 per-cpu 行、无 raw_iar 行；cpus=2：cpu=1 received=1 + raw_iar=0x401；
  cpus=4：cpu=1/2/3 各 received=1 + raw_iar=0x401，×3 重复）。
  抽查 4 核日志：`grep -a '\[ipi\]' test-results/aarch64-uefi-smp/*/cpus-4-run-1.stdout.log`
- [ ] SPI 通路不回归：
  ```sh
  make PROFILE=aarch64-clang test-aarch64-gic-spi
  ```
- [ ] hosttest 不回归：
  ```sh
  make -C hosttests PROFILE=aarch64-clang \
       OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver
  ```
- [ ] G6 收尾自查（源级，R1-7）：
  ```sh
  git diff --stat master -- kernel/arch/x86_64 kernel/include/arch/x86_64 kernel/intr
  # 预期: 空（x86 构建输入逐字节不变, 无需也不跑任何 x86 build）
  ```
- [ ] commit（Task 3.1 + 3.2 合一）：
  ```sh
  git add qemutests/aarch64_uefi_smp.py kernel/arch/aarch64/smp.c \
          kernel/arch/aarch64/ipi_test.c kernel/arch/aarch64/main.c
  git commit -m "feat(aarch64): SGI/IPI 跨核通路 + SMP harness 断言

  - gic_send_sgi(GICD_SGIR): filter=others 广播 SGI 0 作 IPI 载荷
  - ipi_test.c 内存序协议(R1-6, 与 aarch64_percpu.h 既有模式同构):
    AP arch_atomic_fetch_add 原子计数 + stlr RELEASE 置完成标志,
    BSP ldar ACQUIRE 轮询——release/acquire 严格配对
  - R1-9 CPUID E2E: AP1 回发 SGI 1 给 BSP, BSP handler 上下文读
    gic_dbg_last_iar() 捕获原始 IAR, 打 [ipi] bsp raw_iar=0x401
    (CPUID=1<<10 | SGI 1)
  - secondary_idle: 尾循环前 arch_local_irq_enable()——本 Phase 唯一
    生产行为变化; AP banked 只使能 SGI 0/1/2 (CNTP 仍关, gic_cpu_init 白名单)
  - harness --expect-gic 追加 IPI 断言: send 恰一/每核 received=1 恰一/
    summary 数值自洽/raw_iar cpus>=2 恰一且 cpus=1 必无
  - SMP 1/2/4 ×3 全绿; SPI/hosttest 零回归; x86 源级零改动

  Co-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

**验证命令（本 Task 全量）**：
```sh
python3 qemutests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang test-aarch64-uefi-smp
make PROFILE=aarch64-clang test-aarch64-gic-spi
make -C hosttests PROFILE=aarch64-clang OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver
```

---

## Self-Review 三查（实现完成后必须执行）

### 查 1：spec 覆盖率（G1-G6 → Task 映射）

| Spec 目标 | 承载 Task | 验证 |
|---|---|---|
| G1 driver 泛化 | 1.1 + 1.2 | hosttest 8 suite 全绿 + `test-aarch64-uefi-smp` 不回归 |
| G2 save/restore + pt_regs_t | 2.2 | clobber 探针 RED→GREEN 留档（确定性 trampoline：自发 SGI 2 + AAPCS64 x0-x2 实参判据，R2-4）+ `_Static_assert` + `--expect-gic` marker + 全程无链接断裂（shim 协议，R1-1） |
| G3 通用 dispatch | 2.2 | `[gic] dispatch ready` + unexpected intid=40 探针（enable/route→注入→观察递送恰一次→拆除，R1-2/R2-5）+ `[tick]` 不断流 |
| G4 SPI 通路 | 2.3a + 2.3b | 2.3a harness（--self-test/--diagnostic-dtb/干净 select 循环）对现状内核 FAIL（RED 留档）→ 2.3b 后 `test-aarch64-gic-spi` PASS（armed → 注入 → handled count=1） |
| G5 SGI/IPI | 3.1 + 3.2 | SMP 1/2/4 ×3 全绿（cpus=1 为 targets=0 形态）；cpus≥2 `[ipi] bsp raw_iar=0x401`（R1-9，per-CPU trace 槽隔离 R2-6） |
| G6 范式对齐 + 零回归 | 全部 | `git diff --stat master -- kernel/arch/x86_64 kernel/include/arch/x86_64 kernel/intr` 为空（源级零改动即构建输入不变，R1-7；不跑任何 x86 build）；`make PROFILE=aarch64-clang test-aarch64-uefi-smp` 全绿 |

spec §5.2 布局（272B / 34 槽 / 16 对齐）↔ regs.h + entry.S 偏移逐条核对；
spec §2.3 EOIR 完整回写 ↔ hosttest suite_ack_eoi + dispatch-CPUID case + E2E raw_iar；
spec §7.3 探针顺序 ↔ irq_probe.c（configure→inject→观察递送恰一次→teardown）；
spec §7.4 DTB 解析 ↔ dtb_parse.c；spec §7.5 内存序 ↔ ipi_test.c 注释块。

### 查 2：占位符扫描

```sh
grep -rn "TODO\|FIXME\|XXX\|占位\|placeholder\|实现 X\|写实现时\|按实现统一" \
  kernel/arch/aarch64/gic_driver.c kernel/arch/aarch64/gic.c \
  kernel/arch/aarch64/trap.c kernel/arch/aarch64/ipi_test.c \
  kernel/arch/aarch64/spi_test.c kernel/arch/aarch64/irq_probe.c \
  kernel/include/arch/aarch64/gic.h kernel/include/arch/aarch64/regs.h \
  kernel/include/arch/aarch64/gic_pub.h \
  hosttests/cases/test_gic_driver.c qemutests/aarch64_gic_spi.py
# 预期: 空（entry.S/time.c 的注释性文字不算）
```
本 v3 plan 中 v1 的两处旧占位（SPI harness 的 selectors 混用、--diagnostic-dtb
"按实现统一"）已通过 Task 2.3a 的完整 harness 代码消除，v2 的 --self-test fixture
矛盾已按 R2-1 修正——自查时确认交付代码与本 plan 一致，无临时代码残留
（`grep -n "selectors" qemutests/aarch64_gic_spi.py` 为空）。R3-1/R4-1 新增
`arch_publish_handler_table()` wrapper 同步写明屏障语义与唯一调用点，R4-2
定位为 aarch64 专属头（`kernel/include/arch/aarch64/gic_pub.h`，**不进
`kernel/include/arch/barrier.h`**——后者被 e1000.c:9 include，改它破坏 x86
边界），无"按实现统一"残留。

### 查 3：类型一致性（跨 Task 接口）

- `gic_handler_fn` 唯一定义于 gic.h；cntp_tick_handler / pl011_rx_handler /
  ipi_handler / bsp_reply_handler / probe_handler 五处签名逐字一致
  `(uint32_t intid, uint64_t param, struct pt_regs *regs)`。
- `struct gic_dev` 字段（gicd/gicc/nr_intids/**dbg_last_iar**）在 gic.h /
  gic_driver.c / gic.c / test_gic_driver.c 四处一致；`gic_dev_current()` 返回
  `struct gic_dev *`。
- `gic_driver_set_unexpected(void (*)(uint32_t))`：声明在 gic.h API 块，定义在
  gic_driver.c，hosttest（注入/清除两态）、gic.c wrapper（log_unexpected）与
  irq_probe.c（probe_unexpected_cb，观察 intid 40 递送）消费同一符号（R1-3/R2-5）。
- `gic_driver_set_cpu_index(uint32_t (*)(void))` / `gic_driver_trace_get(uint32_t)`
  / `GIC_TRACE_CPUS`：声明在 gic.h，定义在 gic_driver.c；wrapper 注入
  `k_cpu_index`（TPIDR 槽）并经 `gic_driver_trace_get(k_cpu_index())` 实现
  `gic_dbg_last_iar()`；hosttest 注入 `probe_cpu_index`——四方一致（R2-6）。
- `gic_clear_pending(dev,intid)`（driver）与 `gic_clear_pending_irq(intid)`
  （wrapper）成对；`gic_set_pending`/`gic_force_pending` 同理（R1-2）。
- `gic_send_sgi(dev, sgi, targets, filter)` 参数序在 gic.h / ipi_test.c /
  test_gic_driver.c 一致（filter 是第 4 参）。
- `arch_publish_handler_table()`：声明+定义同在
  `kernel/include/arch/aarch64/gic_pub.h`（**仅 aarch64 头**——R4-2 不进
  `kernel/include/arch/barrier.h`，后者被 `kernel/driver/e1000.c:9` include，
  改它破坏 x86 编译输入零变更边界），aarch64 = `__asm__ __volatile__("dsb ishst"
  ::: "memory")` static inline；x86 不需要此原语（强内存模型，Device MMIO
  写自然带发布语义）；host 走 aarch64 target 编译（PROFILE=aarch64-clang），
  与内核同走 `__aarch64__` 分支，wrapper 静态 inline 自然可用。**唯一调用点** =
  Task 3.2 `gic_ipi_test()` 在两次 `gic_register_handler()` 成功后、首次
  `gic_send_sgi()` 前——R3-1 跨核发布屏障；`gic_register_handler` 本身**不**
  内置 barrier，避免无谓开销。其他注册点（cntp 仅 BSP 接收因 AP CNTP
  disabled、pl011_rx SPI→BSP 本核、probe_sgi 自触发）不需本屏障。
- IPI harness 断言严格恰一次：`received==1`（多发=风暴、漏发=丢 IPI）/
  `summary targets=received`（自洽）/ `raw_iar==0x401` 恰一条（cpus≥2）。
  不允许多次确认——多余即 FAIL（不假阳、不假阴；AP 收到 SGI 0 后回 SGI 1
  只走 `cpu==1` 路径，SGI 1 用 `FILTER_LIST targets=0x01` 只给 BSP，不
  致 BSP 收到多次）。
- PT_REGS_* 偏移 ↔ regs.h 字段序 ↔ entry.S stp/str 偏移三方核对
  （x30@240 / sp_el0@248 / elr@256 / spsr@264 / SIZE=272）。
- `ipi_flag_release/ipi_flag_acquire` 只在 ipi_test.c 内定义与使用；
  `arch_atomic_fetch_add` 的实参是 `volatile uint64_t*`（ipi_received 是
  uint64_t 数组）。
- marker 字符串逐字核对（harness regex ↔ kputs 文案）：
  `[gic] GICv2 driver: intids=` / `[gic] dispatch ready` /
  `[gic-probe] save-restore OK` / `[gic-probe] unexpected intid=40 survived` /
  `[gic] spi-test armed intid=` / `[gic-spi] intid=`+`handled count=` /
  `[ipi] send sgi=0 filter=others` / `[ipi] cpu=`+`received=` /
  `[ipi] summary targets=`+`received=`+`status=PASS` /
  `[ipi] bsp raw_iar=0x`+kputx（期望 401，cpus≥2）。

---

## 附：本 plan 引用的全部真实文件/行号依据（v2 重核于 2026-09-17, HEAD=3a9fe16, R1-8）

- kernel/arch/aarch64/: gic.c(43L), entry.S(116L), time.c(133L), trap.c(20L),
  smp.c(226L), head.S(755L, secondary_start 580-672, TPIDR BSP 312-323/AP 650),
  dtb.c(83L), dtb_parse.c(279L, gic 115-121, pl011 122-127, timer 128-135),
  pl011.c(135L), main.c(264L), make.config(45L), linker.ld(137L),
  aarch64_percpu.h(cpu_id@offset 8, stlr/ldar helpers 93-115)
- kernel/include/arch/aarch64/: regs.h(pt_regs_t 19-28), dtb.h(40L), smp.h(14L)
- kernel/include/arch/: regs.h facade(__aarch64__ 分发 27-28), irq.h(47/65-109),
  atomic.h(aarch64 arch_atomic_* 43-110), cpu.h(aarch64 78-90)
- x86_64 范式（只读）: regs.h:18-44, entry.S:3-49/28-49, irq.c:14-79,
  irq_hooks.c:65-85, kernel/intr/dispatch.c:9-30,
  kernel/include/intr/interrupt.h:16-52
- 构建/测试: kernel/Makefile:42-43/70-71/92/192-194/222-224,
  mk/components/run.mk:125-148/161-172/258, mk/components/image.mk:96-164,
  mk/profiles/aarch64-clang.mk:6/57-58, mk/targets/aarch64.mk:5-8,
  qemutests/aarch64_uefi_smp.py(578L: 16-34/99-227/230-236/263-348/382-384/
  387-398/406-439/442-536/539-574), hosttests/Makefile(431L, TEST_BINS 62-82,
  尾部 lwip 块), hosttests/include/test_framework.h(144L, __test_stats 15-16)
- 任务描述三处路径纠正：mk/components/aarch64.mk 与 mk/qemu.mk 不存在
  （真身 image.mk:96-164 / run.mk）；thirdpart/aarch64/inc/aarch64.h 不存在
  （GICv2 常量真身 kernel/arch/aarch64/reg.h:18-65）
