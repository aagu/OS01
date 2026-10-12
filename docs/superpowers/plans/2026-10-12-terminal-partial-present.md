# Terminal Partial-Present Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用部分矩形上传降低 terminal 内交互式 monitor 的 framebuffer 写入量与提交耗时，并保持 shell、显示所有权和模式恢复正确。

**Architecture:** 新增独立矩形 ioctl，保留完整帧 ABI 和逐行 bounce/lease 路径。renderer 按字符行积累待提交 damage，display 在整个逻辑帧成功后 ack，并用独立重试标志与主循环握手。先建立统一统计并保存完整帧基线，再启用 partial-present。

**Tech Stack:** freestanding C、OS01 UAPI/libgfx、现有 C hosttests、Python unittest、QEMU TCG/serial/QMP。

**Spec:** [2026-10-12-terminal-partial-present-design.md](../specs/2026-10-12-terminal-partial-present-design.md)，评审批准后的 commit `135f34b`；执行者必须同时阅读 spec 与本 plan。此 plan 待用户评审，不开始实现。

## Global Constraints

- `GFX_PRESENT_RECT=0x4709`；新请求32 B，字段偏移0/8/12/16/20/24/28；旧 `GFX_PRESENT=0x4703`、16 B请求及reserved校验不变。
- view-local坐标，pixels为完整view源缓冲区基址，stride严格等于view width×4；不隐式裁剪，不按绘制clip裁剪present，不做ENOTTY整帧fallback。
- renderer容量64，满则full；只在所有矩形成功后ack；33 ms合帧和250 ms恢复策略不变。
- `user/terminal/terminal.c`、`terminal_core.[ch]`、`kernel/driver/fb.c`、`kernel/include/driver/fb.h`保持不改；全部既有probe与默认ash保持。不得重引入整帧bulk helper或改变monitor刷新频率。
- 任意结构变化后`make clean`再重建；hosttests也清理旧对象。所有QEMU运行重新解析profile firmware/image路径，普通镜像不用systest/selftest配置。
- 新kernel日志使用log/debug接口；新统计不得新增serial_printk。用户态统计保存errno，关闭自身描述符，不占用terminal.c已有serial fd。
- 稳态窗口至少30秒；真实成功逻辑帧平均写入量<300,000 B、平均耗时≤10,000,000 ns，成功帧数非零；CPU目标约1–3%，必须来自scheduler计费。失败耗时和已写量分别报告。
- 无kernel新子系统、无boot_context/syscall编号变更；UAPI只有kernel/include/uapi/gfx.h一份定义。
- 顺序执行Task 1→6；任务间接口共享，不能并行写同一文件。每任务验证通过后独立commit；任何失败先定位，不降低验收门槛。

## Review Focus

1. range check通过后用户内存仍可能fault；实际inline memcpy必须触发容错清理，而非只测旧copy_from_user mock（Task 1）。
2. 字符行合并后再次更新其中一行，不得丢失此前尚未提交的多行矩形（Task 4）。
3. owner/mode在第N个rect之间变化，旧几何不得落到新view；后续必须完整恢复（Task 5、6）。
4. refresh提交、无damage调用、统计输出失败不能稀释平均耗时或覆盖errno（Task 3、5、6）。
5. serial的ANSI输出与probe可交错，解析器不得把缺失/损坏数据或纯--samples路径当作性能通过（Task 6）。

---

## 文件职责与固定接口

| 文件 | 职责 |
|---|---|
| kernel/include/uapi/gfx.h、kernel/driver/gfx.c | 新ABI、容错矩形上传与逐行实际字节统计 |
| libgfx/gfx.h、libgfx/gfx.c | opaque handle上的矩形wrapper |
| user/terminal/terminal_render.[ch] | damage集合查询/ack，无提交syscall |
| user/terminal/terminal_display.[ch] | 提交、恢复、逻辑帧统计与serial描述符 |
| hosttests/cases/test_gfx_device.c、test_gfx_client.c、test_terminal_render.c、test_terminal_display.c | 扩展既有生产代码host测试 |
| hosttests/mock/gfx_test_runtime.h、hosttests/Makefile | fault测试支持、测试链接/依赖 |
| qemutests/terminal_present_test.py（新）、qemutests/test_terminal_present_runner.py（新） | 聚焦QMP像素/性能driver与不启动QEMU的自测 |
| user/test_gfx/test_gfx.c、qemutests/run_test.py、mk/components/run.mk、docs/build/build.md | guest矩形场景、harness注册与命令文档 |

跨任务接口固定如下（均为内部类型，除libgfx函数外不进入UAPI）：

```c
#define TERM_DAMAGE_MAX_RECTS 64u
typedef struct { uint32_t x, y, w, h; } term_damage_rect_t;
typedef struct {
    bool full;
    uint32_t count;
    term_damage_rect_t rects[TERM_DAMAGE_MAX_RECTS];
} term_damage_t;
void term_render_get_damage(const term_render_t *r, term_damage_t *out);
void term_render_ack_damage(term_render_t *r);

/* terminal_display_ops_t新增；缺失rect hook才允许内部full兼容路径。 */
int (*gfx_present_rect)(gfx_handle_t *, uint32_t, uint32_t, uint32_t, uint32_t);
uint64_t (*now_ns)(void);
int (*stats_open)(void);
int (*stats_write)(int fd, const char *text, size_t len); /* 0/-1 */
int (*stats_close)(int fd);
```

`term_render_t`持有`term_damage_t pending_damage`。`terminal_display_t`持有`bool present_retry_pending`、`int stats_fd`（-2未打开/-1禁用）、`terminal_present_metrics_t metrics`与`uint64_t metrics_begin_ns`。metrics所有计数为uint64_t：`full_frames, partial_frames, success_ns, success_bytes, success_max_ns, noop_calls, skipped_calls, failed_attempts, failed_ns, failed_completed_rect_bytes, failed_max_ns, rect_calls`。失败字节未知前缀在输出中显式写`failed_prefix=unknown`。

## Task 1: 新UAPI与kernel矩形容错路径

**Files:** Modify `kernel/include/uapi/gfx.h`、`kernel/driver/gfx.c`、`hosttests/cases/test_gfx_device.c`、`hosttests/mock/gfx_test_runtime.h`、`hosttests/Makefile`；必要mock链接更新`hosttests/cases/test_gfx_file_lifecycle.c`、`test_fb_writers.c`。

**Interfaces:** Consumes既有`fb_gfx_writer_begin`/`fb_writer_end`/`fb_write_row_leased`。Produces `gfx_present_rect_req_t`、`GFX_PRESENT_RECT`，以及static `gfx_ioctl_present_rect(file_t *, gfx_present_rect_req_t *)`；旧ioctl行为保持。

- [ ] **1. 写ABI与矩形范围失败测试。** 在既有TEST_ENTRY表注册`test_present_rect_offsets_and_sentinel`、`test_present_rect_invalid_geometry`、`test_present_rect_errors_and_cleanup`。64×48 fake FB、view=(4,5,8,6)、rect=(2,1,3,2)：断言只改FB的x=6..8/y=6..7，源读取view的相同local位置；总write bytes=24，bounce申请=12，begin/end各一次。分别覆盖1×1右下角、full rect、零尺寸、UINT32_MAX、reserved、stride、无效请求/源地址和各lease errno。
- [ ] **2. 写真实memcpy中途fault测试。** 当前`fault_after_calls`只影响copy函数，不能覆盖v1的inline memcpy。host用mmap+PROT_NONE保护源的后续行；临时SIGSEGV handler仅对该保护区且`current->fault_jmp`已安装时调用`__builtin_longjmp(current->fault_jmp,1)`，其他fault保持fail。完成后恢复handler与映射。断言EFAULT、前缀可见、后续行sentinel、fault_jmp/cleanup/arg恢复、bounce释放、lease释放。请求copy fault沿用旧mock；增加kmalloc失败与write-row失败注入计数。
- [ ] **3. 运行red验证。** `make PROFILE=x86_64-clang test-gfx-device`，预期新ABI未定义导致编译失败；加ABI后仍须看到新dispatch断言失败，不能仅凭编译失败结束red阶段。
- [ ] **4. 实现请求与dispatch。** 字段顺序严格按spec；包含stddef供offsetof断言。无锁validate后按`first=pixels+y*stride+x*4`、`length=(h-1)*stride+w*4`检查全部乘加与用户范围，地址溢出EFAULT，几何错误EINVAL；一次租约、w×4 bounce、一次容错上下文、逐行writer。不要在fb层加新API，也不要把用户指针直接交给fb。
- [ ] **5. 实现kernel统计。** 在gfx.c私有统计中区分full/rect成功call时间，并累计每次成功row的bytes，失败后的成功前缀也计入；所有出口计失败次数。计数用64位atomic操作避免SMP丢更新；日志使用log_debug，每秒至多一条，通过mock日志验证字节计数，保持原probe原样。longjmp分支不读取setjmp后改变的未定义自动变量，局部前缀计数须volatile或直接atomic累加。
- [ ] **6. 运行green与旧路径回归。** 结构变化后`make clean`、`make -C hosttests clean`；再运行`make PROFILE=x86_64-clang test-gfx-device test-gfx-file-lifecycle`、`make PROFILE=x86_64-clang test-resolution-host RES_CASE=writers`。要求新增case实际执行、旧full与中途fault通过，无lease/分配残留。
- [ ] **7. Commit。** 只stage本任务文件；message：`feat(gfx): add leased partial rectangle present ioctl`。

## Task 2: libgfx矩形wrapper

**Files:** Modify `libgfx/gfx.h`、`libgfx/gfx.c`、`hosttests/cases/test_gfx_client.c`（既有ioctl wrapper记录新请求）；如依赖需更新`hosttests/Makefile`。

**Interfaces:** Consumes Task 1 UAPI。Produces `int gfx_present_rect(gfx_handle_t *h, uint32_t x, uint32_t y, uint32_t w, uint32_t h_)`。

- [ ] **1. 写失败测试。** `test_present_rect_request_and_errno`断言源为handle的完整pixels基址、reserved=0、stride=width×4、新ioctl恰一次；set_clip到不同区域后请求不变。NULL/零尺寸/越界/UINT32_MAX返回-1/EINVAL且ioctl次数不增。成功前errno置EDOM，成功后仍EDOM；EACCES/EAGAIN/ESTALE/EIO/ENOTTY逐项透传。
- [ ] **2. 运行red。** `make PROFILE=x86_64-clang test-gfx-client`，预期缺少函数/行为；保留失败输出。
- [ ] **3. 实现wrapper。** 减法式几何校验，不分配，不改handle内部布局。完整view请求明确调用旧`gfx_present`（一个旧ioctl），其他请求恰一个新ioctl；测试钉死这个选择，不进行失败fallback。
- [ ] **4. 运行green。** `make PROFILE=x86_64-clang test-gfx-client test-gfx-primitives`；旧open/close/present生命周期测试也通过。
- [ ] **5. Commit。** `feat(libgfx): expose view-local rectangle present`。

## Task 3: 统一逻辑帧统计与完整帧baseline

**Files:** Modify `user/terminal/terminal_display.[ch]`、`hosttests/cases/test_terminal_display.c`、必要的`hosttests/Makefile`链接支持。此任务仍提交整帧，不启用renderer damage。

**Interfaces:** Produces上文metrics类型/字段及stats/now_ns ops；private `static int display_submit(terminal_display_t *, uint64_t now_ms, bool *submitted)`供普通present与redraw共用。Task 5保留这一入口，替换其内部整帧实现。`submitted`只在实际发ioctl且全部成功时true。

- [ ] **1. 写统计失败测试。** fake时钟固定一次真实提交耗时6,000,000 ns，4×3 view写48 B；普通入口/refresh redraw分别断言full_frames=1、success_ns=6,000,000、success_bytes=48。失败帧只增加failed_attempts/failed_ns，background只增加skipped_calls。fake stats_write改变errno并推进时钟100 ms，断言不污染submit返回errno或耗时。
- [ ] **2. 写输出生命周期失败测试。** 懒打开一次；打不开置-1不重复忙open；close幂等且不重复close；1秒前不输出、1秒后输出一次；写失败不会覆盖EAGAIN；最后partial interval可输出，但清理不能复发；输出保存真实begin/end_ns。
- [ ] **3. 运行red。** `make PROFILE=x86_64-clang test-resolution-host RES_CASE=terminal`；预期新增metrics/hook或计数断言失败。
- [ ] **4. 实现统一入口与输出。** 把flush、full ioctl、错误分类与结束采样统一到display_submit；测量完成后才格式化/输出。真实clock采用CLOCK_MONOTONIC；serial ops封装open/write/close，stats_write处理EINTR/short write后返回0/-1。默认输出开启且每秒最多一条，无新增构建variant；所有统计I/O保存errno。
- [ ] **5. 固定输出协议。** 每条为`[term-present] version=1 begin_ns=... end_ns=... full_frames=... partial_frames=... success_ns=... success_bytes=... success_max_ns=... noop_calls=... skipped_calls=... failed_attempts=... failed_ns=... failed_max_ns=... failed_completed_rect_bytes=... failed_prefix=unknown rect_calls=...`；区间计数输出成功后重置，输出失败保留以供后续合并。初始化begin在首次统计调用，最终close的短区间也具有完整边界；不承诺强制每秒唤醒，只在既有调用/refresh时判断输出到期。
- [ ] **6. 运行green。** `make clean`、`make -C hosttests clean`后运行`make PROFILE=x86_64-clang test-resolution-host RES_CASE=terminal`；断言所有既有mode/ENOMEM/errno测试通过。对stats_open/write/close和clock提供默认真实ops；现有测试fake全部覆盖这些hook，避免测试真写/dev/serial。
- [ ] **7. 采baseline并保存镜像。** 用正常flags构建`make PROFILE=x86_64-clang disk.img`，通过`make -s PROFILE=x86_64-clang print-run-paths`取实际路径。把此时firmware/disk复制到`/tmp/os01-terminal-present-baseline/`，记录文件SHA256、当前commit和差异、QEMU版本/argv与实际模式。用serial stdio启动TCG、q35、512 MiB、-smp 2、-snapshot、-display none；等真实shell提示后输入`monitor`，排除入场后至少前5秒，再采≥30秒并用q退出、echo确认shell存活。保存原term-probe、新term-present和monitor CPU行。此时新统计必须显示稳态full_frames非零、partial_frames=0，不能用--samples代替交互。baseline日志存入`build/x86_64-clang/test-results/terminal-present/baseline/`；不提交镜像/日志。
- [ ] **8. Commit。** `feat(terminal): measure real logical presents and preserve full-frame baseline`；commit hash与构建manifest补记到baseline目录。

## Task 4: Renderer待提交damage集合

**Files:** Modify `user/terminal/terminal_render.[ch]`、`hosttests/cases/test_terminal_render.c`。core与terminal.c不改。

**Interfaces:** Produces上文`term_damage_t`、get/ack，term_render_init默认full。现有`term_render_flush`和`term_render_clear`签名不变。

- [ ] **1. 写集合失败测试。** 测试先ack初始化full、隐藏光标以隔离cell damage：font8×16，cells(row1,col2)/(row3,col7)得到rect(16,16,8,16)/(56,48,8,16)，不能合大bbox；同行col2与col4合span宽24；相邻同行跨度合h32。flush后core dirty已清，pending仍在；再次flush无新变化不能清pending；ack后为空。
- [ ] **2. 写失效与累积失败测试。** `test_damage_merged_rows_updated_again`先合两行，再修改其中一行，断言pending覆盖全部旧/新像素；`test_damage_overflow_promotes_full`构造65个不可合并span（至少130字符行，交错x且隔行），第64个保持列表、第65个full。cursor旧/新cell、隐藏cursor擦除、clear、scroll、alt切换、resize/reinit和网格外余量逐项验证；full状态覆盖后续新增damage。
- [ ] **3. 运行red。** `make -C hosttests test-terminal-render`，预期查询接口或集合断言失败。
- [ ] **4. 实现集合与绘制收集。** 在flush绘制cell前记录本行min/max，按font尺寸转local像素；cursor绘制包含damage。clear和任何scroll使full。集合合并只允许相同y/h横向合并、x/w相同且纵向相邻；重复迭代合并直到稳定，覆盖不足时允许保守重叠rect，绝不删除旧覆盖。先尝试合并再检查容量；最终按y/x排序。get返回拷贝，ack清full/count而不改像素。
- [ ] **5. 运行green。** `make clean`、`make -C hosttests clean`后运行`make -C hosttests test-terminal-render`和`make PROFILE=x86_64-clang test-resolution-host RES_CASE=terminal`；display尚未使用damage，旧测试仍必须通过。
- [ ] **6. Commit。** `feat(terminal): retain row-span damage until present acknowledgement`。

## Task 5: Display部分提交与失败重试握手

**Files:** Modify `user/terminal/terminal_display.[ch]`、`hosttests/cases/test_terminal_display.c`。

**Interfaces:** Consumes Task 2 wrapper、Task 3统一计时入口、Task 4 damage；Produces rect ops hook、present_retry_pending与spec §7状态表。现有对外present/refresh/poll_timeout签名保持。

- [ ] **1. 写多rect与ack失败测试。** 两个span只调用两个rect hook，总bytes按每个w×h×4相加；全成功ack。第2个EAGAIN时第1个成功bytes计入failed_completed_rect_bytes，未知失败前缀明确输出，整个damage保留；下一轮重试再次包含两rect。缺rect hook走单full；真实rect hook返回ENOTTY不得fallback。no damage不调用ioctl且只加noop。
- [ ] **2. 写调度握手失败测试。** now=100失败→deadline350；refresh349不提交、refresh350在无新PTY情况下提交，成功清retry/deadline、refresh_presented=true。模拟main读取该标志清dirty_pending，断言无第3次提交。无retry deadline的一般dirty即使refresh多次也不提交。重试再次EAGAIN→deadline600、errno不变；partial恢复后新增dirty合并且不丢覆盖。
- [ ] **3. 写状态表失败测试。** 参数化EAGAIN/ESTALE/EACCES/EIO/ENODEV/ENXIO/EBADF/ENOTTY/ENOMEM/EFAULT/EINVAL/EBUSY；普通与refresh入口分别断言spec状态、return、errno和计数责任。rect之间owner变化要保留damage并background，恢复full；mode变化要rebind新尺寸full且旧rect不发新view。资源准备deadline不能自行开启partial retry；full redraw失败保持redraw_pending。
- [ ] **4. 写统计防稀释失败测试。** 用同一6 ms帧分别普通提交/refresh到期重试，成功统计一致；插入1000个no-op/skipped，success_ns和成功帧数不变。失败耗时不进入success；full_bytes包含actual view余量；partial统计包括全部rect。统计hookI/O仍保护errno。
- [ ] **5. 运行red。** `make PROFILE=x86_64-clang test-resolution-host RES_CASE=terminal`，预期partial次数、ack、deadline或统计断言失败。
- [ ] **6. 实现选择与状态转换。** 在display_submit中flush后get快照，full走旧present、列表按y/x走rect、空走noop；保留所有damage直到全成功。实际success统一ack和清retry；普通EAGAIN/ESTALE设retry，其他普通失败仍main负责；refresh依spec表先查询owner/mode，再按到期且失败标志执行一次，actual success才置refresh_presented。计时与metrics按Task 3协议累积。
- [ ] **7. 运行green与owner回归。** `make clean`、`make -C hosttests clean`后运行`make PROFILE=x86_64-clang test-resolution-host RES_CASE=terminal`、`make -C hosttests test-display-owner test-fb-writers`；全部新增case实际注册执行。`git diff -- user/terminal/terminal.c user/terminal/terminal_core.c kernel/driver/fb.c`必须为空。
- [ ] **8. Commit。** `feat(terminal): submit partial damage with explicit retry handshake`。

## Task 6: QEMU像素正确性、性能门控与最终回归

**Files:** Create `qemutests/terminal_present_test.py`、`qemutests/test_terminal_present_runner.py`；Modify `user/test_gfx/test_gfx.c`、`qemutests/run_test.py`、`mk/components/run.mk`、`docs/build/build.md`。现有monitor_test.py的1/2/8功能矩阵不改。

**Interfaces:** 新聚焦suite名`terminal-present`（本任务实现之前不存在）。Python入口`test_terminal_present(tester=None) -> bool`与`main(argv=None) -> int`；CLI固定`--image PATH --firmware PATH --build-dir PATH --baseline-log PATH`。正常验收baseline-log必填，新增`--capture-baseline`时不需要baseline-log，仅采集完整帧数据；正常验收不能设置该标志。Make入口新增`TERMINAL_PRESENT_BASELINE_LOG`，默认`$(BUILD_DIR)/test-results/terminal-present/baseline/serial.log`，透传同名环境变量给dispatcher。聚焦driver使用现有ProcessSession与test_display_owner.py的QMP/parse_ppm/Surface，所有运行有独立artifact目录；不得复用会raw mmap的gfx测试进程做后续setres。

- [ ] **1. 写parser/gate失败自测。** `python3 -m unittest qemutests.test_terminal_present_runner`：覆盖ANSI与probe交错、字段缺失/重复、非单调时间/重叠区间、版本错误、空成功帧、失败窗口、计数溢出、300000 B恰等（FAIL）、10000000 ns恰等（PASS）、10000001 ns（FAIL）；至少30秒完整区间且成功分母非零。新增`evaluate_present_window(records)`用整数交叉乘法比较总bytes/总frames和总ns/总frames，不平均每秒平均数；有失败的稳态窗口先FAIL并保留失败统计，不能通过挑掉失败降低成本。
- [ ] **2. 写driver失败自测。** fake ProcessSession/QMP覆盖shell未就绪、QMP/截图失败、monitor缺CPU行、q退出后shell标记缺失、child非零退出、截图rect外变化、baseline模式不同。证明`--samples` transcript不能通过交互性能测试。新unittest模块追加到TEST_HARNESS_MODULES，默认unittest不得启动QEMU。
- [ ] **3. 运行red。** 执行上述unittest，预期parser/driver/注册缺失造成明确失败；保留结果。
- [ ] **4. 实现guest矩形surface模式。** 将guest入口改为`int main(int argc, char **argv)`，扩展`/bin/test_gfx --present-rect-surface`，无参数旧行为不变。新模式不mmap FB：开带非零origin的小view、acquire、full填yellow sentinel，然后先画rect内blue、rect外red到私有buffer，仅present指定rect；从独立`/dev/serial`输出ready标记并等待stdin推进。driver在full sentinel和partial后分别QMP截图，逐像素断言rect内blue、rect外与之前完全相同。继续1×1右下角和full rect场景，标记退出、release/close，确认terminal恢复。不使用会设置raw_mmap_seen的旧guest读回模式运行setres。
- [ ] **5. 实现聚焦driver与suite注册。** 启动普通镜像，明确`-accel tcg -M q35 -m 512 -smp 2 -snapshot -serial stdio`与QMP socket；持续pump serial避免PTY背压。先运行guest surface，再在terminal内运行monitor，隐藏cursor稳态采集≥30秒，记录interval、实际width/height/stride、完整argv、QEMU版本、镜像/firmware SHA256、所有metrics、CPU行及原term-probe。用q与Ctrl-C各验证一次shell存活；用终端ANSI fixture验证clear、scroll、光标显隐、alt屏退出和setres往返后的QMP画面恢复。扩大父runner预算到能容纳该窗口，超时明确FAIL。结果走既有RunArchive/RunReport，存`build/x86_64-clang/test-results/terminal-present/`，保留截图、raw logs、JSON汇总。
- [ ] **6. 固定baseline比较。** 使用Task 3保留的同宿主/同模式baseline原日志，主比较为新版metrics、CPU与old probe共同报告。如需要重复baseline，driver对`/tmp/os01-terminal-present-baseline/`镜像运行只采baseline模式（CLI增`--capture-baseline`，允许完整帧成本超新门槛，但仍要求≥30秒与真实提交）；不得用新镜像伪造baseline。模式或加速器不匹配直接报告不可比较，不猜handoff分辨率。idle窗口至少10秒，无full/rect调用，排除初始化与退出。若CPU进程迁移，报告实际所属CPU和计费，不能强称CPU0；与1–3%目标差距必须解释。
- [ ] **7. 运行host/harness green。** `python3 -m unittest qemutests.test_terminal_present_runner`、`make test-harness`、`make test-host`、`make test-static`；确认suite解析、target capability gate与测试模块注册正确。文档明确新增命令及与旧monitor功能矩阵的区别。
- [ ] **8. clean重建与QEMU验收。** `make clean`后`make PROFILE=x86_64-clang disk.img`，重新`print-run-paths`；运行`make PROFILE=x86_64-clang test-qemu SUITE=terminal-present`（新suite从baseline artifact读取，找不到baseline即FAIL并说明先采集）。真实成功平均写入<300000 B且耗时≤10ms，所有surface检查通过；保存长尾/失败和CPU证据。性能未达标时分析rect分布/列表溢出/重复绘制，不能宣称完成或用bulk/改频率绕过。
- [ ] **9. 运行既有QEMU回归。** `make PROFILE=x86_64-clang test-qemu SUITE=gfx`、`make PROFILE=x86_64-clang test-qemu SUITE=monitor`、`make PROFILE=x86_64-clang test-qemu SUITE=resolution`、`make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution`；display owner独立脚本确认为`python3 qemutests/test_display_owner.py --smp 1`及`--smp 2`，在正常镜像构建后运行，两次都PASS。每套件启动独立QEMU，不把raw mmap sticky带入下一套件；新增driver使用-snapshot/私有image，既有owner脚本按当前路径查询正常镜像启动，不假称其已使用私有副本。1/2/8 monitor矩阵、owner close/release/kill、后台切换与fault恢复均必须通过。
- [ ] **10. Commit与最终证据。** `test(terminal): gate partial-present pixels and monitor cost`；git diff --check通过；最终report列出6个任务commit、实际模式、前后总字节/帧/耗时、scheduler CPU、回归结果和artifact路径，不把历史hand-off数据写成新测量。implementation结束再执行整分支review与finishing流程。

## 自查与执行交接

- spec §4–5→Task 1/2；§6→Task 4；§7→Task 5；§8→Task 3/6；§9→各层任务与Task 6最终矩阵；§10→Global Constraints及最终diff检查。
- Task 3先形成统计口径一致的整帧baseline，Task 4/5之后才启用partial；Task 6新target、CLI、模块有明确注册责任，没有假设它们已存在。
- 固定接口统一使用term_render_get_damage/term_render_ack_damage、display_submit与metrics字段；旧公开display入口保持签名。所有6个任务独立完成red/green/commit，最终QEMU失败不能被host mock替代。
- 用户尚未选择执行方式；此文只交付plan。评审后可选择Native（本会话逐任务实现，末尾独立review）或Subagent-driven（逐任务实现与评审）。这些任务共享display/renderer接口，推荐Native以减少重复上下文；实施前仍需用户评审并选择方式。
