# D04 — Doom 平台运行适配

日期：2026-11-10。状态：文档评审通过；仅设计，不表示已实施。

[总览与公共约束](2026-11-10-doom-port-design.md) · [配套实施计划](../plans/2026-11-10-doom-04-platform.md)

## 目标、依赖与交付边界

前置 D01、D02、D03。交付可运行 `/bin/doom` 和真实 DG 后端；可用外部手工准备的访客 IWAD 做 smoke，不依赖 D05 自动打包。不在本阶段实现 QEMU 自动化 suite。

方案沿用原设计选择：完整 `gui_platform` 需要新像素提交接口并协调 LVGL flush；直接读 TTY 缺少 UP，另写 Set 1 解码器则重复维护。首版采用独占 libgfx 视图加 D02 公开解码器，应用承担租约与生命周期，保留单一显示 owner。

## 平台与生命周期

### 入口与状态

状态顺序：`EMPTY → PREPARED → VIEW_OPEN → ACQUIRED → INPUT_BOUND → RUNNING → CLEANUP → CLOSED`。失败可从任何状态进入 CLEANUP；每个资源用独立标志追踪，cleanup 幂等。

`main` 先解析 OS01 参数并保留上游参数，选定 IWAD 的绝对路径，创建/进入数据目录，安装信号处理器并注册 `atexit(cleanup)`，然后 `doomgeneric_Create(argc, argv)`，最后循环 `doomgeneric_Tick()`。注册 cleanup 失败则在进入引擎前报错退出。`DG_Init` 检查 `DG_ScreenBuffer != NULL`；上游在调用它之前分配该缓冲。

平台参数：`--os01-scale N`（N=1..3，缺省 auto）、`--os01-data-dir ABS_PATH`（缺省 `/doom-data`）。从传给引擎的 argv 中剔除平台参数。`-iwad` 相对路径在 `chdir` 前解析成绝对路径；其他文件路径参数以数据目录为工作目录解析，并在用户文档写明。默认 IWAD 为 `/usr/share/games/doom/iwad.wad`；不存在时显示可操作错误，在 acquire 前退出。显式 `-iwad` 不得被默认值覆盖。

首版入口明确拒绝上游 `-scaling`、`-gfxmode` 和 `@response-file` 参数，缺参数/重复冲突的平台参数也须报错。前两项可破坏固定 320×200 RGB32 缓冲约定（尤其大倍数可导致上游写越界）；response file 会在平台参数处理之后再次展开参数。只允许通过 `--os01-scale` 改变最终输出大小，不能把上游所有参数无条件透传并称作兼容。

创建数据目录时确认现有路径是目录，允许 `EEXIST` 的正常复用。父目录应已存在；默认 `/doom-data` 位于可写根文件系统，配置/存档跨进程重启保留。测试必须验证跨 VM 重启的持久性，不能用 `/tmp` 挂载代替。首版单用户单实例，数据目录不可写则失败，不静默丢弃保存。

### DG_Init / cleanup

初始化：短暂打开 `/dev/fb` 读取完整 `fb_info` 后关闭，确认尺寸、RGB32、算术范围；创建唯一全屏 gfx 视图，`gfx_acquire`，打开 `/dev/keyboard`（`O_RDONLY|O_NONBLOCK`），`gfx_bind_input(h, kbd_fd, -1, &state)`，初始化纯解码器。绑定返回的 Caps bit0 转换成 `GUI_MOD_CAPS`，其他修饰键不植入。

`EBUSY` 必须报告并退出，不循环抢占。首版不绑定鼠标，不读取或修改 stdin/TTY termios，不抢 fallback，不为窗口另建第二个 gfx 句柄。

cleanup 顺序：停止泵入和清空应用键状态 → 若已绑定则 unbind → 关闭 keyboard fd → 若曾 acquire 则 release → close gfx → 释放缩放缓冲与平台所有的内存。已经丢失所有权的 release/unbind 失败不得阻断后续清理。引擎拥有 `DG_ScreenBuffer`；避免平台与引擎双重释放。

`SIGINT/SIGTERM` handler 仅设 `volatile sig_atomic_t` 标志，主循环/泵边界读取后走正常退出；handler 内不调用图形、stdio、malloc 或 `I_Quit`。正常信号退出由主线程执行一次 `I_Quit` 保存配置。`SIGKILL`/致命异常由内核 fd teardown 撤销租约/恢复 fallback，不承诺保存。原始键盘租约期间 Ctrl+C 是游戏按键，不是 TTY 的 SIGINT。

### DG_DrawFrame

游戏输入缓冲为 `320×200×4`、`0x00RRGGBB`；不要依赖 libgfx 私有 `pixels` 字段。自动倍数 `s = min(3, floor(W/320), floor(H/200))`，s<1 报错；显式倍数放不下时报错，不静默降级。居中偏移 `x=(W-320s)/2`、`y=(H-200s)/2`。1024×768 的结果为 960×600、(32,84)。

分配一次 `320s×200s` 紧密 RGB32 临时缓冲，逐帧最近邻扩展，然后通过公开 `gfx_sprite_blit(..., use_color_key=false)` 写入 gfx 私有画布。启动时清黑全视图并进行一次完整 present，之后 `gfx_present_rect(h,x,y,320s,200s)`。完整初始化 present 在 `EAGAIN` 时必须保持“待完整提交”，成功前不能改为局部提交，防止遗留上个应用的边框。全视图大小的 rect 由 libgfx 正常转为 `GFX_PRESENT`。

遵循 `user/common/gfx_client_policy.c`：`EAGAIN` 按共享重试间隔延后，保留最近完整画面并继续泵输入，不紧循环；`EACCES/ESTALE` 或其他永久错误打印原因，清理并非零退出。永久失败不重新 acquire，不切到 `/dev/fb` 直写。所有权检查在每次读取/递交按键前执行，即使没有新帧也不能消费陈旧事件。

### 时间与标题

`DG_GetTicksMs` 使用 `CLOCK_MONOTONIC`，内部用 64 位毫秒，DG 返回低 32 位；输入前缀/重试期限使用 64 位值。clock 失败视为致命错误，不回退墙钟或递增假时间。32 位约 49.7 天回绕测试只验证包装行为，不宣称上游支持长期连续运行。

`DG_SleepMs` 使用 `nanosleep`，EINTR 后依剩余时间续睡；有待退出信号时尽快返回给主循环。0 ms 允许直接返回；不忙等补偿 100 Hz tick。引擎保持原有 35 Hz tic 时间推进，不强加“每帧睡 28 ms”。`DG_SetWindowTitle` 可保存标题或打印一次启动信息，全屏没有窗口标题栏，无需新增系统接口。

## 输入契约

### 输入泵与错误

每次 `doomgeneric_Tick` 前泵输入；`DG_GetKey` 只从队列取一项，空队列返回 0，非空返回 1 并设置 `pressed` 和 Doom key。不要在每次 GetKey 调用中无限读设备，否则上游的消费循环可能无法结束。

应用队列不是最后一层容量边界：固定上游 `I_GetEvent` 会连续递交 DOWN，`D_PostEvent` 的 64 项环形队列不检查满，直到 `BuildNewTic` 随后调用 ProcessEvents 才排空。本阶段新增 OS01 专用 `i_input.c` 补丁，每次 `I_GetEvent` 最多消费 **16 项** DG 事件（计数包含 UP），保留原遇到 UP 提前结束的行为；未消费项留在应用队列，下次引擎输入批次再取。不得丢弃或重排以满足预算。OS01 首版没有其他引擎事件生产者，新增生产者时须重新核对引擎环形队列容量。

泵先 `gfx_get_display_state` 校验 foreground，再有限量非阻塞读取（建议每次最多 256 原始字节），逐字节喂 `gui_ps2_feed`，并调用 `gui_ps2_expire`。`EINTR` 重试，`EAGAIN` 表示本轮无更多输入；0 字节、`EOVERFLOW`、`EACCES`、`ESTALE`、`EIO`、不可恢复 poll/read 错误均终止，不能继续带着不可靠的按键状态运行。不要尝试热重绑或恢复游戏。

每次 `doom_input_pop` 递交队列项前也查询 foreground，不复用 pump 时的旧快照；没有新帧时同样执行。pop 返回 1=一项、0=空、-1=锁存错误且清空待发队列。`DG_GetKey` 收到 -1 时在主线程走非零错误退出及 atexit cleanup，不能把 -1 透传给上游的 while 条件，也不能把撤销当成正常空队列继续游戏。

采用至少 256 项固定环形队列，明确计数避免 head==tail 歧义；满时停止并非零退出，禁止丢弃 UP、覆盖旧事件或继续运行导致粘键。发生错误后待发队列立即失效。退出后不向 terminal 回放游戏按键。

### 键值映射

根据 `physical_key` 映射，字母固定小写，数字/标点使用 US 基础键位；不使用修饰后的 codepoint 作为物理身份。上游 `i_input.c` 的 `TranslateKey` 当前是直通，DG 返回的是 `doomkeys.h` 的键值而非原始扫描码。DOWN/UP 对应相同键值，typematic 不重复产生 DOWN，忽略 `GUI_KEY_REPEAT`。

| 物理键 | Doom 值/行为 |
|---|---|
| E0 方向键 | `KEY_UPARROW/DOWNARROW/LEFTARROW/RIGHTARROW` |
| Enter（主键区和 E0）、Escape、Tab、Backspace | 对应 `KEY_ENTER/ESCAPE/TAB/BACKSPACE` |
| Ctrl（左右） | `KEY_FIRE` |
| Alt（左右） | `KEY_LALT`（侧移修饰键） |
| Shift（左右） | `KEY_RSHIFT`（加速与文本大小写） |
| Space | `KEY_USE` |
| F1..F10、主键区字母/数字/标点 | 对应 Doom 功能键和基础 ASCII，支持菜单、武器和存档名称 |
| 未映射键、PrintScreen/Pause 等 | 忽略；不得污染修饰键状态 |

多个物理键映射到同一 Doom key 时，维护每个物理键的按下位图和逻辑键引用计数：0→1 才发 DOWN，1→0 才发 UP。防止按住两个 Shift 后松开一个就停止奔跑，也避免上游 `shiftdown` 计数错乱。首版不强改 WASD 默认绑定；用户可用引擎设置修改。

## 代码边界与验收

应用文件：`user/doom/doom_os01.c`（main/DG）、`doom_runtime.[ch]`（资源/退出）、`doom_input.[ch]`（映射/泵/队列）、`doom_video.[ch]`（缩放/提交）、`app.mk`。应用私有头在同一目录；本阶段不改内核或 UAPI。

app.mk 使用 APP_EXTRA_OBJS 指向 D03 archive 的绝对 profile 路径、APP_SHARED_SOURCES 引入 common/gfx_client_policy.c，链接 `-lgui -lgfx`。由当前目录应用规则发布 `/bin/doom`；保留 immutable generation lease、crt0/sigreturn 和 libc 链接顺序。注册应用时即具备真实 main 和 DG 实现，不提交破坏 make user 的中间版本。

适配源通过 APP_CFLAGS 引用 D03 的 profile 私有补丁后源码头（例如 `$(USER_BUILD_DIR)/doom-upstream/doomgeneric`），并统一定义 `DOOMGENERIC_RESX=320`、`DOOMGENERIC_RESY=200`、`OS01_DOOM`。不得因平台 TU 缺宏而使用上游默认 640×400，也不加 libgui/internal 路径。适配对象在编译前依赖私有副本准备 stamp，防止并行 make 先编译尚不存在的头。本阶段的 `0004-os01-input-budget.patch` 纳入 D03 manifest 的有序补丁列表及摘要；不改写 submodule。

根 `test-doom-host DOOM_CASE=` 扩展 input/video/runtime 组；all 继续覆盖已交付组。宿主覆盖各初始化失败点和归还顺序、信号只设标志、时钟/睡眠错误、整数缩放、首帧完整重试、逻辑键合并、队列满和租约撤销；输入回归须经过真实补丁后 I_GetEvent、D_PostEvent 和引擎消费边界，验证超过 63 个连续 DOWN 及后续 UP 均按序到达，不只验证应用队列。

验收：`make user` 得到无未解析符号的 x86 ELF，map 不带 LVGL 平台；无 IWAD 时在 acquire 前给出有用错误；指定真实 IWAD 后可开局、控制、正常菜单退出并回到 terminal。配置/存档自动回归与 SMP/强杀完整证据属于 D06，不以本阶段 smoke 替代。
