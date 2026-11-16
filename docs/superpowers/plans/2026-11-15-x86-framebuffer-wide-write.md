# x86 Framebuffer Wide Write Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkboxes for tracking.

**Goal:** 先把真机验证过的 32 位顺序写用到普通 gfx 提交，再单独降低开机日志滚屏的写回开销。

**Architecture:** x86 的 gfx 提交和内核控制台共用一个只写像素的短例程。gfx 继续在现有 writer lease 下逐行写入；控制台的 `FB_SHADOW=1` 路径继续在 RAM 内滚动，只把最终像素写回 framebuffer。两部分分开实施和验收。

**Tech Stack:** OS01 x86_64 / UEFI GOP，freestanding C，现有 hosttests、QEMU gfx 和真机 A/B 夹具。

**Spec:** 本文件的范围、接口和验收条件；现有诊断入口见 [驱动文档](../../driver/driver.md#真机整屏写入-ab)。

## 基线与范围

真机是 1024×768、32 bpp、pitch 4096，每帧 3 MiB；ABBA × 2 均通过像素抽查。A/B 使用相同 framebuffer 映射。

| 四次样本的中位数 | A：逐字节写 | B：32 位顺序写 |
|---|---:|---:|
| 整次提交 | 550.96 ms | 9.47 ms |
| 用户缓冲区到内核 RAM | 5.53 ms | 2.97 ms |
| framebuffer 写入阶段 | 544.89 ms | 6.47 ms |

普通 gfx 先采用已在此机器复现的像素宽写。只改 Tetris 脏矩形会留下慢的整屏和开机滚屏；改 PAT/WC 会同时引入映射属性和别名变量。这两项均留到本计划验收后按需要另做。

## 全局约束

- 保持 GOP/BGA 缓存策略、页表、`/dev/fb` 映射、gfx UAPI、用户拷贝故障恢复和显示所有权语义。诊断 ioctl 仍只在 `OS01_TEST_GFX_PRESENT_AB` 构建启用。
- 宽写只针对 RGB32 像素区，以验证过的对齐 32 位 store 为准；不改通用 `memcpy`，不引入内核 SIMD，也不承诺防撕裂或 scanout 完成时间。
- 保留 `FB_SHADOW=0` 默认值。先比较旧版和新版的 `FB_SHADOW=1`，再决定是否另行改默认值。aarch64 的路径保持原样。
- 保留工作区现有 A/B 和 `tetris smoke` 改动。复用现有测试目标与夹具，避免新增顶层 suite 或给 QEMU 设置性能阈值。

## 审查重点

- 局部矩形必须遵守实际 pitch，最后一个像素、行尾 padding 和相邻行不能被误写。
- gfx 正常路径换用宽写后，诊断 A 必须仍然走逐字节拷贝，否则真机对照失效。
- 日志 glyph 与滚屏必须使用同一套几何；切模式、交接显示、PMM 未就绪或 shadow 分配失败时不能使用旧 shadow。
- 控制台计时不得在持 `Pos.lock` 或相关输出锁时打日志；首次从显存建立 shadow 的成本要和后续滚屏分开。
- 真机性能结论只取同机、同分辨率、同构建选项的样本。像素抽查和启动成功单独作为正确性门槛。

## Task 1：普通 gfx 采用 32 位顺序写

**文件：** 新增 `kernel/include/arch/x86_64/fb_copy.h`；修改 `kernel/driver/fb.c`、`kernel/arch/x86_64/driver/gfx.c`、`kernel/include/driver/fb.h`；复用 `hosttests/cases/test_fb_writers.c`、`user/test_gfx/test_gfx.c` 和 `user/tetris/tetris.c`。

- [x] 保存现有诊断镜像和源码状态，在同一台机器上记录上述 A/B 基线。保留现有 `/bin/tetris smoke` 计时输出，作为后续普通 ioctl 的验收入口。
- [x] 扩充已有 writer 测试：使用宽 8 像素、pitch 48 字节的快照，从非零 x/y 写到第 8 个像素，逐个检查值和前后哨兵；用相同起点测试越界、非整像素长度及未持 lease。要求非法请求返回 `-EINVAL` 且显存不变。修改前的非整像素长度测试应失败。
- [x] 实现 `x86_fb_copy_rgb32(void *dst, const void *src, size_t bytes)`：只复制不重叠的 RAM 到像素区；当地址和长度均 4 字节对齐时用 `volatile uint32_t` 顺序写，其他情况保守走字节拷贝。只由确认过 RGB32 格式和边界的路径调用。
- [x] 在 `fb_write_row_leased()` 保留现有 lease、格式和边界约束，并在写入前拒绝非整像素长度。让普通 `GFX_PRESENT` 和 `GFX_PRESENT_RECT` 通过同一个正常 writer 使用宽写。诊断 writer 共享地址计算和检查，但 A 显式走旧 `memcpy`，B 走宽写；不要另做第二套提交循环。
- [x] 运行 `make PROFILE=x86_64-clang test-resolution-host RES_CASE=writers`、`make PROFILE=x86_64-clang test-gfx-device test-gfx-fault-bridge` 和 `make PROFILE=x86_64-clang test-qemu SUITE=gfx`。诊断镜像按驱动文档跑 `gfx-present-ab`，要求八次提交以及像素抽查全部通过；反汇编普通 writer，核实对齐路径是 32 位目标写入。
- [ ] 真机启动普通构建，保持 1024×768、pitch 4096，同样的启动参数。连续运行 5 次 `/bin/tetris smoke`，保留 `[TETRIS] PRESENT`：均须 `rc=0`，目标中位数 **≤20 ms**。再交互玩 Tetris，退出后确认 ash 显示恢复。未达标时先复跑原 A/B 确认普通路径是否吃到相同收益。**（待真机验收）**

## Task 2：开机日志 shadow 滚屏复用宽写

**文件：** `kernel/core/printk.c`、`kernel/tty/console.c`、`kernel/include/core/printk.h`；在 `kernel/arch/x86_64/platform/boot.c` 调用诊断入口；复用 Task 1 的拷贝例程和现有 framebuffer writer 测试。

- [x] 建立独立滚屏基线：旧版以 `FB_SHADOW=1 LOG_TARGET=both` 启动，使用 `KERNEL_EXTRA_CFLAGS=-DOS01_TEST_FB_SCROLL` 启用夹具。在 `x86_64_boot_devices()` 的计时器校准后、显示交接前调用 `void printk_fb_scroll_probe(void)`。在现有锁和 lease 下释放已有 shadow（如有），沿同一 printk 滚屏路径预热一次，再触发固定 8 次滚屏。记首次建 shadow 的 `seed_ns` 和每次稳定滚屏的 `total_ns`、`fb_ns`；只在锁和 lease 都释放后用 `log_info` 输出 `[FB SCROLL]` 结果。新旧镜像使用同一夹具；没有有效时钟或未建立 shadow 时标为无效样本。
- [x] 先用最小内存夹具覆盖滚屏几何：3 条扫描行，每行 2 个可见像素、pitch 12 字节，行尾 padding 放哨兵；滚 1 行，断言可见行上移、底行清零、padding 不变。再覆盖 framebuffer 长度差 1 字节的输入，断言不写。将测例接入已有 `test_fb_writers.c`，只把地址与行拷贝计算抽成可用 RAM 测试的帮助函数；不模拟 PMM 或设备。
- [x] 让 x86 的 `putchark_snap()`、`putchar_at_snap()` 和两个滚屏入口统一从 lease 快照取得 width、height、stride 和可写范围。为 shadow 增加 pitch 与映射身份校验；同址但 pitch 改变也必须重建。RAM shadow 采用紧凑行，显存按实际 pitch 访问，校验可见范围后才拷贝。首次逐行同步已有内容，保留启动画面；后续滚屏只读 RAM、按可见行宽用 Task 1 的例程写回，不碰 padding。
- [x] 保持显示交接时释放 shadow；没有 shadow 时保留直接滚屏分支，x86 按实际 pitch 逐行处理可见像素。无效几何或映射范围不足时不碰像素。PMM 尚未就绪、分配失败、模式切换和 aarch64 均不得依赖 shadow。保持现有锁顺序，不新增锁内日志。
- [x] 跑 writer 测试、x86 `test-qemu SUITE=phase-0`（分别带 `FB_SHADOW=0`、`FB_SHADOW=1`）、`test-qemu SUITE=gfx`、`test-static`，以及 `make PROFILE=aarch64-clang test-aarch64-boot-logo`。启动期间确认画面和滚屏正确，handoff 后 terminal 正常；不能把 QEMU 测得的耗时当成真机门槛。
- [ ] 真机对比旧版与新版 `FB_SHADOW=1`，保持 1024×768 和相同日志配置：比较同一 8 次稳定滚屏的中位数，工程目标 **至少快 2 倍**；另记首次建立 shadow 的耗时及启动至 ash 的变化。滚屏性能可以独立判定成败，不能用 gfx 的 58 倍结果代替。**（待真机验收）**

## 交付

- [x] 将普通 gfx、日志滚屏拆成可分别审查及回退的改动。记录构建参数、镜像、测试结果与真机原始行；真机数据缺失时注明待验收。
- [x] 如果普通 gfx 达标且滚屏仍慢，以写回时间、首次建 shadow 和滚屏频次判断下一步。若是频次，另做日志刷新合并；若宽写后显存阶段仍超预算，再设计 UC/WC 对照。

---

## 实施记录（2026-11-16，宿主环境）

### 与计划的两处偏离（均为满足目录分层规范）

1. **新增中性 facade `kernel/include/arch/fb_copy.h`**：`kernel/driver/fb.c` 与
   `kernel/core/printk.c` 属于架构中立层，**不得** include `arch/x86_64/` 头。
   宽写原语因此走 facade：弱默认 `fb_copy_rgb32()` 在
   `kernel/driver/arch_fb_write.c`（逐字节），x86_64 强覆盖在
   `kernel/arch/x86_64/driver/fb_copy.c`（32 位顺序写，
   `kernel/include/arch/x86_64/fb_copy.h`）。可见行几何帮助函数放在 facade 里，
   宿主回归直接调用同一份代码。
2. **滚屏的 RAM 侧也用宽写**：libc 的 `memmove` 是逐字节循环，影子内部滚动
   （数 MB）也是一笔开销。向上滚动时按**从顶行到底行**的顺序搬移，避免覆盖
   后续还要读的源行。每次只拷贝一行，源行与目标行不重叠，可以使用宽写。

### 构建与验证记录

| 项 | 命令 | 结果 |
|---|---|---|
| 宿主 writer 回归 | `make PROFILE=x86_64-clang test-resolution-host RES_CASE=writers` | 212/212 通过 |
| 宿主 gfx device | `make PROFILE=x86_64-clang test-gfx-device` | 635/635 通过 |
| 宿主 fault bridge | `make PROFILE=x86_64-clang test-gfx-fault-bridge` | 14/14 通过 |
| QEMU gfx 套件 | `make PROFILE=x86_64-clang test-qemu SUITE=gfx` | GFX/TETRIS/TERM/DESKTOP 全 PASS |
| QEMU phase-0 | `make PROFILE=x86_64-clang FB_SHADOW=0 test-qemu SUITE=phase-0` | PASS |
| QEMU phase-0 | `make PROFILE=x86_64-clang FB_SHADOW=1 test-qemu SUITE=phase-0` | PASS |
| 静态审计 | `make PROFILE=x86_64-clang test-static` | 16 项全 PASS |
| aarch64 字标 | `make PROFILE=aarch64-clang test-aarch64-boot-logo` | PASS（fb-visible / logo / no-fb） |

**回归证据：** 去掉 `fb_write_row_leased()` 的非整像素长度检查后，新增的
非整像素长度用例立刻失败（`rc=0` 而非 `-EINVAL`，且显存被改写）——该用例
确实是新行为守卫，不是空断言。

**反汇编核验**（`llvm-objdump build/x86_64-clang/kernel/kernel.elf`）：
`fb_copy_rgb32()` 的对齐分支是 `movl (%rsi,%rdx,4),%r8d` /
`movl %r8d,(%rdi,%rdx,4)` —— 32 位目标写入；非对齐分支才尾调用 `memcpy`。

### 滚屏夹具输出（QEMU，**仅冒烟信号，不构成真机门槛**）

夹具镜像：`make PROFILE=x86_64-clang FB_SHADOW=1 LOG_TARGET=both \
KERNEL_EXTRA_CFLAGS=-DOS01_TEST_FB_SCROLL disk.img`
（QEMU GOP：1440×900，stride 5760。）

```
[FB SCROLL] cfg w=1440 h=900 stride=5760 seed_ns=30034646
[FB SCROLL] total_ns=5197855,5197504,5198126,5227014,5213126,5185181,5198828,5211141
[FB SCROLL] fb_ns=2421694,2420771,2437226,2471508,2420741,2419197,2419838,2443192
[FB SCROLL] median total_ns=5198477 fb_ns=2421232
```

同机同夹具的**旧实现**基线（把 `printk.c` / `console.c` / `printk.h` /
`fb.c` 临时回退到 HEAD 并加同一夹具后实测）：

```
[FB SCROLL] total_ns=190783096,191152328,190031704,189654500,189752829,189935130,189807032,191003226
```

首次 QEMU 测量：稳态滚屏中位数 **190.4 ms → 5.2 ms（约 36 倍）**。
这组新版数据采自修正 shadow 搬移顺序之前，只记录耗时；验收以修正后的复测为准。
真机 1024×768 / pitch 4096 上的旧版 vs 新版对照 **仍待验收**：
需按上面的夹具命令分别构建新旧镜像，比较同一 8 次稳态滚屏的中位数，
工程目标 ≥2 倍；同时记录 `seed_ns` 与启动到 ash 的时间变化。

### 下一步判断依据（按计划交付项）

* 普通 gfx 是否达标只有真机能判定；宿主/QEMU 未设性能阈值。
* 滚屏的新瓶颈已从「逐字节搬移」变成「宽写回写」（QEMU 上 fb 阶段占
  总耗时约 47%）。若真机上显存阶段仍超预算，再设计 UC/WC 对照。
* 若真机滚屏频次本身是问题（开机日志量大），另做日志刷新合并。
