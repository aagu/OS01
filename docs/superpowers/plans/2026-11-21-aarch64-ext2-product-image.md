# aarch64 产品镜像迁移 ext2 根文件系统 — 实施计划

日期：2026-11-21 · 分支：`aarch64-ext2-product-image` · 状态：实施中

## 背景与意图

ext2 内核侧自 2026-11-18「VFS 门」起已在 aarch64 工作（selftest 变体镜像即 GPT 双分区，
`test_aarch64_vfs` 验收 ext2 标记读）。但 aarch64 **产品**变体镜像仍是无分区表 64 MiB
FAT32 superfloppy（`mk/components/image.mk` 的 `else` 分支，mtools 直接灌 FAT）——
根文件系统是 FAT32，ext2 不参与产品布局，且 FAT 无符号链接导致 `/bin/sh` 只能是
busybox 的整文件拷贝。

本任务把 aarch64 产品镜像迁到与 x86_64 相同的 mkdisk GPT 双分区布局：
ESP FAT32（BOOTAA64.EFI + kernel.elf）→ `/boot`，ext2 rootfs → `/` 承载产品用户态。

## 设计裁决

1. **不加 `rootfs` capability**：image.mk 头注明的契约（"No mbedTLS/GUI apps/rootfs
   manifest on this arch"）仍然成立。产品文件清单沿用现产品分支的 mcopy 集合，改为
   生成一张 aarch64 专用小 manifest（模式同 selftest 分支的
   `rootfs.selftest.manifest`），不引入 `config/rootfs.mk`（doom/mbedTLS/GUI 不进
   aarch64）。
2. **manifest 内容**（7 行，远低于 mkdisk 的 128 行上限）：
   - `/kernel.elf` 0644 —— mkdisk `--esp-kernel kernel.elf` 消费它放 ESP（loader 读
     `\kernel.elf`），同时照 x86 `/kernel.bin` 惯例落在 ext2 根
   - `/etc/inittab` 0644
   - `/bin/init` `/bin/busybox` `/bin/terminal` `/bin/test_fp` 0755
   - `/bin/sh=busybox` **软链** —— ext2 带来的新能力；argv[0] 分发语义与 x86
     `/bin/sh→busybox` 一致，terminal.elf 的 `/bin/sh` 回退路径不受影响
   - **不加** busybox applet 软链集（grep/sed/awk 等悬空软链是 roadmap 待决缺陷，
     不复制到 aarch64）
3. **内核零改动**：`fs_boot_mounts` 的 first-disk 路径（aarch64 UEFI loader 不置
   `BOOT_CONTEXT_HAS_BOOT_DEVICE`）→ `mount_partitioned_disk` → GPT 双分区挂载，
   与 selftest 变体同路径；`/boot` 挂载点由 VFS 发布逻辑创建（x86 先例）。
4. **不保留 superfloppy 兼容分支**：整盘 FAT32 回退挂载路径在 aarch64 失去产品级
   覆盖，如实在 changelog 记录覆盖面变化（FAT32 读覆盖经由 ESP→/boot 保留）。
5. **测试跟随设计，不做兼容妥协**（用户明示）：harness 断言若与新布局冲突则更新
   断言，不改设计迁就旧断言。

## 改动清单

- `mk/components/image.mk`：`ifdef AARCH64_UEFI_DISK` 块内，`$(AARCH64_UEFI_DISK)`
  规则的 else（产品）分支改为：生成 `rootfs.product.manifest`（变体感知目录）→
  `check-deps` → `mkdisk --esp-kernel kernel.elf`；同时更新块头注释。
- harness/文档：核对 `qemutests/aarch64_user_runtime.py` 的 product 证据解析是否
  依赖 FAT 布局；更新描述 superfloppy 的文档段落（docs/build/build.md、
  docs/arch.md 若有）。
- 闭环记录：`docs/changelog.md`、`docs/roadmap.md` 近期闭环索引。

## 验证

1. `make PROFILE=aarch64-clang aarch64-uefi` —— 构建产品镜像；宿主侧核对 GPT 头、
   双分区、ext2 magic、`/bin/sh` 为 symlink、`/kernel.elf` 在 ESP。
2. `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product`
   （2 核 shell IO 冒烟）与 `USER_RUNTIME_CASE=fp`（`/bin/test_fp` 经 ash 运行）。
3. `make PROFILE=aarch64-clang test-aarch64 MODE=smp` —— selftest 变体（GPT+ext2）
   回归门，证明分支改动未波及既有 GPT 流。
4. `make test-host` —— 宿主回归（无 image.mk pin，防意外）。

## 明确不做

- 不给 aarch64-clang 加 `rootfs`/`mbedtls`/GUI capability
- 不动 `run.mk`（无 pin 影响；no-ack 是编译期选择器，非二进制注入，不受迁移影响）
- 不动内核 C 代码与 `config/rootfs.mk`
- 不回改历史 spec/plan
