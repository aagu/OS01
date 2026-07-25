# 架构评审 — Group 1: Boot + 内核入口

> **审查日期**: 2026-07-25
> **覆盖文件**: `kernel/arch/x86_64/head.S`, `entry.S`, `trampoline.S`, `kernel/kernel/main.c`, `kernel/arch/x86_64/linker.ld`, `trampoline.ld`, `kernel/include/kernel/bootinfo.h`, `boot/uefi/main.c`, `kernel/include/kernel/arch/x86_64/gate.h`, `kernel/arch/x86_64/trap.c`

## 问题清单

| # | 级别 | 子系统 | 简述 | 状态 |
|---|------|--------|------|------|
| 1 | P1 | GDT/TSS | GDT TSS 位置与文档不一致，索引偏移 1 | 待处理 |
| 2 | P1 | SMP/TSS | `set_tss64()` 只操作全局 TSS，BSP 的 per-CPU `init_tss` 未初始化 | 待处理 |
| 3 | P1 | head.S | `ignore_int` handler 中 dead code（162-186 行不可达） | 待处理 |
| 4 | P2 | BootInfo ABI | bootloader 端 `boolean_t` vs 内核端 `uint8_t` 不一致 | 待处理 |
| 5 | P2 | GDT | slot 8-10 的 `.fill 100` 过度预分配 | 待处理 |
| 6 | P2 | head.S | GDT 注释未标注哪些 slot 被 TSS 运行时覆盖 | 待处理 |
| 7 | P2 | trampoline.S | 段选择器使用魔数而非命名常量 | 待处理 |

---

### [P1] 1. GDT TSS 位置与文档不一致，索引偏移 1

- **位置**: `kernel/arch/x86_64/head.S:106-109`, 行 253 注释; `kernel/include/kernel/arch/x86_64/gate.h:51`; `docs/architecture.md` GDT 表格
- **现象**: 
  - `head.S:107` 将 TSS descriptor 写入 `GDT_Table+64` (slot 8), 对应 selector 0x40
  - 静态 GDT 表格 slot 8 初始值为 KERNEL Data 32-bit (0x00cf92000000ffff), 被运行时覆盖
  - 注释 `/*10 ~ 11 TSS ... */` 与实际 slots 8-9 不符
  - `docs/architecture.md` 说 TSS 在 0x48/0x50, 但实际在 0x40/0x48
  - `set_tss_descriptor(n, addr)` 写入 `GDT_Table[n]` + `GDT_Table[n+1]`，调用方需传入正确的 index
- **建议**: 
  1. 统一 GDT 索引常量为 `#define TSS_GDT_IDX 8` / `#define TSS_GDT_SEL 0x40`
  2. 修正 `head.S` 中注释
  3. 修正 `docs/architecture.md` GDT 表格
  4. 确保 `set_tss_descriptor()` 调用方使用一致的 index

### [P1] 2. `set_tss64()` 只操作全局 TSS，BSP 的 per-CPU `init_tss` 未初始化

- **位置**: `kernel/kernel/main.c:140` 和 `271-272`
- **现象**: 
  - `kernel_main:140`: `set_tss64(TSS64_Table, ...)` 填充全局 TSS (位于 head.S 的 `.data` 段)
  - `kernel_main:271-272`: BSP 设置 `percpu_data[0].tss = &init_tss[0]`, `percpu_data[0].tss_hw = TSS64_Table`
  - `init_tss[0]` 从未被 `set_tss64()` 初始化——全是零
  - `__switch_to` 虽在每次切换时更新 per-CPU TSS，但在第一次切换前（包括 BSP 上的异常/中断使用 IST 栈），`init_tss[0]` 的 rsp0 和 IST 字段无效
  - AGENTS.md 已标注: "Per-CPU init_tss[NR_CPUS] 存在但 GDT slot 仍需要 per-CPU 更新"
- **建议**: BSP 初始化时对 `init_tss[0]` 同步调用 `set_tss64()`:
  ```c
  set_tss64(&init_tss[0], 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7800, 0x7400, 0, 0, 0, 0);
  ```
  或确保 `percpu_data[0].tss = &init_tss[0]` 的赋值在 TSS 填充之后。

### [P1] 3. `ignore_int` handler 中 dead code 残留

- **位置**: `kernel/arch/x86_64/head.S:125-187`
- **现象**:
  - `ignore_int` 打印错误消息后进入 `Loop: hlt; jmp Loop` 死循环 (L162-164)
  - 后续 pop 序列 (L166-186) 和 `iretq` (L187) 完全不可达
  - 虽无害（`sys_vector_install()` 后 `ignore_int` 被覆盖），但 20+ 行死代码增加维护困惑
- **建议**: 
  - 删除 L166-187 的死代码
  - 或将 handler 改为真正的 iretq 返回（如保留，需要修正栈恢复顺序）

### [P2] 4. BootInfo ABI: bootloader 端 `boolean_t` vs 内核端 `uint8_t`

- **位置**: `boot/uefi/main.c:37` vs `kernel/include/kernel/bootinfo.h:40`
- **现象**: 
  - Bootloader 端 `BootFromBIOS` 定义为 `boolean_t` (UEFI 类型)
  - 内核端定义为 `uint8_t`
  - MS ABI 中 `boolean_t` 是 1 字节，当前无实际影响（最后一个字段，无 padding 分歧）
  - 但 struct 定义不一致违反 LLP64 vs LP64 ABI 的"必须完全匹配"规则
- **建议**: 统一为 `uint8_t`，消除对 UEFI 类型定义的隐式依赖

### [P2] 5. GDT slot 8-10 的 `.fill 100` 过度预分配

- **位置**: `kernel/arch/x86_64/head.S:253`
- **现象**: `GDT_Table` 后 `.fill 100, 8, 0` 预分配 800 字节，实际仅需 8×16=128 字节 (NR_CPUS=8, TSS descriptors 各 16B)
- **建议**: 
  ```asm
  .fill ((NR_CPUS * 2) + 2), 8, 0
  ```
  或至少添加注释说明分配依据

### [P2] 6. GDT 注释未标注哪些 slot 被 TSS 运行时覆盖

- **位置**: `kernel/arch/x86_64/head.S:244-252`
- **现象**: GDT 表格的每行注释说明了编译时的初始化值，但未说明 slots 8-9 在 `setup_TSS64` 中被覆写为 TSS descriptor
- **建议**: 在表格后添加注释:
  ```asm
  /* slots 8-9 are overwritten by setup_TSS64 at runtime → TSS descriptor */
  ```

### [P2] 7. trampoline.S 段选择器使用魔数而非命名常量

- **位置**: `kernel/arch/x86_64/trampoline.S:38,42-47,70,74-77`
- **现象**: `$0x08`, `$0x10`, `$0x18`, `$0x20` 等硬编码值。trampoline GDT 是局部定义的，但与常规 GDT 布局毫不相干——需要阅读整个 trampoline GDT 定义才能理解各选择器的含义
- **建议**: 使用宏定义:
  ```asm
  #define TRAMP_CS32  0x08
  #define TRAMP_DS32  0x10
  #define TRAMP_CS64  0x18
  #define TRAMP_DS64  0x20
  ```
