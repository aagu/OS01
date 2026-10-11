# 简化显示所有权与自动恢复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 用户选择其他执行方式时再切换流程。

**Goal:** GUI 通过已有 gfx FD 独占物理显示；正常归还或最后一个 FD 引用关闭时恢复 fallback，后台 terminal 持续消费 PTY 并在恢复后全量重绘。

**Architecture:** 每个 open file 的 `gfx_view_t` 同时作为显示所有权身份，不新增 session 设备、注册表或 compositor。前台/fallback 指针与独立 ownership generation 放在已有 `fb_state.c` 协调器中，复用 control mutex、state spinlock 和帧级 writer 排空机制。terminal 注册为首版 fallback，其他 GUI 显式 acquire；view 重建通过原子 fallback 替换保持角色。

**Tech Stack:** freestanding C、x86_64、现有 VFS file 引用计数、libgfx、LVGL 9.5.0、hosttests、QEMU Python。

**Spec:** 本文「设计契约」记录用户已确认的简化方案及本次实施所需细节；相关基线：[2D API spec](../specs/2026-09-30-2d-graphics-api-design.md)、[模式切换 spec](../specs/2026-10-06-qemu-resolution-switcher-design.md)。

**Baseline:** `87e14b29`（2026-10-07）；路径以当前 `user/<app>/` + `app.mk` 组织为准。本文仅规划，不表示实现或验收已完成。

## Global Constraints

- 仅一个前台 owner、一个 fallback；不支持嵌套 GUI 抢占、多个 VT、输入焦点管理或多窗口合成。
- fallback 是通用 gfx view，第一版由 terminal 注册；不是内核保存终端字符屏幕。
- ownership generation 不复用 `fb_state.generation`；前者变化触发重绘，后者变化继续使用现有模式恢复流程。
- 所有正常物理写入遵守同一所有权检查；不把分配 view、查询信息或在用户态绘制缓冲作为接管。
- file 引用计数按现有语义；最后一个引用关闭才释放角色。dup/fork 继承不等于新 owner，子进程/exec 前必须显式关闭继承的 gfx FD；当前 F_SETFD 是占位，不能依赖 CLOEXEC。
- 不持有 spinlock 调用 uaccess、分配、等待或 file_put；不新增第二套 writer 排空锁。
- 新内核公开接口放 `kernel/include/driver/`，ABI 放 `kernel/include/uapi/`；改变 struct 后按项目要求 `make clean`。
- 普通模式禁止 writable raw fb mmap；read-only 像素回读继续兼容，真正安装只读 PTE。测试模式保留 raw writable 路径，和独占模式互斥。
- 无完整权限模型的阶段，fallback 注册是协作接口：首个注册成功，替换必须持有旧 fallback FD；不宣称防止恶意进程抢占身份。

## Review Focus

1. 接管期间有旧帧提交：先关闭 admission，再排空 writer，更新 owner 后重开，Task 1/2 钉死整帧顺序。
2. GUI 被杀死或 FD 继承：最后引用释放恢复 fallback，内核不持有阻止释放的 owner file 引用，Task 2/3/6 验证。
3. fallback 关闭或模式变化：清除悬空指针；旧 view 到新 view 原子换绑，不因关闭旧 view 丢角色，Task 1/4 验证。
4. 后台终端输出和显存抢写：继续 PTY 解析，暂停像素渲染/提交；恢复全部重绘，Task 4/6 验证。
5. raw mmap 绕过独占：普通模式 writable mmap 拒绝、readonly PTE 不可写；测试模式 writable sticky 与 acquire 双向互斥，Task 2/6 验证。

## 设计契约

### 状态与错误语义

协调器新增 `const void *owner`、`const void *fallback`、`uint64_t owner_generation`（初始 1）；cookie 就是 gfx view 地址，仅比较身份。指针非 owning，不增加 file 引用；释放 callback 必须先 detach 再释放 view。外部状态只返回 flags/generation，绝不泄露内核指针。

- 首个合法 full-screen fallback 注册：无 owner 时成为 owner；已有普通 GUI owner 时只成为恢复目标。再次注册同一对象幂等；不同 fallback 返回 `EBUSY`。
- acquire：必须是已配置且当前模式有效的 view（允许局部 view，仍独占整个显示设备的提交权）；owner 为 NULL、fallback 或自身时允许（自身幂等）；其他 GUI owner 存在时 `EBUSY`，不覆盖旧 owner。
- release：自身为普通 owner 时恢复存活 fallback，否则回到 kernel；自身已是 fallback 或非前台时幂等，不影响他人。
- final close：owner/fallback 任一身份都必须清除；owner 关闭恢复 fallback；fallback 在后台关闭不抢走 GUI；两者相同时关闭后回到 kernel。
- replace fallback：在新 view FD 上携带旧 FD；校验旧 FD 是 gfx、旧 view 等于当前 fallback、新 view 已配置且属于当前模式，才换绑。旧 fallback 前台时 owner 一并换绑；GUI 前台时保持 GUI owner。失败保持旧角色，旧 view 关闭不再产生角色变更。
- 角色身份改变时 ownership generation 增加；mode generation 不变。状态查询不要求 view 的 mode generation 最新，以便 stale fallback 查询并重建。
- present：模式转换中 `EAGAIN`，模式陈旧 `ESTALE`，设备失败 `EIO`；非 owner `EACCES` 表示正常后台暂停。请求结构/用户指针错误沿用既有 errno，不能破坏 writer 计数。
- 无 owner 时允许内核控制台绘制，未 acquire 的用户 view 仍不能 present；owner 存在时正常内核 fb writer 被拒绝，串口仍输出。
- panic 保持串口优先及现有 best-effort 路径，不让 panic 获取 control mutex 或等待恢复；本阶段不承诺 panic 从活动 GUI 强制抢屏。

### UAPI 与 libgfx

在 `kernel/include/uapi/gfx.h` 添加未占用编号（实现前复核）：

| ioctl | 编号 | 参数 |
|---|---|---|
| `GFX_ACQUIRE` | `0x4704` | 无 |
| `GFX_RELEASE` | `0x4705` | 无 |
| `GFX_REGISTER_FALLBACK` | `0x4706` | 无 |
| `GFX_GET_DISPLAY_STATE` | `0x4707` | 输出 `gfx_display_state_t` |
| `GFX_REPLACE_FALLBACK` | `0x4708` | 输入 `gfx_replace_fallback_t` |

`gfx_display_state_t = { uint64_t generation; uint32_t flags; uint32_t reserved; }`，16 bytes；flags 定义 `FOREGROUND=1`、`FALLBACK=2`，reserved 输出 0。
`gfx_replace_fallback_t = { int32_t old_fd; uint32_t reserved; }`，8 bytes；reserved 输入必须 0。
现有三个 gfx ioctl 和结构布局不变；这次将 present 从任意 view 提交改为显式授权，所有自有调用者同步迁移。

libgfx 公共接口：

```c
int gfx_acquire(gfx_handle_t *h);
int gfx_release(gfx_handle_t *h);
int gfx_register_fallback(gfx_handle_t *h);
int gfx_get_display_state(gfx_handle_t *h, gfx_display_state_t *out);
int gfx_replace_fallback(gfx_handle_t *old_h, gfx_handle_t *new_h);
```

封装使用内部 FD，不新增公共 raw FD getter。三个无参数命令（acquire/release/register fallback）仍显式调用 `ioctl(fd, cmd, (uint64_t)0)`：当前 libc ioctl 无条件读取第三个 variadic 参数，不能省略。NULL 参数 `EINVAL`，失败保留 ioctl errno。不依赖 `FD_CLOEXEC`：当前 `sys_fs.c` 中 F_SETFD 忽略该标志，完整实现属于独立 FD ABI 工作。terminal 在 fork 的 ash 子分支、其他客户端在 fork 子分支或 exec 前必须显式 `gfx_close` 继承副本（fork 后父地址空间不受影响）。后台普通应用持有 view 仍可绘制自己的 RAM buffer。

### 并发与最后关闭

所有角色控制与模式 SET 共用 `fb_control_lock()`。切换操作在排空后以既有顺序 `control mutex → Pos.lock → display_state_lock` 提交角色与普通 console 可绘制状态；任何路径不得反向取锁。
新增 `fb_gfx_writer_begin(lease, mode_seq, cookie)`，在 `display_state_lock` 一次临界区内同时检查 owner、模式、transition 和增加 active_writers；不能先检查 owner 再调用旧 writer_begin。
旧 `fb_writer_begin` 作为内核正常 writer 入口，同锁检查 owner==NULL。

final close 不走可失败的排空协议：最后 file 引用释放说明该 view 没有仍持引用的 present/ioctl。detach 在 control mutex 下清指针并更新角色；不得因为后台设备故障/排空超时跳过 detach。后台 fallback 被关闭时，即使 GUI 有 writer 也只清 fallback，不换 GUI owner。必须用测试证明 syscall 路径持 file 引用直至 writer_end，且 release_file 在文件表锁外执行。

### raw mmap 与兼容范围

- `fb_mmap` 根据 `VMA_PROT_WRITE` 判断 writable，read-only 安装时清除 PTE write bit。
- 为防止 readonly 映射被升级或丢失 MMIO 属性，`do_mprotect` 在 mm 锁下预检整个目标范围，遇任何 `VMA_IO` 返回 `EPERM`，不得先修改其他 VMA/PTE。普通构建 writable 请求返回 `EPERM`（映射前拒绝，不设置 sticky）。`FB_RESOLUTION_TEST=1` 保留 writable 故障测试。
- 所有 raw 映射仍维持现有 `raw_mmap_seen` sticky，模式改变仍 `EBUSY`；本阶段不做 VMA 撤销/生命周期计数。
- 另记 writable raw sticky：有 writable raw 映射历史时 acquire/register fallback 返回 `EBUSY`；已有 owner/fallback 时 writable mmap 返回 `EBUSY`。检查和建立映射共用 control mutex，避免竞态。
- writable raw sticky 与原有 raw sticky 都必须覆盖失败后的残留：writable 请求部分 map 失败，回滚后只要还存在任何已安装 PTE，就同时设置两种 sticky；成功映射同样设置两种 sticky。失败且回滚确认无残留时不设置 sticky。整个建立/回滚/残留检测/记录过程持有 control mutex；普通构建映射前拒绝 writable，不会留下 PTE。
- read-only 映射允许像素回读，不禁止 ownership 切换；仍会限制模式 SET，记录为已知边界。
- `test_gfx`、`tetris_dump` 改为 `PROT_READ`；`test_terminal_screen` 保持请求只读；`test_resolution mmap` 保持测试模式 writable 语义，独立重启测试 raw 场景。

### terminal 与客户端策略

terminal 注册 fallback 后才第一次 present；移除 `FBIOSURRENDER` 作为主要接管手段。旧 ioctl 继续兼容串口/console 抑制语义，但不能授予 gfx 所有权。
每次事件循环查询本 view 的 display state，后台不渲染、不提交，继续消费 PTY/更新 core/串口输出；状态检查至多延迟 250ms，首版不新增事件 FD 或 poll 通知。
恢复 foreground 或 ownership generation 变化时 `term_core_mark_all_dirty` + 全量重绘；所有 present 路径（refresh 重绘、普通帧、退出清屏）都受同一前台检查。`EACCES` 不累计故障，不设 serial_only，保留 redraw_pending；mode 的 `EAGAIN/ESTALE` 继续沿用现有恢复。
模式恢复在新 view 准备完成且 mode 二次查询一致、core resize 成功后，先 `gfx_replace_fallback(old,new)`，成功后才交换 handle/关闭旧 view；replace 失败关闭新 view、保留旧 view 和重绘重试状态。core resize 已成功但换绑失败时保留一致的新 core geometry，并在下一次 mode refresh 重试，期间禁止对旧尺寸 view 渲染。

GUI 先准备离屏首帧，再 acquire，再真实 present。acquire/首次提交失败退出时关闭所有已取得资源，恢复不依赖 ANSI 转义、固定 550ms 等待或 atexit。LVGL 的 smoke 明确纯离屏，showcase/desktop/tetris 必须报告真实屏幕失败，不静默伪装成功。
输入焦点不在本计划内：terminal 现有 PTY 输入转发不改；现有经 shell 启动且从 stdin 读键的 GUI 仍由 terminal 转发。直接读取物理键盘的 GUI 竞争问题留在 GUI-2，不能把显示 owner 说成输入 owner。

## 文件职责

| 文件 | 改动 |
|---|---|
| `kernel/driver/fb_state.c`、`kernel/include/driver/fb_state.h` | 单锁所有权、generation、角色切换、writer 授权与 raw writable sticky |
| `kernel/driver/gfx.c`、`kernel/include/uapi/gfx.h` | 新 ioctl、替换 FD 校验、present 入口、final close detach |
| `kernel/driver/fb.c`、`kernel/memory/vma.c`、`kernel/tty/console.c` | raw/PTE 权限、IO mprotect 拒绝与普通控制台协调（fb_surrendered 不再是 ownership 权限来源） |
| `libgfx/gfx.c`、`libgfx/gfx.h` | 包装、显式关闭生命周期 |
| `user/terminal/terminal_display.c/.h`、`user/terminal/terminal.c` | 后台状态、全量恢复、模式换绑、fork 后关闭继承 view |
| `user/test_lvgl/test_lvgl.c`、`user/desktop/desktop.c`、`user/tetris/tetris.c`、`user/common/gfx_client_policy.c/.h` | acquire/release 与失败清理，移除延时协调 |
| `user/test_gfx/test_gfx.c`、`user/tetris_dump.c` | 显式授权、只读回读，多 view 测试主动 release 后切换 |
| `hosttests/cases/test_display_owner.c`、已有 fb/gfx/terminal 用例 | 状态机、生命周期、竞态与恢复测试 |
| `user/test_display_owner/test_display_owner.c`、`user/test_display_owner/app.mk` | 专用 guest 控制器、崩溃/继承/只读写入与 mprotect 探针 |
| `config/inittab.display-raw`、`qemutests/test_resolution_switcher.py` | 测试模式 raw 与 drain 各自使用私有直接 ash 镜像，保持无 fallback；普通镜像不变 |
| `qemutests/test_display_owner.py` | 端到端自动验收与 runner 单元验证 |
| `hosttests/Makefile`、`mk/components/run.mk`、`docs/build/build.md` | 注册 host 目标及目录程序/rootfs 自动发现契约核查 |
| `docs/gui/gui.md`、`docs/driver/driver.md`、`docs/roadmap.md`、`docs/changelog.md` | 新协议、兼容边界、完成记录 |

## Task 1: 单锁所有权协调器与 writer 授权

**Interfaces:** `int fb_gfx_writer_begin(fb_lease_t *, uint64_t mode_seq, const void *cookie)`；`int fb_owner_acquire(const void *, uint64_t expected_mode)`；`int fb_owner_release(const void *)`；`int fb_owner_register_fallback(const void *, uint64_t expected_mode)`；`int fb_owner_replace_fallback(const void *old_cookie, const void *new_cookie, uint64_t expected_mode)`；`void fb_owner_detach(const void *)`；`int fb_owner_get_state(const void *, gfx_display_state_t *)`。身份/configured 和 fallback full-screen 校验由 gfx 层完成；coordinator 不解引用 cookie，在 control mutex 与 state lock 内复核 expected_mode，防止前置校验后被 mode SET 竞态穿过。

- [ ] 在 `hosttests/cases/test_display_owner.c` 添加失败用例：A fallback → B owner → C acquire `EBUSY`；B 重复 acquire/release 幂等；释放恢复 A；后台 A detach 保留 B；B detach 无 fallback 回 kernel；旧 A 换新 A2 后 A detach 不影响 A2；角色切换不改变 mode generation。
- [ ] 将目标 `test-display-owner` 注册到 hosttests 和根 harness。运行 `make PROFILE=x86_64-clang test-display-owner`，确认新行为用例失败，不能只因 mock 无法编译而跳过行为验证。
- [ ] 在 `fb_state.c/.h` 实现上述接口，复用既有 transition 与控制锁；并将正常内核 writer 和 gfx writer 分开授权。初始化没有角色时 owner_generation=1。
- [ ] 添加并运行 interleaving 用例：旧帧 begin → acquire 开始排空 → 新 writer `EAGAIN` → old end → 新 owner 提交；排空失败旧 owner/generation 不变；角色检查和 writer_count 不存在间隙；uaccess fault 所有出口释放 lease。
- [ ] 运行 `make PROFILE=x86_64-clang test-display-owner` 和 `make PROFILE=x86_64-clang test-resolution-host RES_CASE=writers`；同时检查断言汇总与退出码，避免既有测试框架统计复位缺陷掩盖失败。
- [ ] 提交本任务实现与测试，消息 `feat(gfx): coordinate exclusive display ownership with writer leases`。

## Task 2: gfx UAPI、最后关闭与 raw mmap 边界

**Interfaces:** 上述五个 ioctl；`gfx_release_file` 在清 view table/free 前调用 `fb_owner_detach`；`fb_mmap` 的 writable 判定与 sticky 检查在 control mutex 下。

- [ ] 在 `test_gfx_device`/`test_gfx_file_lifecycle`/fb ioctl host 用例中添加失败测试：未配置 acquire `EINVAL`、非 full-screen fallback 注册 `EINVAL`、stale acquire `ESTALE`、非法 old_fd `EBADF`、非 gfx FD/不为 fallback `EINVAL`、reserved 非零 `EINVAL`、错误用户指针 `EFAULT`；后台有效 present `EACCES` 且零像素写入、零 writer 泄漏。
- [ ] 实现固定宽度 ABI 和 static asserts；replacement 使用 `files_get_file` 取得旧 FD 引用，校验设备类型/ops 后读取私有 view，完成换绑后在所有锁外 file_put。不能依据用户输入 PID 或内核地址认证角色。
- [ ] 实现 release callback 无条件 detach；不对 owner/fallback 执行 file_get 形成引用循环。证明持有 dup 时 close 原 FD 不恢复，最后 dup close 才恢复；并发查询/close 不解引用已释放 cookie。
- [ ] 在 fb mmap host 用例验证普通 writable `EPERM`、readonly 页表无 WRITE、mprotect 尝试写/执行/PROT_NONE 均 EPERM 且整个范围无变更、测试模式 writable sticky 与所有权双向 `EBUSY`；注入部分 map 失败 + unmap 失败，确认残留可写 PTE 令后续 acquire/register 均 `EBUSY`，成功回滚无残留不设置 sticky；模式 SET 对 readonly sticky 的现有行为保持。
- [ ] 保持 console 状态变化的锁顺序；统一 `fb_surrendered` 与普通 console active 的切换，kernel fallback 恢复时不保留错误的 surrender 状态。设备永久失败时仍清角色指针，保留串口回退。
- [ ] 运行 `make PROFILE=x86_64-clang test-gfx-device`、`test-gfx-file-lifecycle`、`test-resolution-host RES_CASE=ioctl`、`test-display-owner`；先验证失败，再实现通过。提交 `feat(gfx): expose ownership ioctls and restore on final close`。

## Task 3: libgfx 包装与显式 FD 继承契约

**Interfaces:** 使用设计契约的五个 libgfx 接口；不引入自动 acquire，也不调用占位 F_SETFD 假装防泄漏。

- [ ] 在 `hosttests/cases/test_gfx_client.c` 添加 wrapper 参数/errno/FD 校验用例：old/new 反向参数不混淆、replace reserved=0；三个无参数命令的 mock 核对显式第三参数 `(uint64_t)0`；错误参数不发 ioctl；成功不额外清 errno。
- [ ] 运行 `make PROFILE=x86_64-clang test-gfx-client` 确認新增测试失败；实现 wrappers，完善 mocks，复跑通过。
- [ ] 在 `test_display_owner` guest helper 规划 `dup-close`、`fork-close`、`close-before-exec` 子命令：fork 前关闭或子分支立即关闭继承引用；展示裸 fork 保留引用时只承诺最后关闭恢复，不能声称任意 owner PID 死亡都恢复。
- [ ] 提交 `feat(libgfx): add display ownership wrappers`。

## Task 4: terminal 后台处理与恢复目标原子换绑

**Interfaces:** `terminal_display_ops_t` 新增上述 ownership wrapper 的注入操作；display helper 保存 `owner_generation`、`foreground` 和 `redraw_pending`，定义正常暂停返回值 `TERMINAL_PRESENT_SKIPPED=1`（0 已提交，-1 错误）。

- [ ] 在 `hosttests/cases/test_terminal_display.c` 添加失败用例：后台收到 PTY/core dirty 不调用 render/present；owner generation 恢复必须完整重绘；EACCES 不计故障；mode 新 view 先 prepare/replace 后关旧；replace 失败不释放旧角色；后台模式恢复不偷回 foreground。
- [ ] terminal 初始化准备内存后注册 fallback，再首次提交；失败完整 cleanup 回退。ash fork 子分支立即关闭 gfx 副本；移除显示协调用途的 CMD_HOLD_MS 和 FBIOSURRENDER，保留 ANSI alt-screen 正常解析。
- [ ] 修改所有呈现入口（包括 refresh/redraw 与 teardown）统一检查前台状态；主循环持续 drain PTY/串口，禁止“后台”导致退出/serial_only 或零超时忙轮询。无 dirty 时也最多 250ms 检查恢复。
- [ ] mode refresh 使用原子替换；已有 GUI foreground 时只替换 fallback。GUI 因 mode stale 退出后，终端可用新 view 重绘；如果准备失败，角色保留，用户状态不丢。
- [ ] 运行 `make PROFILE=x86_64-clang test-resolution-host RES_CASE=terminal` 和相关 terminal core/render 测试，检查汇总。提交 `feat(terminal): pause display in background and redraw on restore`。

## Task 5: GUI 客户端与旧测试迁移

**Interfaces:** GUI 首帧在 RAM 准备完成后 gfx_acquire；明确失败退出并关闭 handle，release 可主动归还；模式 ESTALE/EIO 沿用现有公共策略（退出），不扩大为 GUI view 自动重建。

- [ ] 更新 `test_lvgl`：smoke 固定离屏、不得触摸物理显示；showcase/wait 模式显式 acquire，去掉固定 550ms 屏幕让位延时；present 错误退出并归还，不能静默离屏通过屏幕验收。
- [ ] 更新 desktop/tetris 的 startup、错误出口与正常 close；第二 GUI acquire `EBUSY` 明确报告。公共 present policy 明确 EACCES 为失去前台、归还并退出，不忙重试；terminal 使用自己正常暂停策略。
- [ ] 更新 test_gfx：需要呈现的每个 view 先 acquire；full/small 多 view 测试使用 release → acquire 切换，保留对内核像素输出的验证。test_gfx/tetris_dump raw 回读请求 PROT_READ。
- [ ] 增加 `user/test_display_owner/app.mk`（链接 -lgfx）并核对当前 `user/apps.mk` 发现契约；以干净 rootfs 证明 /bin/test_display_owner 被打包，不恢复旧硬编码 USER_PROGRAMS。
- [ ] 增加 guest 子命令 `hold`（获取后输出 READY 并持续睡眠）、`kill-owner`（子进程获取、父进程 SIGKILL 后 waitpid）、`dup-close`、`close-before-exec`、`readonly-write`（子进程写只读映射，父进程确认受信号终止）、`readonly-mprotect`（升级权限返回 EPERM 且映射不变）、`present-fault`（取得 owner 后触发失败并关闭）。所有结果结构化输出，失败非零退出。
- [ ] 构建 `make PROFILE=x86_64-clang disk.img`；运行 `scripts/audit_lvgl_symbols.sh`，以及现有 libgfx host 回归；提交 `feat(user): migrate fullscreen clients to explicit display ownership`。

## Task 6: 端到端恢复与文档闭环

**Interfaces:** `qemutests/test_display_owner.py` 复用已有 serial-stdio/QMP runner，25s 启动与每个 case 10s 单调执行期限；测试输入只使用当前经 terminal→PTY 的 shell 路径，不承诺 GUI 焦点功能。

- [ ] 写 runner 失败测试：无换行 prompt、仅命令回显、标记跨块、QEMU 静默、非零 guest 退出；用唯一结果整行识别、selectors 分块读取，所有异常 finally 回收进程并保留 profile 日志。
- [ ] 执行正常 close、显式 release、首次 present 失败、SIGKILL、dup 最后关闭、显式 close-before-exec；每例都验证 `SHELL_ALIVE`、terminal 未被 respawn，以及画面由 fallback 全量恢复（QMP 截图/像素采样），仅看到日志不算画面恢复。
- [ ] 后台启动持续输出的 shell 任务并让 hold GUI 前台至少 1s：PTY 输出持续到达串口、GUI 画面不被覆盖；归还后 terminal 显示更新后的文字。测试后台输出超过单个 PTY buffer，避免小输出掩盖停止 drain。
- [ ] 以独立启动执行只读回读及非法写、raw writable 测试模式；普通 owner 期间 writable mmap 不能建立。mode 切换/owner 接管 interleaving 用 host 确定性测试兜底，QEMU 运行普通 2 CPU 场景检查无覆盖；保持现有 resolution rollback/fault 用例。普通 `make PROFILE=x86_64-clang test-qemu SUITE=resolution` 与 `make PROFILE=x86_64-clang FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution` 都需通过；raw 专用 case 在未注册 fallback 的隔离启动环境运行，不能在普通 terminal 已注册的环境建立 writable raw 映射。现有 `FBIOTEST_HOLD_WRITER` 继续使用普通 `fb_writer_begin`，不添加绕过 ownership 的 writer 入口；将 `_drain_case` 从 terminal 已注册 fallback 的共享故障启动移出，另用无 fallback 私有直接 ash 镜像单独启动，验证 `HOLD ok` → SET 排空超时 `EBUSY` → `RELEASE ok`，模式/generation 不变且 writer 计数归零。新增 `config/inittab.display-raw` 使用 `tty1:respawn:/bin/busybox ash`（init 连接物理 TTY），resolution runner 对 raw 与 drain case 分别使用私有镜像替换 inittab（两个 case 不共享启动，避免 raw sticky 干扰 drain）；用 runner 单元测试验证分流及源镜像 hash 不变；不得覆盖普通 config/inittab 或共享 disk.img。
- [ ] 运行 `make clean` 后普通 `make PROFILE=x86_64-clang disk.img`，`make PROFILE=x86_64-clang test-host`、`make PROFILE=x86_64-clang test-static`、`make PROFILE=x86_64-clang test-resolution-host`、`make PROFILE=x86_64-clang test-qemu SUITE=gfx`（覆盖 test_gfx/tetris/terminal screen/desktop 的现有屏幕验收）、`python3 qemutests/test_display_owner.py`、`python3 qemutests/test_lvgl_runner.py`。runner 新增参数 `--smp 1`/`--smp 2` 并分别运行。resolution 故障镜像使用既有 FB_RESOLUTION_TEST 变体单独验收，不与普通 raw 策略混跑。
- [ ] 审查 git diff；更新 gui/driver 文档，roadmap 删除已完成显示协调项或仅保留未完成 follow-on，changelog 记录实际验证命令和限制。提交 `test(gfx): verify takeover recovery and document ownership contract`。

## 非目标与后续优化

- 完整 FD_CLOEXEC/F_DUPFD_CLOEXEC 实现另行处理；本任务只保证自有客户端显式关闭，继承者持引用时依然按最后关闭语义恢复。

- 本阶段控制权仍是协作角色，不是 seat/登录会话安全授权；多用户安全另行设计。
- 不新增输入设备焦点，GUI-2 单独实施；不暂停整个 terminal/GUI 进程。
- 不实现模式恢复事件 FD、截图 ioctl、raw 映射撤销、VSYNC、damage present 或 compositor。
- readonly raw sticky 对模式 SET 的限制后续用受控读回接口消除；当前保留既有安全边界。
- fd 身份方案不支持任意 GUI 切换栈；需要多个后台桌面时再增加会话管理，不能用一个裸 owner 指针模拟栈。
