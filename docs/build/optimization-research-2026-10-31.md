# 测试框架与构建系统优化调研

调研日期：2026-10-31。代码基线：`05620330`。范围：GNU Make 构建、hosttests、Python harness、QEMU E2E、GitHub Actions。本文是调研结论和候选路线，不代表已批准的实施设计。

## 结论

建议保留 GNU Make、profile/variant 隔离、single-writer sysroot 和现有测试协议，按“正确性与可观测性 → 去重与缓存 → 精细失效 → 有界并行”推进。当前没有证据表明迁移 CMake/Bazel 或更换 unittest 是首要收益来源。

最值得先做的工作：

1. 补齐构建失效契约，优先检查 mbedTLS 的 libc 头文件失效和编译失败处理。
2. 明确测试分层和场景归属，梳理重叠场景；清理 CI 的重复检查，建立套件覆盖清单，恢复目前跳过的 AArch64 build contract。
3. 对用户程序和 BusyBox 缩小 sysroot generation 变化造成的重建范围。
4. 为构建阶段和测试阶段分别计时，再引入编译缓存、有界测试并行。
5. 将超长的 `hosttests/Makefile` 拆成公共规则与子系统模块，统一测试注册，保留现有根目录入口。

## 1. 当前架构：应当保留什么

| 层次 | 当前实现 | 评价 |
|---|---|---|
| 根入口 | `Makefile` 100 行，组合 `mk/components/*.mk` | 已完成组件拆分，无需重写总入口 |
| 配置 | `mk/project.mk`、profiles、toolchains、capability gate | profile 输出隔离、受控子 make 环境值得保留 |
| sysroot | staging manifest → 不可变 generation → 原子 symlink 发布；consumer lease | 已有明确所有权和发布边界 |
| 内核增量 | `kernel/Makefile` 的 `-MD -MP`、`.d` include、`.cflags` stamp | 已有头依赖与编译选项失效，不能再按“完全没有头依赖”评估 |
| 用户增量 | `.d`、app.mk 自动发现；generation 改变后 `-B` | 同 generation 内可增量，跨 generation 较粗 |
| 宿主测试 | 147 个普通二进制 + 2 个 exit-status 二进制；独立负向框架契约 | 生产代码宿主编译、严格退出码和协议校验是优势 |
| Python 框架 | 23 个显式注册 unittest 模块；另有负向 C 框架契约 | 显式白名单避免误发现 QEMU 脚本，应继续保留 |
| QEMU | 公共 ProcessSession、独立 runner、超时与进程组清理 | 应收敛公共能力，不必强迫所有套件共用一套业务 runner |
| 证据 | RunArchive、原子 result.json、stdout/stderr、镜像摘要 | 已有基础，优先扩展现有 schema |
| CI | harness / x86 / AArch64 / contract / nightly / images | 已有分层，主要缺口是去重、覆盖与复用 |

定位依据：`mk/components/{sysroot,kernel,user,image,run}.mk`、`kernel/Makefile:395`、`hosttests/Makefile:62`、`qemutests/harness/{process,result}.py`、`.github/workflows/ci.yml`。

文档存在滞后：AGENTS.md 仍要求结构变化后 clean，并说明没有头依赖；实际内核已有依赖文件。`docs/build/build.md` 还把内核描述为 generation 变化后全量重编，而 `mk/components/kernel.mk` 当前明确使用稳定 sysroot 路径及头级增量。仍须遵守现行 clean 要求，待新增失效契约验证完整后再统一更新说明。

## 2. 测量与证据边界

本次未执行 clean、全量镜像构建、破坏性 contract 或真实 QEMU E2E，未修改构建及测试实现。以下是本机已有工作树、热缓存条件下的小测，不是 CI 性能基准。本机工具版本为 Clang 23.1.4、GNU Make 4.4.1、Python 3.14.8；与 CI Dockerfile 声明的工具版本不同，不作跨环境速度比较。

| 项目 | 观察值 | 能说明什么 |
|---|---:|---|
| BusyBox 源码树逐文件 sha256 + sort + 汇总摘要 | 0.787 s，单次 | recipe 每次 FORCE 执行，即使编译复用也有扫描成本 |
| mbedTLS 同类摘要 | 0.370 s，单次 | 与 BusyBox 合计约 1.16 s，仅是摘要部分 |
| `git rev-parse HEAD` + `git status --porcelain` | 中位 0.0347 s，5 次 | host runner 每个二进制报告都执行一次，可考虑每批次采集 |
| 已有本地 result.json | 1352 个 | 可立即建立统计工具，无需先改所有 runner |
| hosttests 历史记录 | 1184 条，中位 0.2372 s | 短测试中进程启动、元数据和归档成本值得拆分测量 |
| systest 历史记录 | 2 条，中位 46.3618 s | 样本太少，只用于确认 E2E 与短宿主测试量级差异 |
| kernel-runtime-link 历史记录 | 9 条，中位 8.5323 s | 值得优先计时的静态检查之一 |
| header-object-audit 历史记录 | 9 条，中位 3.1108 s | 第二个值得单独计时的审计 |

归档汇总在本次框架回归完成之前采集。历史样本可能跨版本、配置或故障注入，不用于推断当前失败率。宿主 `duration_s` 包含报告中采集 Git 元数据前后的部分时间，并非纯测试主体时间；应增加细分指标后再判断并行收益。

## 3. 按优先级排序的发现

### P0：先建立可信的增量与成功判定

**mbedTLS 的失效输入不完整。** `mk/components/sysroot.mk:93` 使用 staging/libc 头文件编译，但从第 102 行开始计算的 digest 包含 mbedTLS 源树、配置、entropy.c 和 Clang banner，没有 libc 头内容或实际编译选项。虽然依赖 libc-install stamp，recipe 被唤醒后仍可能因为 digest 相同而复用对象。由规则推导出“libc 头修改可能不重编 mbedTLS”的风险；本次没有做修改头文件的动态复现。

建议加入实际头依赖（这里是 `-isystem`，应考虑 `-MD`，单用 `-MMD` 会漏掉系统头）或 consumed-header digest，并对编译命令、目标三元组和工具链身份建模。

**mbedTLS 当前可在部分编译失败后继续归档并写成功 receipt。** 第 116–140 行的循环把失败计数后继续；后续仍执行 `ar`、写 receipt 和 touch stamp，没有检查 `fail == 0`。需要先确认哪些文件允许不构建，再通过明确源清单或显式可选集合表达，禁止用“编译失败就略过”决定库内容。验收要覆盖故意注入编译错误、不发布成功 receipt、修复后能重试。

**配置和工具切换的失效规则不统一。** 内核 `.cflags` 已覆盖 C/ASM 编译选项，但注释明确 LDFLAGS-only 变化尚未纳入；stamp 也没有完整编译器身份。用户程序、libc 的通用规则未见对应命令 stamp。建议统一区分 compile-command key 与 link-command key，先用契约测试验证具体缺口，避免盲目扩大重编范围。

**dry-run 契约不能靠 `make -n` 字面含义保证。** CI 明确跳过 AArch64 contract，注释归因于递归 recipe 的 dry-run 副作用；当前是否仍能复现需在临时 checkout 中验证，不能把注释当现存故障证明。GNU Make 对含 `$(MAKE)` 或带 `+` 的 recipe 有特殊执行规则，当前项目也大量依赖它保留 jobserver。应把“仅递归 make”与文件写入分开，并测试文件内容、路径集合和必要的 mtime 均不变化。[GNU Make 递归调用规则](https://www.gnu.org/software/make/manual/make.html#MAKE-Variable)

### P1：低风险去重、补覆盖和观测

**问题一：hosttests 的 Makefile 超长，职责集中。** `hosttests/Makefile` 已达 4897 行，将测试清单、生产源码与 mock 组合、编译选项、链接规则、focused 入口和运行协议放在同一文件。新增测试需要在长文件中寻找并仿照已有规则，容易复制不同版本的写法；公共规则与子系统特殊需求也难以区分。这个问题应独立治理，不必等到并行调度阶段。

建议让主 Makefile 只负责 profile 校验、公共配置、显式模块 include 和聚合入口；按内存、调度、文件系统、驱动、GUI 等子系统拆分 `.mk`，提取公共编译、链接、depfile 和 runner 规则。特殊编译选项与 mock 保留在所属模块，避免为了统一模板改变测试语义。验收以职责边界和改动范围为准：新增某子系统测试只需修改所属模块，公共规则有唯一来源，拆分前后登记集合、focused 选择、编译选项和运行结果一致；不能只把长文件机械切成多个文件。

**问题二：测试场景重叠，分层与编写规范不明确。** 用户指出现有场景存在重叠；本次已确认 CI 入口重复执行同一审计，但尚未逐项审查全部用例，不能把所有跨层覆盖都认定为冗余。当前还缺少统一的场景归属、各层应断言什么、何时允许重复覆盖以及新增测试应放在哪里的明确规则。仅按 host/static/QEMU 等执行方式分类，无法回答这些问题。

建议先编写测试规范，再按“被测行为、触发条件、输入、断言、故障模式”建立场景矩阵。每个场景记录稳定 ID、被测组件、测试层、主验收用例、关联用例、mock 边界及保留跨层覆盖的理由。建议的分工如下：

| 测试层 | 主要责任 | 场景边界 |
|---|---|---|
| hosttests | 生产逻辑的边界值、状态迁移、错误处理与可控并发协议 | 替代硬件和环境边界，避免复制生产算法作为测试实现 |
| static / build contract | 源码、ABI、布局、构建依赖、旗标、产物发布规则 | 能由静态或构建证据判定的规则在此验收 |
| test-harness | runner 的判定、超时、清理、协议解析与证据归档 | fake guest 输出用于验证框架，不能充当内核行为通过的证据 |
| kernel selftest | 需要真实内核上下文的内部机制 | 保留宿主无法代表的中断、CPU、内存和调度集成行为 |
| QEMU E2E | 启动链、跨组件接线、用户可见行为与硬件集成 | 聚焦代表性链路，细粒度输入组合优先由较低层覆盖 |

先区分三种重叠：同一用例被多个入口重复运行、同层用例具有相同输入与断言、跨层验证同一行为的不同集成风险。前两类优先合并或共享 fixture；跨层覆盖需说明各自能够发现的故障，不能因为名称相似就删除。统一命名、fixture 隔离、超时、PASS/FAIL/SKIP、失败诊断及注册要求；新增用例必须指明新增了哪项风险覆盖。

验收要求：现有场景形成可审查的覆盖矩阵；每个重叠组都有合并、保留或删除的理由；删除前确认独有断言和故障覆盖已被承接；每项需求有明确主验收位置，必要的跨层覆盖可追溯。测试数量下降本身不作为优化成功指标。

**CI 有可确认的重复覆盖。** `.github/workflows/ci.yml` 的 x86 job 先运行 `test-runtime`，随后 `test-static`；后者的 prerequisite 就是 `test-runtime`。此外又单独执行 kernel-layout 和 user-canary，这两项也在 static 内。分成多次 make 调用不能共享 Make 本次依赖图的去重。

建议每轮只运行一次完整 `test-static`，保留 focused targets 供本地调试。如果 `test-gui-incremental` 改变产物，必须明确需要在改变后再验证哪些检查，不能机械删除所有后置检查。同步调整 `test_ci_workflow.py`，从钉死重复命令改为验证所需覆盖和正确依赖。

**已注册 E2E 不等于 CI 覆盖。** 当前 CI 的直接 E2E 调用以 phase-0、systest、kernel-selftest 和 AArch64 部分模式为主；network、inittab-phase、gfx、resolution、driver-model、monitor、terminal-present、gui、gui-desktop、intr-x86 未见对应直接调度，nightly 也没有补齐。Python runner 的 fake 回归不能替代真实 guest 验收。

建议建立显式 suite registry，记录架构、能力、variant、是否写镜像、timeout、SMP/RAM 矩阵、PR/nightly 层级、结果路径。CI 使用 registry 的显式子集，并做注册项与调度项的差集审计。PR 保留基础启动/syscall/SMP；按子系统补充相关 guest 测试；nightly 覆盖其余注册套件。依赖触及公共 UAPI/调度/内存时扩大选择，未知变更回退完整集合。

**先采集 wall time，再决定调度。** 扩展现有结果记录，区分 stage/install、digest、sysroot publish、compile、link、image、QEMU boot、guest case、archive 时间，以及重编对象数、relink 数、cache 命中数。成功运行也上传轻量汇总，失败再附完整日志和截图。不能把当前有限的本地日志当作 CI 基准。

**报告元数据可批量复用。** `run_hosttests.py:325` 每个二进制都读取 Git 信息，第 533 行串行运行。149 个二进制按本机小测外推，Git 查询部分约 5 秒，尚未实测完整 suite 节省量。可在批次开始记录 revision/dirty，在各报告复用，并明确这是批次快照；测试执行目录与临时文件应独立。

**框架自身也需要阶段诊断与总预算。** 当前 unittest 默认点状输出在长用例期间无法指出正在运行的测试。建议输出每模块/慢用例耗时，给涉及真实子进程、socket 和管道的 fixture 增加外层 deadline。例如 `test_gui_desktop_runner.py:357` 在进入有 timeout 的 `proc.wait` 前先调用无超时的 `stdout.readline()`；若 helper 启动阶段卡住，后面的 timeout 不能覆盖这个等待。这里是静态风险定位，不是对本次运行停留位置的确认。业务协议测试可用 fake clock 缩短等待；实际进程生命周期测试仍保留真实计时。

### P2：缩小重建范围与降低 no-op 开销

**用户程序每次 generation 改变都 `-B`。** `mk/components/user.mk:65` 比较 generation id；任一 sysroot 组件更新，都可能触发全部用户程序重编。BusyBox 的摘要同样包含 generation id；摘要改变后先删除私有树再复制重建。

建议分开建模“消费的头文件”“链接的 archive”“工具和命令”。第一阶段保持私有 generation 路径和 lease，只加编译缓存、统计失效原因；第二阶段在证明头删除/新增、库替换、ABI 变化均正确后，引入稳定消费路径或按消费内容计算 key。generation-only 变化不应等价于所有对象失效，库变化应重链接对应消费者。BusyBox 优先复用私有对象树，配置/overlay/工具链发生不兼容变化再清理。

**staging 的 FORCE 路径有真实工作。** sysroot.mk 每次调用 component install 再摘要；例如 libc install 会先删除 staging 树，随后重装。当前内容 stamp 和 `cp -p` 已减少向下游传播，但没有消除前段工作。建议先统计各阶段成本，再以输入 manifest、组件依赖和命令签名决定是否 install。必须继续处理删除、重命名、文件模式和 manifest 成员变化，不可只用目录 mtime 或 Git HEAD 替代内容判定。

**编译缓存适合作为独立渐进优化。** CI 镜像明确未安装 ccache；受控 `env -i` 又意味着不能只在外面设置环境变量就期待缓存配置透传。增加明确 launcher 和允许的缓存参数，按架构、工具链、关键配置隔离，验证 target 与 host 编译都覆盖。路径归一化可提高跨 checkout 复用，但不能设置 `base_dir=/` 或忽略头文件来换命中率。[ccache 官方手册](https://github.com/ccache/ccache/blob/master/doc/manual.adoc)

**CI 构建产物与缓存分别处理。** 缓存优先保存可再生的编译缓存；跨 job 传递同一提交测试镜像用 artifact，并附工具链、profile、variant 和内容摘要。不要直接恢复锁目录、lease、带绝对路径的整个 build 树。master-images 可以评估复用检查通过的 normal 镜像，需验证最终检查并未换成测试 variant，避免再全量编译一次。GitHub 官方也强调缓存缺失时必须能够重新生成依赖。[GitHub 缓存文档](https://docs.github.com/en/actions/concepts/workflows-and-actions/dependency-caching)

### P3：隔离完成后有界并行和模块化

**宿主测试。** 编译阶段先评估根入口 `make -jN test-host`（当前 CI 调用没有 `-j`）；运行阶段再给 run_hosttests 增加有界进程并发。保留每二进制 timeout、独立目录、失败后继续、确定性汇总和最坏退出码。不要直接用线程并发解析当前 `parse_v1`：`result.py` 中 `_REQUIRED` 是模块级可变字典，源码注明单线程假设。可先使用进程隔离，或先把状态局部化再引入线程。

**静态审计。** 当前多项是同一 recipe 的串行命令，单加 `make -j` 不会自动并行。先拆成明确的 build prerequisite 和独立审计 target；会触发构建、改 sysroot、修改源码或调用增量探针的检查继续隔离，纯只读审计再有界并行。

**QEMU。** 先把 build 与 run 调度分离，一份 variant 构建供多个 CPU 配置使用；每次运行使用独立日志、QMP socket、端口与可写 overlay/私有镜像。`TestRunner` 默认 snapshot=False，且 Make 层使用共享 `normal.before/normal.after` 文件，不适合直接并行多套 suite。QEMU 官方说明 `-snapshot` 把写入落在临时文件；需要验证持久化写入的测试仍应使用专用副本。[QEMU 镜像说明](https://www.qemu.org/docs/master/system/images)

现有 M1 matrix 等 harness 可能已经一次构建多次启动，应逐项复用，避免重复包装。CPU 密集 SMP 和 terminal-present 性能门控需要资源预算，性能基准不与其它 QEMU 竞争。

**模块化后的依赖收口。** hosttests 拆分按 P1 的独立治理项推进；`mk/components/run.mk` 的 1946 行也可按相同原则拆分。hosttests 只有部分规则使用 depfile；迁移前先盘点其余手工头依赖，尤其避免把多个 `.c` 编到同一 ELF 的命令机械加 `-MMD` 后覆盖 depfile。按对象生成独立 depfile，再统一 include。

测试清单应是一个显式数据源，生成或导出 TEST_BINS、focused groups、help 和 CI coverage 信息；继续禁止全目录 unittest 自动发现，避免启动 QEMU 脚本。

## 4. 路线比较

| 路线 | 收益 | 主要成本与风险 | 建议 |
|---|---|---|---|
| A：保留 Make，补契约、CI 去重、分阶段计时、编译缓存 | 工作边界清楚，可逐项验证收益 | 需要统一失效输入和受控环境参数 | 首选，先实施 |
| B：在 A 后统一 suite registry，拆大型 Make 文件，引入有界 runner 调度 | 降低维护成本，便于覆盖审计和并行 | 跨 Make/Python/CI 接口迁移；要保留现有入口和证据格式 | 第二阶段 |
| C：迁移 CMake/Ninja、Meson 或 Bazel | 有机会改善图管理或远程缓存 | 自定义 UEFI、sysroot 发布、第三方 overlay、variant、dry-run 合约仍须重写；不能自动解决 guest 结果判定 | 当前不建议；实测 Make 图成为主要瓶颈后再做局部原型 |

## 5. 建议的实施顺序与验收

| 批次 | 范围 | 可检查的完成条件 |
|---|---|---|
| 1 | 增量正确性审计、mbedTLS 成功判定、dry-run 调查、文档对齐 | 头文件/编译旗/链接旗/工具切换各有有意义的契约测试；故障不写成功 receipt；AArch64 contract 能在独立 checkout 验证 |
| 2 | 测试分层规范、场景矩阵与重叠治理、hosttests Makefile 职责拆分、CI 去重、阶段统计 | 每项需求有主验收位置；重叠场景有明确处理理由且独有覆盖不丢失；新增测试只修改所属模块；拆分前后测试集合与行为一致；每轮所需审计只执行一次；未调度 E2E 有显式归属 |
| 3 | launcher + ccache、小范围 staging/用户程序失效优化 | 冷缓存结果与无缓存一致；热缓存有命中数据；未消费的组件变化不重编；库变化能触发对应重链接 |
| 4 | 显式 registry、其余大型 Make 文件拆分、测试规范的注册检查 | 现有根入口、capability、variant、退出码和归档契约保持；新增测试只需一处登记并关联场景归属 |
| 5 | 宿主/审计/QEMU 有界并行 | 每任务独立状态、失败继续汇总、无遗留进程；镜像和固件基线保持；性能 suite 单独调度 |

每一批都应采集：cold build、warm no-op、单 `.c` 修改、私有头修改、公共 UAPI 修改、archive-only 修改、旗标/工具切换、输入删除、variant 切换。破坏性场景只在临时 checkout/profile 运行，结构变化继续遵守 `make clean` 要求。

性能目标应在基线后确定；本次不声称可实现某个提速百分比。优先验收“结果正确、失效原因可解释、没有重复执行”，随后比较总耗时的中位数与 P95。

## 6. 本次验证

- 阅读构建组件、host runner、结果协议、CI workflow 和现有构建文档。
- 统计已登记测试、depfile 和历史结果；执行源码摘要与 Git 元数据的小测。
- 执行 `make test-harness`：负向 C 框架契约通过；完整回归运行超过 8 分钟、末段连续约两分钟无新输出后人工中断，执行会话退出码 130。没有 unittest 最终汇总，不标为 PASS 或断言存在某个失败用例。日志：`/tmp/os01-research-harness.log`。定位当前用例及运行停留原因留待专项排查。
- 未运行完整 host/static/QEMU/contract，因此不对这些测试的当前通过状态作结论。
