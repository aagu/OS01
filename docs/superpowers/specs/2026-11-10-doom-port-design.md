# OS01 Doom 移植：分阶段设计与实施总览

日期：2026-11-10。状态：D01–D06 分组评审及跨文档总审均通过；尚未开始实现。

本文替代原单体 spec，保留为稳定入口。详细要求已迁移到下列 6 份 spec，每份配套一份 plan；不再维护另一份完整设计副本。依据为 [可行性评估](../../assessments/2026-11-08-doom-port-feasibility.md)。

[文档评审记录](../reports/2026-11-10-doom-doc-review.md)记录评审轮次、修订要点和结论；文档批准不表示移植已完成。

## 公共目标与约束

- OS01 基线 `7ccc3fb5ab161b291c45dd56d6a8eba96893f24c`；doomgeneric 固定 `e257a09527062bfb986b6e5e0780839b6cbd5b71`。
- 首版为 x86_64、无声、键盘、shell 启动、320×200 软件渲染和 1–3 倍整数放大；配置/存档可重启读回，退出恢复 terminal。
- 不新增 syscall/UAPI，不做 aarch64 用户态、声音、鼠标操作、联机、desktop 页面、完整 libm 或通用 system/fscanf。
- 默认镜像不带 IWAD、不自动下载；显式资源须校验并记录来源。Freedoom 分发验证可后续完成，不能捏造版本/哈希或宣称全关卡兼容。
- 只通过根 make、profile 私有 build 和 immutable sysroot generation 构建；不改写第三方 submodule，不使用宿主 libc 替代目标实现。
- 所有 spec/plan 都是未来工作，checkbox 未执行；本轮只改文档，不执行实现或提交/合并/推送。执行某份 plan 前须评审对应 spec/plan。

## 工作包与交接

| 工作包 | Spec | Plan | 前置 | 可独立验收的交付 |
|---|---|---|---|---|
| D01 | [设计](../specs/2026-11-10-doom-01-libc-design.md) | [实施计划](../plans/2026-11-10-doom-01-libc.md) | 无 | 标准 libc 接口、宏边界与双架构库检查 |
| D02 | [设计](../specs/2026-11-10-doom-02-ps2-api-design.md) | [实施计划](../plans/2026-11-10-doom-02-ps2-api.md) | 无 | 公开解码器、安装头与静态链接探针 |
| D03 | [设计](../specs/2026-11-10-doom-03-engine-design.md) | [实施计划](../plans/2026-11-10-doom-03-engine.md) | D01 | 固定引擎 archive、补丁、配置生产测试；不注册应用 |
| D04 | [设计](../specs/2026-11-10-doom-04-platform-design.md) | [实施计划](../plans/2026-11-10-doom-04-platform.md) | D01/D02/D03 | 真实 DG 后端与 /bin/doom，可手工准备 IWAD smoke |
| D05 | [设计](../specs/2026-11-10-doom-05-assets-design.md) | [实施计划](../plans/2026-11-10-doom-05-assets.md) | 现有 rootfs 构建 | IWAD 校验、manifest 与增量打包；不要求可玩 |
| D06 | [设计](../specs/2026-11-10-doom-06-e2e-design.md) | [实施计划](../plans/2026-11-10-doom-06-e2e.md) | D01–D05 | 隔离 QEMU suite、SMP/持久化/归还证据与用户文档 |

建议依次实施 D01 → D02 → D03 → D04 → D05 → D06；依赖图允许 D02/D05 提前完成，这不表示默认授权并行实施。每份 plan 只验收自己拥有的接口，不重复执行全部 E2E。

D03 以 archive 为边界，避免为了“能链接”添加生产空桩；D04 才加入 app.mk。D05 以资源制品为边界，D06 才增加测试镜像 variant。修改跨包接口时同步更新生产方和消费方 spec，局部实现细节只改其所属 plan。

## 当前代码校正与要求归属

| 项目 | 当前代码事实 | 本次处理 |
|---|---|---|
| `fseek/ftell/SEEK_*` | `libc/include/stdio.h` 已声明，stdio 定位功能已合入 | 使用现有实现，复跑真实 WAD 读取验收 |
| `sscanf %i/%o` | `libc/stdio/vsscanf.c` 已支持，`sscanf.c` 只是包装 | 配置二次加载仍必须测，不再作为前置待合入分支 |
| `math.h` | 已存在，含 `INFINITY/NAN/signbit/isfinite`，没有 `fabs` | 扩展现有头，不能覆盖重建 |
| `stdio.h` 宏污染 | `LEFT` 等格式标志、`do_div`、`is_digit` 仍公开 | 在 libc 内收敛，不给 Doom 加 `#undef LEFT` shim |
| `strings.h/abs/remove` | 当前公开头/实现仍缺所需接口 | 纳入本次最小 libc 补充 |
| 输入复用 | `gui_input_create()` 要求存活的 `gui_platform_t`，创建 LVGL indev；事件由 `gui_platform_step()` 递交 | 不能把它当独立键盘解码器直接挂到 Doom 的 gfx 视图 |
| 纯键盘解码器 | `libgui/src/input_ps2.c` 已有 `gui_ps2_init/feed/expire`，无 IO/LVGL 调用，头在 `libgui/internal/` | 公开这一既有能力，不复制状态机 |
| 输出尺寸 | 上游 `doomgeneric.h` 默认 **640×400**，引擎内部游戏画面为 320×200 | 所有引擎/适配单元统一定义 `DOOMGENERIC_RESX=320`、`DOOMGENERIC_RESY=200` |
| 正常退出 | 上游 `config.h` 明确 `#undef ORIGCODE`；`I_Quit()` 的 `exit(0)` 位于 `#if ORIGCODE` 内 | 需要受控补丁，否则菜单确认退出后继续循环 |
| 配置持久化 | `m_config.c` 的 `SaveDefaultCollection/LoadDefaultCollection` 主体也被 `#if ORIGCODE` 禁用；原 loader 使用当前 libc 没有的 `fscanf` | 需选择性启用并改写 OS01 的逐行读取；单补 scanf 不会自动获得配置持久化 |
| zenity | `i_system.c` 的 Unix 分支直接编译两个 `system()` 调用，没有现成 OS01 开关 | 用明确 OS01 补丁关闭该路径，不能假定已有可用宏 |
| 构建发现 | `user/apps.mk` 自动发现目录应用，但只为 `user/<app>/` 本地对象添加 APP_CFLAGS | 80 个外部源不能直接塞入 APP_SOURCES 后假定规则自动成立 |
| 发布与打包 | ELF 发布到 `$(USER_ARTIFACT_DIR)`；`config/rootfs.mk` 驱动 rootfs，`image.mk` 只消费制品 | 不写旧的 `user/obj`，不从镜像规则递归构建引擎 |
| desktop 注册表 | 当前条目是内建 GUI 页面，不是任意 ELF launcher | 首版只提供 shell 入口，单改 registry 不构成桌面接入 |

| 原单体章节 | 迁移位置 |
|---|---|
| §1/§3 范围与方案 | 本总览；D02/D04 的接口选择 |
| §4 文件归属 | 各 spec 文件边界和各 plan Files |
| §5 libc | D01 |
| §6 引擎、退出、配置及键码往返 | D03 |
| §7 DG/生命周期与 §8 输入泵 | D04 |
| §8.1 公开解码器 | D02 |
| §9 构建 | D03 archive；D04 应用发现/发布 |
| §10 资源 | D05 |
| §11 验证 | 每包自有验收；D06 跨包 QEMU/性能证据 |
| §12 完成定义 | 下节 |

## 完成定义

D01–D06 的必需场景全部完成并附真实证据后，才称首版移植完成。D01/D02/D03 可以独立合入；D04 的手工 smoke 不能替代 D06；默认不带 WAD 的交付可先完成，Freedoom 实测与分发材料单独记录。

本轮只重组文档并补齐实施计划，没有运行引擎、加载 WAD 或重新编译。历史评估的链接结果不等于运行证据。固定上游源码在上一轮通过本地 checkout 核对，详细校正已分配到各工作包。

## 参考

- [原可行性评估](../../assessments/2026-11-08-doom-port-feasibility.md)
- [libc completion spec](2026-11-08-libc-completion-design.md) 与 [validation report](../reports/2026-11-08-libc-completion-validation.md)
- [构建系统契约](../../build/build.md)
- [doomgeneric 固定源码](https://github.com/ozkl/doomgeneric/tree/e257a09527062bfb986b6e5e0780839b6cbd5b71)，其中 [`i_system.c`](https://github.com/ozkl/doomgeneric/blob/e257a09527062bfb986b6e5e0780839b6cbd5b71/doomgeneric/i_system.c)、[`doomgeneric.h`](https://github.com/ozkl/doomgeneric/blob/e257a09527062bfb986b6e5e0780839b6cbd5b71/doomgeneric/doomgeneric.h)、[`m_config.c`](https://github.com/ozkl/doomgeneric/blob/e257a09527062bfb986b6e5e0780839b6cbd5b71/doomgeneric/m_config.c) 已通过本地固定版本 checkout 核对。
