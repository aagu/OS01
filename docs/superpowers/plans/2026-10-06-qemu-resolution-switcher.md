# QEMU Resolution Switcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 x86_64 QEMU Standard VGA 上实现分辨率查询与事务性热切换，并保留 terminal、ash 和 PTY 会话。

**Architecture:** fb 层管理一致显示快照和写入租约；BGA 作为 PCI 驱动核验显存、执行带读回及回滚的模式事务。SET 关闭准入并排空整帧 writer，再更新硬件与 generation；terminal 定期查询并以资源事务恢复，原始 mmap 采用本次启动 sticky 禁切换规则。

**Tech Stack:** freestanding C、现有 PCI/VMM/EEVDF/blocker/devfs、libgfx、host C tests、Python QEMU/QMP runner、Clang。

**Spec:** [已批准 v3](../specs/2026-10-06-qemu-resolution-switcher-design.md)，基线提交 `2531bfed328cd58cd809c70435f281f852db136d`。执行者先完整阅读两份文档；plan 决定接口与任务，spec 决定行为，冲突时停止并修订设计，不自行削减保证。

## Global Constraints

- 支持 `-vga std` 的 PCI VGA，默认 16 MiB，也允许能力探测确认的其他显存容量。非 x86 后端返回 `-ENODEV`；不得改变 `boot_context` ABI。
- 候选表：640×480、800×600、1024×768、1280×720、1280×800、1280×1024、1440×900、1600×900、1920×1080，均为 32bpp、stride=width×4。
- generation 使用 uint64_t，启动为 1；同模式 SET 不写硬件、不清屏、不递增；回滚成功递增，回滚失败永久 EIO。
- 锁顺序 `display_mutex -> Pos.lock -> display_state_lock`；用户拷贝/清屏/等待/日志不持 state 自旋锁；writer 不获取 display_mutex。
- 排空轮询请求间隔 1ms、总截止 1s；调度实际唤醒受 tick 影响，超时依据单调时钟，不承诺实际唤醒精度为1ms。
- 空闲 poll 的 timeout 最大250ms；模式恢复失败使用250ms重试间隔；正常可分配资源时空闲切换1s内完成查询与首帧。
- raw mmap 成功或无法彻底撤销失败映射后，改变布局的 SET 返回 EBUSY，munmap/进程退出不解除，需重启。
- fb_info=20、fb_modes_req=332/alignment4、fb_set_mode_req=12、fb_state=32/generation offset24；ioctl=0x4601..0x4605，与 spec 完全一致。
- `FB_RESOLUTION_TEST=1` 仅测试配置，生产为0且无控制节点、命令或注入分支；不得混用 OS01_SYSTEST/KERNEL_SELFTEST。
- 新源/公开头目录成对，x86 页表代码放 kernel/arch/x86_64；新日志使用 log_*，失效 framebuffer 场景只能 serial 安全输出。
- 结构体变化后必须 `make clean`；所有构建/测试从根 Makefile、显式 `PROFILE=x86_64-clang` 进入，组件 Makefile 不作为直接入口。

## Review Focus

1. A→B→A 与失败回滚返回相同尺寸：仍按 generation 重建，不能走 fatal（T5/T7）。
2. 中途 EFAULT、ENOMEM 或用户 PTE 安装失败：不泄露租约、锁或原始映射准入（T3/T5/T7）。
3. SET 排空超时而布局未变：EAGAIN 仅暂态，原 view 和图形应用继续工作（T2/T8/T10）。
4. 无 BGA、探测恢复失败和永久硬件故障：GOP 可信时保留显示，不可信时 serial 会话存活（T4/T5/T7/T10）。
5. 空闲终端、全 view 槽已满、前台程序正在运行：限频恢复，不重启会话，正确 winsize/SIGWINCH（T6/T7/T10）。

## 文件与边界、执行依赖

- `kernel/driver/fb_state.c` / `kernel/include/driver/fb_state.h`（新）：快照、准入、writer 租约、bootstrap 与后端发布；fb.c 保留 devfs 入口。
- `kernel/arch/x86_64/memory/fb_map.c` / `kernel/include/arch/x86_64/fb_map.h`（新）：checked 大页映射/验证；不扩展通用 VMM ABI。
- `kernel/driver/bga.c` / `kernel/include/driver/bga.h`（新）：PCI probe、能力和寄存器事务；非 x86 在该源编译分支提供安全 stub。
- `user/setres_parse.c/.h`（新用户内部 helper）、`user/setres.c`（新程序）：纯解析与设备操作；helper 从 user/Makefile 的 C_SOURCES 排除。
- `user/terminal_display.c/.h`（新用户内部 helper）：可 host-test 的恢复状态机；terminal.c 负责事件循环/PTY，helper 也从独立程序列表排除。
- `kernel/driver/fb_test.c` / `kernel/include/driver/fb_test.h`、`kernel/include/uapi/fb_test.h`（新，仅测试构建）：测试控制面；`user/test_resolution.c`（新，仅测试打包）guest 辅助程序。
- 修改：fb/gfx/printk/logo/console、x86 early boot、PTY/file dispatch、user客户端、profile/发布/rootfs、hosttests和QEMU runner、构建文档。

依赖顺序 T1→T2→T3→T4→T5；T6 依赖 T1，可单独验证但按本文顺序交付；T7依赖T5/T6，T8依赖T5，T9依赖T5/T7，T10集成全部。不要并行修改共同文件。每项通过测试和review后独立commit；执行时使用 worktree 技能确定隔离环境。

新增统一聚合根目标 `test-resolution-host RES_CASE=<uapi|state|writers|bga|ioctl|pty|terminal|clients|hooks|all>`。每个case对应下面指定测试二进制；T1创建分发框架，后续任务注册自己的case，未知/未注册case必须报错，不能空跑PASS。all只运行已注册case，完整交付时必须包含九组。实际bin路径由HOST_TEST_BUILD_DIR解析，不写死build目录。所有新增测试进入普通test-host目录及运行清单。

---

### Task 1: 固定 UAPI 并接入测试入口

**Files:** Create kernel/include/uapi/fb.h, hosttests/cases/test_fb_uapi.c; Modify kernel/include/driver/fb.h, hosttests/Makefile, mk/components/run.mk, docs/build/build.md。

**Interfaces:** 产生 spec §5 的五条 ioctl、四个struct和常量；保留fb_get_info/fb_write_row当前签名。产生上述 test-resolution-host 分发入口。

- [ ] **Step 1: 写 ABI 失败测试。** `test_fb_uapi_layout` 断言 `sizeof(fb_info)==20`、`sizeof(fb_modes_req)==332`、alignment4、`sizeof(fb_set_mode_req)==12`、`sizeof(fb_state)==32`、generation offset24、五个命令号分别0x4601..0x4605；比较原fb_info五字段顺序。测试引用真实新头，不定义替代struct。
- [ ] **Step 2: 建立uapi case并验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=uapi`，预期缺少uapi/fb.h的编译错误；未知case预期非0。新测试框架可先提交在同一任务工作区，不标任务完成。
- [ ] **Step 3: 实现UAPI头及私有头引用。** 原read ABI保持20字节；不更改gfx UAPI。添加头保护与stdint依赖，公开结构使用定宽类型，内核/用户Static_assert使用同一头。
- [ ] **Step 4: 验证GREEN与发布。** Run同一uapi命令，预期实际运行该bin、0失败；`make PROFILE=x86_64-clang sysroot`发布头并核查usr/include/uapi/fb.h与源一致（sysroot生成路径从profile获取，不猜测）。
- [ ] **Step 5: Commit。** 只暂存本任务文件，message `feat: define framebuffer mode control ABI`。

### Task 2: 可信bootstrap、checked映射与写入租约

**Files:** Create fb_state.c/.h、arch/x86_64/memory/fb_map.c及对称公开头、hosttests/cases/test_fb_state.c、hosttests/mock/fb_resolution_runtime.h/.c；Modify x86_64/platform/boot.c、core/printk.c、hosttests/Makefile。

**Interfaces:** 新增类型 `fb_snapshot_t { struct fb_state state; uint32_t *addr; uint64_t mapped_size; }`，`fb_lease_t { fb_snapshot_t snapshot; bool held; }`。产生：
```c
void fb_bootstrap_state(uint64_t phys, uint64_t gop_bytes, const struct fb_info *info);
void fb_publish_initial_mapping(uint32_t *addr, uint64_t mapped_size);
int fb_snapshot_read(fb_snapshot_t *out); /* 短锁，writer可用，不取mutex */
int fb_writer_begin(fb_lease_t *lease, uint64_t expected_generation);
void fb_writer_end(fb_lease_t *lease); /* held=false为no-op */
int fb_transition_begin(bool boot_probe); /* boot无等待；runtime有界排空 */
void fb_transition_end(void); /* 故障状态不重新开放 */
void fb_mark_failed(void);
int fb_x86_map_checked(uint64_t phys, uint64_t size, uint32_t **out_addr);
```
expected_generation=0为可信内核writer，非0精确匹配；失败不取得租约。fb_state私有mutex由bootstrap创建；`fb_control_lock/unlock(void)`供后续任务协调，不由writer调用。`fb_transition_begin`只负责准入/排空，调用者必须已持mutex或位于BSP boot probe。`fb_mark_failed`只改变可信度和准入，不编程硬件。T2同时定义仅OS01_HOST_TEST可见的 `void fb_state__test_set_generation(uint64_t generation)`，在state lock下设置测试代数，供T3精确构造stale/ABA，实际SET代数变化由T5生产事务测试验证；生产构建不链接该符号。

- [ ] **Step 1: 写失败测试。** `test_bootstrap_before_first_writer`：未映射begin返回EAGAIN；首次可信映射generation1且writer可用，即使backend未ready。`test_transition_drains_frame`用pthread barrier停在已取得租约处，断言关闭准入后新writer EAGAIN、硬件mock调用数0，旧writer结束后transition才成功。`test_drain_timeout` fake clock到1s，断言EBUSY、准入恢复、generation1；失败state断言EIO。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=state`，预期新生产符号缺失/断言失败，不接受fixture不运行。
- [ ] **Step 3: 实现状态与时钟等待。** state lock必须spin_init；irqsave短锁操作计数/快照。runtime排空用 `arch_clocksource_read_ns()` 和现有BLOCKER_NANOSLEEP/blocker_wait协议封装fb_state.c私有 `fb_drain_pause(uint64_t deadline_ns)`，请求下一次唤醒=min(now+1000000,deadline)，wake predicate读取current->wakeup_ns，blocker_wait的signal_can_wake=false；醒来复查单调时钟，返回时清零wakeup_ns。不通过SYS_nanosleep传内核指针。boot只检计数，不调用blocker/current。所有超时出口恢复准入。
- [ ] **Step 4: 实现checked映射/验证和early接入。** 使用vmm_get_next_level_checked，硬件PTE只在x86文件；2MiB对齐、范围加法溢出检查；验证每个PMD的phys/cache flags并刷新TLB，失败不发布更大长度。保持初始GOP映射；在首次logo之前发布实际地址，frame_buffer_init重映射同步快照。测试分配第N个table失败和错物理地址，断言old mapped_size不扩大。
- [ ] **Step 5: 验证/Commit。** Run state case、`make PROFILE=x86_64-clang test-qemu SUITE=phase-0`；预期0失败和正常logo/boot。Commit `feat: coordinate framebuffer state and writer admission`。

### Task 3: 所有writer与raw mmap遵守准入

**Files:** Modify core/printk.c、driver/logo.c、tty/console.c、include/tty/console.h、driver/fb.c、driver/gfx.c、include/driver/gfx.h、core/panic.c、相关gfx mock；Create hosttests/cases/test_fb_writers.c。

**Interfaces:** 消费T2租约。新增 `int fb_write_row_leased(const fb_lease_t *lease, uint32_t x, uint32_t y, const void *pixels, uint32_t bytes)`；保留fb_write_row作为申请/释放单次租约的wrapper，gfx逐行仅调用leased版本。console新增 `void console_notify_resize_locked(void)`，要求Pos.lock已持有，仅重置两套光标。gfx_view增加uint64_t mode_seq，CREATE_VIEW从同一snapshot取得布局与generation。

- [ ] **Step 1: 写失败测试。** `test_present_fault_releases_lease` 在第2行copy_from_user_ft失败，断言EFAULT、active_writers0、随后transition成功。`test_view_aba_stale`记录generation1，publish两次后宽高回原值，断言旧view ESTALE且显存0写入。`test_console_transition_serial_only`断言serial仍输出、两套cursor未变。`test_mmap_partial_failure`注入第2页失败，验证全部已安装PTE撤销；清理失败分支sticky=true。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=writers`，预期旧present/console/mmap未遵守准入导致断言失败。
- [ ] **Step 3: 改gfx及字符writer。** PRESENT一次租约覆盖整帧/row staging，所有出口释放；矩形验证用减法防溢出。glyph/row内部传入快照不重复计数。color_printk/console光标和surrender/force-enable使用Pos irqsave；SET不在writer持Pos时等待。审计logo及panic直接显存写，panic仅trylock/可用快照，失败serial，不等待mutex或已被当前panic路径持有的Pos锁。
- [ ] **Step 4: 改raw mmap。** 取control mutex固定容量、逐页检查vmm_map_4k_page返回；失败用unmap撤销自身页并TLB同步，不释放设备phys，撤销不可信仍sticky；成功sticky。退出是否存在残留映射先判定再释放mutex。
- [ ] **Step 5: 验证/Commit。** 结构变化先 `make clean`，Run writers case及 `make PROFILE=x86_64-clang test-gfx-device test-gfx-file-lifecycle`；0失败。Commit `fix: serialize framebuffer writers across mode changes`。

### Task 4: PCI BGA后端探测与硬件事务

**Files:** Create driver/bga.c、include/driver/bga.h、hosttests/cases/test_bga_resolution.c、hosttests/cases/test_bga_non_x86.c；Modify fb_state.c/.h、hosttests/Makefile、相关PCI driver-count host fixtures（test_device_boot.c、test_pci_driver_model.c、mock/arch9注册fixture）。

**Interfaces:**
```c
typedef struct bga_caps { uint64_t vram_bytes; uint32_t max_width, max_height, max_bpp; } bga_caps_t;
enum bga_result { BGA_APPLIED, BGA_ROLLED_BACK, BGA_FAILED };
int bga_probe(struct pci_device *pdev, const struct pci_device_id *id);
enum bga_result bga_apply_mode(const struct fb_info *target);
uint32_t bga_filter_modes(const bga_caps_t *caps, uint64_t mapped_size,
                         struct fb_info out[FB_MAX_MODES]);
int fb_install_backend(const bga_caps_t *caps, uint32_t *addr, uint64_t mapped_size);
```
结果区别布局恢复与永久故障，bga_apply_mode只操作寄存器，不改Pos/清屏；调用前必须关闭准入并排空。硬件transport为bga.c内部read/write callbacks，OS01_HOST_TEST可替换，生产使用arch_inw/outw及pci_config_read32/write32。fb_install_backend在boot transition下发布能力/映射、模式表和ready；不循环依赖probe。

- [ ] **Step 1: 写失败测试。** `test_probe_preserves_pci_and_dispi`逐操作注入错误，断言BAR/command/ID/ENABLE/index恢复；command/status write高16位0。`test_probe_identity`匹配1234:1111但class错误、GOP基址不同、0容量均拒绝。`test_filter_capacity`800×600初始1.92MB/真实16MiB时包含1920×1080；2MiB时只含640/800；8MiB含1920×1080。`test_apply_readback`注入错误virtual_width/offset，断言ROLLED_BACK；回滚读回错断言FAILED；NOCLEARMEM位必须存在。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=bga`，预期未定义后端或事务断言失败。
- [ ] **Step 3: 实现PCI绑定probe。** PCI_DRIVER_DECLARE匹配表class DISPLAY_VGA；基址不匹配返回ENODEV，后续候选仍能probe，只接受一个后端。bootstrap已存在；boot transition active_writer非0立即拒绝。BAR sizing包含32/64位保存恢复、低command位/高status清零；错误出口恢复验证。ID5精确握手、VIDEO_MEMORY_64K、GETCAPS保存恢复，初态32bpp/零offset/pitch正确才接受。调用T2 checked map，成功才install。配置恢复失败fb_mark_failed、probe EIO而非DEVICE_UNSAFE。
- [ ] **Step 4: 实现事务与stub。** 保存完整有效布局及index；按spec disable/program/enable/布局读回，virtual_height为派生检查；回滚恢复后完整读回。非x86 probe ENODEV且无端口引用执行。host stub测试使用仅OS01_HOST_TEST有效的OS01_HOST_TEST_NON_X86分支选择宏，以宿主编译器编译真实bga.c的非x86分支；该分支不得包含x86端口实现，port mock被调用立即失败。test_bga_non_x86断言probe ENODEV、零PCI/端口访问；bga_apply_mode在无后端状态返回BGA_FAILED，不能执行I/O。候选表使用uint64计算和min容量，不修改boot_context。
- [ ] **Step 5: 验证/Commit。** Run bga/state cases、phase-0；核对PCI其他设备仍绑定、没有driver-count硬编码回归。Run `make PROFILE=aarch64-clang aarch64-uefi-kernel`，必须看到实际编译/链接或已有合格产物，不能用目录名kernel目标。bga case同时运行test_bga_non_x86的真实stub/no-port检查；当前aarch64显式源清单不包含bga，aarch64内核构建只证明既有架构未回归，不能替代stub证据。Commit `feat: probe Standard VGA and verify BGA mode transactions`。

### Task 5: fb安全ioctl及完整SET协调

**Files:** Modify driver/fb.c、fb_state.c/.h、tty/console.c；Create hosttests/cases/test_fb_resolution_ioctl.c。

**Interfaces:** 产生UAPI行为；公开kernel函数 `int fb_set_mode(const struct fb_set_mode_req *req)`、`int fb_get_state(struct fb_state *out)`、`int fb_get_modes(uint32_t capacity, struct fb_modes_req *out)`；GET任务上下文取control mutex，writer只用snapshot_read。新增私有 `fb_commit_layout_locked(const struct fb_info *info, bool redraw_invalidated)`协调Pos及generation，调用方按mutex→Pos→state顺序持锁。

- [ ] **Step 1: 写失败测试。** `test_query_pointer_directions`纯输出GET不copy_from_user，GET_MODES只读capacity且剩余元素0；capacity0/count0/total真实、capacity1截断成功、17 EINVAL。`test_ioctl_fault_cleanup`NULL/跨页/只读输出EFAULT，unknown(cmd,NULL) ENOTTY、surrender(NULL)0。`test_set_noop`generation/硬件调用/清屏均不变；`test_set_rollback`EIO但原尺寸、generation+1、cursor0；`test_set_failed_backend`后续GET/PRESENT EIO且硬件调用不增加；raw sticky改变布局EBUSY而同模式0。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=ioctl`，旧fb只支持surrender，应断言失败。
- [ ] **Step 3: 实现指针分支。** 使用syscall_check_user_range方向和容错拷贝，锁外staging/写回、GET_MODES输出先清零。保留read offset/短读20字节ABI与无BGA GET/surrender；不按新结构扩大旧read。
- [ ] **Step 4: 实现SET状态机。** 先验证请求，mutex内复核ready/noop/sticky，transition排空，调用T4事务。APPLIED在无writer时清新区域再一致发布generation/Pos/cursor；ROLLED_BACK清旧区域再generation++返回EIO；FAILED永久关闭准入。所有recoverable出口transition_end和unlock；日志不拿短锁且显存不可信仅serial。
- [ ] **Step 5: 验证/Commit。** Run ioctl/writers/bga cases和 `make PROFILE=x86_64-clang test-gfx-device`；0失败。Commit `feat: expose safe transactional framebuffer ioctls`。

### Task 6: PTY master窗口与SIGWINCH

**Files:** Modify tty/pty.c、include/tty/pty.h、fs/file.c、对应hosttests/include/fs/file.h等PTY镜像；Create hosttests/cases/test_pty_winsize.c。

**Interfaces:** `int pty_ioctl(pty_t *pty, int cmd, void *arg)`为master/slave公共窗口及pgrp处理；旧pty_slave_ioctl委托它并保留slave特有命令，master调用公共部分并保留TCGETS。pty_t新增state_lock及ws_xpixel/ws_ypixel，四字段与pgrp在该短锁下取得一致快照。

- [ ] **Step 1: 写失败测试。** `test_master_slave_winsize`master写(30,100,800,600)，slave读四字段一致；改变任一字段才signal_pgrp(保存的前台组,SIGWINCH)一次；pgrp0和完全相同尺寸0次；user fault无状态改变，signal在锁外。master TIOCSPGRP/TIOCGPGRP及TCGETS回归。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=pty`，预期当前master ENOTTY和没有SIGWINCH断言失败。
- [ ] **Step 3: 实现公共处理。** pty_alloc每次memset后spin_init state_lock；不使用table lock跨用户拷贝或signal。复用现有signal_pgrp和定宽winsize ABI；更新fd_ioctl weak stubs及所有host linkage。并发resize读回必须四字段一致。
- [ ] **Step 4: 验证GREEN。** 结构变化 `make clean` 后Run pty case、test-host相关PTY/信号用例；新增signal host mock只观测真实公共生产逻辑，不模拟窗口算法。
- [ ] **Step 5: Commit。** `feat: support PTY master window resize notifications`。

### Task 7: terminal资源事务与空闲恢复

**Files:** Modify user/terminal_core.c/.h、terminal_render.c/.h、terminal.c、user/Makefile；Create user/terminal_display.c/.h、hosttests/cases/test_terminal_display.c；Extend test_terminal_core.c/test_terminal_render.c。

**Interfaces:** `int term_core_resize(term_core_t *t, int rows, int cols)`；新内部 `terminal_display_t`保存gfx/core/render/font/info/generation、恢复deadline、serial_only、pending_winsize。`int terminal_display_refresh(terminal_display_t *d, int fb_fd, uint64_t now_ms)`：0=无需恢复或成功、1=250ms后重试、-1=永久设备故障serial_only；`int terminal_display_present(terminal_display_t *d, uint64_t now_ms)`保持errno；恢复内部使用gfx_open/close、GET_STATE、term_render_init。这些helper不拥有PTY/ash或fd，terminal.c持有并清理资源。

- [ ] **Step 1: 写失败测试。** core双屏缩小/扩展、alt/parser/光标保存与夹取；逐个新数组ENOMEM旧core字节/指针不变。`test_terminal_recover_aba`同尺寸新generation创建新view并重新绑定renderer；`test_terminal_prepare_failure`gfx/数组失败旧handle/core仍存活、重试250ms；全16槽已占用不提前关闭旧view。`test_terminal_recover_race`准备新view后再次切换，释放new/retry无UAF。`test_idle_poll_bound`无dirty也timeout<=250，恢复<=1s；永久EIO serial_only而不退出。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=terminal`，预期resize/恢复接口不存在和旧无超时loop断言失败。
- [ ] **Step 3: 实现core及display helper。** core准备全部数组后才swap，保留两屏左上重叠矩形、不reflow；全部新格dirty、scroll_pending0。display按GET_STATE→新gfx→再次GET_STATE→core_resize→renderer_init→swap→关闭旧view→重绘/present顺序。ESTALE/EAGAIN限频重试，准备失败不退出，fatal计数仅用于非模式相关既有错误；errno在清理调用前保存。
- [ ] **Step 4: 接入terminal.c生命周期/PTY。** 保留fb_fd父生命周期、ash fork子关闭；事件循环每次检查generation，poll上限250ms不受CMD_HOLD阻挡；后台fail仍转发键盘/PTY至serial。fork后用getpgid(ash_pid)设置master TIOCSPGRP，处理ash快速退出失败路径，不改物理TTY/组。初始及恢复成功master TIOCSWINSZ四字段、失败单独限频重试不重建已成功图形。cleanup不得对已释放旧handle二次释放。
- [ ] **Step 5: 验证/Commit。** Run terminal case、现有core/render hosttests、`make PROFILE=x86_64-clang image`确认helper不生成独立ELF且terminal链接正确。Commit `feat: preserve terminal sessions across display mode changes`。

### Task 8: setres发布与既有图形客户端失效策略

**Files:** Create user/setres.c、setres_parse.c/.h、hosttests/cases/test_setres.c；Modify user/Makefile、mk/components/user.mk、config/rootfs.mk、user/desktop.c、test_lvgl.c、tetris.c、test_gfx.c、libgfx/gfx.c及hosttests/cases/test_gfx_client.c。

**Interfaces:** `enum setres_action { SETRES_HELP, SETRES_LIST, SETRES_SET }; struct setres_args { enum setres_action action; uint32_t width,height; }; int setres_parse(int argc, char *const argv[], struct setres_args *out)`；0成功、-1非法。gfx_present保留原签名与errno，不引入隐式重建。

- [ ] **Step 1: 写失败测试。** parser `1280x720`/`1280 720`正确，0/-1/4294967296/空字段/后缀/多余参数拒绝。help断言open次数0；-l当前1366×768（不在白名单）仍单独输出，capacity固定16；EBUSY提示sticky需重启。mock gfx_present EAGAIN→0时应用不退出且重试>=250ms，ESTALE/EIO时释放资源并非0退出；errno不能被cleanup覆盖。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=clients`，预期缺少CLI及旧应用忽略失败断言失败。
- [ ] **Step 3: 实现CLI及发布链。** 参数数值先安全uint32解析再设备白名单确认；无设备/list/SET错误返回1。C_SOURCES排除setres_parse与terminal_display辅助TU，setres显式链接parse；mk/components/user.mk的USER_PROGRAMS加入setres，rootfs映射/bin/setres，保留普通通用链接。程序重复fb_info定义引用uapi头。
- [ ] **Step 4: 改所有正常长期gfx用户。** desktop/test_lvgl/tetris处理EAGAIN保留view/事件输入，限频重试；ESTALE/EIO释放退出且输出诊断。短期test_gfx保持测试断言，补errno契约；`rg gfx_present user`审计漏掉的生产调用。libgfx present包装不得改errno传播；新增客户端policy host fixture运行真实分支，必要时提取用户内部 `gfx_client_policy.c/.h` 为各应用共用并从C_SOURCES排除、显式链接，接口 `int gfx_client_present_policy(int err)` 返回0=成功、1=重试、-1=退出，时间节流由调用方持deadline。
- [ ] **Step 5: 验证/Commit。** Run clients case、test-gfx-client、image；查看产物发布及guest `/bin/setres -h`存在（正式boot验证在T10）。Commit `feat: add setres and handle stale graphics views`。

### Task 9: 隔离测试配置与受控故障后端

**Files:** Create driver/fb_test.c、include/driver/fb_test.h、include/uapi/fb_test.h、user/test_resolution.c、hosttests/cases/test_fb_resolution_hooks.c；Modify mk/project.mk、mk/profiles/x86_64-clang.mk、kernel/Makefile、user/Makefile、mk/components/user.mk、config/rootfs.mk、fb.c/bga.c、terminal_display.c。

**Interfaces:** 测试专用命令号0x4650..0x4656分别SNAPSHOT、ARM_MISMATCH、ARM_ROLLBACK_FAILURE、HOLD_WRITER、RELEASE_WRITER、ARM_TERMINAL_ENOMEM、CONSUME_TERMINAL_ENOMEM。固定控制请求 `struct fb_test_req { uint32_t version, target_pid, reserved[2]; uint64_t token, reserved64; }` 32字节；version必须1、reserved必须全0。SNAPSHOT是纯输出例外，响应 `struct fb_test_snapshot { struct fb_state state; uint16_t regs[11]; uint16_t reserved; uint64_t active_writers; }` 64字节，regs按DISPI index0..10顺序、reserved=0；静态断言大小和offset。非测试构建不安装fbtest头/节点/guest helper，不编译注入branch。

- [ ] **Step 1: 写失败测试。** flags0/1切换产物路径不同；未知值和与其他变体混用parse-time非0。fault只有token指定SET事务消费一次、GET不消费、另PID SET不消费；指定terminal ENOMEM仅下一次prepare、非目标PID消费false。hold租约close/exit自动释放，不因错误请求重复计数；snapshot恢复index且用户fault无泄露。
- [ ] **Step 2: 验证RED。** Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=hooks`，预期缺少hook及配置flag未隔离。
- [ ] **Step 3: 实现profile贯通。** FB_RESOLUTION_TEST加入显式子make白名单，1时KERNEL_VARIANT/USER_VARIANT/IMAGE_VARIANT均为resolution-test，路径、FLAGS fingerprint、artifact receipts均带配置；0为原normal路径。与OS01_SYSTEST/NETTEST/KERNEL_SELFTEST/CANARY/ARCH9_FAULT非none互斥，非x86为1时明确报错；不共享修改过的init/rootfs或terminal对象。guest helper只加入该变体USER_PROGRAMS/rootfs。
- [ ] **Step 4: 实现受控测试面。** fbtest使用file-aware ops持私有lease/token状态，release_file释放；ARM由同一进程下一次布局SET携token关联（在普通SET ABI外由测试open文件登记current PID，仅一条pending事务）。寄存器新读回/rollback读回一次性伪造，物理编程仍真实。terminal通过fbtest CONSUME读取自身target PID的单次准备故障，不更改全局malloc。shutdown/fault全部清除待消费状态。生产构建没有节点注册或test TU链接。
- [ ] **Step 5: 验证/Commit。** Run hooks case；依次 `make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 image`、`make PROFILE=x86_64-clang FB_RESOLUTION_TEST=0 image`，断言两组目录独立且生产nm无fbtest/injection符号、rootfs无/bin/test_resolution。Commit `test: isolate framebuffer fault injection fixtures`。

### Task 10: QEMU硬件、画面与会话验收

**Files:** Create qemutests/test_resolution_switcher.py；Modify qemutests/run_test.py、mk/components/run.mk、docs/build/build.md、docs/architecture.md。不修改bootloader/UEFI构建；复用boot/uefi/arch/x86_64/boot.c的x86_parse_config/arch_setup_graphics读取config.txt能力。

**Interfaces:** `test_resolution(tester) -> bool`注册run_test dispatcher；根 `make PROFILE=x86_64-clang test-qemu SUITE=resolution` 使用生产配置并运行surface/session正常用例；`make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution`运行隔离故障子集。Python runner从print-run-paths取得FW/IMG，显式q35/-vga std/-smp2、serial可写pipe、独立QMP unix socket；QMP连接失败必须FAIL，不能跳过截图。新增Python `prepare_resolution_image(source: pathlib.Path, mode: str, destination: pathlib.Path) -> pathlib.Path`，只操作runner私有复制镜像；下面给出配置与污染验证。

- [ ] **Step 1: 写runner失败测试及guest断言。** guest辅助模式 `state|hold|fault|pty-watch|mmap`打印版本化JSON行及exit码，测试构建才提供；正常生产用例通过setres输出和现有/proc获取PID/PTY。Python unittest fake QMP/serial验证surface宽高/哨兵像素错误FAIL、terminal/ash PID变化FAIL、echo但无画面FAIL、fault配置混用FAIL。初态800×600往返用例，白名单外初态独立只查/输出。`test_prepare_image_isolation`：fake/copiedGPT定位ESP后mcopy只写destination的::/config.txt；源sha256不变；配置确切字节为 `resolution 800x600\n`。GPT损坏/ESP缺失/mcopy非0必须FAIL，不能继续默认尺寸启动。
- [ ] **Step 2: 验证RED。** Run `python3 -m unittest discover -s qemutests -p 'test_resolution_switcher.py'`和新root suite，预期分发/断言未实现失败；不要以QEMU未安装当预期RED。
- [ ] **Step 3: 实现QMP与生产正常用例。** 等实际shell prompt；save初态→1280×720→640×480→保存初态；同模式generation不变/无清屏；已有view的ABA失效；空闲后台setres1s内新frame、PID/PTY稳定、Ctrl-C；观察前台SIGWINCH和四字段窗口。普通rootfs保留terminal作为交互启动，不换init为测试程序。800×600由prepare_resolution_image配置：runner把所选production或resolution-test镜像复制到本次运行的独立结果目录，再用GPT主分区表定位ESP（按EFI System Partition GUID，不写死LBA），mcopy的image参数使用副本路径@@(ESP起始LBA×512)，把只含 `resolution 800x600\n` 的配置文件写到ESP卷根::/config.txt。QEMU只打开副本；不改profile原image、EFI、firmware或仓库config。启动GET_STATE和QMP都必须精确800×600，fallback到其他模式判为环境能力阻塞而非PASS。白名单外初态单独副本写 `resolution 1280x960\n`，要求实际初态精确1280×960且GET_MODES不包含它；OVMF若不提供该模式明确报告环境阻塞，不静默跳过。随后切到支持模式或重启，不SET回白名单外模式。增加原normal镜像→800副本→原normal镜像三次启动验证：原normal两次当前模式与源SHA256一致；两种镜像配置均执行副本隔离校验。无需新增UEFI flag、fingerprint或boot_context字段。
- [ ] **Step 4: 实现故障及兼容用例。** T9helper驱动失配/rollback failure/writer超时/target terminal准备ENOMEM；生产套件中的PID/画面/echo来自现有接口，精确前台SIGWINCH/winsize观测和租约ABA的guest断言由隔离测试helper子集执行，不调用生产不存在的helper；永久失败最后、后续用例重启。无BGA用不支持的显示设备且仍有可信GOP；raw mmap后sticky/munmap仍拒绝；capacity/userfault/CLI错误按host+guest分工。desktop/LVGL EAGAIN超时仍存活，ESTALE退出、terminal恢复。读取物理DISPI一致采样仅测试后端；生产正常硬件尺寸以QMP surface验证，避免生产暴露调试接口。
- [ ] **Step 5: 完成全量门禁并提交。** 先make clean；Run `make PROFILE=x86_64-clang test-resolution-host RES_CASE=all`、`make PROFILE=x86_64-clang test-host test-static`、生产resolution、测试resolution、`make PROFILE=x86_64-clang test-qemu SUITE=gfx`、`make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall`；kernel selftests独立运行，不混flags。`make PROFILE=aarch64-clang aarch64-uefi-kernel`验证架构构建未回归，并运行T4的真实非x86 stub/no-port host检查，二者证据分别记录；所有测试失败必须修复或记录环境阻塞，不把SKIP当PASS。记录日志、QMP截图、构建配置和产物路径。Commit `test: verify resolution switching and terminal session recovery`。

## 自审覆盖表与完成标准

| Spec | 实现/验证任务 |
|---|---|
| §1范围、非目标/架构 | T4/T8/T9 |
| §2容量/bootstrap/probe/映射/白名单 | T2/T4 |
| §3所有writer/锁/租约/raw mmap/旧客户端 | T2/T3/T8 |
| §4事务/noop/rollback/permanent failed | T4/T5/T9/T10 |
| §5ABI/指针/errno | T1/T5 |
| §6core资源事务/idle恢复/PTY/会话 | T6/T7/T10 |
| §7sysroot/用户发布/rootfs/clean | T1/T8/T9 |
| §8host/硬件/QMP/故障/隔离 | 每任务RED/GREEN、T9/T10 |

完成意味着生产和测试配置独立、规定行为有测试证据、全部相关门禁通过及整分支review无必要修改；plan评审批准不是代码实现通过。此计划只提供实现路径，不在写plan阶段修改产品代码。执行方式须由用户确认后使用选定执行技能。
