# D04 — Doom 平台运行适配 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付真实 DG 平台、/bin/doom 和完整图形/输入归还。

**Architecture:** libgfx 独占视图与输入租约；D02 纯解码器产生边沿，D03 引擎驱动游戏逻辑。应用只用公开头和图形原语，不接 LVGL 平台。

**Tech Stack:** freestanding C、Clang/LLVM、GNU Make、现有 hosttests/Python harness、QEMU/QMP。

**Spec:** [Doom 平台运行适配](../specs/2026-11-10-doom-04-platform-design.md)。状态：文档评审通过，全部步骤未执行。

**前置：** D01、D02、D03 已完成；手工 smoke 可用外部访客 IWAD，自动打包/E2E 分别在 D05/D06。

## Global Constraints

- 遵守[总览公共约束](../specs/2026-11-10-doom-port-design.md)；仅根 make、profile 私有产物，不覆盖已有工作区修改。
- 无新 syscall/UAPI；非阻塞 /dev/keyboard；gfx_bind_input mouse_fd=-1。
- 320×200 RGB32，auto=min(3,W/320,H/200)；不足1或显式倍数放不下则失败。
- SIGINT/SIGTERM handler仅设置sig_atomic_t；cleanup幂等；永久图形/输入错误退出不抢回。

## Review Focus

- 先完整帧 EAGAIN，成功前不退成局部帧 → T1。
- 两个物理 Shift/Ctrl/Alt 不产生过早UP → T2。
- 队列满、DESYNC/撤销后不能继续投递 → T2。
- 每个初始化失败点与信号路径只归还一次 → T3。
- 相对 IWAD/chdir 与 -scaling/-gfxmode/@ 绕过 → T4。

## 执行与验证约定

以下新命令只在拥有它的任务建立后可用；“预期通过”是未来验收要求，不是本轮结果。测试先记录目标缺失/行为 RED，再实现并用同一入口得到 GREEN；仅新增公开路径时允许编译 RED，既有逻辑修复须有行为断言。写测试这一步同时接通所属测试组/根路由，确保 RED 确实运行测试；unknown target/case 不算功能 RED。每任务提交只包含明确列出的本任务文件，不使用 git add .；提交前检查实际 diff。跨任务接口以对应 spec 和下列 Interfaces 为准。

### Task 1: 整数缩放与呈现 (T1)

**Files:** 新增 `user/doom/doom_video.[ch]`、`hosttests/cases/test_doom_video.c`；修改 `hosttests/mk/doom.mk`、`mk/components/run.mk` 的 video 组。

**Interfaces:** 定义 doom_video_t（应用私有）；int doom_video_open(doom_video_t*,unsigned requested_scale)，int doom_video_draw(doom_video_t*,const uint32_t *pixels)，void doom_video_close(doom_video_t*)，gfx_handle_t *doom_video_handle(doom_video_t*)；draw成功0，EAGAIN返回1待重试，永久错误-1/errno。

- [ ] 写测试：`center_1024_768` 结果960×600@(32,84)；`nearest_neighbor_1_2_3` 逐像素匹配；`full_present_retry` 首次EAGAIN后仍完整提交；`fatal_present_cleanup` 永久错误不acquire；溢出/过小尺寸失败。
- [ ] 运行 RED：`make test-doom-host DOOM_CASE=video`；保存实际失败原因，确认不是其他基线故障。
- [ ] 实现：记录缺实现 RED；从 /dev/fb 校验尺寸，gfx_open/acquire后清黑，使用可复用缩放缓冲和公开gfx_sprite_blit。使用共享present policy/重试间隔，不读私有pixels；close仅释放本模块持有资源。
- [ ] 验证 GREEN：`make test-doom-host DOOM_CASE=video`。预期目标组全部通过/制品满足断言；真实运行结果单独留证。
- [ ] 审阅差异并提交本任务明确文件，提交信息：`feat(doom): add centered integer-scaled gfx presentation`。

### Task 2: 键盘租约、逻辑键和队列 (T2)

**Files:** 新增 `user/doom/doom_input.[ch]`、`hosttests/cases/test_doom_input.c`、`hosttests/cases/test_doom_input_engine.c`、`thirdpart/patches/doomgeneric/0004-os01-input-budget.patch`；修改 `thirdpart/doomgeneric.manifest`、`hosttests/mk/doom.mk`、`mk/components/run.mk` 的 input 组。

**Interfaces:** 定义 doom_input_t；int doom_input_open(doom_input_t*,gfx_handle_t*)，int doom_input_pump(doom_input_t*,uint64_t now_ms)，int doom_input_pop(doom_input_t*,int *pressed,unsigned char *key)，void doom_input_close(doom_input_t*)。pop 在递交前检查 foreground，返回 1/0/-1 分别为一项/空/错误且清队列；DG_GetKey 遇 -1 非零退出，不透传。OS01 I_GetEvent 每批最多 16 项。

- [ ] 写测试：`two_ctrl_one_release` 一次DOWN且第二个松开才UP；`e0_release_identity` DOWN/UP同值；`ring_capacity_256` 满时失败而非覆盖；`lease_error_clears_pending` EOVERFLOW/EACCES/ESTALE后无投递；`owner_revoked_between_pump_pop` 泵成功后撤销owner、无新帧时pop仍返回-1且无递交；`engine_batch_preserves_64_down_up` 向健康应用队列放入 64 个不同映射键 DOWN 和后续 UP，经真实补丁后 I_GetEvent/D_PostEvent 以及每批后的事件消费，最多16项一批，128项完整按序到达且最终无按键按住；typematic不增加事件。
- [ ] 运行 RED：`make test-doom-host DOOM_CASE=input`；保存实际失败原因，确认不是其他基线故障。
- [ ] 实现：记录缺实现 RED；kbd非阻塞打开后绑定mouse=-1，Caps bit0转GUI位。每泵先检查owner，最多读256原始字节，喂D02解码器；按物理键位图与Doom逻辑键计数合并。pop不读取设备，但每次递交前重新校验owner。增加 0004 补丁在 I_GetEvent 局部计数限制16项并保留首个UP结束行为；将补丁加入manifest和内容键。生产集成测试链接补丁后的 i_input.c/d_event.c，并按 BuildNewTic 的 I_StartTic→ProcessEvents 顺序消费真实事件环；不得用一个无容量限制的假 D_PostEvent 掩盖上游丢事件。
- [ ] 验证 GREEN：`make test-doom-host DOOM_CASE=input`；`make test-gui-host CASE=input-decode`。预期目标组全部通过/制品满足断言；真实运行结果单独留证。
- [ ] 审阅差异并提交本任务明确文件，提交信息：`feat(doom): adapt leased PS2 input to Doom key edges`。

### Task 3: 运行时、信号和清理 (T3)

**Files:** 新增 `user/doom/doom_runtime.[ch]`、`hosttests/cases/test_doom_runtime.c`；修改 `hosttests/mk/doom.mk`、`mk/components/run.mk` 的 runtime 组。

**Interfaces:** 定义 doom_runtime_t组合video/input状态；int doom_runtime_open(doom_runtime_t*,unsigned)，void doom_runtime_cleanup(void)供atexit；uint64_t doom_runtime_now_ms(void)；其余DG错误由主线程统一非零退出。增加应用私有 gfx_handle_t *doom_video_handle(doom_video_t*) 向input借出句柄，不另开视图。

- [ ] 写测试：`failure_at_each_stage` 断言unbind→close kbd→release→close gfx顺序且恰好一次；`double_cleanup`无双释放；`signal_handler_only_flag`无IO；`clock_failure_fatal`不假计时；`sleep_eintr_remaining`续睡且退出信号可返回。
- [ ] 运行 RED：`make test-doom-host DOOM_CASE=runtime`；保存实际失败原因，确认不是其他基线故障。
- [ ] 实现：记录行为 RED；组合T1/T2资源，atexit注册失败在Create前返回。单调时间内部64位、DG返回低32位；nanosleep续睡；main线程检查退出标志，已初始化引擎正常信号路径只调用一次I_Quit。
- [ ] 验证 GREEN：`make test-doom-host DOOM_CASE=runtime`；`make test-doom-host DOOM_CASE=all`。预期目标组全部通过/制品满足断言；真实运行结果单独留证。
- [ ] 审阅差异并提交本任务明确文件，提交信息：`feat(doom): manage runtime lifetime and cooperative shutdown`。

### Task 4: 真实入口与应用发布 (T4)

**Files:** 新增 `user/doom/doom_os01.c`、`user/doom/app.mk`；修改 `user/doom/engine.mk` 与必要的 `user/Makefile` 依赖；扩展 `qemutests/test_doom_build_contract.py` 与runtime参数测试。

**Interfaces:** 产生 main及6个DG_*标准签名；APP_EXTRA_OBJS引用D03 archive，APP_LDLIBS=-lgui -lgfx，APP_SHARED_SOURCES=common/gfx_client_policy.c。APP_CFLAGS 使用 D03 私有副本的 doomgeneric 头路径以及 DOOMGENERIC_RESX=320/DOOMGENERIC_RESY=200/OS01_DOOM，适配对象依赖副本准备stamp。

- [x] 写测试：`iwad_before_chdir` 相对路径先转绝对；`reject_unsafe_engine_args` 拒绝-scaling/-gfxmode/@及缺参数；`missing_iwad_no_acquire` 无WAD不夺屏；`parallel_adapter_headers_ready` 并行make不会先编适配对象再准备头；`adapter_engine_dimensions_match` 所有平台/引擎TU均使用320×200；`dg_getkey_error_exits` pop错误触发非零cleanup且不作为while成功条件；`final_elf_link` 无未定义符号且map无LVGL/platform。
- [x] 运行 RED：`make test-doom-host DOOM_CASE=runtime`；`make test-doom-host DOOM_CASE=build`；保存实际失败原因，确认不是其他基线故障。
- [x] 实现：实现--os01-scale 1..3与--os01-data-dir默认/doom-data，剔除平台参数，默认IWAD固定路径。注册真实DG后才加入app.mk；main为Create后Tick前泵入循环，DG_GetKey只pop。保留stdin/TTY和fallback角色。
- [x] 验证 GREEN：`make user`；`make test-doom-host DOOM_CASE=all`；`make test-static`；` 在外部准备的访客IWAD上执行开局/控制/菜单退出smoke并记录证据`。预期目标组全部通过/制品满足断言；真实运行结果单独留证。
- [x] 审阅差异并提交本任务明确文件，提交信息：`feat(doom): publish the playable OS01 Doom application`。

## 交付检查

make user 发布真实 ELF，map 审计与手工真实 IWAD smoke通过。未做D06时不得称整个首版已完成。

- [ ] 按 spec 对照每条接口/错误/测试要求，记录实际通过与未运行项。
- [ ] 将产物接口和验证证据交给下一工作包；本 plan 未列的游戏功能不顺带扩展。
