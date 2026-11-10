# libc spec / plan 文档评审记录

日期：2026-11-08。用户要求：独立子 agent 评审 spec 和 plan，由主 agent 修订，重复直到明确 APPROVE。本记录只描述文档评审，不是产品实现或测试通过报告。

## 评审对象

- [Spec](../specs/2026-11-08-libc-completion-design.md)
- [Plan](../plans/2026-11-08-libc-completion.md)
- 范围：原 libc 扫描建议；按用户后续指示排除 glob/globfree 和仅为其增加的 fnmatch 改造。
- Reviewer：独立子 agent `libc_doc_review`；主 agent 核对代码依据后修改文档，reviewer 不编辑文档。

## 第一轮：R1 → REQUEST_CHANGES

| 必须修订 | 代码依据 | R2 修订 |
|---|---|---|
| a/x/e 模式承诺缺少内核支撑 | SYS_open 无 O_EXCL 检查，F_GETFD/F_SETFD 为占位，fd_write 不实现 O_APPEND | a/a+ 保留；e/x 明确 EINVAL；新增独立 T6 真实 append 回调和锁/短写/目标验收契约 |
| fcntl/lseek 包装错误未入范围 | fcntl 无条件 va_arg 且裸负 errno；lseek 裸负 errno | T7 修真实 fcntl 并直接测试；T9 修真实 lseek 并直接测试，不只通过流的 mock |
| shutdown 和最终目标测试未实际接线 | network suite 启动 nettest 而非 socktest；可选 probe 没有固定运行入口 | T4 接 nettest + NetworkServices 10003 EOF 后响应；T17 固定 systest required case，桌面走 gui-desktop |
| regex RED 顺序矛盾 | T1 先删空壳后 T15 再要求测试原空壳行为 | T1 保留 regex 实现和记录单 TU 基线，顺延后的 T16 改头前验证 RED 并迁移类型/定义 |

同时纳入非阻塞建议：南半球跨年 DST、mktime 成功返回 -1 的 errno 区分、ungetc 后 SEEK_CUR 逻辑位置、PATH_MAX/SSIZE_MAX 的任务归属和一致性检查。

## 第二轮：R2 → REQUEST_CHANGES

Reviewer 确认第一轮四个问题已解决，提出两项必须修订：

1. 对齐分配的 libk 条件性契约仍不确定。R3 明确 posix_memalign/aligned_alloc 只在用户 libc 定义，libk 不添加 stub，用户与 kernel API 清单分开；共享 calloc/reallocarray 溢出检查不依赖 libk 未定义的 errno。
2. append 虽串行写文件，锁外更新 file_t offset 仍会被共享 fd 的迟返回者覆盖。R3 把回调改为接收 struct file *，EOF 选择、字节提交和共享位置更新在同一 mount mutex 内完成；禁止 fd_write 返回后再次写位置，增加 dup/fork 交错和最终 offset 验证。

同时修复依赖图：regex 依赖浮点任务创建的第三方来源清单；EOPNOTSUPP=95 复用既有定义。

## 第三轮：R3 → APPROVE

独立 reviewer 明确返回 **APPROVE（R3）**，必须修订项为零。确认 glob 排除、用户/libk 边界、append 共享位置提交、fcntl/lseek 修复、regex RED 顺序、第三方依赖和目标验收入口一致；第一、二轮全部必须修订项均已处理。

批准针对 spec 和 plan 的完整性与可执行性，不代表实现、构建或运行测试已经通过，也不替代用户的实施指令。

批准版本 SHA256：

- Spec：`be13cc2e7f03f920755aa00918377acb79d19338bb30e56ea17a513f27f3a2e5`
- Plan：`ca442c472f6a4a35b0f2cb221ea2e042bef4c70bac2c5bbe6e2b1648ef70f288`

## 主 agent 文档检查

R2/R3 已检查：17 个连续任务编号、任务引用无悬空、相对文档链接有效、glob 不在任务/测试选择器中、无尾部空白；R3 另检查 struct file * append 契约和第三方依赖引用。未运行产品构建、host 或 QEMU 测试；计划中的命令属于未来实施验收。
