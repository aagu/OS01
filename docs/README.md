# OS01 文档索引

> 新读者从 [`../README.md`](../README.md) 进入。先读 [AGENTS.md](../AGENTS.md) 的 Quick start + Critical gotchas，再回到这里。

## 主题树

按 `kernel/<subsystem>/` 源目录对称组织：

```
docs/
├── [architecture.md](architecture.md)         ← 全局骨架：boot chain + 内存布局 + 初始化序列
├── [arch.md](arch.md)                         ← multi-arch facade 矩阵（weak-default / strong-override）
├── [structure.md](structure.md)               ← 源/头目录结构
├── [roadmap.md](roadmap.md)                   ← 优化路线图
├── [changelog.md](changelog.md)               ← 完成工作汇总
├── [decisions.md](decisions.md)               ← 关键设计决策总账（含开源 OS 借鉴表）
│
├── arch/                                      ← arch-neutral 抽象细则
│   ├── [cross-boundary-symbols.md](arch/cross-boundary-symbols.md)
│   └── [entropy-source-facade.md](arch/entropy-source-facade.md)
│
├── boot/         [boot.md](boot/boot.md)                              ← UEFI 引导 + boot_context ABI
├── memory/       [memory.md](memory/memory.md) + [cow-mmap.md](memory/cow-mmap.md) + 导读
├── interrupt/    [interrupt.md](interrupt/interrupt.md) + 导读
├── smp/          [smp.md](smp/smp.md)                                  ← 多核启动 / IPI / 负载均衡
├── sched/        [scheduler.md](sched/scheduler.md) + complexity + 导读
├── syscall/      [syscall.md](syscall/syscall.md) + 导读
├── signal/       [signal.md](signal/signal.md)                        ← 信号投递 / handler / sigreturn
├── time/         [timer.md](time/timer.md)                            ← clocksource / clockevent
├── fs/           [filesystem.md](fs/filesystem.md) + [io-multiplexing.md](fs/io-multiplexing.md)
├── driver/       [driver.md](driver/driver.md)                        ← keyboard / serial / ahci / e1000 / fb
├── net/          [network.md](net/network.md)                         ← lwIP + 驱动 + socket
├── gui/          [gui.md](gui/gui.md)                                 ← 2D 图形 + 终端 + 游戏
├── log/          [log.md](log/log.md)                                 ← 日志子系统 + DEBUG_CHANNELS
├── subsys/       [subsys.md](subsys/subsys.md)                        ← 子系统注册框架
│
├── build/       [build.md](build/build.md) + [toolchain.md](build/toolchain.md)  ← 编译/运行/调试
├── debug/       [debug.md](debug/debug.md)                            ← 调试方法（活跃）
│
├── random/                                                              ← 熵源（facade 测试 / 质量治理 / x86_64 调研）
├── superpowers/                                                         ← 设计 spec + 实施 plan（过程档案）
└── archive/                                                             ← 历史归档：aarch64 closure / debugging journal / 2026-07 评审
```

## 按用途查找

**「我刚拿到 OS01 代码，从哪里开始？」**
[../README.md](../README.md) → [structure.md](structure.md) → [architecture.md](architecture.md) → 选一个子系统

**「我要改 / 加 / 调一个子系统」**
先读该子系统的 `docs/<subsys>/*.md`，再看 `kernel/<subsys>/` 源码 + `kernel/include/<subsys>/` 公开头，最后看 `kernel/selftest/test_<subsys>.c`

**「我要修 bug / 跑调试」**
[debug/debug.md](debug/debug.md) + [build/build.md](build/build.md) 第 2-3 章；历史排障参考 [archive/debugging/](archive/debugging/)

**「我想知道为什么这么做」**
[decisions.md](decisions.md)（含开源 OS 借鉴表） + [roadmap.md](roadmap.md)

## 关键陷阱（高频踩坑）

摘自 [../AGENTS.md](../AGENTS.md)：

- **`boot_context` ABI**：UEFI 引导器（x86_64 + aarch64）在固定物理地址构造 `boot_context` v2 结构。bootloader 是 LLP64（`sizeof(long)=4`），kernel 是 LP64（`sizeof(long)=8`）。**所有字段必须用 `uint32_t`/`uint64_t`，绝不能用 `unsigned long`**。
- **`make clean` 是 struct 改动后的强制步骤**（Makefile 无 header 依赖，stale `.o` 会导致 `sizeof()` 静默失配）。
- **`Phy_To_Virt()` 必须先于解引用**：`alloc_pages()` 返回物理地址。
- **GS base 通过 `wrmsr(IA32_GS_BASE)` 只设置一次**；不要 reload GS selector，否则 per-CPU 数据被毁。
- **COW 页释放**：在 `vmm_unmap_4k_page` 中清除 PTE 前**先检查 `PAGE_COW`**，否则物理地址在 `page_cow_put()` 调用时已失效。

完整陷阱清单见 [../AGENTS.md](../AGENTS.md) 的 "Critical gotchas" 章节。

## 文档维护纪律

1. **`docs/superpowers/specs/*.md` 与 `plans/*.md` 是"当时想怎么做"，代码是"实际怎么做"**——两者冲突以代码为准，并回写文档。
2. **每个子系统入口对称**：源码 `kernel/<subsys>/` + 头 `kernel/include/<subsys>/` + 文档 `docs/<subsys>/*.md`（P4 重构后的硬约定）。
3. **review/group*.md 标记的"待处理"问题**（已归档到 [archive/review/](archive/review/)）：读代码时留意，别把已知坑当新发现。
4. **历史收尾报告** 在 [archive/](archive/)，路径以当前代码为准。
