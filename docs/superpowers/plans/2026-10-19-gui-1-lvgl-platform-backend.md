# P3 GUI-1 LVGL 平台后端 (libgui) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**状态：** 修订版 v2，2026-10-19 独立子 agent 第3轮 APPROVE；本文只规划实施，复审不执行源码变更。

**Goal:** 抽取 libgui 的平台生命周期、显示/离屏后端、tick、flush 和 step，迁移 test_lvgl，并完成已批准 spec 的宿主与 QEMU 验收。

**Architecture:** libgui 静态归档包含 LVGL、平台与共享 gfx policy，公共 sysroot 只发布 GUI 原生头。单进程单实例、同线程串行调用；应用持有 poll 主循环。宿主以原生工具链独立编译同一配置的 LVGL，目标系统归档与宿主对象完全分离。

**Tech Stack:** C11、Clang/LLVM、LVGL v9.5.0、mmap/poll/CLOCK_MONOTONIC、libgfx、Python unittest、既有 ProcessSession/RunArchive 测试基础设施。

**Spec:** `docs/superpowers/specs/2026-10-19-gui-1-lvgl-platform-backend-design.md`（独立评审已通过）。执行者必须同时阅读 spec；以下任务细化实现方式，不放宽其契约。

## Global Constraints

- 公共接口及精确签名取自 spec §3；只安装 `gui.h`、`gui_platform.h`、`libgui.a` 和 LVGL 许可证；`libgui/internal/`、LVGL 头和 lv_conf 不安装。
- 同进程同时仅一个平台上下文，包括尚未销毁的终止态上下文；重复 create 返回 NULL/EBUSY。所有 GUI/LVGL 操作同线程串行，不支持回调内递归 step/destroy。
- 非法 mode/零离屏尺寸/非零屏幕配置尺寸为 EINVAL；有效宽高不超过 LV_COORD_MAX 且 width<=16383、height<=65535，超限 EOVERFLOW。lines=min(buffer_lines?buffer_lines:40,height)。size_t 乘法、页对齐及 LVGL 16/32 位字段均须预检。
- 像素区只用匿名 mmap，MAP_FAILED 不可作为有效映射；小平台结构可 calloc。屏幕首次有脏帧才 acquire；无 auto_acquire 开关。
- libgfx 成功 0、失败 -1+errno；acquire EBUSY 直接终止。present EAGAIN 从失败调用结束后起至少退避 250ms，期间继续 handler；其余永久错误关闭 view 并锁存状态/errno，后续 step 不再运行 handler 或访问 gfx。
- step 每次至多一次 present；pending 仅成功提交后清除；无脏帧不提交。所有返回路径初始化 out=100，正常等待按 spec §4.5 算至 [0,100]，合法 0 不钳成 1；支持 out=NULL。
- `config/lv_conf.h` 唯一配置源，LVGL 子模块源码不改。fatal 使用 `#define LV_ASSERT_HANDLER do { _exit(1); } while (0);` 及 `<unistd.h>`。LVGL 内部 OOM 不承诺返回错误或事务回滚；fatal/kill 不执行 destroy，依靠内核进程退出回收。正常返回和平台可返回错误必须确定性回收。
- 用户态目标只从根 Make 进入；宿主对象只写 HOST_TEST_BUILD_DIR。struct 改动如涉及现有共享布局必须 clean；本任务不修改 kernel/UAPI struct。使用 logger 规则处理任何必要内核变更，但本计划不新增内核实现。

## Review Focus

1. 16 位 header 截断及容量边界：Task 3 参数矩阵 + Task 4 真实 header 验证。
2. EAGAIN 阻塞耗时、期间新 flush 和 timer：Task 3 可控时钟/handler 断言。
3. 所有权丢失后再次 step、清理覆盖 errno：Task 3 调用计数及锁存检查。
4. fatal/kill 恢复 terminal，不能靠输入回显判成功：Task 4 原生 fatal 子进程 + Task 7 acquired 后故障、QMP 和 shell canary。
5. 白色 X 字节、边缘/跨 partial 行和旧 timer 访问：Task 4 真实渲染、50 轮生命周期、映射追踪。

## 文件与执行顺序

Task 1 配置与真实断言审计 → Task 2 头/内部接口 → Task 3 确定性测试先行及平台实现 → Task 4 原生 LVGL 集成 → Task 5 目标归档及 sysroot → Task 6 应用迁移 → Task 7 QEMU 门禁。

目标新源码在 `libgui/src/{platform,tick}.c`；公开头在 `libgui/include/gui/`；内部跨文件契约在 `libgui/internal/gui_tick.h`、`gui_lvgl_private.h`。宿主规则归 `hosttests/Makefile`，focused 根入口归 `mk/components/run.mk`。新 GUI suite 唯一名称为 `gui`，driver 为 `qemutests/gui_test.py`，不新建第二套 QEMU 进程管理。每项完成后只提交其列明的文件；本轮文档修订不执行这些提交。

---

### Task 1: 致命断言配置及真实宏展开审计

**Files:** Modify `config/lv_conf.h`（按宏名定位，现约 513–514 行）；Create `qemutests/gui_build_audit.py`。

**Interfaces:** 审计脚本 CLI `python3 qemutests/gui_build_audit.py --assert-config`，使用 HOST_CC（默认 clang），失败非零、成功打印 `gui assert config: PASS`。后续 Task 5 扩展同一脚本的 sysroot 审计模式。

- [ ] **Step 1: 先编写配置审计。** 脚本使用临时文件，真实包含 `<src/misc/lv_assert.h>`，以 `-Ithirdpart/lvgl -Iconfig -DLV_CONF_INCLUDE_SIMPLE` 和宿主系统头编译；函数中展开 LV_ASSERT、LV_ASSERT_MSG、LV_ASSERT_FORMAT_MSG、LV_ASSERT_MALLOC。另用预处理宏输出检查 fatal handler 调用 `_exit(1)` 且 include 为 unistd.h，防止旧 while(1) 也编译通过。没有修改配置的临时覆盖来伪造正向结果；不链接目标 archive。
- [ ] **Step 2: 运行 `python3 qemutests/gui_build_audit.py --assert-config`。** 当前旧配置应 FAIL（handler 仍永久循环）；故意缺少分号的临时负例还须被真实 LV_ASSERT 展开编译拒绝，负例只写临时目录。
- [ ] **Step 3: 修改唯一 config：**

```c
#define LV_ASSERT_HANDLER_INCLUDE <unistd.h>
#define LV_ASSERT_HANDLER do { _exit(1); } while (0);
```

- [ ] **Step 4: 重跑上述命令。** 正例 PASS、缺分号负例仍被拒绝；真正 pool 耗尽的运行时退出由 Task 4/7 验证，语法审计不代替它。
- [ ] **Step 5: 提交** config 和审计脚本，信息 `build(lvgl): exit on fatal assertions and audit real macro expansion`。

---

### Task 2: 公共头与明确的内部 tick 契约

**Files:** Create `libgui/include/gui/{gui,gui_platform}.h`、`libgui/internal/{gui_lvgl_private,gui_tick}.h`。

**Interfaces:** 公共 API 完整照抄 spec §3.1–3.2，包括 enum 数值、C++ extern C 和错误说明。私有桥接 `lv_display_t *gui_platform_get_lv_display(gui_platform_t *platform)` 照 spec §3.3，NULL 输入返回 NULL/EINVAL。

跨 tick.c/platform.c 的内部接口固定为：

```c
void gui_tick_reset(void);
int gui_tick_now_ms(uint64_t *out); /* 0 成功；-1/errno 失败，同时锁存首个错误 */
uint32_t gui_tick_get_cb(void);    /* LVGL callback，失败返回上次成功 tick */
int gui_tick_error(void);         /* 0 或锁存的正 errno；不再发 syscall */
```

单实例允许 tick 状态为模块私有静态。create 必须首先检查单实例占用；已有上下文时直接 NULL/EBUSY，不调用 reset、不访问设备或 LVGL、不改变活动上下文的任何状态。只有确认无活动上下文后才为新实例 reset；destroy 完成时 reset，不得在每次 step 清错。毫秒计算先转 uint64，callback 再截取低 32 位。

- [ ] **Step 1: 创建两个公共头和私有头。** 仅公共头安装；不暴露 LVGL struct，不增添 GUI-2 输入或控件 Facade。
- [ ] **Step 2: 从 stdin 构造最小 C/C++ consumer，分别运行** `printf '#include <gui/gui.h>\n' | clang -x c -fsyntax-only -Ilibgui/include -` 和对应的 `clang++ -x c++`。二者均 PASS，编译器不获得 LVGL 包含路径。
- [ ] **Step 3: 编译私有桥接 consumer。** 添加 `-Ilibgui/internal -Ithirdpart -Ithirdpart/lvgl -Iconfig -DLV_CONF_INCLUDE_SIMPLE` 后 PASS；缺 LVGL 路径时应失败。不要将这些选项放入全局 user CFLAGS。
- [ ] **Step 4: 提交**本任务四个头，信息 `feat(gui): define public platform API and private tick contract`。

---

### Task 3: 确定性测试先行，完成平台实现及宿主 focused 入口

**Files:** Create `libgui/src/{platform,tick}.c`、`hosttests/cases/test_gui_platform.c`、`hosttests/mock/gui_test_runtime.{c,h}`；Modify `hosttests/Makefile`、`mk/components/run.mk`。

**Interfaces:** 实现 Task 2 全部 API；内部 `struct gui_platform` 对应 spec §4.1，并保存初始化阶段、tick/flush 错误和终止 errno。测试辅助接口 `void gui_test_reset(void)`、`void gui_test_set_now(uint64_t ms)`、`void gui_test_queue_flush(const lv_area_t *area,const uint32_t *pixels)` 及 `struct gui_test_counts`（handler/acquire/present/release/close/ready/mmap/munmap 次数）；辅助接口仅宿主测试编译，不安装。

**宿主构建规则（在运行测试前落地）：**

- 所有原生对象在 `$(HOST_TEST_BUILD_DIR)/gui/`，fixture 状态机目录为 `deterministic/`，真实集成目录为 `integration/`，共同真实 LVGL native archive 在 `lvgl/`。不得复用 `$(BUILD_DIR)/libgui` 或旧 liblvgl 对象；本任务先提供原生 LVGL archive 规则，Task 4 直接消费它，不依赖 Task 5。
- `GUI_HOST_CFLAGS` 使用 HOST_CC、宿主 libc/system headers、libgui/include、libgui/internal、libgfx、thirdpart、thirdpart/lvgl、config，定义 LV_CONF_INCLUDE_SIMPLE；不带 --target/--sysroot/freestanding/内核 linker flags，不使用 FRAMEWORK_INC 强制注入 test_platform.h，不把 OS01 libc/include 或 kernel/include 放在原生系统头前。
- `<uapi/gfx.h>`/`<uapi/fb.h>` 通过生成在 `$(TEST_BLD)/gui/include/uapi/` 的原始头 symlink 解析，只给该根 include 路径；UAPI 内容不复制不改写，errno 使用原生 TLS。头依赖和 config 用 `-MMD -MP` 跟踪。
- 使用同一 config 原生编译 thirdpart/lvgl/src 全部 C 为 `gui/lvgl/obj/<相对路径>.o` 并归档 `gui/lvgl/liblvgl-host.a`。平台、tick、策略和测试另行编译。两个测试 fixture 均不链接目标 libc/libgui/liblvgl；版本检查使用同一 manifest。
- 确定性 fixture 链接真实 native LVGL 以维持类型及 display/header 初始化，使用宿主 linker `--wrap` 包装 clock_gettime、calloc/mmap/munmap、open/read/close、gfx_open/get_info/acquire/present/release/close/sprite_blit、lv_timer_handler、lv_display_flush_ready、lv_display_create。共享 policy 编入 fixture。host OS 调用包装只对被测平台调用生效，其他原生/LVGL 调用转 `__real_*`，不得破坏宿主 runtime。
- handler wrapper 用测试 display 的实际 `flush_cb`（测试 TU 可含 LVGL display 私有头）同步注入队列，不推进真实 renderer；ready wrapper 记录次数后按 fixture 状态安全完成通知。LVGL create wrapper 可透传或返回 NULL；shim NULL 仅验证平台分支，不宣称真实 LVGL OOM 可恢复。gfx mock 失败一律 -1+errno。
- 新二进制 `test_gui_platform.elf` 使用 test_framework.h 的 case 表及 RUN_ALL_TESTS（非空 `[TEST]` protocol，失败退出非零），加入 TEST_BINS，提供 hosttests `test-gui-platform` phony 目标，经 run_hosttests.py 对该 binary 归档执行。根新增 `test-gui-host CASE=platform|lvgl|all`：映射 hosttests focused 目标、默认 all，未知 CASE 在调子 Make 前非零拒绝；all 在 Task 4 完成后依赖两个 fixture。不要修改通用 test-host 的 CASE 语义。两 fixture 在 make test-host 中均执行。

- [ ] **Step 1: 编写并注册确定性测试、mock 和上述原生/focused 构建规则。** 尚无 platform/tick 实现时运行 `make PROFILE=x86_64-clang test-gui-host CASE=platform`，预期因明确缺少平台源码或 API 实现而 FAIL；其他编译/环境失败不得当作预期红灯。
- [ ] **Step 2: 用例矩阵写出以下断言。** 每例 reset fixture，调用实际平台 API；计数器检查清理顺序，而非仅检查退出状态。

| 用例 | 注入与必须断言 |
|---|---|
| config_validation | NULL 配置读取 mock fb；无效 mode、离屏零维度、屏幕非零维度 EINVAL；width 16383/16384、height 65535/65536 的接受/拒绝边界（大画布分配用受控 mock，不分配巨量宿主内存）；buffer_lines=0 默认40并裁剪到height，UINT32_MAX 同样裁剪 |
| capacity_and_header | 乘法先提升size_t；页对齐/uint32长度缩窄检查。生产上限下不可达的size_t溢出用内部 checked-multiply helper 单测，不用无效维度冒充溢出覆盖；真实header字段等于width/lines/stride，异常header回滚EINVAL |
| rollback_each_stage | calloc、fb open/read/短读/格式、clock、gfx open/info、draw/shadow mmap、LVGL返回NULL、header绑定校验逐阶段失败；原errno、已获得资源恰好释放、MAP_FAILED不munmap、release失败仍close、可再次create；create从不acquire |
| singleton_and_queries | 第二create含终止态未destroy时EBUSY；活动上下文先由tick callback锁存EIO、尚未step时再次create返回EBUSY且无额外设备/LVGL调用，随后原上下文step仍ERROR/EIO（不得被reset清除）；destroy(NULL)；destroy后可create；NULL getter/step按spec返回；get_pixel越界/非离屏EINVAL，合法白色得到0x00ffffff |
| terminal_policy | acquire EBUSY/EIO、present EACCES/ESTALE/EIO分别返回规定终止态；cleanup故意改errno，仍返回原错误；连续3次step无handler/acquire/present；destroy不重复close |
| retry_end_time | mock present从100ms阻塞到180ms后返回EAGAIN，deadline=430；429ms仍无提交、430ms提交；EAGAIN期间handler每次运行，注入新flush覆盖待提交画面且保留pending |
| frame_submission | 同一handler内多个partial flush只present一次；无flush不present；成功后无脏帧不present；EAGAIN保留pending；flush错误锁存EINVAL、ready恰好一次、不提交 |
| wait_and_null_out | handler返回0/20/无timer，处理耗时饱和减法、100上限、deadline剩余min；所有提前返回out已赋100；out=NULL仍完整推进 |
| clock_failures | create初读、step前读、tick callback、handler后、present失败后设deadline、计算等待前分别失败；ERROR及原errno锁存，callback保留上次tick，重新create清错；覆盖低32位tick回绕且uint64退避正确 |

- [ ] **Step 3: 实现 tick.c 和 platform.c。** 顺序严格为 spec §4.2 创建/回滚和销毁；flush 只复制且每次 ready；step 先初始化 out/检查终止，再 handler，再一次按需present，最后统一计算等待。时钟/flush错误进入相同终止路径。尺寸/offset转size_t，get_pixel掩掉X；不通过 assert 代替用户参数校验。
- [ ] **Step 4: 运行 focused 测试及未知 CASE 负例。** `make PROFILE=x86_64-clang test-gui-host CASE=platform` 应有全部case PASS与result.json；`CASE=unknown` 应在启动fixture前非零拒绝。所有阶段需看到相关断言确实执行，不接受空日志或只编译不运行。
- [ ] **Step 5: 提交**本任务列明文件，信息 `feat(gui): implement platform against deterministic host contract tests`。

---

### Task 4: 真实原生 LVGL 渲染、生命周期和 pool OOM

**Files:** Create `hosttests/cases/test_gui_lvgl_integration.c`；Modify `hosttests/Makefile`、`hosttests/mock/gui_test_runtime.{c,h}`（按 fixture 分离行为）。

**Interfaces:** 消费 Task 3 的 native LVGL archive 和 Task 2 API。输出 `test_gui_lvgl_integration.elf`，TEST_BINS 注册及 hosttests `test-gui-lvgl`/根 `test-gui-host CASE=lvgl`。编译使用单独 integration 对象目录，不链接确定性 handler/flush/LVGL create wrappers。

- [ ] **Step 1: 编写真实渲染用例并确认其能检出错误。** 使用实际 lv_timer_handler、renderer、display/header、TLSF；只包装 clock/设备/平台分配等 OS 边界。用私有display的active screen绘制纯背景0x181825、无边框/圆角/阴影/透明/文字覆盖的0x1A73E8矩形及白块，buffer_lines=7；采样背景、矩形内部、画布四角、y=6/7/13/14跨partial边界。不能直接写shadow canvas。真实渲染后核对header字段及像素X掩码。临时把期望蓝色改错时fixture必须非零失败，再恢复；不修改renderer伪造红灯。
- [ ] **Step 2: 生命周期断言。** 50次create→控件→step→destroy，逐轮追踪平台映射/释放对数、destroy后display列表为空、LVGL全局deinit；下轮重新init后pool可用量回到相同基线，继续推进时钟和handler不得触达旧映射。可用mprotect/munmap保护旧区并以映射记录确认地址复用不误报；环境支持时给该fixture单独启用ASan并重跑，不把ASan对象混进无ASan archive。离屏所有设备open/ownership计数必须为0。
- [ ] **Step 3: pool OOM 子进程。** 在fork子进程内create真实LVGL平台，依次用4096、256、16、1字节调用实际lv_malloc，每档分配到NULL才进入下一档，全部成功块不释放，耗尽4MiB池。lv_malloc返回NULL本身不触发断言；确认1字节分配也失败后，调用真实 `lv_timer_create(NULL, 1, NULL)`，其首个 `lv_ll_ins_head()` 失败后的 `LV_ASSERT_MALLOC(new_timer)` 必须触发fatal。若此调用意外返回，子进程以2退出并使测试FAIL，不能自行_exit(1)或手工assert伪造成功。该两阶段算法同时用于Task7故障程序。父进程限时waitpid，5秒内必须WIFEXITED且WEXITSTATUS=1；超时kill并wait回收，测试FAIL。子进程不destroy；父进程仍可创建和销毁自己的平台。保持失败样本及退出码证据。
- [ ] **Step 4: 运行** `make PROFILE=x86_64-clang test-gui-host CASE=lvgl`、`make PROFILE=x86_64-clang test-gui-host`、`make PROFILE=x86_64-clang test-host`。两个GUI fixture均有非空case证据、零失败；确认focused没有构建全部内核测试来冒充筛选。
- [ ] **Step 5: 提交**真实集成及规则，信息 `test(gui): verify real host rendering lifecycle and fatal pool exhaustion`。

---

### Task 5: 目标 libgui、sysroot 全链迁移及构建审计

**Files:** Create `libgui/Makefile`；Modify `mk/components/sysroot.mk`、`mk/profiles/x86_64-clang.mk`、`scripts/audit_lvgl_symbols.sh`、`qemutests/gui_build_audit.py`；Delete `liblvgl/Makefile`。本任务只迁移库，应用消费改在Task6；中间仅要求sysroot构建通过。

**Interfaces:** `LIBGUI_BUILD_DIR := $(BUILD_DIR)/libgui`；对象分 `obj/lvgl/<src相对路径>.o`、`obj/platform/{platform,tick}.o`、`obj/common/gfx_client_policy.o`，统一归档 `$(LIBGUI_BUILD_DIR)/lib/libgui.a`。审计脚本新CLI：`--sysroot PATH --build-dir PATH`；旧符号审计增加 `--build-dir PATH`（默认仍x86_64-clang），只合并obj/lvgl子树。

- [ ] **Step 1: 先扩展构建审计，记录迁移前基线。** 在旧树执行 `make PROFILE=x86_64-clang sysroot`，记录真实旧generation路径/manifest/哈希及staging/lvgl存在证据，记录到profile的audit目录；此步骤必须在改staging规则前执行。新审计对旧sysroot预期FAIL（缺libgui/仍有旧LVGL头）。不要先clean销毁增量基线。
- [ ] **Step 2: 实现 libgui Makefile。** 沿用profile-only、manifest版本检查、目标工具链、-MMD/-MP；目标使用staging kernel/libc/libgfx头和config，不链接宿主库。platform/tick/shared policy纳入archive；路径前缀避免同名.o碰撞。install清空自己的staging/gui、验证根路径，只安装spec规定的两个头、archive、`usr/share/licenses/libgui/LVGL-LICENCE.txt`，生成排序manifest。不改子模块。
- [ ] **Step 3: 更新发布链和旧符号审计。** 替换lvgl install stamp定义及SYSROOT_STAMP依赖、generation循环lvgl→gui；profile用LIBGUI_BUILD_DIR取代旧变量；删除旧Makefile并将删除纳入提交。audit_lvgl_symbols.sh从obj/lvgl提取LVGL本身的libc依赖，保留原forbidden检查；不得合并新增平台/gfx依赖然后误判LVGL依赖污染。搜索活动构建/审计入口的旧名并逐项处理，历史文档不批量替换。
- [ ] **Step 4: 先增量迁移。** 不clean，运行 `make PROFILE=x86_64-clang sysroot`；`python3 qemutests/gui_build_audit.py --sysroot build/x86_64-clang/sysroot --build-dir build/x86_64-clang` 必须PASS。检查新generation无旧liblvgl.a/include/lvgl/私有桥接和lv_conf，规定产物及license与manifest一致，保留的旧generation哈希不变；staging/lvgl可遗留，但不得被发布器消费。
- [ ] **Step 5: 检查原生/目标隔离和增量依赖。** 用llvm-nm确认目标archive定义gui API、LVGL及gfx_client_present_policy；用仅sysroot目标系统头的最小C/C++ consumer编译公共头。记录目标对象mtime，分别临时touch唯一config及user/common/gfx_client_policy.h后重跑sysroot，确认受影响对象重编；不需要让mtime变化产生源码差异。符号审计运行 `bash scripts/audit_lvgl_symbols.sh --build-dir build/x86_64-clang` 并PASS。
- [ ] **Step 6: 干净构建。** 增量证据保存后，`make PROFILE=x86_64-clang clean`，再 `make PROFILE=x86_64-clang sysroot` 并重跑构建/符号审计；没有旧staging也必须通过。完整user/image在Task6消费迁移后再跑。
- [ ] **Step 7: 提交**本任务全部文件（包括git rm liblvgl/Makefile），信息 `build(gui): migrate native GUI archive and atomic sysroot publication`。

---

### Task 6: test_lvgl 应用迁移与模式兼容

**Files:** Modify `user/test_lvgl/{app.mk,test_lvgl.c}`、`user/Makefile`。

**Interfaces:** `APP_LDLIBS := -lgui -lgfx`，移除APP_SHARED_SOURCES策略；libc仍由既有链接规则追加。私有APP_CFLAGS精确使用spec §2.3路径，不向其他应用泄漏。

- [ ] **Step 1: 更改链接与include。** 移除全局 `-I$(TARGET_INCDIR)/lvgl -DLV_CONF_INCLUDE_SIMPLE`，只在test_lvgl/app.mk设私有路径。通过app现有target-specific规则应用，不能污染common或其他应用。
- [ ] **Step 2: 替换平台样板。** 删除应用的lv_init/lv_deinit、tick注册、fb/gfx打开、draw buffer、flush、present policy；使用gui create/step/destroy和私有display active screen构造UI。保留build_rich_ui及TLSF检查，不从应用删除display/修改flush。所有正常返回和可返回错误走统一destroy出口；fatal/kill不返回，明确由内核回收。
- [ ] **Step 3: 模式契约。** 新增`--smoke`并保留`smoke`，离屏320×240，以应用启动后的uint64单调1秒预算step+poll，最终像素断言背景及按钮，成功才打印原PASS标志/返回0；不把初始黑画布当完成信号，超时/时钟错误非零。保留`--wait`、`-w`、`wait`、`gui`别名和q/Q/ESC退出；缺省屏幕展示后正常退出。poll处理EINTR、HUP/ERR/NVAL，不读掉smoke/缺省模式的排队shell命令。平台不改变termios或输出ANSI。
- [ ] **Step 4: 构建并检查include隔离。** `make PROFILE=x86_64-clang user`；审计test_lvgl的编译命令拥有私有路径，其他应用没有；最终链接-lgui -lgfx -lc。成功生成artifact；private headers/config依赖文件有效。应用运行证据在Task7采集，不能以编译通过宣称smoke通过。
- [ ] **Step 5: 提交**这三个应用文件，信息 `refactor(test_lvgl): use gui platform lifecycle and poll step loop`。

---

### Task 7: GUI QEMU suite、故障程序及回归门禁

**Files:** Create `qemutests/gui_test.py`、`qemutests/test_gui_runner.py`、`user/test_gui_fault/{app.mk,test_gui_fault.c}`；Modify `qemutests/run_test.py`、`mk/components/run.mk`、`qemutests/test_make_qemu_failure.py`、`docs/build/build.md`。更新既有`qemutests/test_lvgl_runner.py`为受main保护的兼容入口，委托新driver，不保留import时启动QEMU的第二套runner。

**Interfaces:** suite唯一名gui；`test_gui(tester) -> bool` 消费dispatcher的image/firmware/build-dir，内部以ProcessSession和RunArchive执行SMP1/2矩阵（与monitor矩阵一样，不让外层预先启动QEMU）。正常镜像、-snapshot、serial stdio、QMP；每个SMP独立session/结果档案。guest故障程序支持 `--hold`、`--oom`，私有APP_CFLAGS同test_lvgl，链接-lgui -lgfx，正常程序发现机制自动入image。

故障程序先create+真实纯色UI+step，确认present成功即已acquired后输出独立行`[gui-fault] acquired pid=N mode=hold|oom`并fflush。hold继续step+poll直到被kill；oom在输出/保持短暂可截图窗口后执行Task4相同的4096/256/16/1字节分档耗尽，再调用真实 `lv_timer_create(NULL, 1, NULL)` 触发其内部LV_ASSERT_MALLOC；lv_malloc本身无断言。若timer调用意外返回，以2退出使driver失败，不能自行_exit(1)或手工assert冒充fatal。此程序是独立验收工具，不给正常test_lvgl增加故障模式。

- [ ] **Step 1: 先写driver self-tests并确定完整证据。** FakeProcessSession/FakeQMP覆盖下表正例和缺失/回显/乱序/超时/异常负例；不启动QEMU，import无副作用。加入TEST_HARNESS_MODULES，`python3 -m unittest qemutests.test_gui_runner` 在driver缺失时FAIL。

| 场景（SMP1/2均运行） | 运行与可观察结果 |
|---|---|
| boot/handshake | 先等待shell再发送拼接nonce canary，匹配命令之后的独立输出行；回显不含完整目标串，不能把旧日志当新证据 |
| smoke | 执行/bin/test_lvgl --smoke及smoke别名，独立PASS行和shell报告退出码0；guest自身1秒预算严格不放宽；host给2秒收集余量，boot预算独立，不能宣称2秒即满足guest1秒 |
| normal_exit | 缺省/bin/test_lvgl正常退出0；QMP在显示期间看到期望背景/蓝色区域，退出后terminal恢复 |
| wait_quit | /bin/test_lvgl --wait显示后注入q（必要时加换行以适应既有stdin模式），退出0，QMP恢复且shell新canary成功 |
| busy_owner | 后台/bin/test_gui_fault --hold先获得acquired证据并QMP确认纯色面；再运行test_lvgl应非零退出，errno/诊断证明EBUSY；画面仍属hold。随后终止hold并验证恢复 |
| killed_owner | hold已acquired后由shell kill -9捕获的pid并wait；检查guest退出状态与内核signal日志，无遗留进程，fallback恢复 |
| fatal_owner | /bin/test_gui_fault --oom已acquired后真实pool OOM，进程退出码1（与normal shell命令结果关联）；不能只见acquired就判通过，必须有退出及恢复证据 |

每次恢复都在新观察窗口用QMP确认测试纯色/GUI已离开且terminal内容有变化，并发送拼接nonce输出独立shell canary、截图看到对应终端内容；仅PASS日志、输入回显或QEMU进程退出码不算guest退出/显示恢复证据。收集guest命令退出码使用带nonce的shell printf，匹配完整独立行；只读取本次命令发送后的日志切片。每个场景后观察至少1秒无panic；超时、QMP错误、缺退出码、错误SMP或归档写入失败均非零，始终finally回收QEMU及临时资源。

- [ ] **Step 2: 实现故障程序、driver及兼容入口。** 复用既有QMP/PPM解析helper和ProcessSession/RunArchive；GUI driver明确0=PASS、1=FAIL/timeout、2=环境/归档错误的返回约定，保存argv、SMP、日志、截图、image/firmware哈希和逐场景结果。正常镜像只读运行，不写源码树临时日志；BOOT与每条guest命令有独立deadline，hold进程在所有失败路径也尽力清理。
- [ ] **Step 3: 注册顶层唯一入口。** `mk/components/run.mk`增加 `TEST_QEMU_FLAVOR_gui` 空值、`TEST_QEMU_IMG_gui=$(NORMAL_IMAGE)`、SUITE白名单/帮助、正常镜像hash guard的gui例外（与monitor同类），由driver前后哈希验证snapshot运行无改写。`qemutests/run_test.py`增加lazy dispatch到test_gui，保留统一结果/异常处理。将qemutests.test_gui_runner加入显式harness列表；更新dry-run/failure自测和docs/build/build.md。递归make构建与python启动仍在不同recipe行，make -n不启动QEMU。
- [ ] **Step 4: 运行无QEMU验证。** `python3 -m unittest qemutests.test_gui_runner qemutests.test_make_qemu_failure`、`make PROFILE=x86_64-clang test-harness`、`make -n PROFILE=x86_64-clang test-qemu SUITE=gui`：所有fake测试PASS，dry-run展示实际gui dispatch且无QEMU进程启动；缺少场景或错误矩阵的fake负例必须FAIL。
- [ ] **Step 5: 运行真实GUI验收。** `make PROFILE=x86_64-clang test-qemu SUITE=gui`；该单次入口必须执行SMP=1与SMP=2全部表列场景，result.json记录两种cpu_count和各场景，任一缺失非零。不得以phase-0代替GUI门禁。`OS01_SYSTEST`与`KERNEL_SELFTEST`保持0，使用正常交互镜像。
- [ ] **Step 6: 最后回归。** 运行`make PROFILE=x86_64-clang test-static`、`make PROFILE=x86_64-clang test-host`、`make PROFILE=x86_64-clang test-qemu SUITE=phase-0`；另运行Task1配置审计及Task5构建/符号审计。Task5干净sysroot、Task6正常user和本任务image链共同覆盖正常全量构建。新改动后仅重跑相关门禁，失败必须修复后再提交。
- [ ] **Step 7: 提交**本任务列明文件，信息 `test(gui): gate SMP smoke ownership and fatal recovery in QEMU`；记录实际执行命令和结果，不能将本plan的评审通过写成这些测试已通过。

---

## Plan 自审与独立评审记录

- Spec §2构建/隔离→Task2/5/6；§3全部API→Task2/3；§4生命周期/容量/flush/状态机/step→Task3；§5.1确定性/真实/fatal/生命周期→Task3/4；§5.2增量/干净/双语言/符号审计→Task5/6；§5.3迁移/SMP/QEMU→Task6/7。
- 后续执行按checkbox跟踪，每个任务以自己的测试证据结束；本plan的记录仅代表文档评审，不代表代码实现。

| 轮次 | 意见与处理 | 结论 |
|---|---|---|
| 子 agent 第1轮 | 6项REQUEST CHANGES：宿主入口、原生LVGL构建、QEMU覆盖、确定性边界、增量/审计、fatal清理；v2逐项补齐 | 第2轮确认已覆盖 |
| 子 agent 第2轮 | 2项REQUEST CHANGES：lv_malloc不触发断言，改用真实timer分配断言；reset须在单实例检查之后且增加回归断言 | 已修订，第3轮确认解决 |
| 子 agent 第3轮 | 核对真实timer断言路径、单实例检查顺序及前两轮全部修订 | APPROVE；无剩余阻塞项 |
