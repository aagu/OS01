# x86_64 exec PT_INTERP 通路(T1)实施计划

设计:2026-11-27-exec-pt-interp-x86-design.md;分支 exec-pt-interp-x86(worktree)。

## 步骤

1. **形状探针(先于内核改动)**:手写两条 `TARGET_LD` 链接(仿 `user/Makefile` ptls fixture 模式):
   - `ld.so`: `-static -pie --no-dynamic-linker`,自带 `_start`;
   - `dyn_exec`: `-static -pie --dynamic-linker /lib/ld.so`,自带 `_start`,全局部符号。
   readelf 检查:PT_INTERP 字符串、PT_DYNAMIC tags、DT_FLAGS_1 是否带 DF_1_PIE、重定位类型集合。产物形状决定第 2 步放行细节。
2. **内核**:
   - `elf_image.h`:`x86_elf_image_t` 增 `interp_base`(0=无)。
   - `elf_pie.h/.c`:`x86_elf_pie_prepare` 增加 allow-interp 参数(或独立入口),放行并记录 PT_INTERP;其余校验不变;按第 1 步证据裁决 DF_1_PIE 约束。
   - `elf.c`:现主体抽为 `elf_load_image_at(node, mm, out, bias)`(bias 由调用者定);`elf_load` = 采样 bias + 调核;新增 interp 分支:读路径 → `vfs_lookup` → 打开 → 验证(拒 PT_TLS/二级 PT_INTERP)→ 采样+冲突重试(≤16,冲突判据用 PTE 占用错误码)→ 装载 → `out->interp_base`、入口改 interp。
   - `exec.c`:`setup_user_stack` 的 AT_BASE 取 `image->interp_base`;容量常量不变(仍 9 对)。
   - 日志走 `debug_fs`;失败路径与现有回滚所有权一致(interp 叶子独立回滚数组,主镜像页不动——调用者失败即整 mm 销毁)。
3. **用户态夹具**(镜像内 `/lib/ld.so`、`/bin/dyn_exec`,仿 ptls fixture 的构建/发布/gating 模式):
   - `user/systest/fixtures/ldso/`(start.S + main.c)、`user/systest/fixtures/dyn_exec.c`;
   - `user/Makefile` 两条链接规则 + artifact 发布;镜像安装步骤同步(systest 构建档)。
4. **systest 用例**:fork + exec `/bin/dyn_exec` + waitpid,断言 rc==0;两 marker 经串口由 harness 断言顺序(遵循 x86 systest 现有 case 注册模式)。
5. **宿主契约测试**:`hosttests/cases/test_dynlink_shape.c`(`@os01-test` 元数据,C 手解析 ELF,不依赖 readelf):断言第 1 步钉住的形状不变量;`make regen-test-catalog` 随改随生成。
6. **门禁**:`make lib` → `make test-host` → `make test-static` → `make OS01_SYSTEST=1 <systest 套件>`(不与 KERNEL_SELFTEST=1 同跑)→ 现有 x86 phase 冒烟回归。
7. **闭环**:roadmap P5 动态链接器行更新 T1 状态、changelog 记录;ff 合并前按 AGENTS.md 用 `git merge-base --is-ancestor` 核实。

## 风险与裁决点

- lld `--dynamic-linker` + `-static -pie` 产物若含非白名单 DT tag(实测后裁决:调白名单还是调链接 flag,优先调 flag)。
- PTE 占用冲突的现有错误码语义(裁决重试判据)。
- systest 夹具安装路径的既有约定(ptls_exec 如何进镜像,照抄)。
- heap 与 interp 的窗口关系:仅记录限制,不改 brk。
