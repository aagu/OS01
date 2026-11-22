# x86_64 测试内核链接插桩实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标** 让 BP、NMI、跨页读取和坏帧指针复用一次内核编译、一套测试链接制品与一份基础启动盘，同时保持现有证据。

**架构** Make 复用正常内核对象，单独链接测试 ELF 与 BIN。准备入口复制稳定的正常盘、替换 ESP 内核并发布不可变制品。运行入口固定清单，用现有 runner 串行启动，每次使用独立写入层和场景参数。

**工具链** GNU Make、Clang、ELF LLD、Python unittest、QEMU、qemu-img、mtools。

**Spec** [内核测试的链接插桩与 QEMU 制品复用](../specs/2026-11-22-test-kernel-link-instrumentation-design.md)。

## 全局约束

- 本计划交付 spec 第一阶段。AArch64、内存、USB、GFX 的候选边界各自需要验证，仍由现有用例提供证据。
- 使用 `PROFILE=x86_64-clang`、单 vCPU、现有 OVMF 和原异常/回溯断言。现有 `INTR_X86_TIMEOUT` 仍是每个用例的预算，默认 120 秒。
- /boot 由 x86 固件从 ESP 读取 `kernel.bin`。同代 ELF、BIN、盘与固件在整个套件期间固定。每轮退出后清理写入层。
- 保持普通对象编译参数、正常链接回执、自测和 syscall E2E 的互斥规则。测试对象与 wrapper 只在专用链接中出现。
- 每项任务在自己的验收通过后独立提交。避免夹带工作区其他改动。

## 评审重点

| 故障或竞争 | 验收归属 |
| --- | --- |
| 冷启动时两个 Make 交错，测试回执缺失或链接被注入失败 | Task 1 核对编译次数、租约、指纹以及正常回执和制品 |
| FAT 读回不一致、正常镜像被并发替换、缓存代被半发布 | Task 2 核对旧代可读、发布失败不写请求清单、新代哈希独立 |
| 场景缺省、无效、重复或格式错误，以及 SIGNAL_ONLY 误计入 | Task 3 检查选择协议和 FULL 计数 |
| QEMU 启动失败、超时或外部取消时遗留子进程与写入层 | Task 4 检查停机先于清理、套件及时返回 |
| 目录生成或矩阵入口绕过四场景，或 dry-run 真正启动 QEMU | Task 5 检查直接入口、目录入口和 prepare/run 拆分 |

### Task 1 · 共享对象和两阶段测试链接

**文件** 修改 `kernel/Makefile`、`mk/components/kernel.mk`。在现有 `qemutests/test_build_incremental.py` 和 `qemutests/build_contract.sh` 补覆盖。

**接口** 内核子 Make 增加 `intr-x86-kernel`。它在 `$(KERNEL_BUILD_DIR)/intr-x86-link/` 产出 `kernel.elf.stage1`、`kallsyms.o`、`kernel.elf`、`kernel.bin` 和专用链接回执。顶层内部目标 `_intr-x86-link` 以普通 `$(KERNEL_ARTIFACT)` 为先决条件，并接受绝对路径 `INTR_X86_REQUEST_DIR`，把配对 ELF/BIN 放到该请求的 `link/` 供 Task 2 使用。

- [ ] 增加命令追踪断言，普通和测试在同配置下使用同一 C/汇编对象列表，新链路有自己的两阶段输出，测试变体选择不会落入普通 `.cflags`。以干净、已准备好 sysroot/libk 的 x86 工作区运行两个独立 Make 进程，强制覆盖普通先到和测试先到两种顺序，等价对象各编译一次。
- [ ] 抽出现有 x86 两阶段链接规则可共用的构建参数。独立传入输出目录、附加对象和有序 wrap flags。stage1 通过驱动传 flags，最终链接按现有 x86 规则直接传给 LLD。测试版单独生成 kallsyms，由同一 ELF 用正常 objcopy 参数生成 BIN。Task 3 加入真实探针和 wrap flags。
- [ ] 在顶层正常内核入口与 `_intr-x86-link` 之间共用按 profile 的排他构建锁。先按原顺序取得 sysroot 读租约，再等待对象锁。锁覆盖子 Make 和本次请求输出快照，普通构建也在锁内校验并发布自己的产物。同一进程执行测试目标只在检测到正常对象确有变化时重编。由先决条件在测试链接取得锁之前完成普通回执缺失时的原有 `-B` 恢复。测试路径持锁时不递归调用普通目标。测试回执缺失时只失效测试 stage1、最终输出、回执及测试专用的 runtime identity，普通的同名文件继续由原规则拥有。
- [ ] 验证热构建无重编，编译指纹和开关过期后正确重编，测试链接参数或对象变化只重链。启用现有 stage1/final 故障注入点分别断言正常 ELF/BIN、回执不变且本次请求无可消费的 link/。通过相关 Python 构建增量测试及 x86 构建契约相应桶。

### Task 2 · 发布可复用的制品代

**文件** 新建 `qemutests/harness/intr_x86_artifact.py`。修改 `mk/components/image.mk`，把 `qemutests/test_resolution_switcher.py` 中已有的 ESP 解析移动至共享 `qemutests/harness/gpt.py`，继续使用现有分辨率测试。新测试 `qemutests/test_intr_x86_artifact.py` 以元数据注册。

**接口** `prepare_intr_x86(build_dir: Path, base_image: Path, firmware: Path, elf: Path, binary: Path, manifest: Path) -> Path` 发布请求清单。 `load_intr_x86(manifest: Path) -> IntrX86Artifacts` 解析并核验，返回含已解析绝对路径 `elf`、`binary`、`image`、`firmware`、`generation` 与各哈希的不可变对象。准备 CLI 用 `python3 -m qemutests.harness.intr_x86_artifact prepare --build-dir … --base-image … --firmware … --elf … --binary … --manifest …`。本任务同时在 `mk/components/run.mk` 增加只准备的内部目标 `_prepare-intr-x86-artifact`，接收绝对路径 `INTR_X86_REQUEST_DIR`，以正常盘和固件为先决条件，再调用 Task 1 链接并生成 `<request>/artifact.json`。直接调用方用 mktemp -d 创建请求目录；Make 套件入口在 Task 5 统一提供目录。

- [ ] 写测试覆盖非默认 ESP 偏移、非法 GPT、FAT 读回不符、同键重复生成、两个同键发布者、旧代在新代发布后仍可启动，以及清单 ELF 或盘被替换时拒绝。原图只允许以只读方式访问。把正常盘写至同文件系统的私有临时输出，整个镜像成功后原子更名。消费者先固定已打开的正常盘再复制，能读取旧盘或新盘的完整内容。正常镜像生成同步保护其现有可变 rootfs staging 与 manifest，避免 mkdisk 读取一半 staging；所有相关路径保持原制品名称。
- [ ] 用现有 GPT 解析逻辑核对边界后取得 ESP 偏移。仅修改私有磁盘副本，用 mcopy 写入 `::/kernel.bin` 并抽取逐字节比较。正常镜像、固件、请求 ELF/BIN 的 SHA-256 与准备格式版本组成生成键。把清单、ELF、BIN、raw 盘和固件放在 `$(BUILD_DIR)/prepared/intr-x86/<generation>/`。同键发布使用排他锁和临时目录原子更名，已发布的路径禁止覆写。准备清单原子写到请求路径，直到全部读回验证成功再发布。请求内的重复 link 快照在完成或失败后清理。
- [ ] 加载只读校验清单版本、profile、配对文件和记录哈希后固定该代。加载失败时在启动 QEMU 前返回配置错误。运行现有分辨率隔离单测和新增制品单测，检查共享盘永久键命中时只准备一次测试盘。

### Task 3 · 测试对象、场景协议和返回计数

**文件** 新建 `kernel/selftest/fixtures/x86_intr_wrap.c`，先从 `kernel/arch/x86_64/intr/fault_probe.c` 提取 BP/NMI/trace-read/bad-fp 逻辑到测试链接专用对象，复用 `kernel/arch/x86_64/intr/fault_probe_bad_fp.S`。更新 `kernel/Makefile`。新增 `kernel/selftest/fixtures/x86_intr_fw_cfg.c` 与 `kernel/include/selftest/x86_intr_fw_cfg.h` 承载可由宿主链接的解析器。新增 `hosttests/cases/test_x86_intr_fw_cfg.c` 和对应链接规则，按仓库规则登记并再生成。Task 5 删除旧探针源及旧构建路由。

**接口** 测试对象提供 `int __wrap_fs_boot_prepare(void)`、`void __wrap_context_run_return_work(struct pt_regs *frame, enum context_return_mode mode)`，以及健康对照用的 `_Noreturn void __wrap_kernel_start_userspace(void)` 和 `_Noreturn void __real_kernel_start_userspace(void)`，保持现有启动入口的 noreturn 契约。QEMU 命名文件 `opt/os01/intr-wrap-case` 使用单字节 ASCII `b/n/r/f`，对应 `bp/nmi/read/bad-fp`。缺项透传。其他长度或字节以及重复项输出 `[x86-intr-fault] configuration-error` 后停机。单一套件标识 `intr-x86-wrap` 单独输出，保留原 armed、stage、异常及回溯字段。健康对照输出 `[x86-intr-fault] full-returns=<n>`。定义 `int x86_intr_fw_cfg_case(void *ctx, void (*select)(void *, uint16_t), int (*read_byte)(void *))`，返回 0 表示缺项，-1 表示配置错误，正值为有效场景字节。测试读端可返回 -1 表示截断，实际端口读端返回 0–255。目录条目上限 4096，选中项 selector 范围 0x0020–0x3fff。

- [ ] 为纯解析器增加宿主用例。对最终 wrapper 加静态调用与分支检查，覆盖 FULL 计数、SIGNAL_ONLY 透传以及自然对齐的无锁原子操作。测试 fw_cfg 签名、目录计数与 selector 的字节序、56 字节名称边界、错误长度、重复项及非法 selector。跨页计数差值和真实 QEMU 健康对照放在 Task 4。
- [ ] 在 `fs_boot_prepare` wrapper 入口读 QEMU 签名及 fw_cfg 目录。大端解码目录计数、长度和 selector，拒绝超出规范命名文件范围的 selector、超限目录和缺少结尾的名称。按固定 56 字节名称边界扫描，完成全表检查后才读取所选项，防止重复项被隐藏在第一个命中之后。无 fw_cfg 签名视为接口缺省，存在接口但目录无效时输出配置错误。整个 guest 解析只链接到测试 ELF。保留探针已有的关 IRQ、无日志锁前置条件。选择原探针时保留原汇编恢复标签和故障类型。缺项时先透传真实 `fs_boot_prepare`。到达 runtime checkpoint 后，在 `kernel_start_userspace` wrapper 进入用户空间前读取计数。若还未看到 FULL 且 IRQ 已打开，使用 CPU pause 等待真实 timer IRQ。单调时钟超出 250 ms 或完成 100 万次 poll 即退出，任一条件先满足即停止。硬迭代上限独立于 jiffies、校准数据和被测 IRQ。测试以固定时钟及恒零计数证明能在硬上限报告零并透传。健康对照要求 runtime checkpoint 和正计数，报告零时验收失败。计数 wrapper 在透传前做自然对齐的无锁 relaxed 原子递增，trace-read 在同一计数上前后各做原子读。测试 C 文件位于普通 selftest 通配规则以外，只列入 intr-x86 链接对象。检查最终 ELF 三处跳转、栈/调用 ABI 及热路径无锁，验证普通对象列表和 ELF 无新增探针、fw_cfg 解析器及 wrapper。
- [ ] 运行本任务的宿主解析和证据单测，使用 Task 2 的准备目标构建制品，反汇编核对实际链接及正常 ELF 的符号排除。此任务可在 Task 4 runner 进入前验收并提交。

### Task 4 · 串行 runner 和单用例执行

**文件** 修改 `qemutests/x86_intr_fault.py`、`qemutests/exception_trace.py`、`qemutests/harness/qemu_cmd.py` 与相应的现有 unittest。扩展 Task 2 的共享制品模块提供写入层生命周期。

**接口** 两个现有 runner 增加 `--manifest PATH` 启动单例。该模式拒绝同时给出旧的 `--image`、`--elf` 和 `--firmware`，旧直接形式继续要求这些参数。 `x86_intr_fault.py --suite --manifest PATH --qemu PATH --nm PATH --timeout SECONDS --log-dir PATH` 串行推进 `bp,nmi,read,bad-fp`。 `run_intr_x86_suite(args) -> int` 将共同参数交给现有 `run_case`。manifest 模式的 `--profile` 固定为 x86_64-clang，`--build-dir` 默认取制品准备时记录的绝对 profile build dir，Make 和矩阵入口显式传入这两个归档参数。x86 runner 接收现有 kwargs，exception runner 用 args 字段，四个现场用例各产生标准 RunArchive 的 result.json。测试专用 `x86_intr_fault.py --control absent|invalid-value|invalid-length --manifest PATH --qemu PATH --nm PATH --timeout SECONDS --log-dir PATH` 走相同 launcher，对应省略 fw_cfg、发送 `?`、发送 `bb`。 `intr_x86_overlay(base: Path, run_dir: Path) -> ContextManager[Path]` 用 qemu-img 启动显式 raw backing 的 qcow2。共享 QEMU builder 增加可选 `image_format="raw"`，仅新入口用 qcow2。

- [ ] 写 runner 单测检查 suite 固定一次已校验的制品身份、四个调用顺序、每轮最多一个 emulator、失败时停止后续用例。为两个已有 evidence 函数增加精确场景、共享身份、重复和顺序检查。用 stub 模拟 QEMU 启动失败、超时和 SIGTERM，验证 suite 和两个独立 Python 入口均在可捕获中断时先退出 process 再删写入层。验证错误场景或身份时不会发送 QMP NMI，配置错误能提前结束观察。每轮使用独立归档目录，基础盘哈希、实际写入层身份和退出状态分别记录。
- [ ] 复用既有 `ProcessSession`、QMP NMI 和两个证据判定函数。新入口给各自的 QEMU argv 增加 fw_cfg 项，显式指定 qcow2，不传 `-snapshot`。先验收场景标记与套件身份，再进入 NMI 注入或终止判据。两 CLI 复用现有 SIGTERM 转 unwind 和 finally 清理。将安装函数公开为 `install_intr_x86_signal_handlers() -> None`，由两个直接 CLI 和矩阵入口在主线程安装。保留原各场景每条证据检查，按场景建立自己的归档和 deadline。套件入口遇到失败保留用例结果并退出非零。已用的旧直接选项仍可用于旧制品；Task 5 删除 Make 的旧制品生成路径。
- [ ] 运行 `python3 -m unittest qemutests.test_x86_intr_fault qemutests.test_exception_trace qemutests.test_intr_x86_artifact qemutests.test_harness_qemu_cmd`。用 `request=$(mktemp -d)`、`make PROFILE=x86_64-clang _prepare-intr-x86-artifact INTR_X86_REQUEST_DIR="$request"` 生成 `$request/artifact.json`。以该清单先分别直跑两个现有 runner，再用 `--suite` 仓内 CLI 跑四个现场用例。依次执行三个 `--control` 值。absent 要求 runtime checkpoint 和正计数并拒绝 armed/配置错误。两个非法控制要求明确配置错误并拒绝 armed，并验证这类 guest 输出在正式运行路径会返回配置错误码 2。控制命令自身只有观察到其期望结果时才返回 0。

### Task 5 · 接入目录和 Make，移除旧变体

**文件** 修改 `mk/components/run.mk`、`qemutests/run_suite_matrix.py`、`mk/project.mk`、两个 profile、`kernel/core/main.c`、`kernel/arch/x86_64/intr/entry.S`、`kernel/Makefile`、测试目录源登记与现有构建契约。更新 `docs/testing/testing-policy.md` 中受影响的使用说明。

**接口** `test-qemu-intr-x86` 和 `test-qemu SUITE=intr-x86` 使用相同准备/运行逻辑。 `_test-suite-prepare SUITE_ID=intr-x86 PREPARED_MANIFEST=…` 仅准备输入。 `_test-suite-run RUN_CONFIG=intr-x86_default PREPARED_MANIFEST=…` 仅运行四个场景。矩阵清单采用既有 schema v1，只含一个单 vCPU config。Task 2 写 `<request>/artifact.json`；矩阵准备 adapter 写 `PREPARED_MANIFEST`，在 `cases[0]` 内设置 `config_id=intr-x86_default`、绝对 `artifact_manifest_path`、`case_timeout_s=INTR_X86_TIMEOUT` 及 `timeout_s=4 * case_timeout_s + 60`。保留矩阵已有的 profile、资源属性与固件/盘字段供调度器消费。矩阵分流用同一 config 校验清单和 profile，运行时使用已固定的 artifact manifest。额外记录准备时解析的 QEMU 和 llvm-nm 路径，直接及矩阵入口均沿用 profile 的工具设置。 `_intr-x86-link` 不进入通用 runner。

- [ ] 扩展构建和矩阵单测覆盖两个 Make 入口、一次准备/一次套件执行、准备不启动 QEMU、运行不递归构建、dry-run 无启动或发布、非法 profile/冲突变体拒绝。用可执行 fake QEMU 自阻塞并写出 PID，从 `execute_matrix_tasks → _test-suite-run` 触发外层超时，断言 mock 进程被回收、写入层已清理以及后续场景未启动。测试直接 `_test-suite-run` 的 SIGTERM 路径。确认其它 suite 和 AArch64 异常回溯模式仍经各自入口。
- [ ] 为直接目标和矩阵 prepare 目标分别设置一次展开的私有 Make 变量 `INTR_X86_REQUEST_DIR`。使用只计算 UUID、不进行文件 I/O 的生成命令，在对应目标的 recipe 执行前固定绝对路径。直接目标位于 profile 的 test-results 下，矩阵准备请求位于调用方清单目录下。目录创建、递归准备、运行或清单发布使用分开的 recipe 行。递归行只传路径和参数，所有 mkdir、mktemp 与 Python 运行位于非递归行或受现有 dry-run 分支保护，避免 `make -n` 写文件。矩阵 run 在通用镜像复制前识别 `intr-x86_default`。在 `run_suite_config` 自身进程安装 Task 4 的 handler 并调用 `run_intr_x86_suite`，避免新增一层有界子进程。 `_test-suite-run` 的 Python recipe 以 exec 启动。 `execute_matrix_tasks` 从同一 config 读取总预算，外层超时时先 SIGTERM 整个 Make 进程组，保留 20 秒让 runner 关闭它直接持有的 QEMU 和写入层，之后才向仍存活的进程组发 SIGKILL。外部硬杀区别于可捕获取消。把 intr-x86 从固定 normal.before/normal.after 排除，用同代只读验证和独立写入层替代。给旧版路径的拒绝消息关联到现有套件入口。
- [ ] 在新路径现场四个场景通过后，删除四个 `X86_INTR_FAULT_TEST_*` 的启动分支和 `entry.S` 的 trace-read 计数，删除旧探针选择、旧四变体及目录路由。保留普通内核 selftest 的 IRQ 统计和 BOOT-2 尾部探针。更新 `config/test-bridge.json` 已登记的 intr-x86 suite 项，保持 ID、nightly tier、入口和矩阵配置。当前扫描器的 `@os01-test` schema 不表达 suite 路由字段。 `image_var` 改为普通输入盘 `$(BUILD_DIR)/image/disk.img`，实际测试盘由 manifest 解析。运行 `make regen-test-catalog`。同时更新仍期待旧布局的 build-contract 桶。
- [ ] 执行 `make check-test-catalog check-ci-schedule test-harness`、`make PROFILE=x86_64-clang test-contract`、`make test-qemu SUITE=intr-x86`、`make test-qemu-intr-x86`、`make test-qemu SUITE=phase-0`。用唯一的 `matrix_request=$(mktemp -d)` 分别执行 `make PROFILE=x86_64-clang _test-suite-prepare SUITE_ID=intr-x86 PREPARED_MANIFEST="$matrix_request/matrix.json"` 和 `make PROFILE=x86_64-clang _test-suite-run RUN_CONFIG=intr-x86_default PREPARED_MANIFEST="$matrix_request/matrix.json"`。检查矩阵 prepare 产出的 artifact generation 和同配置直接入口相同。单独执行 `make OS01_SYSTEST=1 test-syscall` 和 `make KERNEL_SELFTEST=1 test-kernel-selftest`。确认正常盘保护、每次完整 Make 套件恰好新增四份有效的 result.json，保留原证据、IRQ 自测与构建审计。
