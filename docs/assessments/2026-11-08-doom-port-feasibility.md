# OS01 移植 Doom 的可行性评估

基线：OS01 `fe629c7e`（master）；doomgeneric `e257a09527062bfb986b6e5e0780839b6cbd5b71`（2026-10-11，GPLv2）；clang 23.1.4。

结论：可行。内核和用户态图形/输入/时间能力已满足 Doom 的运行需求；doomgeneric 的 80 个引擎翻译单元对 OS01 libc 的阻塞缺口只有 6 个外部符号和 3 处头文件问题，其中 `fseek`/`ftell`/`SEEK_*` 与 `sscanf` 的 `%i`/`%o` 已由进行中的 libc 补全计划（`feature/libc-completion`，`docs/superpowers/specs/2026-11-08-libc-completion-design.md`）覆盖。剩余工作是少量 libc 补充、约 300 行平台适配层、构建与 WAD 打包。第一版不含声音（内核无音频驱动）。aarch64 当前不构建用户态 libc，本评估只针对 x86_64。

## 实际检查

取 doomgeneric 上游 `Makefile.soso`（另一个业余 OS 的移植）中的引擎源文件列表，去掉其平台文件，共 80 个 `.c`。使用 OS01 用户程序的实际编译参数（`clang --target=x86_64-unknown-none -ffreestanding -fno-builtin`、`--sysroot=build/x86_64-clang/sysroot`、`-fstack-protector-strong -fno-pic -fno-pie -mno-red-zone -O2`），加上游宏 `-DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE`，不使用宿主 libc 头或库。

| 步骤 | 结果 |
|---|---|
| 原样编译 | 4/80 成功；其余 76 个均因 `doomtype.h` 包含的 `strings.h` 不存在而失败 |
| 补一个只转发 `<string.h>` 的 `strings.h` | 64/80 成功；16 个失败，错误见下表 |
| 再以评估用强制包含头补齐声明、`#undef` 冲突宏 | 80/80 成功 |
| 与 `user/obj/crt0.o`、`sigreturn_trampoline.o`、`libc.a` 按 `user/linker.ld` 链接（DG_* 平台函数用空桩代替） | 未解析符号：`abs`、`fabs`、`fseek`、`ftell`、`remove`、`system`，以及 `main`（由平台文件提供，属预期） |

编译阶段的错误（第 2 步）：

| 问题 | 出现次数 | 性质 |
|---|---|---|
| `abs` 未声明 | 20（12 个文件） | 缺接口 |
| `ftell` / `fseek` 未声明、`SEEK_SET` 未定义 | 4 / 2 / 1 | 缺接口（`SEEK_*` 现位于 `sys/stat.h`） |
| `system` 未声明 | 2（`i_system.c`） | 缺接口 |
| `remove` 未声明 | 1（`g_game.c` 存档） | 缺接口 |
| `rename` 未声明 | 1（`g_game.c` 存档） | 声明位置：OS01 只在 `unistd.h` 声明，标准位置是 `stdio.h`；`libc.a` 已有实现 |
| `mkdir` 未声明 | 1（`m_misc.c`） | 声明位置：OS01 只在 `unistd.h` 声明，标准位置是 `sys/stat.h`；`libc.a` 已有实现 |
| `fabs` 未声明 | 1（`v_video.c`，鼠标加速度显示框） | 缺接口 |
| `am_map.c:854` `expected identifier` | 1 | 命名空间污染：OS01 `stdio.h` 公开定义 `ZEROPAD`/`SIGN`/`PLUS`/`SPACE`/`LEFT`/`SPECIAL`/`SMALL` 以及 `do_div`、`is_digit`，与 Doom 的局部枚举 `LEFT` 冲突 |

引擎全部外部依赖（链接前 `llvm-nm -u` 汇总，除 DG_* 外）：`abs atof atoi calloc errno exit fabs fclose fflush fopen fprintf fread free fseek ftell fwrite malloc memcpy memset mkdir printf putchar puts realloc remove rename snprintf sscanf strcasecmp strchr strcmp strdup strlen strncasecmp strncmp strncpy strrchr strstr system toupper vfprintf vsnprintf`，以及栈保护符号。引擎渲染全部使用定点数，唯一的浮点用法是配置里的 `mouse_acceleration`（`atof` 读入、`fabs` 比较）。

## 系统能力对照

| 需求 | 现状 | 判断 |
|---|---|---|
| 显示 | `/dev/gfx0`：`GFX_CREATE_VIEW` 全屏视图、`GFX_ACQUIRE` 显示所有权、`GFX_PRESENT_RECT` 局部刷新；libgfx 封装。`user/tetris` 已走通同一路径 | 满足 |
| 键盘按下/松开 | `/dev/keyboard` 给原始 Set 1 扫描码；libgui `gui_input` 已解码 E0 前缀并给出 `gui_key_event_t{physical_key, action=DOWN/UP/REPEAT}`；`GFX_BIND_INPUT` 将键盘租约绑定到前台视图 | 满足。不能用 TTY/stdin（Tetris 的做法），收不到松开事件 |
| 毫秒计时 | `clock_gettime(CLOCK_MONOTONIC)` | 满足 |
| 睡眠 | `nanosleep` 为真实阻塞睡眠（`kernel/syscall/sys_time.c` blocker 框架）；`user/tetris/tetris.c` 中"nanosleep 坏了"的注释已过时 | 满足；x86_64 LAPIC 周期 100 Hz，唤醒粒度约 10 ms |
| 浮点 | 上下文切换 `fxsave64`/`fxrstor`（`kernel/arch/x86_64/cpu/switch.c`） | 满足 |
| 堆 | brk 堆从程序映像末尾（基址 `0x400000`）到 `0x203FF000`，约 500 MB；用户栈 2 MB | 满足（Doom 区域内存默认 6 MB，加 WAD 缓存） |
| 文件 | `fopen`/`fread`/`fwrite`/`fclose` 已有；stdio 无缓冲，每次 `fread` 是一次 `read` syscall | 满足；`fseek`/`ftell` 待 libc 计划 |
| 根文件系统 | `build/<profile>/image/rootfs.manifest` 打包，磁盘镜像约 200 MB | 满足（WAD 4–30 MB） |
| 声音 | 内核无任何音频驱动 | 不满足；doomgeneric 支持无声编译 |

## 缺口归属

已由 libc 补全计划覆盖：

- `fseek`/`ftell`/`rewind`/`feof` 及标准头中的 `SEEK_*`（spec「stdio」节）。
- `sscanf` 的 `%i`/`%o`。当前 `libc/stdio/sscanf.c` 只支持 `d/u/x/X/s/c/%`；Doom 在 `m_misc.c` 用 `" 0%o"`，在 `m_config.c` 用 `"%i"` 解析配置。当前实现遇到未知转换只是跳过并返回 0，而 `m_config.c` 的 `ParseIntParameter` 不检查返回值、`parm` 未初始化，配置项会读成栈上的垃圾值。首次运行没有配置文件，不受影响；退出时 `M_SaveDefaults` 写出配置后，第二次启动起即受影响。因此 Doom 的移植应排在 libc 计划的 scanf 组之后。

未在该计划中找到，需另行处理：

| 缺口 | 建议 |
|---|---|
| `strings.h` 不存在 | 新增标准头（`strcasecmp`/`strncasecmp` 等声明），属 libc 公开头 |
| `stdio.h` 宏污染（`LEFT` 等 7 个格式化标志、`do_div`、`is_digit`） | 移入 printf 实现私有头。这是 libc 自身问题，不应在 Doom 侧绕过 |
| `rename` 不在 `stdio.h`、`mkdir` 不在 `sys/stat.h` | 补标准位置声明，`unistd.h` 中保留与否按 include 策略定 |
| `abs`（及 `labs`/`llabs`） | 补到 `stdlib.h` |
| `remove` | 补到 `stdio.h`，语义为对文件 `unlink`、对目录 `rmdir` |
| `fabs` | 补最小 `math.h`；Doom 只用这一个数学函数 |
| `system` | 只用于 `I_Error` 调用 `/usr/bin/zenity` 弹窗。两种做法：libc 提供 `fork`+`execve("/bin/sh","-c")`+`waitpid` 的真实实现；或在适配层以宏关闭 zenity 路径。不应提供恒失败的空壳 |

## 适配层要点

doomgeneric 只要求平台实现 `DG_Init`、`DG_DrawFrame`、`DG_SleepMs`、`DG_GetTicksMs`、`DG_GetKey`、`DG_SetWindowTitle` 和 `main`。

- 显示：读 `/dev/fb` 取分辨率，`gfx_open` 全屏视图后 `gfx_acquire`；把 `DG_ScreenBuffer`（XRGB8888，与 `GFX_FORMAT_RGB32` 一致）整数倍放大居中，用 `GFX_PRESENT_RECT` 只提交画面区域。320×200 在 1024×768 下取 3 倍（960×600）。退出路径必须 `gfx_release`。
- 输入：复用 `gui_input`（libgui 已有扫描码状态机），把 `physical_key` 映射到 `doomkeys.h`；`DG_GetKey` 从一个小环形队列出队。
- 计时：`DG_GetTicksMs` 用 `CLOCK_MONOTONIC`，`DG_SleepMs` 用 `nanosleep`。
- 构建：`thirdpart/doomgeneric` + `.manifest`（BusyBox 也是 GPLv2，已有先例）；`user/doom/app.mk` 列出 80 个引擎源文件和平台文件，链接 `-lgui -lgfx`，参照 `user/test_lvgl/app.mk`。上游代码警告很多，需按文件压掉警告，不改上游源码。
- 入口：可登记为 desktop 应用（`user/desktop/desktop_registry.c`），也可由 shell 直接运行。

## WAD 选择

| | doom1.wad（共享版 v1.9） | Freedoom |
|---|---|---|
| 许可 | 只允许原样免费分发，不是开源 | BSD 风格，可修改、可分发 |
| 内容 | 第 1 章 9 关，原版素材 | Phase 1（4 章，对应 Ultimate Doom）/ Phase 2（32 关，对应 Doom II），全部重制素材 |
| 大小 | 约 4 MB | 二三十 MB |
| 引擎模式 | 共享版：只有第 1 章，不能加载 PWAD | 完整版，无此限制 |
| 兼容性 | doomgeneric/Chocolate Doom 的标准参考数据 | 目标兼容原版引擎，但历史上个别关卡超出原版限制（如 visplane 溢出）；当前版本未实测 |

建议：开发调试用 doom1.wad，以便区分移植缺陷和数据问题；随镜像分发用 Freedoom Phase 1。两者都不提交进 Git，构建时下载并在 manifest 中记录 sha256；通过 `-iwad` 选择。

## 风险

- 提交带宽：`GFX_PRESENT_RECT` 每帧经内核 bounce 逐行拷贝。960×600×4 ≈ 2.3 MB/帧，35 fps ≈ 80 MB/s。KVM 下预计无压力；CI 使用 TCG，可能掉帧，必要时降到 2 倍（640×400，约 1 MB/帧）。未实测。
- 计时粒度：100 Hz tick 使 `DG_SleepMs(1)` 实际睡 10 ms 左右。Doom 逻辑按 35 Hz tic 以时间推进，不影响正确性，只会让帧间隔不均匀。
- 输入归属：不经 `GFX_BIND_INPUT` 绑定时，按键会同时进入 terminal/ash。
- 声音：后续可选 PC 喇叭（PIT 通道 2，工作量最小）或 AC97/HDA（新子系统，工作量大）。
- 许可证：doomgeneric 为 GPLv2；仓库根目录未见 LICENSE 文件，引入前需确认分发方式（BusyBox 已有同类先例）。

## 验证边界

这是编译与链接级检查，不是 ELF 启动、显示接入或 QEMU 运行验证。链接时平台函数为空桩，未执行任何引擎代码；未加载任何 WAD。提交带宽、帧率和 Freedoom 关卡兼容性均未实测。`system`、`fabs`、`abs` 等的补法是建议，不代表 libc 计划已接受。

doomgeneric 来源：https://github.com/ozkl/doomgeneric

评估用的源码、桩文件、编译脚本和逐文件诊断位于 `/tmp/os01-doom-assess/`（临时目录，不纳入 Git）：`src/doomgeneric` 为上游克隆，`shim/` 为评估用 `strings.h` 与强制包含头，`scripts/compile.sh` 为编译脚本，`out/*.err` 为逐文件诊断。
