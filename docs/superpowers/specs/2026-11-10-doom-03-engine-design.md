# D03 — Doom 引擎构建与配置补丁

日期：2026-11-10。状态：文档评审通过；仅设计，不表示已实施。

[总览与公共约束](2026-11-10-doom-port-design.md) · [配套实施计划](../plans/2026-11-10-doom-03-engine.md)

## 目标、依赖与交付边界

前置 D01。交付固定源码、受控补丁、真实 OS01 sysroot 编译的静态引擎 archive，以及配置往返的生产代码测试。无需 D02、设备或 WAD；本阶段不声称可运行游戏。

## 引擎与补丁契约

从 `Makefile.soso` 的源列表导出**排除 `doomgeneric_soso.c` 的 80 个引擎单元**，在仓库内固定显式清单。包含 `dummy.c`、`doomgeneric.c`、`i_input.c`、`i_video.c`，不通配引入 SDL/X11/其他平台入口。实际数量以固定清单校验；上游升级必须重新审查列表。

统一宏：`NORMALUNIX`、`LINUX`、`_DEFAULT_SOURCE`、`DOOMGENERIC_RESX=320`、`DOOMGENERIC_RESY=200`、OS01 专用宏（建议 `OS01_DOOM`）。不定义 `ORIGCODE`、`CMAP256`、`SNDSERV`、`FEATURE_SOUND`；增加构建探针确认最终特性状态。`FEATURE_MULTIPLAYER/FEATURE_DEHACKED/FEATURE_WAD_MERGE` 沿用固定上游的禁用状态，不承诺相应功能。

必须覆盖以下有记录的 OS01 条件补丁，可合并为少量 patch：

1. `i_system.c`：zenity 的函数定义与 `I_Error` 中的调用都避开 OS01 分支。错误文本仍输出 stderr，错误退出仍为非零。不冒充 Windows/DJGPP，也不提供恒失败的 `system` 桩。
2. `i_system.c`：`I_Quit()` 执行引擎 `I_AtExit` 链后，在 OS01 路径调用 `exit(0)`。保留引擎的配置保存/声音关闭等顺序；平台清理由 libc `atexit` 执行。不能把 `I_Quit` 再注册为 libc `atexit`，否则可能递归退出或重复保存。
3. `m_config.c`：OS01 的 `GetDefaultConfigDir()` 返回 `"./"` 而非 `"."`。当前 `M_LoadDefaults()` 直接连接 `configdir` 与文件名，`"."` 会生成 `.default.cfg` 等意外路径；修正后得到 `./default.cfg` 与 `./doomgenericdoom.cfg`。`.savegame/` 沿用现有逻辑，避免扩展通用路径系统。
4. `m_config.c`：仅对 OS01 启用 `SaveDefaultCollection` 的既有保存逻辑，并实现 `LoadDefaultCollection` 的 OS01 分支，不能全局开启 `ORIGCODE`（会拉入 SDL 等原始路径）。loader 使用已有 `fgets` 加 `sscanf`/转换函数处理完整行，避免新增通用 `fscanf`：有界读取，超长行整行丢弃，空行/未知项忽略，输入始终前进；读取错误必须终止而非 `while (!feof)` 空转。保留 quoted string、key scancode 转换、十进制/八进制/十六进制及浮点配置语义。OS01 数值解析必须检查转换成功与范围，不能沿用 `ParseIntParameter` 未初始化输出的失败路径；坏配置项保留默认值。保存需检查 fopen/写入/fclose 错误并报告，不能把不可写当作成功退出。配置首启缺失是正常情况，其他打开错误需报告。正常存档的 `G_DoSaveGame/G_DoLoadGame` 不受该 ORIGCODE 开关屏蔽，仍需完整运行验收。

OS01 配置的键码往返策略必须与 [D04 输入契约](2026-11-10-doom-04-platform-design.md)一致：保持文件内使用 vanilla scancode，但 OS01 的 `scantokey[29]` 对应 `KEY_FIRE`，`scantokey[57]` 对应 `KEY_USE`；反向保存分别写 29、57，其他映射及 Shift 的既有 54 特例保留。不能原封不动复用上游表：它使用 `KEY_RCTRL` 和 ASCII Space，不含 Doomgeneric 默认的 `KEY_FIRE=163/KEY_USE=162`，直接保存会写 163/162，再加载被 >=128 检查清零。对不可映射的配置键值保留默认并报告，不生成无效绑定；配置回归必须验证首启→保存→重启后 Ctrl 开火、Space 使用仍然有效，以及改绑普通字母的往返。

补丁只应用到 profile 私有构建副本，例如 `$(USER_BUILD_DIR)/doom-upstream/`。输入摘要至少包含实际源码内容、上游提交、清单、补丁内容/顺序、工具链与全部编译宏。补丁失败立即失败，不忽略 fuzz/冲突后继续发布 ELF。submodule 工作树保持原样。

## 构建契约

1. 本阶段不创建 `user/doom/app.mk`，不注册 `/bin/doom`，避免发现一个没有平台入口的半成品应用。引擎对象归档为 `$(USER_BUILD_DIR)/obj/doom-engine/libdoom-engine.a`；D04 通过 APP_EXTRA_OBJS 接入。
2. 引擎由 user 子构建在 `$(USER_BUILD_DIR)/obj/doom-engine/` 编译；使用 profile 的编译器、目标参数、**leased immutable SYSROOT_GENERATION_DIR**，不调用上游 Makefile，不借宿主头/库。引擎规则由 user 构建上下文加载；root 包含 `apps.mk` 进行发现时不能误触发副本准备或编译。
3. 引擎编译保留栈保护、无 PIC/PIE、无 red zone、依赖文件，增加 `-fno-builtin`；平台保持项目警告策略。第三方警告抑制仅作用于引擎规则，列表根据真实诊断收窄，不全局 `-w`。
4. 对象依赖内容摘要和 `.d`，最终 ELF 依赖引擎清单、补丁、APP_EXTRA_OBJS 与 app 配置键；配置变化必须重建，不能只靠目录时间戳。根发布仍由 `mk/components/user.mk` 持有 generation lease 并校验制品。
5. 缺失 submodule/提交不符/补丁失败须给出定位信息并失败；不在普通 make 中自动联网 clone，不污染第三方源码树。
6. user 子构建的 all 增加引擎 archive 依赖，由根 `make user` 的既有 generation lease 路径驱动；本阶段根发布仍仅发布既有 ELF。aarch64 无 userland 能力，不得因此构建 Doom。应用发现与发布属于 D04。

任何需跨递归 make 的新增参数，必须接入 `mk/project.mk` 的受控参数通道或显式传参；当前 `env -i` 与 `MAKEOVERRIDES=` 会清除未登记的隐式传播。WAD 参数只参与镜像资源选择，不应改变引擎编译键；路径/来源文本必须正确引用，不能直接拼入未转义 shell 命令。

本次不改全局 APP_SOURCES 的路径语义；如需要新 include 文件承载专用规则，可加 `user/doom/engine.mk` 并在 user 构建中显式加载。不能靠 `../../thirdpart/...` 路径越界生成源树对象。

## 文件与验收

拥有 `.gitmodules`、`thirdpart/doomgeneric`、`thirdpart/doomgeneric.manifest`、`thirdpart/doomgeneric-sources.mk`、`thirdpart/patches/doomgeneric/*.patch`、`user/doom/engine.mk`、`user/Makefile` 的引擎规则与相关根 user 依赖。私有副本路径及内容摘要按上文实现；不要写 source-tree 对象。

建立根聚焦入口 `make test-doom-host DOOM_CASE=build|config|all`，本阶段 all 仅运行已交付组，未知 case 非零失败。测试注册于 `hosttests/mk/doom.mk` 并进入既有 inventory，根路由由 `mk/components/run.mk` 拥有。

验收：80 个生产单元全部编译；archive 对 libc 的依赖由 D01 提供，无 `system/fscanf` 新依赖；DG_* 仍是明确的待接入平台接口，不能把 archive 的 nm 未定义列表冒充最终 ELF 链接失败。补丁/源码/宏/清单变化正确重建，aarch64 不拉起引擎。配置测试通过公共 M_SetConfigDir/M_SetConfigFilenames/M_BindVariable/M_LoadDefaults/M_SaveDefaults 驱动真实补丁后 m_config.c。固定上游只有统一绑定接口 `void M_BindVariable(char *name, void *location)`，测试须按默认配置表类型提供 int、float 或 char* 存储，不虚构按类型 Bind API。断言文件内容与再次加载的值，至少覆盖 Ctrl/Space、字母改绑、进制、浮点、坏项、超长行和读写错误。

交付不包含生产 DG 空桩、不安装测试 ELF、不引入 app.mk；最终无未解析符号的可执行文件由 D04 验收。
