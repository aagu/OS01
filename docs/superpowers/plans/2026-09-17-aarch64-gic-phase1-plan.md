# aarch64 GICv2 Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 aarch64 中断路径从"entry.S 零保存 + time.c 硬编码 PPI 30"泛化为通用 GICv2 框架：driver（hw 访问/分类/enable/handler 表）+ 全量 save/restore + pt_regs_t + 通用 dispatch + PL011 SPI 33 通路 + SGI/IPI 跨核，SMP 1/2/4 ×3 全绿。

**Architecture:** 三层：`gic_driver.c`（纯逻辑、指针式 MMIO、hosttest 可编译）→ `gic.c` 生产 wrapper（挂 DTB 基址，`gic_init/gic_cpu_init` 签名不变）→ `trap.c::el1_irq` + `entry.S::el1_irq_entry`（31 GPR + sp_el0/elr/spsr 全量保存）。handler 表镜像 x86_64 `(nr, param, regs)` 签名但不编 kernel/intr。测试三层：hosttest mock MMIO（Task 1）、QEMU serial 断言（--expect-gic 扩展 + 新 SPI socket harness）、破坏性 clobber 探针。

**Tech Stack:** freestanding C（clang -target aarch64-none-elf）、AArch64 手写汇编（entry.S）、GNU Make profile 构建（mk/profiles/aarch64-clang.mk）、host clang 单元测试（hosttests/）、Python QEMU harness（qemutests/）。

**Spec:** docs/superpowers/specs/2026-09-17-aarch64-gic-phase1-design.md

## Global Constraints

- **Worktree**：全部工作在 `feat/aarch64-gic` worktree（/home/aagu/aarch64-gic）完成，不碰 master。
- **不动 x86_64**：`kernel/arch/x86_64/`、`kernel/include/arch/x86_64/`、`kernel/intr/` 零改动（Task 末 `git diff --stat` 自查，G6 验收）。
- **不编 kernel core**：kernel/Makefile:42-43 白名单不动；新文件只放 `kernel/arch/aarch64/`（wildcard 自动收编，kernel/Makefile:70-71）；新头文件只放 `kernel/include/arch/aarch64/`。
- **探针门控**：所有测试/探针内核代码 `#if OS01_SELFTEST`（main.c:18/224 既有模式）；唯一生产行为变化 = AP 尾循环开 DAIF.I 收 IPI（Task 3.2，commit message 显式声明）。
- **hw 层零依赖**：`gic_driver.c` 不 include boot_log/dtb/smp，不打日志，只返回错误码（hosttest 前提，spec §4.1）。`struct pt_regs` 用前向声明（facade 按 `__aarch64__` 分发，host 编译会 #error，kernel/include/arch/regs.h:29-30）。
- **ISR 顺序契约不变**（phase1 spec §2.3）：重装 TVAL → EOI → 打印。
- **构建/测试入口**（全部真实 target，从 repo 根）：
  - hosttest：`make -C hosttests PROFILE=aarch64-clang OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver`（root `test` 被 rootfs capability 门挡，run.mk:258）
  - 全量回归：`make PROFILE=aarch64-clang test-aarch64-uefi-smp`（run.mk:161-172，KERNEL_SELFTEST=1 构建 + qemutests/aarch64_uefi_smp.py --cpus 1 2 4 --repeat 3）
  - SPI 注入：`make PROFILE=aarch64-clang test-aarch64-gic-spi`（Task 2.3 新增）
  - x86 不受影响抽查：`make PROFILE=x86_64-clang kernel.bin`
- **commit 尾注**：`Co-Authored-By: Claude Code <noreply@anthropic.com>`；commit 划分 = 1 docs + 3 功能（GIC driver / entry.S+dispatch+SPI / SGI），RED 测试与 GREEN 实现同 commit 落地（commit 时全绿）。
- 每个 RED 步骤必须**先跑出预期失败并留存输出**再写实现（superpowers:test-driven-development）。

---

### Task 0: spec/plan 自纳入（文档 commit，放最前）

**Files:**
- Create: `docs/superpowers/specs/2026-09-17-aarch64-gic-phase1-design.md`（本 plan 的 Spec，已存在）
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
  git commit -m "docs(superpowers): aarch64 GICv2 Phase 1 spec + implementation plan

  - spec: GICv2 硬件模型 / 现状审计(真实 line no.) / pt_regs_t 精确布局(272B)
    / dispatch 流程 / x86_64 范式对照 / G1-G6 / non-goals
  - plan: 7 Task RED/GREEN（hosttest mock-MMIO + QEMU --expect-gic +
    SPI socket 注入 + SGI/IPI）
  - 纠正三处未验证路径: mk/components/aarch64.mk 与 mk/qemu.mk 不存在
    (真身在 image.mk:96-164 / run.mk), thirdpart/aarch64/ 不存在
    (GIC 常量真身在 kernel/arch/aarch64/reg.h)

  Co-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

---

### Task 1.1: RED — hosttest GICv2 driver（mock MMIO）

**Files:**
- Create: `hosttests/cases/test_gic_driver.c`
- Modify: `hosttests/Makefile`（TEST_BINS 追加 + 三条规则 + PHONY；追加位置：TEST_BINS 列表末尾 `test_lwip_rand.elf` 之后 ~line 83；规则追加在文件尾部 test_lwip_rand 规则块之后）

**Interfaces:**
- Consumes（尚不存在——这正是 RED 的来源）: `kernel/include/arch/aarch64/gic.h` 全部 API（见 Task 1.2 Produces）
- Produces: 可重复的失败命令 `make -C hosttests ... test_gic_driver`；测试夹具（mock MMIO 数组 + 断言集），Task 1.2 完成后原样转绿

**mock 设计**（写进测试文件头注释）：
```c
/* mock MMIO：两个普通数组，把地址交给 gic_dev_init。寄存器偏移沿用
 * kernel/arch/aarch64/reg.h 的值（host 侧在测试里重定义同值宏，不 include
 * reg.h——它带 aarch64 inline asm 访问器）。 */
static uint32_t gicd_mock[0x400];        /* 覆盖到 SGIR 0xF00 需 0x3C1 项，取整 0x400 */
static uint32_t gicc_mock[0x10];         /* 覆盖到 GICC_AIAR 0x20 → 取 0x10 不够，用 0x10? */
```
注意：GICC 偏移最大 GICC_AHPPIR=0x28，数组取 `gicc_mock[0x10]` 不够 → **取 `gicc_mock[0x20]`（0x80 字节）**。

- [ ] 写 `hosttests/cases/test_gic_driver.c` 骨架（真实代码）：
  ```c
  /* hosttests/cases/test_gic_driver.c — GICv2 driver 单元测试（spec §4.1/G1）。
   *
   * 编译【真实生产文件】kernel/arch/aarch64/gic_driver.c（host clang，无修改），
   * MMIO 是两个 mock 数组。覆盖: init(TYPER/IIDR)、分类、enable/disable、
   * priority/targets、handler 注册表、SGIR 编码、IAR/EOIR 往返(含 CPUID 位)。
   */
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

  /* handler 注册表探针 */
  static uint32_t hit_intid; static uint64_t hit_param; static uint32_t hit_calls;
  static struct pt_regs dummy_regs;
  static void probe_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
  { (void)regs; hit_intid = intid; hit_param = param; ++hit_calls; }
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
      /* SPI 33: ISENABLER[1] bit1, 优先级字节 33, 目标 byte → cpu0 */
      assert_eq(0, gic_irq_config(&dev, 33, true, 0x00, 0x01));
      assert_eq((uint32_t)(1u << 1), rd(gicd_mock, M_GICD_ISENABLER + 4));
      assert_eq(0x00u, (rd(gicd_mock, M_GICD_IPRIORITYR + 8) >> 8) & 0xff);
      assert_eq(0x01u, rd(gicd_mock, M_GICD_ITARGETSR + 8) & 0xff);
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
      TEST_SUITE("IAR/EOIR roundtrip (CPUID 位)");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      uint32_t iar = (2u << 10) | 35u;                  /* SGI 风格: CPUID=2 */
      gicc_mock[M_GICC_IAR / 4] = iar;
      assert_eq((int)iar, (int)gic_ack(&dev));          /* 原始值, 含 CPUID */
      gic_eoi(&dev, iar);
      assert_eq((int)iar, (int)rd(gicc_mock, M_GICC_EOIR));  /* 原样写回 — D7 修复 */
      gicc_mock[M_GICC_IAR / 4] = 1023;                 /* spurious */
      assert_eq(1023, (int)gic_ack(&dev));
  }

  static void suite_dispatch(void)
  {
      TEST_SUITE("gic_dev_dispatch");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      hit_calls = 0;
      assert_eq(0, gic_register_handler(35, probe_handler, 0x5678, "d35"));
      gicc_mock[M_GICC_IAR / 4] = 35;
      gic_dev_dispatch(&dev, &dummy_regs);
      assert_eq(1, (int)hit_calls);
      assert_eq(35, (int)hit_intid);
      assert_eq(0x5678, (int)hit_param);
      assert_eq(35, (int)rd(gicc_mock, M_GICC_EOIR));   /* handler 后 EOI */
      /* spurious: 不写 EOIR、不调 handler */
      gicc_mock[M_GICC_IAR / 4] = 1023; gicc_mock[M_GICC_EOIR / 4] = 0;
      gic_dev_dispatch(&dev, &dummy_regs);
      assert_eq(1, (int)hit_calls);
      assert_eq(0, (int)rd(gicc_mock, M_GICC_EOIR));
      /* unexpected: 无 handler → 仍 EOI（防 GIC 锁死, 对齐 time.c:105-115 语义） */
      gicc_mock[M_GICC_IAR / 4] = 60;
      gic_dev_dispatch(&dev, &dummy_regs);
      assert_eq(1, (int)hit_calls);
      assert_eq(60, (int)rd(gicc_mock, M_GICC_EOIR));
      gic_unregister_handler(35);
  }

  static void suite_sgi(void)
  {
      TEST_SUITE("SGIR encoding + set_pending");
      mock_reset(2); gic_dev_init(&dev, gicd_mock, gicc_mock);
      gic_send_sgi(&dev, 5, 0x03, GICD_SGIR_FILTER_LIST);
      assert_eq((int)(((0u << 24) | (3u << 16) | 5u)), (int)rd(gicd_mock, M_GICD_SGIR));
      gic_send_sgi(&dev, 0, 0, GICD_SGIR_FILTER_OTHERS);
      assert_eq((int)(1u << 24), (int)rd(gicd_mock, M_GICD_SGIR));
      gic_set_pending(&dev, 40);                        /* ISPENDR 测试注入 */
      assert_eq((uint32_t)(1u << 8), rd(gicd_mock, M_GICD_ISPENDR + 4));
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
  - TEST_BINS 列表（`$(TEST_BLD)/test_lwip_rand.elf` 之后）追加：
    ```make
    $(TEST_BLD)/test_gic_driver.elf \
    ```
  - `.PHONY` 行（`test_lwip_rand` 旁）追加 `test_gic_driver`。
  - 文件尾部追加规则（镜像 test_lwip_rand 模式，hosttests/Makefile 尾部 LWIP 块）：
    ```make
    # GICv2 driver hosttest: host-compile the PRODUCTION
    # kernel/arch/aarch64/gic_driver.c against mock-MMIO arrays (spec §4.1).
    # gic_driver.c is dependency-free (no arch asm, no UART logging); the
    # pt_regs type is forward-declared in <arch/aarch64/gic.h>.
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
  hosttests/cases/test_gic_driver.c:8:10: fatal error: 'arch/aarch64/gic.h' file not found
  ```
  留存输出（RED 证据）。**不 commit**（与 Task 1.2 同 commit）。

---

### Task 1.2: GREEN — gic.c 泛化真实现

**Files:**
- Create: `kernel/include/arch/aarch64/gic.h`（公共 API 头）
- Create: `kernel/arch/aarch64/gic_driver.c`（纯逻辑 driver，host 可编译）
- Modify: `kernel/arch/aarch64/gic.c`（整文件重写为生产 wrapper，`gic_init/gic_cpu_init` 对外签名不变——main.c:247 与 smp.c:208 的调用点零改动）
- Modify: `kernel/include/arch/aarch64/smp.h:9-10`（`gic_init/gic_cpu_init` 声明改为 include `<arch/aarch64/gic.h>` 后保留，避免重复声明漂移——核对后若一致可不动）

**Interfaces:**
- Consumes: reg.h 的寄存器偏移常量（kernel/arch/aarch64/reg.h:27-65，全部已存在，含 GICD_ICENABLER/ISPENDR/ITARGETSR/SGIR）；`dtb_gicd_base()/dtb_gicc_base()/dtb_cntp_ppi()`（kernel/arch/aarch64/dtb.c:16-19）
- Produces（后续 Task 依赖的精确签名）：
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
  };

  /* hw 层 —— 全部经 dev 指针；返回 0 成功 / 非 0 失败；不打印 */
  int  gic_dev_init(struct gic_dev *dev, volatile uint32_t *gicd, volatile uint32_t *gicc);
  void gic_dev_dist_enable(struct gic_dev *dev);    /* GICD_CTLR = 1 */
  void gic_dev_dist_disable(struct gic_dev *dev);
  void gic_dev_cpu_enable(struct gic_dev *dev);     /* PMR=0xff + GICC_CTLR=1 + dsb/isb */
  enum gic_irq_type gic_irq_type(uint32_t intid);
  int  gic_irq_config(struct gic_dev *dev, uint32_t intid, bool enable,
                      uint8_t prio, uint8_t targets); /* IGROUPR0+ISENABLER/ICENABLER
                                                       + IPRIORITYR + ITARGETSR(仅 SPI) */
  int  gic_irq_enable(struct gic_dev *dev, uint32_t intid);
  int  gic_irq_disable(struct gic_dev *dev, uint32_t intid);
  uint32_t gic_ack(struct gic_dev *dev);            /* 原始 IAR（含 CPUID 位） */
  void gic_eoi(struct gic_dev *dev, uint32_t iar);  /* 原样写回完整 IAR (spec §2.3/D7) */
  void gic_send_sgi(struct gic_dev *dev, uint32_t sgi, uint8_t targets, uint8_t filter);
  void gic_set_pending(struct gic_dev *dev, uint32_t intid); /* ISPENDR — 测试注入 */

  /* handler 注册表 —— 模块级全局（Phase 1 非 per-CPU, spec §4.1） */
  int  gic_register_handler(uint32_t intid, gic_handler_fn fn, uint64_t param,
                            const char *name);       /* 0 / -1 越界 / -2 已注册 */
  int  gic_unregister_handler(uint32_t intid);       /* 0 / -1 越界 / -3 未注册 */
  gic_handler_fn gic_get_handler(uint32_t intid, uint64_t *param_out);

  /* 通用 dispatch（trap.c 的 el1_irq 是它的三行壳） */
  void gic_dev_dispatch(struct gic_dev *dev, struct pt_regs *regs);

  /* 生产 wrapper（gic.c）—— 基址来自 DTB；签名兼容旧调用点 */
  int  gic_irq_configure(uint32_t intid, bool enable, uint8_t prio, uint8_t targets);
  void gic_force_pending(uint32_t intid);            /* 探针用 wrapper */
  #endif
  ```

步骤：

- [ ] 写 `kernel/include/arch/aarch64/gic.h`：内容即上面的 Produces（去掉注释中的出处标注亦可，签名逐字保持）。
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
      dev->gicd = gicd; dev->gicc = gicc;
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
      /* SPI 路由；SGI/PPI 的 ITARGETSR 只读 banked, 不写 */
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
      __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
  }
  ```
  注意：`__asm__ dsb/isb` 在 host x86_64 编译会失败——**把这两条移出 driver**：
  `gic_dev_cpu_enable` 只写 PMR/CTLR；屏障放在生产 wrapper `gic_cpu_init()` 里
  （gic.c:24 已有 `dsb sy; isb`，保持原位）。hosttest 的 `suite_cpu_iface` 只断言
  PMR/CTLR 两个 mock 值——**Task 1.1 的该 suite 与此一致，无需改**。
- [ ] 实现 ack/eoi/sgi/pending：
  ```c
  uint32_t gic_ack(struct gic_dev *dev) { return r32(dev->gicc, GICC_IAR); }

  void gic_eoi(struct gic_dev *dev, uint32_t iar)
  { w32(dev->gicc, GICC_EOIR, iar); }        /* 完整 IAR 原样写回 — D7 修复 */

  void gic_send_sgi(struct gic_dev *dev, uint32_t sgi, uint8_t targets, uint8_t filter)
  {
      w32(dev->gicd, GICD_SGIR,
          ((uint32_t)(filter & 3u) << 24) | ((uint32_t)targets << 16) | (sgi & 0xfu));
  }

  void gic_set_pending(struct gic_dev *dev, uint32_t intid)
  { w32(dev->gicd, GICD_ISPENDR + (intid / 32u) * 4u, 1u << (intid % 32u)); }
  ```
- [ ] 实现 handler 表 + dispatch（真实代码）：
  ```c
  struct gic_handler_slot {
      gic_handler_fn fn;
      uint64_t param;
      const char *name;
  };
  static struct gic_handler_slot handlers[GIC_HANDLER_MAX];

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

  /* unexpected 回调：生产 wrapper 注入 PL011 打印；hosttest 保持静默 */
  void gic_driver_set_unexpected(void (*cb)(uint32_t intid));
  static void (*unexpected_cb)(uint32_t) = 0;

  void gic_dev_dispatch(struct gic_dev *dev, struct pt_regs *regs)
  {
      uint32_t iar = gic_ack(dev);
      uint32_t intid = iar & 0x3ffu;
      if (intid == GIC_INTID_SPURIOUS) return;          /* 绝不写 EOIR */
      uint64_t param = 0;
      gic_handler_fn fn = gic_get_handler(intid, &param);
      if (fn) fn(intid, param, regs);                   /* 设备清源在 EOI 前 */
      else if (unexpected_cb) unexpected_cb(intid);     /* 对齐 time.c:105-115 语义 */
      gic_eoi(dev, iar);
  }
  ```
  （`gic_driver_set_unexpected` 声明补进 gic.h Produces 列表——后续 Task 引用同名。）
- [ ] 重写 `kernel/arch/aarch64/gic.c` 为生产 wrapper（真实代码全文）：
  ```c
  /* 生产 wrapper：driver 核心(gic_driver.c) + DTB 基址 + UART 日志。
   * gic_init/gic_cpu_init 对外签名不变（main.c:247 / smp.c:208 调用点零改动）。 */
  #include <stdint.h>
  #include <arch/aarch64/boot_log.h>
  #include <arch/aarch64/dtb.h>
  #include <arch/aarch64/smp.h>
  #include <arch/aarch64/gic.h>

  static struct gic_dev g_gic;
  struct gic_dev *gic_dev_current(void) { return &g_gic; }

  static void log_unexpected(uint32_t intid)            /* driver 回调 → PL011 */
  {
      kputs("[gic] unexpected IRQ intid=");
      kputu(intid);
      kputs("\n");
  }

  void gic_cpu_init(void)                                /* 每核各跑一次（banked） */
  {
      gic_dev_cpu_enable(&g_gic);
      /* banked SGI/PPI 白名单：SGI 0（IPI, Task 3）+ CNTP PPI（dtb） */
      (void)gic_irq_config(&g_gic, 0, true, 0x00, 0x00);
      uint32_t cntp = dtb_cntp_ppi();
      (void)gic_irq_config(&g_gic, cntp, true, 0x00, 0x00);
      __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
  }

  void gic_init(void)
  {
      if (gic_dev_init(&g_gic, (volatile uint32_t *)dtb_gicd_base(),
                       (volatile uint32_t *)dtb_gicc_base()) != 0) {
          log_err("[gic] FATAL: GICD IIDR=0\n");
          for (;;) __asm__ __volatile__("wfi" ::: "memory");
      }
      gic_driver_set_unexpected(log_unexpected);
      gic_dev_dist_enable(&g_gic);
      gic_cpu_init();
      log_info("[gic] GICv2 driver: intids=");
      kputu(g_gic.nr_intids);
      log_info(", CPU interface @ 0x");
      kputx(dtb_gicc_base());
      log_info("\n");
  }

  int gic_irq_configure(uint32_t intid, bool enable, uint8_t prio, uint8_t targets)
  { return gic_irq_config(&g_gic, intid, enable, prio, targets); }

  void gic_force_pending(uint32_t intid)
  { gic_set_pending(&g_gic, intid); }
  ```
  注意与旧行为的差异（有意）：gic_cpu_init 现在还使能 SGI 0（Task 3 前置，无害——
  SGI 只有软件写 SGIR 才会来）；打印文案改为 `[gic] GICv2 driver: intids=N, ...`
  （harness --expect-gic 断言的 marker，spec §7.2）。
- [ ] **跑 GREEN（hosttest）**：
  ```sh
  make -C hosttests PROFILE=aarch64-clang \
       OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver
  ```
  预期：全部 suite 通过，exit 0，末行 `test_gic_driver: N total, N passed, 0 failed`。
  （Task 1.1 的 RED 在此刻转绿——同一份测试文件零改动。）
- [ ] **跑内核冒烟**（确认 wrapper 无回归）：
  ```sh
  make PROFILE=aarch64-clang aarch64-uefi && \
  timeout 60 qemu-system-aarch64 -M virt,gic-version=2,acpi=off -cpu cortex-a53 \
    -smp 2 -m 512 -drive if=pflash,format=raw,file=build/aarch64-clang/image/QEMU_EFI.fd \
    -drive if=none,file=build/aarch64-clang/image/aarch64-uefi.img,format=raw,readonly=on,id=disk \
    -device virtio-blk-device,drive=disk -serial stdio -display none -no-reboot 2>&1 | head -40
  ```
  注意：此命令需要 -dtb（生产固件不透出 DTB）。**用现成回归代替**：
  ```sh
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：9/9 case PASS（`[gic] GICv2 driver: intids=` 新行出现且不影响既有断言——
  kernel_failure 只匹配 `[smp|spinlock]` 前缀的 FATAL/PANIC/FAIL/DEGRADED，
  aarch64_uefi_smp.py:230-236，新行不误伤）。
  若此刻 time.c 仍在用 gicc_read32（reg.h 访问器）——没冲突，它继续工作（EL1h 槽
  还是老路径），Task 2.2 才切。
- [ ] commit（含 Task 1.1 的测试）：
  ```sh
  git add hosttests/cases/test_gic_driver.c hosttests/Makefile \
          kernel/include/arch/aarch64/gic.h kernel/arch/aarch64/gic_driver.c \
          kernel/arch/aarch64/gic.c kernel/include/arch/aarch64/smp.h
  git commit -m "feat(aarch64): GICv2 driver 泛化 + mock-MMIO hosttest

  - gic_driver.c: 指针式 MMIO(struct gic_dev) + SGI/PPI/SPI/invalid 分类
    + enable/disable/priority/targets + handler 注册表 + gic_dev_dispatch
    (IAR→查表→handler→EOIR 原样回写完整 IAR, 修 D7 CPUID 位丢失)
    + SGIR 编码 + ISPENDR 测试注入; 零依赖可 host 编译
  - gic.c 重写为生产 wrapper: DTB 基址 + PL011 日志 + unexpected 回调;
    gic_init/gic_cpu_init 签名不变(main.c/smp.c 零改动);
    gic_cpu_init 额外 banked 使能 SGI 0(Task 3 前置)
  - hosttests/test_gic_driver: mock MMIO 数组覆盖 init(TYPER/IIDR)/分类/
    enable(ICENABLER)/prio/targets/注册表/SGIR/EOIR 往返/dispatch 三分支

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
- Modify: `qemutests/aarch64_uefi_smp.py`（新增 `--expect-gic` flag；断言函数 + self_test fixtures；改动点：argparse ~550 行区、self_test() ~99-227 区、passed() ~263-348 区）

**Interfaces:**
- Consumes: 既有 `passed()/run_case()/main()` 结构（aarch64_uefi_smp.py:263/442/539）
- Produces: `--expect-gic` flag（Task 2.2 把它加进 run.mk:167 的标准 target；Task 3.1 在同一 flag 下追加 IPI 断言）；断言的 marker 行（kernel 侧由 Task 2.2 产出）：
  - `[gic] GICv2 driver: intids=` 前缀行恰一条
  - `[gic] dispatch ready` 恰一条
  - `[gic-probe] save-restore OK` 恰一条，且 `[gic-probe] save-restore FAIL` 出现即拒绝
  - `[gic-probe] unexpected intid=40 survived` 恰一条
- **本 Task 不改任何 kernel 侧文件**——RED 就是对未改内核跑新断言。

- [ ] 在 `passed()` 前新增断言函数（真实代码；插在 `hard_kernel_failure` 之后 ~line 261）：
  ```python
  def gic_evidence_ok(text: str) -> bool:
      """--expect-gic: GICv2 框架证据（spec §7.2）。
      断言 marker 恰一条（多打/漏打都拒），clobber FAIL 行出现即拒。"""
      text = text.replace("\r", "")
      if re.search(r"^\[gic-probe\][^\n]*\bFAIL\b", text, re.MULTILINE):
          return False
      for pattern in (
          r"^UEFI-A64: ",           # 占位防误配（下述才是真断言）
      ):
          pass
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
  （写实现时去掉占位 for 循环，保留 checks 四元组循环；缩进与文件风格一致。）
- [ ] `passed()` 签名加 `expect_gic: bool = False`（镜像 expect_selftest，line 263），函数体在 `ram_summary_ok` 检查后追加：
  ```python
      if expect_gic and not gic_evidence_ok(text):
          return False
  ```
- [ ] `acceptance_evidence()`（line 382-384）透传：
  ```python
  expect_gic = getattr(args, "expect_gic", False)
  if args.expect_no_ack is not None:
      return degraded_passed(text, expect_selftest=expect_selftest)  # no-ack 不要求 gic
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
      # --expect-gic: 四条 marker 恰一条; FAIL 行拒; 漏任一拒。
      gic_log = current_log_for_2_cpus + "".join([
          "[gic] GICv2 driver: intids=96\n",
          "[gic] dispatch ready\n",
          "[gic-probe] save-restore OK\n",
          "[gic-probe] unexpected intid=40 survived\n",
      ])
      assert passed(gic_log, cpus=2, expect_gic=True), "all gic markers present must pass"
      base = current_log_for_2_cpus
      for marker in ("[gic] GICv2 driver: intids=96\n", "[gic] dispatch ready\n",
                     "[gic-probe] save-restore OK\n",
                     "[gic-probe] unexpected intid=40 survived\n"):
          assert not passed(base + marker, cpus=2, expect_gic=True), \
              f"missing {marker.strip()} must reject"
      assert not passed(gic_log.replace("save-restore OK", "save-restore FAIL"),
                        cpus=2, expect_gic=True), "clobber FAIL must reject"
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
  `[gic] dispatch ready` / `[gic-probe] ...` 行（grep 验证并留存）。
  注意：此刻 Task 1.2 已并入（`[gic] GICv2 driver: intids=` 行可能已存在），
  失败点应落在 dispatch/probe 三条 marker——把 stdout.log 的
  `grep -c 'gic' <log>` 结果记进 RED 证据。
  **不 commit**（与 Task 2.2/2.3 同 commit）。

---

### Task 2.2: GREEN — entry.S 全量 save/restore + pt_regs_t + 通用 dispatch（trap.c）

**Files:**
- Modify: `kernel/include/arch/aarch64/regs.h:19-28`（pt_regs_t 加 x30 + Section 2 偏移常量）
- Modify: `kernel/arch/aarch64/entry.S:61-79`（EL1h IRQ 槽改 `b el1_irq_entry`；表后新增 el1_irq_entry）
- Modify: `kernel/arch/aarch64/trap.c`（el1_irq 三行壳 + arch_install_exception_vectors 保持 no-op）
- Modify: `kernel/arch/aarch64/time.c`（删 el1_irq_dispatch:96-133 的硬编码比较，tick 改注册 handler；导出 `g_ticks` 给探针）
- Create: `kernel/arch/aarch64/irq_probe.c`（clobber 探针 + unexpected-intid 探针，`#if OS01_SELFTEST`）
- Modify: `kernel/arch/aarch64/main.c`（SELFTEST 块里调探针；注册 tick handler 后打 `[gic] dispatch ready`）
- Modify: `kernel/arch/aarch64/gic.c`（`gic_init` 尾部打 dispatch ready 由 main.c 打——二选一，选 main.c，见步骤）
- Modify: `mk/components/run.mk:167`（test-aarch64-uefi-smp 的 python 参数追加 `--expect-gic`）

**Interfaces:**
- Consumes: Task 1.2 的 `gic_dev_current()/gic_dev_dispatch()/gic_register_handler()/gic_ack()/gic_eoi()/gic_irq_configure()/gic_force_pending()`；`arch_local_irq_enable/disable`（arch/irq.h:88-96）
- Produces（Task 2.3/3.2 依赖）：
  ```c
  /* trap.c —— entry.S 的 el1_irq_entry bl 到这里 */
  void el1_irq(struct pt_regs *regs);
  /* gic.c */
  struct gic_dev *gic_dev_current(void);
  /* time.c —— 探针/harness 观察 */
  extern volatile uint64_t g_ticks;      /* 由 static 改为全局导出 */
  bool arch_tick_start(void);            /* 内部注册 cntp handler（签名不变） */
  /* irq_probe.c（OS01_SELFTEST 门控） */
  void gic_clobber_probe(void);          /* 打印 save-restore OK/FAIL 行 */
  void gic_unexpected_probe(void);       /* 软件 pend SPI 40, 打印 survived 行 */
  ```

步骤（先内核侧代码就位、用旧 entry.S 跑出探针 RED，再改 entry.S 转绿）：

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
  entry.S 需要能 include 它：entry.S 目前无 include；在文件头加
  `#include <arch/aarch64/regs.h>`（kernel/Makefile .S 规则走 C 预处理器，
  kernel/Makefile:222-224，-Iinclude 已有，kernel/Makefile:92）。regs.h 的 C 段被
  `#ifndef __ASSEMBLER__` 挡住，.S 只见 Section 2 宏——安全。
- [ ] 写 `kernel/arch/aarch64/irq_probe.c`（真实代码全文；`#if OS01_SELFTEST` 门控，
  文件级 include 门控避免非 SELFTEST 构建拉进符号）：
  ```c
  /* 破坏性探针（spec §7.3）——证明 entry.S save/restore 真实生效。
   *
   * clobber 探针: 哨兵进 x3/x4/x5/x18 → wfi 等一个 tick → 校验。
   * 必须整体在一个 asm 块内（编译器不能替它恢复哨兵）。旧 entry.S（零保存）
   * 下 C dispatch 按 AAPCS64 可自由毁 x0-x17 → 必红; 全量保存后必绿。
   *
   * unexpected 探针: 软件写 GICD_ISPENDR 置无 handler 的 SPI 40 →
   * dispatch 打 unexpected + EOI → 再等一个 tick 证明存活。 */
  #if OS01_SELFTEST
  #include <stdint.h>
  #include <stdbool.h>
  #include <arch/aarch64/gic.h>

  extern volatile uint64_t g_ticks;      /* time.c 导出 */

  void gic_clobber_probe(void)
  {
      uint64_t bad = 0;
      __asm__ __volatile__(
          "mov  x3,  #0x1111\n\t"
          "mov  x4,  #0x2222\n\t"
          "mov  x5,  #0x3333\n\t"
          "mov  x18, #0x4444\n\t"
          "adrp x6, g_ticks\n\t"
          "add  x6, x6, :lo12:g_ticks\n\t"
          "ldr  x7, [x6]\n"
          "1: wfi\n\t"
          "ldr  x8, [x6]\n\t"
          "cmp  x8, x7\n\t"
          "b.ls 1b\n\t"                  /* 等至少一个 tick (无符号比较) */
          "cmp  x3,  #0x1111\n\t b.ne 2f\n\t"
          "cmp  x4,  #0x2222\n\t b.ne 2f\n\t"
          "cmp  x5,  #0x3333\n\t b.ne 2f\n\t"
          "cmp  x18, #0x4444\n\t b.ne 2f\n\t"
          "mov  %0, #0\n\t b    3f\n"
          "2: mov  %0, #1\n"
          "3:"
          : "=r"(bad)
          :
          : "x3", "x4", "x5", "x6", "x7", "x8", "x18", "cc", "memory");
      if (bad == 0) kputs("[gic-probe] save-restore OK\n");
      else          kputs("[gic-probe] save-restore FAIL regs=x3/x4/x5/x18\n");
  }

  void gic_unexpected_probe(void)
  {
      extern void kputs(const char *);
      uint64_t before = g_ticks;
      gic_force_pending(40);             /* SPI 40 无 handler → unexpected 路径 */
      uint64_t deadline = arch_cycle_counter() + arch_cycle_freq() * 2;
      while (g_ticks == before) {
          if ((uint64_t)arch_cycle_counter() > deadline) {
              kputs("[gic-probe] unexpected intid=40 TIMEOUT\n");
              return;
          }
          arch_cpu_pause();
      }
      kputs("[gic-probe] unexpected intid=40 survived\n");
  }
  #endif
  ```
  补 include：`<arch/cpu.h>`（arch_cycle_counter/arch_cpu_pause，kernel/include/arch/cpu.h:78-90）
  与 `void kputs(const char *)` 声明（boot_log.h 更好：`#include <arch/aarch64/boot_log.h>`）。
  extern 声明去重后文件头统一 include。
- [ ] 改 `kernel/arch/aarch64/time.c`：
  - `static volatile uint64_t g_ticks`（line 39）→ `volatile uint64_t g_ticks`（去 static，导出）。
  - 删除 `el1_irq_dispatch()`（96-133 整段），新增注册式 handler（真实代码）：
    ```c
    #include <arch/aarch64/gic.h>
    #include <arch/aarch64/dtb.h>

    /* 注册进 GIC handler 表的 tick ISR（顺序契约不变: 重装 TVAL → EOI 由
     * dispatch 在返回后做 → 打印。EOI 移交 dispatch, handler 只做设备侧）。 */
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
    ```
    注意顺序差：EOI 现在发生在 handler 返回之后（dispatch 统一做），比旧路径
    （time.c:118-119 打印前 EOI）晚一个打印的距离——**TVAL 重装仍在最前**，
    phase1 spec §2.3 的"先重装避免丢 tick"核心不变；EOI 后移只延长该 INTID 的
    active 窗口（同优先级不嵌套本来就掩着），行为等价。在函数头注释里写明。
  - `arch_tick_start()` 尾部（return true 前）注册：
    ```c
        if (gic_register_handler(dtb_cntp_ppi(), cntp_tick_handler, 0,
                                 "cntp-tick") != 0)
            return false;
    ```
- [ ] 改 `kernel/arch/aarch64/trap.c`（真实代码核心）：
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
  - main.c 的 `#if OS01_SELFTEST` 区（224-240 之后、dtb_init 之前不行——探针要
    在 IRQ enable 后）。在 `arch_local_irq_enable()`（main.c:261）与最终 halt 循环
    （263）之间插入：
    ```c
    #if OS01_SELFTEST
        kputs("[gic] dispatch ready\n");
        gic_clobber_probe();
        gic_unexpected_probe();
    #endif
    ```
    （`[gic] dispatch ready` 打点放这里：VBAR 已装(188-189)、handler 已注册
    （arch_tick_start 内）、dispatch 链闭合。）
  - 构建 + 手跑一次，抓 serial：
    ```sh
    make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi
    python3 qemutests/aarch64_uefi_smp.py --cpus 1 --repeat 1 --timeout 90 \
      --expect-selftest --diagnostic-dtb=auto \
      --firmware build/aarch64-clang/image/QEMU_EFI.fd \
      --image build/aarch64-clang/image/aarch64-uefi.img \
      --qemu qemu-system-aarch64 --log-dir /tmp/gic-clobber-red-$$ ; \
    grep -a "gic" /tmp/gic-clobber-red-*/cpus-1-run-1.stdout.log
    ```
    **预期（RED 证据，旧 entry.S 零保存）**：
    `[gic-probe] save-restore FAIL regs=x3/x4/x5/x18` 出现（harness 本身 exit 1
    或因 dispatch ready 缺失/FAIL 行而失败均可，重点是 FAIL 行进日志）。
    若探针意外 OK：说明 -O2 下 dispatch 链恰好没碰这几个寄存器——把探针哨兵
    扩到 x0-x2/x6-x9 再跑（asm 块同构扩展），必须先见到 FAIL 才继续。
- [ ] **改 entry.S**（关键 GREEN 步骤）：
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
  # 预期三行 marker 全在, 且是 OK/survived 不是 FAIL
  ```
- [ ] x86 零回归抽查：
  ```sh
  make PROFILE=x86_64-clang kernel.bin && git diff --stat master -- \
    kernel/arch/x86_64 kernel/include/arch/x86_64 kernel/intr
  # 预期: 构建成功; diff 为空(相对本分支起点 master)
  ```
- [ ] **不单独 commit**——与 Task 2.3 合并为"entry.S+dispatch+SPI"功能 commit。

**验证命令（本 Task 全量）**：
```sh
python3 qemutests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang test-aarch64-uefi-smp
make PROFILE=x86_64-clang kernel.bin
```

---

### Task 2.3: GREEN — SPI 测试中断源走 handler 表（PL011 RX, INTID 33）

**Files:**
- Modify: `kernel/arch/aarch64/dtb_parse.c:122-127`（pl011 节点补 `interrupts` 解析 → `pl011_spi`）
- Modify: `kernel/include/arch/aarch64/dtb.h:16-20`（struct aarch64_platform_info 加 `uint32_t pl011_spi;`）+ 访问器声明
- Modify: `kernel/arch/aarch64/dtb.c`（新增 `uint32_t dtb_pl011_spi(void)`，实现放 dtb_pl011_base 旁 ~line 18）
- Modify: `kernel/arch/aarch64/pl011.c`（新增 RX 中断三函数，文件尾部 kputx 之后）
- Create: `kernel/arch/aarch64/spi_test.c`（`#if OS01_SELFTEST`）
- Modify: `kernel/arch/aarch64/main.c`（SELFTEST 区在 arch_tick_start 成功后、irq_enable 前调 `gic_spi_test_init()`）
- Create: `qemutests/aarch64_gic_spi.py`（socket chardev 注入 harness）
- Modify: `mk/components/run.mk`（新增 `test-aarch64-gic-spi` target，放 test-aarch64-uefi-smp-no-ack 块之后 ~line 195）

**Interfaces:**
- Consumes: Task 1.2 `gic_register_handler/gic_irq_configure`；Task 2.2 `el1_irq` dispatch 链；`kputs/kputu`（pl011.c:88/99）
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
   * harness(qemutests/aarch64_gic_spi.py) 在看到 armed 行后向 PL011
   * 注入 1 字节并断言 handled 行。level 触发契约: handler 内先清设备源
   * （读 DR + ICR），EOI 由 dispatch 在返回后统一做（spec §2.3）。 */
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
          gic_irq_configure(intid, true, 0x00, 0x01) != 0) {   /* 路由 BSP(bit0) */
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
- [ ] 写 `qemutests/aarch64_gic_spi.py`（真实代码骨架；复用 aarch64_uefi_smp 的
  DTB 生成与命令拼装）：
  ```python
  #!/usr/bin/env python3
  """PL011 RX → GIC SPI 通路注入测试（spec §7.4）。

  用 -chardev socket 起 QEMU serial：读端扫 '[gic] spi-test armed intid=33'，
  向 socket 写 1 字节，断言 '[gic-spi] intid=33 handled count=1'。
  复用 aarch64_uefi_smp.generate_diagnostic_dtb（同目录 import）。
  """
  import argparse, os, socket, subprocess, sys, time, selectors
  from pathlib import Path
  sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
  from aarch64_uefi_smp import generate_diagnostic_dtb

  def qemu_command(args, dtb, sock):
      return [args.qemu, "-M", "virt,gic-version=2,acpi=off", "-cpu", "cortex-a53",
              "-smp", "2", "-m", "512",
              "-drive", f"if=pflash,format=raw,file={args.firmware}",
              "-drive", f"if=none,file={args.image},format=raw,readonly=on,id=disk",
              "-device", "virtio-blk-device,drive=disk",
              "-chardev", f"socket,id=ser0,path={sock},server=on,wait=off",
              "-serial", "chardev:ser0", "-display", "none",
              "-no-reboot", "-no-shutdown", "-dtb", dtb]

  def main() -> int:
      p = argparse.ArgumentParser()
      p.add_argument("--firmware"); p.add_argument("--image")
      p.add_argument("--qemu"); p.add_argument("--log-dir")
      p.add_argument("--timeout", type=float, default=90.0)
      args = p.parse_args()
      Path(args.log_dir).mkdir(parents=True, exist_ok=True)
      dtb = generate_diagnostic_dtb(args.qemu, args.log_dir, 2)
      sock = os.path.join(args.log_dir, "pl011.sock")
      proc = subprocess.Popen(qemu_command(args, dtb, sock),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
      log = bytearray(); ok = False
      try:
          client = None
          deadline = time.monotonic() + args.timeout
          while time.monotonic() < deadline and proc.poll() is None:
              if client is None:
                  try:
                      client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                      client.connect(sock)
                      client.setblocking(False)
                  except OSError:
                      try: client.close()
                      except Exception: pass
                      client = None
                      time.sleep(0.2); continue
              r, _, _ = selectors.DefaultSelector() and (), (), ()   # 见下: 用 select
              import select
              r, _, _ = select.select([client], [], [], 0.2)
              if r:
                  chunk = client.recv(4096)
                  if chunk:
                      log.extend(chunk)
                      text = log.decode("utf-8", "replace").replace("\r", "")
                      if "[gic] spi-test armed intid=" in text and not ok:
                          client.send(b"G")          # 注入 1 字节 → PL011 RX IRQ
                      if "[gic-spi] intid=" in text and "handled count=1" in text:
                          ok = True; break
          (Path(args.log_dir) / "serial.log").write_bytes(log)
          print(f'{{"event": "spi-case", "result": "{"PASS" if ok else "FAIL"}"}}')
          return 0 if ok else 1
      finally:
          proc.terminate()
          try: proc.wait(timeout=2)
          except subprocess.TimeoutExpired: proc.kill(); proc.wait()

  if __name__ == "__main__":
      sys.exit(main())
  ```
  （写实现时把上面临时的 selectors/select 混用清干净——只用 `select.select` 轮询
  0.2s；`chmod +x` 不必，用 `python3` 调。）
- [ ] `mk/components/run.mk` 新增 target（no-ack 块后，真实代码）：
  ```make
  # PL011 RX → GIC SPI 注入测试（spec §7.4）。复用 SMP 套件的固件/镜像/DTB 机制。
  .PHONY: test-aarch64-gic-spi
  test-aarch64-gic-spi:
  	$(call require_aarch64_uefi)
  	$(call require_capability,uefi)
  	$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi
  	python3 qemutests/aarch64_gic_spi.py \
  	  $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto) \
  	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
  	  --image "$(AARCH64_UEFI_DISK)" \
  	  --qemu "$(AARCH64_QEMU)" \
  	  --log-dir "$(OS01_ROOT)/test-results/aarch64-gic-spi/$$(date -u +%Y%m%dT%H%M%S)-$$$$"
  ```
  （harness 若不接 --diagnostic-dtb 参数则从命令行去掉该项——按实现统一。）
- [ ] **跑 GREEN**：
  ```sh
  make PROFILE=aarch64-clang test-aarch64-gic-spi
  ```
  预期：`{"event": "spi-case", "result": "PASS"}`，exit 0；
  `test-results/aarch64-gic-spi/*/serial.log` 里依次出现
  `[gic] spi-test armed intid=33` → `[gic-spi] intid=33 handled count=1`。
- [ ] **全量回归**（Task 2.2 + 2.3 合并验证）：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test && \
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：9/9 PASS（SPI armed 行是新增输出，不与既有断言冲突——kernel_failure
  只认 `[smp|spinlock]` 前缀，`spi-test arm FAIL` 走 `[gic]` 前缀不会被误杀，
  但 --expect-gic 的四条 marker 不含它；如需把它纳入拒绝集，在 gic_evidence_ok
  加 `^\[gic\][^\n]*\bFAIL\b` 拒绝规则——做）。
- [ ] commit（Task 2.1 + 2.2 + 2.3 合一）：
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
    (15对 stp + str x30 + 3 个系统状态) → bl el1_irq → 对称恢复 → eret
  - trap.c: el1_irq = gic_dev_dispatch 三行壳; time.c 删硬编码 intid
    比较, tick 改注册 handler (TVAL 重装仍最先, EOI 移交 dispatch)
  - 破坏性探针(OS01_SELFTEST): clobber(x3/x4/x5/x18 哨兵跨 tick) +
    unexpected(SPI 40 软件置 pending 存活); --expect-gic 进标准回归
  - SPI: dtb_parse 补 pl011 interrupts 解析(33); pl011 只开 RXIM;
    gic_spi_test_init 注册 handler; 新 harness aarch64_gic_spi.py 用
    chardev socket 注入 1 字节断言 handled count=1
  - EOIR 修复落地: dispatch 写回完整 IAR(D7)

  Co-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

**验证命令（本 Task 全量）**：
```sh
make PROFILE=aarch64-clang test-aarch64-gic-spi
python3 qemutests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang test-aarch64-uefi-smp
```

---

### Task 3.1: RED — SMP IPI 跨核 SGI 测试（harness 断言先行）

**Files:**
- Modify: `qemutests/aarch64_uefi_smp.py`（--expect-gic 追加 IPI 断言 + self_test fixtures）

**Interfaces:**
- Consumes: Task 2.1 的 `gic_evidence_ok/passed(expect_gic)`
- Produces: 断言的 marker 行（kernel 侧由 Task 3.2 产出）：
  - `[ipi] send sgi=0 filter=others` 恰一条
  - `[ipi] cpu=<n> received=<k>` 对 1..cpus-1 每核恰一条
  - `[ipi] summary targets=<cpus-1> received=<cpus-1> status=PASS` 恰一条（cpus=1 时 targets=0 received=0）

- [ ] `gic_evidence_ok` 的 checks 四元组追加（真实代码）：
  ```python
      checks += [
          (r"^\[ipi\] send sgi=0 filter=others$", 1),
          (r"^\[ipi\] summary targets=(\d+) received=(\d+) status=PASS$", 1),
      ]
  ```
  并在 checks 循环后追加 per-cpu 校验（需要 cpus 参数——把 `gic_evidence_ok(text)`
  改签名 `gic_evidence_ok(text, cpus)`，调用点 `passed()` 内透传）：
  ```python
      # 每个非 BSP 核恰一行 received（cpus=1 时无此行）
      ipi_cpus = re.findall(r"^\[ipi\] cpu=(\d+) received=(\d+)$", text, re.MULTILINE)
      if len(ipi_cpus) != cpus - 1 or {int(c) for c, _ in ipi_cpus} != set(range(1, cpus)):
          print(f"FAIL: ipi per-cpu lines {ipi_cpus}, want cpus 1..{cpus - 1}")
          return False
      # summary 行数值自洽（regex 捕获组在上面那条 check 里已匹配，这里复扫）
      m = re.search(r"^\[ipi\] summary targets=(\d+) received=(\d+) status=PASS$",
                    text, re.MULTILINE)
      if not m or tuple(map(int, m.groups())) != (cpus - 1, cpus - 1):
          return False
      # received=k 每核必须恰为 1（多发=风暴, 漏发=丢 IPI）
      for _, k in ipi_cpus:
          if int(k) != 1:
              return False
  ```
- [ ] `self_test()` 追加 fixtures（真实代码）：
  ```python
      # --expect-gic 的 IPI 断言（Task 3）：cpus=2 与 cpus=1 两形态 + 各类破坏。
      def with_ipi(log, cpus_n):
          lines = ["[ipi] send sgi=0 filter=others\n"]
          lines += [f"[ipi] cpu={c} received=1\n" for c in range(1, cpus_n)]
          lines += [f"[ipi] summary targets={cpus_n - 1} received={cpus_n - 1} status=PASS\n"]
          return log + "".join(lines)
      g2 = with_ipi(gic_log, 2)
      assert passed(g2, cpus=2, expect_gic=True), "ipi complete (2 cpus) must pass"
      g1 = with_ipi(gic_log, 1)
      assert passed(g1, cpus=1, expect_gic=True), "ipi targets=0 (1 cpu) must pass"
      assert not passed(with_ipi(gic_log, 2).replace("received=1\n", "", 1)
                        .replace("cpu=1 received", "cpu=1 received"),
                        cpus=2, expect_gic=True), "missing cpu=1 line must reject"
      assert not passed(gic_log + "[ipi] summary targets=1 received=1 status=PASS\n",
                        cpus=2, expect_gic=True), "summary without send/per-cpu must reject"
      assert not passed(with_ipi(gic_log, 2).replace("cpu=1 received=1",
                                                     "cpu=1 received=2"),
                        cpus=2, expect_gic=True), "received=2 must reject"
      assert not passed(with_ipi(gic_log, 2).replace("targets=1 received=1",
                                                     "targets=1 received=0"),
                        cpus=2, expect_gic=True), "received mismatch must reject"
  ```
  （第一条否定例的 replace 链写干净：直接用 `with_ipi(gic_log,2).replace("[ipi] cpu=1 received=1\n","")`。）
- [ ] **跑 harness 自测（应绿）**：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test
  ```
- [ ] **跑 RED**（对 Task 2.3 后的内核——无 IPI marker）：
  ```sh
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：**9/9 FAIL**（exit 1），stdout.log 无任何 `[ipi]` 行（grep 留存）。
  这一步同时证明 Task 2.x 的改动没顺手把 IPI 做了。
  **不 commit**（与 Task 3.2 同 commit）。

---

### Task 3.2: GREEN — SGI send + handler，SMP 1/2/4 ×3 全绿

**Files:**
- Modify: `kernel/arch/aarch64/smp.c:204-226`（secondary_idle 尾循环前开 DAIF.I）
- Create: `kernel/arch/aarch64/ipi_test.c`（`#if OS01_SELFTEST`）
- Modify: `kernel/arch/aarch64/main.c`（SELFTEST 区在 gic_unexpected_probe 之后、halt 循环前调 `gic_ipi_test(dtb_cpu_count())`）

**Interfaces:**
- Consumes: Task 1.2 `gic_send_sgi/gic_register_handler/GICD_SGIR_FILTER_OTHERS`；Task 2.2 `el1_irq` dispatch 链 + `g_ticks`；`aarch64_boot_percpu_t`（aarch64_percpu.h:21-22，cpu_id 在槽 offset 8）；TPIDR_EL1（BSP head.S:312-323 / AP head.S:650 都已设）；`arch_cycle_counter/arch_cycle_freq`（cpu.h:78-86）
- Produces:
  ```c
  /* ipi_test.c（OS01_SELFTEST） */
  void gic_ipi_test(uint32_t cpu_count);   /* 发 SGI 0 (filter=others) → 轮询 → 打 summary */
  ```

- [ ] 改 `secondary_idle`（smp.c:219-225 区；真实 diff）：
  ```c
      /* Includes a late AP: go=2 persists even if the BSP already resumed
       * ticks. APs keep their CNTP disabled; the only enabled banked lines
       * are SGI 0 (IPI) and — for the BSP — the CNTP PPI. Unmask DAIF.I
       * so the AP can take SGIs through el1_irq (GIC Phase 1, spec §7.5). */
      cntp_ctl_el0_write(cntp_ctl_el0_read() & ~UINT64_C(1));
      arch_local_irq_enable();
      __asm__ __volatile__("isb" ::: "memory");
      for (;;) arch_cpu_halt();
  ```
  （`arch_local_irq_enable` 来自 <arch/irq.h>，smp.c 已 include 链上有 arch/cpu.h；
  需补 `#include <arch/irq.h>`。这是**唯一的生产行为变化**，commit message 已声明。）
- [ ] 写 `kernel/arch/aarch64/ipi_test.c`（真实代码核心）：
  ```c
  /* SGI/IPI 跨核测试（spec §7.5）。BSP 发 SGI 0 (All others) → AP handler
   * 经 TPIDR_EL1 槽取本核逻辑号、只递增 per-CPU 计数（AP 不打印——多核并发
   * 写 PL011 会绞线, spec §8 R4）→ BSP 有界轮询（cntvct deadline）→ 打 summary。 */
  #if OS01_SELFTEST
  #include <stdint.h>
  #include <stdbool.h>
  #include <arch/irq.h>
  #include <arch/cpu.h>
  #include <arch/aarch64/gic.h>
  #include <arch/aarch64/dtb.h>
  #include <arch/aarch64/boot_log.h>
  #include "aarch64_percpu.h"

  #define IPI_SGI_ID      0u
  #define IPI_WAIT_SECONDS 2u

  static volatile uint32_t ipi_received[AARCH64_BOOT_MAX_CPUS];

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
      if (cpu < AARCH64_BOOT_MAX_CPUS)
          ipi_received[cpu] = ipi_received[cpu] + 1;   /* volatile 写, BSP 轮询 */
  }

  void gic_ipi_test(uint32_t cpu_count)
  {
      if (cpu_count > AARCH64_BOOT_MAX_CPUS) cpu_count = AARCH64_BOOT_MAX_CPUS;
      for (uint32_t i = 0; i < AARCH64_BOOT_MAX_CPUS; ++i) ipi_received[i] = 0;
      if (gic_register_handler(IPI_SGI_ID, ipi_handler, 0, "ipi0") != 0) {
          log_err("[ipi] register FAIL\n");
          return;
      }
      kputs("[ipi] send sgi=0 filter=others\n");
      gic_send_sgi(gic_dev_current(), IPI_SGI_ID, 0, GICD_SGIR_FILTER_OTHERS);

      uint64_t deadline = arch_cycle_counter()
                        + arch_cycle_freq() * IPI_WAIT_SECONDS;
      uint32_t got = 0;
      for (;;) {
          __asm__ __volatile__("dmb ish" ::: "memory");
          got = 0;
          for (uint32_t i = 1; i < cpu_count; ++i)
              if (ipi_received[i] != 0) ++got;
          if (got + 1 >= cpu_count) break;
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
      kputu(got);
      kputs(got + 1 >= cpu_count ? " status=PASS\n" : " status=FAIL\n");
  }
  #endif
  ```
  注意 `gic_register_handler(0, ...)` 在 `gic_cpu_init`（Task 1.2）已 banked 使能
  SGI 0 bit0——每核的 ISENABLER0 bank 都开，AP 无需再配。BSP 侧 handler 注册
  在发送前完成；共享表对 AP 立即可见（同一内核映像）。
- [ ] main.c 在 `gic_unexpected_probe()` 之后追加（同一 SELFTEST 块内）：
  ```c
      gic_ipi_test(dtb_cpu_count());
  ```
- [ ] **跑 GREEN**：
  ```sh
  python3 qemutests/aarch64_uefi_smp.py --self-test && \
  make PROFILE=aarch64-clang test-aarch64-uefi-smp
  ```
  预期：self-test passed；**9/9 case PASS**（cpus=1：targets=0 received=0 PASS；
  cpus=2：cpu=1 received=1；cpus=4：cpu=1/2/3 各 received=1，×3 重复）。
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
- [ ] G6 收尾自查：
  ```sh
  git diff --stat master -- kernel/arch/x86_64 kernel/include/arch/x86_64 kernel/intr
  # 预期: 空
  make PROFILE=x86_64-clang kernel.bin
  # 预期: 构建成功
  ```
- [ ] commit（Task 3.1 + 3.2 合一）：
  ```sh
  git add qemutests/aarch64_uefi_smp.py kernel/arch/aarch64/smp.c \
          kernel/arch/aarch64/ipi_test.c kernel/arch/aarch64/main.c
  git commit -m "feat(aarch64): SGI/IPI 跨核通路 + SMP harness 断言

  - gic_send_sgi(GICD_SGIR): filter=others 广播, SGI 0 作 IPI 载荷
  - ipi_test.c: BSP 发送 → AP handler 经 TPIDR_EL1 槽取逻辑号只递增
    计数(不打印, 防并发绞线) → BSP cntvct 有界轮询 → 逐核 + summary 行
  - secondary_idle: 尾循环前 arch_local_irq_enable()——本 Phase 唯一
    生产行为变化; AP banked 只使能 SGI 0 (CNTP 仍关, gic_cpu_init 白名单)
  - harness --expect-gic 追加 IPI 断言: send 恰一/每核 received=1 恰一/
    summary 数值自洽; cpus=1 形态 targets=0
  - SMP 1/2/4 ×3 全绿; SPI/hosttest/x86 构建零回归

  Co-Authored-By: Claude Code <noreply@anthropic.com>"
  ```

**验证命令（本 Task 全量）**：
```sh
python3 qemutests/aarch64_uefi_smp.py --self-test
make PROFILE=aarch64-clang test-aarch64-uefi-smp
make PROFILE=aarch64-clang test-aarch64-gic-spi
make -C hosttests PROFILE=aarch64-clang OS01_PROFILE_FILE=$PWD/mk/profiles/aarch64-clang.mk test_gic_driver
make PROFILE=x86_64-clang kernel.bin
```

---

## Self-Review 三查（实现完成后必须执行）

### 查 1：spec 覆盖率（G1-G6 → Task 映射）

| Spec 目标 | 承载 Task | 验证 |
|---|---|---|
| G1 driver 泛化 | 1.1 + 1.2 | hosttest 8 suite 全绿 + `test-aarch64-uefi-smp` 不回归 |
| G2 save/restore + pt_regs_t | 2.2 | clobber 探针 RED→GREEN 留档 + `_Static_assert` + `--expect-gic` marker |
| G3 通用 dispatch | 2.2 | `[gic] dispatch ready` + unexpected intid=40 survived + `[tick]` 不断流 |
| G4 SPI 通路 | 2.3 | `test-aarch64-gic-spi` PASS（armed → 注入 → handled count=1） |
| G5 SGI/IPI | 3.1 + 3.2 | SMP 1/2/4 ×3 全绿（cpus=1 为 targets=0 形态） |
| G6 范式对齐 + 零回归 | 全部 | `git diff --stat master -- kernel/arch/x86_64 kernel/include/arch/x86_64 kernel/intr` 为空；x86 kernel.bin 可构建 |

spec §5.2 布局（272B / 34 槽 / 16 对齐）↔ regs.h + entry.S 偏移逐条核对；
spec §2.3 EOIR 完整回写 ↔ hosttest suite_ack_eoi；spec §7.4 DTB 解析 ↔ dtb_parse.c。

### 查 2：占位符扫描

```sh
grep -rn "TODO\|FIXME\|XXX\|占位\|placeholder\|实现 X" \
  kernel/arch/aarch64/gic_driver.c kernel/arch/aarch64/gic.c \
  kernel/arch/aarch64/trap.c kernel/arch/aarch64/ipi_test.c \
  kernel/arch/aarch64/spi_test.c kernel/arch/aarch64/irq_probe.c \
  kernel/include/arch/aarch64/gic.h kernel/include/arch/aarch64/regs.h \
  hosttests/cases/test_gic_driver.c qemutests/aarch64_gic_spi.py
# 预期: 空（entry.S 的注释性文字不算）
```
同时检查 plan 里标注"写实现时清干净"的两处（Task 2.3 harness 的 select 混用、
Task 3.1 self_test 的 replace 链）确实已清理。

### 查 3：类型一致性（跨 Task 接口）

- `gic_handler_fn` 唯一定义于 gic.h；cntp_tick_handler / pl011_rx_handler /
  ipi_handler / probe_handler 四处签名逐字一致
  `(uint32_t intid, uint64_t param, struct pt_regs *regs)`。
- `struct gic_dev` 字段（gicd/gicc/nr_intids）在 gic.h / gic_driver.c / gic.c /
  test_gic_driver.c 四处一致；`gic_dev_current()` 返回类型 `struct gic_dev *`。
- PT_REGS_* 偏移 ↔ regs.h 字段序 ↔ entry.S stp/str 偏移三方核对
  （x30@240 / sp_el0@248 / elr@256 / spsr@264 / SIZE=272）。
- `gic_send_sgi(dev, sgi, targets, filter)` 参数序在 gic.h / ipi_test.c /
  test_gic_driver.c 一致（filter 是第 4 参）。
- marker 字符串逐字核对（harness regex ↔ kputs 文案）：
  `[gic] GICv2 driver: intids=` / `[gic] dispatch ready` /
  `[gic-probe] save-restore OK` / `[gic-probe] unexpected intid=40 survived` /
  `[gic] spi-test armed intid=` / `[gic-spi] intid=`+`handled count=` /
  `[ipi] send sgi=0 filter=others` / `[ipi] cpu=`+`received=` /
  `[ipi] summary targets=`+`received=`+`status=PASS`。

---

## 附：本 plan 引用的全部真实文件/行号依据（核查于 2026-09-17, HEAD=3a9fe16）

- kernel/arch/aarch64/: gic.c(43L), entry.S(117L), time.c(133L), trap.c(21L),
  smp.c(226L), head.S(755L, secondary_start 580-672, TPIDR BSP 312-323/AP 650),
  dtb.c(84L), dtb_parse.c(279L, gic 115-121, pl011 122-127, timer 128-135),
  pl011.c(136L), main.c(265L), make.config(45L), linker.ld(137L),
  aarch64_percpu.h(cpu_id@offset 8)
- kernel/include/arch/aarch64/: regs.h(facade 分发 25-31 的是上级 arch/regs.h;
  pt_regs_t 19-28), dtb.h(41L), smp.h(15L)
- x86_64 范式（只读）: regs.h:18-44, entry.S:3-49/28-49, irq.c:14-79,
  irq_hooks.c:65-85, kernel/intr/dispatch.c:9-30,
  kernel/include/intr/interrupt.h:16-52, kernel/include/arch/irq.h:47/65-109
- 构建/测试: kernel/Makefile:42-43/70-71/192-194/222-224,
  mk/components/run.mk:125-148/161-172/258, mk/components/image.mk:96-164,
  mk/profiles/aarch64-clang.mk:6/57-58, mk/targets/aarch64.mk:5-8,
  qemutests/aarch64_uefi_smp.py(579L: 16-34/99-227/230-236/263-348/387-398/
  406-439/442-536/539-574), hosttests/Makefile(TEST_BINS 55-83, 尾部 lwip 块),
  hosttests/include/test_framework.h
- 任务描述三处路径纠正：mk/components/aarch64.mk 与 mk/qemu.mk 不存在
  （真身 image.mk:96-164 / run.mk）；thirdpart/aarch64/inc/aarch64.h 不存在
  （GICv2 常量真身 kernel/arch/aarch64/reg.h:18-65）
