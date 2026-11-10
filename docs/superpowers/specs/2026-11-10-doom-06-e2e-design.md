# D06 — Doom QEMU 集成验收

日期：2026-11-10。状态：文档评审通过；仅设计，不表示已实施。

[总览与公共约束](2026-11-10-doom-port-design.md) · [配套实施计划](../plans/2026-11-10-doom-06-e2e.md)

## 目标、依赖与交付边界

前置 D01–D05。交付真实游戏的 QEMU 证据、测试 runner 自测和用户文档；本阶段补充隔离的测试观测与控制接口，不重写平台功能，不以 runner 的 fake 测试替代访客运行。

## 场景与运行契约

新增 `doom` suite，登记 `mk/tests/catalog.mk` 和现有 dispatcher/run 规则；使用 rootfs 能力、SMP=1/2、512 MiB、isolated 镜像策略。每个测试启动前复制含固定校验 WAD 的镜像到临时目录，所有存档写该副本，不写普通 image。通过正常 terminal/ash 启动 Doom，不以绕开终端的 init 替换掩盖归还问题。

复用既有 QMP 和 screendump 工具。串口用于启动命令/日志，真实游戏控制用 QMP PS/2 key make/break，不能用串口 ASCII 假装物理键事件。通过握手判定 boot/首帧就绪，不固定 sleep 18 秒后假定成功。单 CPU runner 接口为 `--smp 1|2`，不接收“自动矩阵”值；已 prepare 的 run 只消费 manifest 分配的 CPU/镜像/固件且绝不构建。顶层 `test-qemu SUITE=doom`/批量调度器拥有 SMP=1/2 矩阵，每个配置只执行一次。manifest 为两个独立配置记录 CPU、输入哈希与 `timeout_s`；runner 每 CPU 完整场景集预算初值 240 秒，外层单配置预算为该值加 60 秒清理余量，串行双配置顶层预算为两份外层预算之和再加 60 秒。同步修改 `run_suite_matrix.py` 及其实际调度入口，不沿用固定 300 秒截断更长配置；如 TCG 实测需要调整，显式修改固定预算及其测试，不运行时自动加时。超时保存日志/截图并失败。

| 场景 | 必须观察到的证据 |
|---|---|
| 标题与开局 | 实际 IWAD 加载；QMP 截图游戏矩形非空、黑边正确；Enter 开始 E1M1，后续截图变化 |
| 持续控制 | 按住/松开方向键、Ctrl 开火、Space 使用、Shift 加速；组合键松开一个仍保留另一个；UP 后动作停止 |
| 菜单与退出 | Escape 正常进入菜单，退出确认成功；子进程返回 0；终端重新显示且能执行唯一 nonce 的 shell 命令 |
| 配置二次加载 | 修改引擎设置，正常退出；检查 default.cfg 内容，再次启动读回同值；Ctrl 开火/Space 使用及普通字母改绑在重启后仍有效，不可只检查文件存在 |
| 存档与读档 | 进入关卡、存档、继续移动、读档恢复；正常退出并重启同一镜像副本后仍可加载 |
| 错误 IWAD | 缺失/损坏文件有原因日志且非零退出，terminal 可操作，无 zenity/system 依赖 |
| 资源忙/撤销 | 已有前台 owner 时按 EBUSY 退出；丢失 owner/分辨率失效时清理退出，不能抢回前台 |
| 强制杀死 | 宿主测试通过独立控制路径发送 SIGKILL，租约解除，终端恢复；不能依赖已被独占的 TTY 键盘发 kill |

强杀测试通过下述 guest 控制 helper，在正常 terminal/ash 内启动并监督自己的 Doom 子进程；helper 在 fork 得到确切 PID 后按有限超时发 SIGKILL，并 waitpid 记录真实退出状态，不能按进程名猜 PID或依赖被游戏占有的物理键盘。正常菜单退出必须经过上游补丁后的 `I_Quit`，不得用测试专用快捷退出代替。

## 测试观测与控制的生产方

D06 新增 `user/doom/diag.{c,h}`、`user/doom_testctl/{app.mk,main.c}` 和 `thirdpart/patches/doomgeneric/0005-os01-test-observe.patch`，修改 `user/doom/app.mk`、main/DG 后端、D03 engine.mk、`user/Makefile`、根 user 构建参数及 `config/rootfs.mk`。沿用 D03 的有序补丁内容摘要。诊断是单独的 `OS01_DOOM_DIAG` 构建身份：仅 DOOM_TEST variant 启用；引擎 archive、适配器对象、user 发布制品/stamp 均纳入该身份，普通镜像不链接观测 hook、不安装 testctl。DOOM_TEST 不只隔离磁盘路径，必须隔离诊断 user 制品；同 profile 正常→测试→正常重建不得复用错误对象。不得改共享 submodule 或 immutable sysroot。

观测默认只读，经实际执行位置产生，不能注入成功或改 tic。0005 在 `G_Ticker` 的真实玩家命令已消费且本 tic 世界更新后记录 `gametic/leveltime/gamestate`、地图、玩家位置/角度、实际 `ticcmd` 的 forward/sidemove/angleturn/buttons，以及已加载键绑定；保存/加载成功记录从真实 `G_DoSaveGame/G_DoLoadGame` 成功路径产生，附槽位与状态摘要，不以菜单点击或文件存在代替。D04 后端在实际 gfx present 返回后统计成功帧、EAGAIN、其他错误和单调时间；失败不能计为成功帧。每条记录通过 `/dev/serial` 一次 write 输出有版本、run nonce、PID、递增序号与类型的有限长度完整行；每 tic 状态记录有界，仅限测量窗口，窗口外按需摘要。启动 READY 只在真实 IWAD 初始化、首个成功 present 后输出；runner 拒绝回显、错 nonce/PID、重复/倒序及缺结尾的记录。

`doom_testctl` 由 ash 启动，fork/exec 正常 Doom，记录 waitpid 退出码/信号；只用既有 syscall/libgfx，不替代 terminal。其测试模式覆盖预占前台制造真实 EBUSY、按时 SIGKILL，以及为诊断 Doom 子进程提供限定场景参数。撤销测试由 Doom 诊断控制点调用自己的真实 gfx release，随后回到未经短路的 D04 owner 检查；mode 失效测试由 testctl 在确认子进程首帧后，打开真实 `/dev/fb`，使用 `FBIOGET_MODES` 选择另一合法分辨率并执行 `FBIOSET_MODE`，通过 `FBIOGET_STATE` 记录成功与 generation 变化；随后等待 Doom 原有 owner/input/present 检查返回真实失效错误并非零退出。不能二次 GFX_CREATE_VIEW（现有同 fd 重建返回 EINVAL），也不能用 libgfx 缓存的 gfx_get_info 证明 mode 有效。两项负向测试必须观察实际 ioctl/owner/generation 变化，不能伪造 errno 或直接调用 fatal 当作通过。标准 x86_64 QEMU 环境必须覆盖真实模式切换；只有实施时有证据说明后端不支持的特殊环境，才可将该环境单列未覆盖并保留 D04 宿主回归，不能宣称该环境完整 E2E 通过。诊断控制点只在指定负向场景启用，正常控制/配置/存档场景只允许读取、不能改写游戏状态。正常菜单退出仍通过真实 `I_Quit`。

持续控制以连续 ticcmd 的 movement/buttons 判定 UP 后对应命令清除，并检查世界位置/角度或实际玩家动作产生变化；不能要求有惯性的玩家瞬间静止。组合键必须在同一观测窗口证明仅释放对应动作。重启配置同时比较 cfg、真实加载绑定和随后输入产生的 ticcmd；跨 VM 读档比较保存/加载路径记录与恢复的地图/玩家状态摘要，随后继续 tic 推进。实际截图独立验证像素/布局；计数来自 present 返回，不能从截图数推算。观测功能自身的宿主 fixture 测试进入 `make test-doom-host`，构建隔离/解析测试进入零环境 `make test-harness`，后者导入不得要求 IWAD、编译器或 QEMU。

默认 CI 无可用 WAD 时可运行纯宿主/构建检查；显式请求 `doom` E2E 而资源缺失必须清楚报错，不能绿色 skip。完成报告须分列 doom1 与 Freedoom 的实测版本/哈希及通过场景，未运行的标记未验证。

## 性能记录

记录 TCG/KVM、CPU 数、分辨率、缩放倍数、真实呈现帧数、耗时和 EAGAIN 次数。960×600×4 每帧约 2.3 MB 是载荷估算，不是实际吞吐测量；本设计还多一次用户态缩放/精灵拷贝，不沿用“约 300 行即可完成”的工作量结论。TCG 慢时可显式 `--os01-scale 2` 重测，不静默修改逻辑 tic 或把 35 fps 作为所有环境的硬验收线。

## 隔离、文件与完成条件

新增测试开关 `DOOM_TEST=1`，控制 `doom` suite 的隔离镜像及上述诊断构建身份；接入 `mk/project.mk` 的 IMAGE_VARIANT 与受控递归参数，拒绝与其他破坏性 fixture 共用。该 variant 使用正常 inittab/terminal；只有 DOOM_TEST 进入受控递归白名单；D05 四项资源绝不加入递归参数字符串，继续使用 D05 的原始值环境桥接。在顶层已显式 DOOM_TEST=1 时，doom prepare 直接依赖当前根 make 的隔离 DISK_IMG，跳过通用递归 image 分支；批量 prepare 也复用该根层入口，禁止借 MAKEOVERRIDES 再展开四项原始输入。对真正需要的递归 user 构建清除这四项覆盖，仅传诊断构建身份，不传资源。suite prepare 构建隔离镜像，runner 再复制为每场景独立可写副本；不得先把 WAD 写进普通 image 再复制。跨 VM 存档场景复用同一个场景副本，其余场景互不共享保存状态。

文件：`qemutests/doom_test.py`、`qemutests/test_doom_runner.py`、`mk/tests/catalog.mk`、`mk/components/run.mk`、`qemutests/run_test.py`、`qemutests/run_suite_matrix.py` 及其调度调用方、`qemutests/test_doom_build_contract.py`、`mk/project.mk`、`docs/games/doom.md`、`docs/build/build.md`。runner 自测进入 TEST_HARNESS_MODULES，由 `make test-harness` 覆盖。

本阶段另拥有引擎镜像许可材料：新增 `scripts/prepare_doom_engine_notice.py`，读取 D03 的固定 submodule `LICENSE`、`thirdpart/doomgeneric.manifest` 和有序补丁（含 D04 的输入批次补丁），生成 profile 私有制品，由 `config/rootfs.mk`、`mk/components/image.mk` 安装到 `/usr/share/licenses/doom/LICENSE`、`/usr/share/doc/doom/SOURCE.txt`、`/usr/share/doc/doom/PATCHES.txt` 以及 `/usr/share/doc/doom/patches/<原补丁文件名>`。SOURCE 记录 URL/提交与构建基线，PATCHES 记录顺序、原因和每份 SHA256；完整补丁内容随镜像保留。D05 独立资源阶段不要求引擎存在；本阶段在 `/bin/doom` 被发布时安装这些材料，无 IWAD 的 Doom 镜像也必须安装。许可证/manifest/补丁内容变化是 notice 制品和 rootfs 的真实内容依赖，不能靠普通文件路径不变而跳过更新。

`qemutests/test_doom_engine_notice.py` 同时核对制品和 rootfs 条目，覆盖无 WAD 但有引擎、patch 内容原地变化、缺许可文件/缺补丁时拒绝发布；进入 `make test-harness`。这些镜像说明不代替二进制分发前对相应源码材料的核对。

新增入口为 `make PROFILE=x86_64-clang DOOM_TEST=1 test-qemu SUITE=doom`（连同 D05 四项资源变量）；suite catalog 记录 SMP=1/2、512 MiB、isolated、有限 timeout，分组预算明确且不自动加时。显式 E2E 缺资源须失败，不能绿色 skip。

完成报告保存于 `docs/superpowers/reports/2026-11-10-doom-validation.md`（实际执行日期变化时可用新日期）：列出源码/资源哈希、每 CPU/场景结果、原始日志/截图位置、测量环境、未验证项。首版完成以总览的 D01–D06 验收为准；Freedoom 分发实测可后续补充，不承诺全部关卡。
