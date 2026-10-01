# AArch64 Follow-ups Deferred — 2026-09-18

**Trigger**: GICv2 Phase 1 merge to master (`545a935` + fix `2481e1f`, merge `6be7735`) + Phase 0 build investigation in worktree `feat/aarch64-timer`.

## 2 deferred items

### ⏸️ F1 — aarch64-uefi build in fresh worktree (`feat/aarch64-timer`)

**Symptom** (`master @ 6be7735`, fresh worktree `feat/aarch64-timer`):

```sh
$ make PROFILE=aarch64-clang SMP=4 run-aarch64-uefi
ERROR: posix-uefi submodule not initialized
Run: git submodule update --init
make: *** [/home/aagu/aarch64-timer/mk/components/uefi.mk:101:
            /home/aagu/aarch64-timer/build/aarch64-clang/stamps/uefi-runtime.stamp] Error 1
```

**Root cause** (未深挖): 新 `git worktree add` 默认不初始化 submodules;`thirdpart/posix-uefi` 在新 worktree 里目录存在但文件未 checkout,build 路径 `uefi.mk:101` 的 stamp 触发时报错。`master` 主工作区验证 submodule 已就位(早前 `git clone + submodule update --init` 残留),所以 `master` 工作区构建不报错。

**Status**: Phase 0 搁置 (user 验证 master 正常,新 worktree 隔离开销不应回退)。

**Deferred 修复方案** (后续 worktree 起点必做):
```sh
git worktree add <path> -b <branch> master
(cd <path> && git submodule update --init --recursive)
```

或加进 `mk/components/uefi.mk:101` 的 stamp 之前加个自动检测 + warning: 如果 `thirdpart/posix-uefi/` 空目录或缺少关键文件,跑 `git submodule update --init` 自我修复。

**Why this is benign now**: Phase 1 (Timer 集成) Task 1.1 RED hosttest 走 host 编译,不依赖 posix-uefi,Worktree 干净可开工。Task 2.2 GREEN QEMU E2E 才会触及。届时进入 worktree 第一步必跑 `git submodule update --init --recursive`。

### ⏸️ F2 — Phase 0 clean 流程留下 `build/aarch64-clang/` 残留 (`feat/aarch64-timer`)

**Symptom**: 清完 build 后 `git status` 仍有 ` m thirdpart/posix-uefi` (submodule 工作树 modified content)。build 错误首因排除后此为次要现象,可能是 `make clean` 或 `make` 在 submodule 目录里留下的临时文件。

**Status**: 不影响 Phase 1 RED hosttest。Phase 2.2 GREEN QEMU E2E 之前必跑 `git submodule update --init --recursive` 把 submodule 工作树对齐,此问题会自然消解。

**Deferred**: 不深挖;进入 Timer 实现前用以下一次性清理:

```sh
cd ~/aarch64-timer
git submodule update --init --recursive
git status --short  # 应 clean
```

## Why these are deferred (not bugs to fix now)

1. **master 是绿的**: user 验证 `master` 分支 `make PROFILE=aarch64-clang SMP=4 run-aarch64-uefi` 正常,Phase 0 的"构建失败"信号在新 worktree 隔离环境里复现,且根因不是 Phase 1 merge 引入的代码 regression。
2. **Phase 1 不需要修**: Timer 集成的 Task 1.1 hosttest + Task 1.2 GREEN (driver 内联) 都可 host 编译,Task 2.2 QEMU E2E 之前会自然触发 submodule init 修复。
3. **修复成本低**: Task 2.2 前的 prep step 一行命令解决。

## Phase 0 验证结论 (2026-09-18)

- ❌ **未做**:深挖"make PROFILE=aarch64-clang SMP=4 run-aarch64-uefi 失败"的根因(已确认为 worktree submodule 未 init,非 GIC Phase 1 merge 引入)。
- ✅ **已做**:`make clean` + `rm -rf build/aarch64-clang/` + subagent 启动诊断 + 终止诊断(走 user 判断路径)。
- 📝 **记录**:本 follow-up doc 留档,后续 worktree 起点必看。

## 相关

- [[optimization-roadmap-v4]] §P2 aarch64 适配
- [[aslr-vs-aarch64-next-task-2026-09-17]] GIC 选型记录
- [[gic-phase1-timout-bug-2026-09-18]] Phase 1 收尾 fix
- [[post-unification-followups-2026-09-16]] 同模式 follow-up 模板