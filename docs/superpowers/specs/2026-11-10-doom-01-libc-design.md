# D01 — Doom 前置 libc 补充

日期：2026-11-10。状态：文档评审通过；仅设计，不表示已实施。

[总览与公共约束](2026-11-10-doom-port-design.md) · [配套实施计划](../plans/2026-11-10-doom-01-libc.md)

## 目标、依赖与交付边界

独立补齐 Doom 所需的 libc 标准接口和头文件边界。无 Doom 源码、WAD 或 GUI 依赖，可单独合入；不实现 `system()`、`fscanf()` 或完整 libm。当前 master 已有 fseek/ftell 与 scanf %i/%o，不重复实现。

## 接口与行为

### 标准头与宏边界

- 新增 `libc/include/strings.h`，提供现有 `strcasecmp/strncasecmp` 的一致声明，类型依赖显式包含；不借此宣称完整 BSD strings API。
- 在 `stdio.h` 增补 `rename` 和 `remove`，在 `sys/stat.h` 增补 `mkdir`；与 `unistd.h` 的既有声明保持类型一致，首版保留旧位置以兼容消费者。
- 把 `ZEROPAD/SIGN/PLUS/SPACE/LEFT/SPECIAL/SMALL`、`do_div`、`is_digit` 移至 stdio 私有头；`vsprintf.c`、`floatconv.c` 等需要的实现显式包含。检查所有 libc/libk 消费者；两个架构的 `do_div` 路径必须保留。
- 不在 Doom 编译中强制包含“缺声明汇总头”，不以关闭 implicit declaration 警告作为解决方案。

### 数值与文件接口

`stdlib.h` 增补真实的 `abs/labs/llabs`。最小负数的标准未定义情形不新增饱和语义，其余值必须正确；实现不得递归调用自身。

在现有 `math.h` 增加 `double fabs(double)` 的可用实现，清除 binary64 符号位，覆盖负零、无穷、NaN；可采用头内实现，避免新建 `libm` 与修改全局链接约定。若选择外部符号，应置于已有 libc 源发现范围并验证 `-fno-builtin` 链接，不能只补声明。不要覆盖现有浮点宏。

`remove(path)` 属于用户态 libc：对目录调用 `rmdir`，对其他路径调用 `unlink`，成功返回 0，失败返回 -1 并保留对应 errno。实现可用 `lstat` 分类，符号链接必须删除链接本身，不能删除其目标；禁止对任意 `unlink` 失败无条件回退后覆盖原错误。覆盖普通文件、空/非空目录、缺失路径和指向目录的 symlink。`__is_libk` 构建不得引入用户 syscall，按现有用户态专用函数惯例隔离。

本次不实现通用 `system()`。`fseek/ftell`、scanf、环境和 `atexit` 依赖当前 master 的真实实现；增加的是针对 Doom 实际用法的回归覆盖，而非第二套实现。

## 文件与验收

公开头在 `libc/include/`；格式宏进入既有 `libc/include/stdio/internal.h`，更新 `libc/stdio/vsprintf.c`、`floatconv.c` 等实际消费者。数值实现放 `libc/stdlib/abs.c`，remove 放 `libc/stdio/remove.c`；fabs 采用现有 math.h 内静态内联位操作实现，不引入 libm archive。

扩展 `scripts/audit_libc_api.py` 的既有接口分类，声明与导出审计遵循现有组；fabs 为头内实现，不能误要求外部导出。宿主测试链接/重命名生产符号，不能意外使用宿主 abs/remove。

完成条件：独立标准头编译无污染、stdio 格式化无退化；abs/labs/llabs 普通值与 fabs 特殊值正确；remove 普通文件、空/非空目录、symlink、缺失路径和 errno 正确；x86 libc/libk 及 aarch64 libk 检查通过。最小负整数不加入标准未定义行为断言。

验证入口使用现有 `test-libc-host LIBC_CASE=api|format|integer|float|path`、`test-libc-api-audit` 与 `test-static`。按任务运行所属组，最终全量验收一次；不依赖尚未交付的 Doom E2E。
