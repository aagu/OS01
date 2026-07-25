# 构建系统评审 (Group 10)

**范围**: 根 Makefile, `kernel/Makefile`, `kernel/arch/x86_64/{make.config,linker.ld,trampoline.ld}`, `boot/uefi/Makefile`, `tools/mkdisk.c`, `tools/Makefile`

---

## P0: 无

---

## P1: 并发安全性

### P1-BUILD-1: `tools/mkdisk.c` 临时文件并发冲突 (critical)

**文件**: `tools/mkdisk.c:193,204`

**问题**: 硬编码临时文件路径 `/tmp/_mkdisk_esp.img` 和 `/tmp/_mkdisk_rootfs.img`。并行执行 `make` 的两个实例（或同一构建中重入）会互相覆盖临时文件，导致损坏的 disk.img 或静默失败。

```c
run_cmd("dd if=/dev/zero of=/tmp/_mkdisk_esp.img bs=1M count=%d 2>/dev/null", FAT32_SIZE_MB);
run_cmd("mkfs.vfat -F 32 /tmp/_mkdisk_esp.img 2>/dev/null");
// ...
run_cmd("dd if=/dev/zero of=/tmp/_mkdisk_rootfs.img bs=1M count=%d 2>/dev/null", EXT2_SIZE_MB);
```

**建议**: 使用 `mkstemp()` 或包含 PID 的路径（如 `/tmp/_mkdisk_esp.$$.img`）。

---

### P1-BUILD-2: `tools/mkdisk.c` 无文件操作错误检查

**文件**: `tools/mkdisk.c:157,174,179,182,188`

**问题**: `fwrite`、`fread`、`fseek` 的返回值未被检查。如果磁盘空间不足或写入失败，工具静默产生损坏镜像。例如：

```c
fwrite(mbr, SECTOR_SIZE, 1, f);  // 未检查返回值 == 1
fwrite(gpt_hdr, 92, 1, f);       // 同上
```

`run_cmd()` 在 `system()` 返回非零时仅发出警告（`WARNING: returned %d`），不中止构建：

```c
int ret = system(buf);
if (ret != 0) {
    fprintf(stderr, "  [cmd] WARNING: returned %d\n", ret);
}
```

`mkfs.vfat` 或 `mke2fs` 失败时，构建继续，产生不可引导的镜像。

**建议**: 对 `fwrite`/`fread` 添加错误检查并 `exit(1)`。`run_cmd()` 非零返回时中止或 `exit(1)`。

---

### P1-BUILD-3: `boot/uefi/Makefile` 通过 `include` 引入子模块构建系统，但子模块可能未初始化

**文件**: `boot/uefi/Makefile:13`

**问题**: `include uefi/Makefile` 引用 `boot/uefi/uefi/Makefile`（posix-uefi 子模块）。若子模块未初始化（`git submodule update --init`），此 `include` 导致 Make 报错。根 Makefile 无此依赖的防护检查（与 BusyBox 不同，后者在 line 75-77 有显式检查）。

**建议**: 在 `boot/uefi/Makefile` 中添加存在性检查，或由根 Makefile 在构建 BOOTX64.EFI 前验证子模块。

---

## P2: 正确性与健壮性

### P2-BUILD-4: `kernel/Makefile` `.d` 依赖文件未跟踪 ASM 源文件

**文件**: `kernel/Makefile:183`

**问题**: `ALL_DEPS` 仅包含 `C_OBJECTS` 对应的 `.d` 文件：

```makefile
ALL_DEPS := $(patsubst %.o,%.d,$(C_OBJECTS))
-include $(ALL_DEPS)
```

`ASM_OBJECTS` 对应的 `.d` 文件未被包含。汇编文件的 `.include` 指令变更不触发重编译。

**建议**: 添加 `ASM_DEPS := $(patsubst %.o,%.d,$(ASM_OBJECTS))` 并一起 `-include`。

---

### P2-BUILD-5: `kernel/Makefile` 构建产物临时复制到源树

**文件**: `kernel/Makefile:137-139`

**问题**: trampoline 构建流程将 `trampoline.bin` 从 BUILD_DIR 复制到源目录 `$(ARCHDIR)/trampoline.bin`，再通过 `-b binary` 链接，然后删除。若构建在此区间中断，源目录遗留生成文件，污染 `git status`。

```makefile
$(BUILD_DIR)/$(ARCHDIR)/trampoline_bin.o: $(BUILD_DIR)/$(ARCHDIR)/trampoline.bin
	cp $< $(ARCHDIR)/trampoline.bin        # ← 源污染
	$(LD) -r -b binary $(ARCHDIR)/trampoline.bin -o $@
	rm -f $(ARCHDIR)/trampoline.bin        # ← 异常中断时未执行
```

**建议**: 直接从 `$(BUILD_DIR)` 路径使用 `-b binary` 链接，避免复制到源树。

---

### P2-BUILD-6: `tools/mkdisk.c` 备份 GPT 写回过程中使用已修改的 header 缓冲区

**文件**: `tools/mkdisk.c:227-243`

**问题**: Phase 4 写备份 GPT 时复用 phase 2 的 `gpt_hdr` 缓冲区，原地修改 `my_lba`、`alternate_lba`、`partition_entry_lba` 后重新计算 CRC。此操作假设 GPT header 布局与之前完全相同。若将来修改 header 生成逻辑，备份写入可能产生不一致的 GPT。

**建议**: 为备份 GPT 使用独立的缓冲区或结构体，减少耦合。

---

### P2-BUILD-7: `tools/mkdisk.c` 字符设备路径未转义

**文件**: `tools/mkdisk.c:213-221`

**问题**: 根文件系统文件注入通过 shell 循环执行：

```c
snprintf(glob_cmd, sizeof(glob_cmd),
         "for f in %s/bin/*; do ... done", rootfs_dir);
system(glob_cmd);
```

若 `rootfs_dir` 或文件名包含 shell 元字符（空格、`;`、`` ` ``），将产生意外的命令执行。开发工具风险较低，但持续集成环境中应修复。

**建议**: 使用 `execvp` 调用 `debugfs` 逐文件写入，或使用 `scandir()` + 参数化调用。

---

### P2-BUILD-8: 根 Makefile `kernel.bin` 与 `kernel/kernel.bin` 两个目标功能重复

**文件**: `Makefile:50-56`

**问题**: 两个目标均执行 `make -C kernel kernel.bin`。仅 `kernel/kernel.bin` 标记 `.PHONY`；`kernel.bin`（无前缀）依赖文件存在性和时间戳。二者功能相同但实现方式不同，可能产生混淆。

**建议**: 保留一个 `.PHONY` 目标即可。

---

### P2-BUILD-9: `kernel/Makefile` kallsyms stage1 .elf 在最终链接前删除

**文件**: `kernel/Makefile:157`

**问题**: `rm -f $(BUILD_DIR)/kernel.elf.stage1` 在 stage1 生成 kallsyms 后立即删除。若最终链接失败（例如 undefined symbol），用户需完全重建 stage1（包括 nm + kallsyms 生成 + 汇编），显著延长调试周期。

**建议**: 保留 stage1 .elf 或添加 `.PRECIOUS`。

---

## P3: 次要问题

### P3-BUILD-10: `OVMF.fd` 下载无校验和

**文件**: `boot/uefi/Makefile:19-20`

**问题**: `wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd` 无校验和验证。不同日期的 nightly 构建可能引入不兼容的 UEFI 行为。

**建议**: 添加已知良好版本的 SHA256 校验（可缓存），或由用户手动提供 OVMF。

---

### P3-BUILD-11: `tools/mkdisk.c` GUID 生成回退使用弱 PRNG

**文件**: `tools/mkdisk.c:92-94`

**问题**: `/dev/urandom` 不可用时回退到 LCG：

```c
unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
for (int i = 0; i < 16; i++) { seed = seed * 1103515245 + 12345; out[i] = (uint8_t)(seed >> 16); }
```

在容器或最小环境中不可用 `/dev/urandom` 时，生成的 GUID 可预测且同一秒内启动的两个进程可能冲突。不影响引导能力，但违反 GPT UUID 唯一性原则。

---

### P3-BUILD-12: `kernel/Makefile` 的 `irq.o` 专用规则无实际差异化

**文件**: `kernel/Makefile:141-144`

**问题**: 注释声称 `irq.o` 需要特殊处理以避免 `.bss section` 问题，但规则内容与通用 `.c → .o` 规则完全相同（仅指定 `ALL_CFLAGS` 和 `-MMD -MP`）。该规则已无用。

**建议**: 移除死规则。

---

## 摘要

| 严重级 | 数量 | 关键风险 |
|--------|------|----------|
| P0 | 0 | — |
| P1 | 3 | 并发临时文件冲突、文件操作无错误检查、子模块依赖防护缺失 |
| P2 | 6 | ASM 依赖未跟踪、源树污染、GPT 写回脆弱性、shell 注入风险、目标重复、stage1 过早删除 |
| P3 | 3 | OVMF 无校验、GUID 弱 PRNG、死规则 |

## 跨组关联

- **Group 1 (Boot)**: 构建系统必须在 linker.ld 中链接 `_start` 入口点。任何链接脚本变更需 `make clean`。
- **Group 2 (Memory)**: linker.ld 中 `.data.init_task` 的 32KB 对齐为内核初始任务栈提供保护页。
- **Group 3 (SMP)**: trampoline.ld 确保 AP 启动代码定位于物理地址 0x8000，为 INIT-SIPI-SIPI 协议所必需。
