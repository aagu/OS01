# aarch64 跨进程 inode 级 MAP_SHARED 实施计划

> **执行技能** 逐任务使用 superpowers:subagent-driven-development 或 superpowers:executing-plans。用复选框记录完成状态。

**目标** 让 aarch64 上独立 mmap 同一 inode 的进程共享页，并让 fork 继承的匿名共享对象支持 fork 后首次触页。

**架构** 架构中立的共享对象持有稀疏页表项及后端节点引用。aarch64 的 VMA 持有对象引用，resident 记录借用 PA。CPU 缺页和 uaccess 统一实例化入口，使用现有 managed-root 更新与 TLB 边界。

**技术栈** 内核 C、宿主测试框架、aarch64 QEMU 产品镜像及现有 Python runner。

**规格** [aarch64 跨进程 inode 级 MAP_SHARED](../specs/2026-11-22-inode-map-shared-design.md)

## 全局约束

- 页大小 4 KiB。文件键为 `(mount, ino)`，页键为对象内页索引。文件支持 ext2、tmpfs 和 vfat 常规文件。匿名调用创建独立对象，fork 继承对象。独立进程经 tmpfs 文件名会合。
- 映射侧内容在至少一个 VMA 引用存活时保留。磁盘回写、fd I/O 一致性、truncate 撤页、共享内存 ABI 和 x86_64 VM 接入为后续范围。
- 注册表锁内仅改索引和引用。候选分配、VFS、页表更新和释放均在锁外。所有叶子在所属 VMA 释放前完成撤页与架构同步。
- 优先沿用现有宿主用例与产品 runner。测试由仓库顶层命令运行。x86_64 syscall E2E 与内核自测分开。

## 评审关注点

- 两个挂载实例具有同一 inode 号，页必须隔离。任务 2 核对 key 与引用。
- 同身份 rename 可能误入目标 unlink，尤其是 vfat 大小写别名。任务 1 测后端状态，任务 4 测改名后重新会合。
- MAP_FIXED 参数或分裂准备失败可能丢旧叶。任务 3 核对原 PA、数据与引用。
- uaccess 部分准备完成后 I/O 或分配失败可能留下叶或影响对端。任务 3 核对回滚状态及缓存复用。
- 只读 fd 的 VMA 经分裂、fork、降权后可能绕过最大写权限。任务 3 核对整段原子拒绝，任务 4 走用户 ABI。

## 文件分工

| 文件 | 责任 |
| --- | --- |
| `kernel/fs/{ext2,tmpfs,fat}.c` | 同身份 rename 短路，tmpfs 稀疏读。 |
| `hosttests/cases/test_vfs_iops_fops.c` | 后端身份、链接数、数据与稀疏 EOF 回归。 |
| `kernel/include/memory/shared_backing.h`、`kernel/memory/shared_backing.c` | 对象资格、注册表、页填充及最终释放。头文件只公开不透明对象和函数。 |
| `kernel/include/memory/vma.h`、`kernel/include/arch/aarch64/user_vm_{policy,internal}.h` | VMA 对象与最大写权限字段，resident 所有权枚举，共享内部 VMA 引用函数。 |
| `kernel/arch/aarch64/memory/user_vm_{policy,fork}.c` | mmap、缺页/预拷贝、保护及撤页、fork 事务。 |
| `hosttests/{cases/test_user_vm.c,mk/memory.mk,mock/user_vm_runtime.c,mock/user_vm_runtime.h}` | 编译真实对象与 VM 代码，注入失败、模拟文件身份及共享 PA 验证。 |
| `user/test_map_shared.c`、`mk/components/run.mk`、`qemutests/{aarch64_user_runtime,test_aarch64_user_runtime}.py` | 产品 EL0 探针、镜像选择、结果契约及 runner 单测。 |
| `docs/{roadmap,arch}.md`、`docs/memory/cow-mmap.md`、`docs/build/build.md` | 验收后的范围、接口语义及可重复命令。 |

宿主 RED/GREEN 使用 `make PROFILE=x86_64-clang test-host`。复用已有 `test_vfs_iops_fops` 与 `test_user_vm` 测试二进制和元数据，因此无需新增目录项。若测试目录检查发现生成物变化，用 `make regen-test-catalog` 更新。

### 任务 1：固定后端身份及 tmpfs 稀疏读

**输入与产出** 沿用各后端的 read/rename 签名和 VFS 层入口。在后端锁内判定相同对象，使已有节点引用可用于注册表身份。

- [ ] 在 `hosttests/cases/test_vfs_iops_fops.c` 增加 `test_tmpfs_read_truncate_holes`。仅将空文件截断扩至 8193 字节，从 4095 读 4098 字节，断言完整计数与全零。从 8192 请求 2 字节返回 1，之后 EOF 返回 0。错开洞块的读覆盖循环分段。
- [ ] 增加 `test_rename_same_file_keeps_identity`，分别挂载三个真实后端的宿主 fixture。写入 2 字节，持有旧节点，ext2/tmpfs 执行同路径 rename，vfat 使用 8.3 大小写别名。断言成功、后续 lookup 得原 ino、nlink 仍为 1、无 orphan 且数据可读。释放全部旧节点再 lookup，确认文件内容仍在。复用 FAT 普通 rename 和 orphan 用例验证异槽路径。
- [ ] 跑 host 命令，确认新增问题由当前实现触发。若 tmpfs 同路径测试会触发进程级错误，先观察并记录更小的独立失败，再跑整套。
- [ ] 修改 `tmpfs_vfs_read`，在已有挂载锁内用 `tn->size - offset` 截断长度，未分配块用零填对应 chunk 后继续循环。修改 `ext2_vfs_rename`、`tmpfs_vfs_rename` 与 `fat_rename`，在锁内、任何删除和 orphan 写入之前，分别按相同 inode、相同内部节点、相同 LBA/槽内 offset 短路成功。
- [ ] 跑 host 命令，两个新用例和原有 VFS 用例均 PASS。提交该后端修复。

### 任务 2：建立架构中立的共享对象

**文件** `kernel/memory/shared_backing.c`、`kernel/include/memory/shared_backing.h`、`hosttests/cases/test_user_vm.c`、`hosttests/mk/memory.mk`、`hosttests/mock/user_vm_runtime.{c,h}`。

**接口** 输出参数仅在成功时写入。get 和 put 按一个对象引用记账，page 产出借用 PA，承诺调用者有对象引用期间 PA 有效。

```c
struct shared_backing;
struct vfs_node;
int shared_backing_open_file(struct vfs_node *node, struct shared_backing **out);
int shared_backing_create_anon(struct shared_backing **out);
void shared_backing_get(struct shared_backing *backing);
void shared_backing_put(struct shared_backing *backing);
int shared_backing_page(struct shared_backing *backing, uint64_t index, uint64_t *pa_out);
```

- [ ] 在 `user_vm_runtime.{c,h}` 提供真实布局的 mount/node fixture、逐节点数据和可注入的 `vfs_read`、node get/put 计数，保留旧私有文件测试的默认数据。再在 `test_user_vm.c` 加 `test_shared_backing_identity` 和 `test_shared_backing_fill_race`。前者用两个不同 VFS 节点共用 mount/ino 及同 index，断言对象指针与 PA 都相同；分别修改 mount、ino 或 index，断言不同页；两匿名对象的 index 0 分离。后者用两线程和后端 mock 屏障迫使同 index 的两个候选进入 read，允许一方先发布和修改，再放行另一方。断言两方 PA 相同、完整初始化且修改仍在，并核对失败者候选被释放、最后对象 put 回到页/节点基线。
- [ ] 加 `test_shared_backing_fill_retry`。注入对象元数据、页节点、PA 分配失败，预期 `-ENOMEM`。注入后端 `-ENOMEM` 与其它错误，分别预期 `-ENOMEM`、`-EFAULT`，再恢复读取成功。以长度 3 的数据检查尾部零填，并覆盖后端失败时二次查找取得已经发布的页。
- [ ] 让宿主链接真实 `shared_backing.c`。复用 `USER_VM_HOST_CFLAGS` 与实际 VFS 类型，在 `hosttests/mk/memory.mk` 为该用例编译及链接加 `-pthread`。参照 `hosttests/cases/test_usb_msc.c` 的宿主 pthread 声明启动和 join 两个线程，用有界原子阶段变量会合。测试专用 force-include 在 `test_platform.h` 的 `OS01_HOST_SPINLOCK_T_PROVIDED` 门前声明自己的锁类型和函数，实际用原子 acquire/release。保护并发计数及分配记录，在后端 mock 中断言分配/读/回收时当前线程未持注册表锁，测试失败时解除等待并收割已启动线程。以接口桩或尚无实现的链接失败记录 RED，随后删除桩。
- [ ] 实现文件键与对象内页索引。两层分别采用固定桶、冲突链及按需分配的页节点。静态 spinlock 使用 `pmm.c` 的解锁初始值；通过 irqsave/restore 守护查找、发布、引用与摘除。文件准入采用 VFS_FILE、已注册挂载类型名（ext2/tmpfs/vfat）、非零 `node->ino`，目录为 `-EISDIR`，其它不支持项为 `-EOPNOTSUPP`。文件对象持有一个节点引用；同键并发打开只有一个发布，销毁落选对象时归还其节点引用。匿名创建不插入文件表。
- [ ] 实现查页。命中直接返回。未命中时在锁外分配条目和 PA，并初始化整页；有后端时先拒绝无法无损换算为字节的 index，再用 `vfs_read(node, index << PAGE_4K_SHIFT, PAGE_4K_SIZE, Phy_To_Virt(pa))`。再查并发布完整候选。错误时仍做二次查找，已有页优先。释放失败或落选候选。最后 put 在锁内摘除对象，锁外释放页、元数据并 put 节点。
- [ ] 跑 host 命令，确认并发、失败和回收用例 PASS。用 `make PROFILE=aarch64-clang aarch64-uefi` 验证共享 C 文件进入产品内核构建。提交对象实现。

### 任务 3：一次接通 aarch64 映射、fork 与拆除

**接口** VMA 新增 `struct shared_backing *vm_shared_backing`，使用新标志 `VMA_SHARED_MAYWRITE`（`0x200`）记录文件共享最大写权限。resident 新增 `USER_VM_PAGE_BACKING` 所有权，与原有 PRIVATE/COW 及 `USER_VM_PAGE_IO` 来源区分。aarch64 公共调用签名沿用当前接口。内部使用 `void aarch64_user_vm_vma_get_refs(vma_t *v)`、`void aarch64_user_vm_vma_put_refs(vma_t *v)` 和 `static int instantiate_absent(mm_t *mm, uint64_t va, uint32_t map_vm, user_vm_page_t **out)`。

- [ ] 扩充 `test_user_vm.c` 和 `user_vm_runtime.{c,h}`。基于任务 2 的 fixture 给 mock 文件加 flags 及 fd get/put 计数。
  - `test_inode_shared_fault_and_uaccess`：两个 mm、不同节点和 VA、相同 inode/index。断言 CPU fault 与 `user_vm_prepare_copy` 同 PA 且 PMM COW 为 0。私有文件 uaccess 首读得到后端已知数据。跨两页 copy 注入第二页读或叶安装失败，断言第一暂存叶也撤回，其他映射 PA 和数据维持，重试复用缓存。
  - 更新原有 `test_mmap_shared_anon_fork_write_through` 的共享段。移除旧写故障升级断言，改为父子直接 RW/NX、同 PA、`USER_VM_PAGE_BACKING`、PMM COW 引用为 0。保留该用例后半段的私有 COW 测试。
  - `test_shared_anon_fork_backing`：fork 前第 0 页为 RW、第 1 页 stash、第 2 页未触碰。断言父 RW 叶未降权，子继承 stash，父子第 2 页首次触页同 PA。恢复 stash 看见共享内容。让 publish 返回 `-ENOMEM`，断言无子叶和子 VMA、父权限与 PA 未动、计数与基线相符。
  - `test_shared_vma_retire`：部分 munmap 与 split 校验右段页偏移；成功同对象 MAP_FIXED 保留内容。注入 VMA/分裂准备失败及负、非对齐、末字节越界 offset，断言旧 VA/PA/数据保留。逐一销毁 mm（直接驱动实际 `user_vm_destroy`）并观察 present/stashed 页先撤后释放，末引用后页/节点/临时 fd 回基线。
  - `test_shared_file_protect_ceiling`：O_WRONLY、O_RDONLY+RW 均为 `-EACCES`。相邻两 VMA 一段 O_RDWR、一段 O_RDONLY，跨段扩写失败且前段未被升级/分裂。分裂只读 VMA 后 fork，在子进程尝试扩写两个片段并逐一断言 `-EACCES`。覆盖 O_RDWR 降权、分裂、fork、关闭 fd 后重新扩写的正例。
- [ ] 先在头文件声明本任务的元数据和枚举，保留旧运行路径。跑 host 命令，确认旧实现的独立映射、不驻留 fork 和 uaccess 行为使新增断言失败。
- [ ] 接入 `user_vm_mmap`。一次取 fd，并通过同一出口归还；用其判定 `/dev/mem` 或普通映射。普通文件共享模式检查 low-two-bit access mode、长度及 `(int64_t)off >= 0`、4 KiB 对齐、`alen - 1 <= INT64_MAX - off` 后创建对象；VMA 转移对象与节点引用。匿名共享分配新对象。均在 MAP_FIXED 撤旧叶前取得新引用。分裂、fork 元数据拷贝及所有 VMA 撤销路径复用引用函数。heap、设备与其它不适用的 aarch64 VMA 构造明确初始化该指针为空。mprotect 在分配 split 或修改前扫描整段的最大写权限。
- [ ] 用 `instantiate_absent` 替代 CPU fault 和 copy pass 3 内各自的填页路径。用 VMA 和 `vm_pgoff + ((va - vm_start) >> PAGE_4K_SHIFT)` 取共享 PA，或分配私有 PA、零填并按原 VFS 规则填文件数据。准备完后才安装叶，record 暂由调用者持有。VMA 借用页直接安装对应逻辑权限。首次接线若实际为可执行叶，沿用原 CPU fault 的 `aarch64_elf_clean_code_page` / `aarch64_elf_sync_icache`，统一入口覆盖 CPU fault 与 uaccess，继续执行现有 PROT 准入。回滚和正常撤叶完成后，只有私有 PA 或最后一个 PMM COW 引用可以触发 record 释放 PA。对象页由 VMA 引用约束。保留 copy prepare 既有全范围校验、私有 COW 和 staged rollback 的边界。
- [ ] 扩展 `user_vm_clone_fork`。共享 backing 页使用已有子 VMA 对象引用，借用同 PA，按原 logical 权限创建叶或还原 stash；跳过 PMM COW acquire/release、父权限降级和私有 COW 验证。失败沿用先撤子叶、后撤子 VMA 的顺序。更新 resident 与权限文档，清理只为旧 MAP_SHARED 借用 PMM COW 而存在的写故障分支。物理 I/O 仍由原有路径负责。exec 和 exit 经 `aarch64_user_image_destroy` 在 IRQ 启用时回收已脱离的旧根，继续使用此路径完成最后对象释放。
- [ ] 跑 host，通过新增及既有 VM、私有 COW、设备用例。另用下列 M4 自测命令启动单核镜像。在绝对时限内验收 `[TEST] PASS aarch64_user_vm`、`aarch64_user_fork_cow`、`aarch64_user_svc_uaccess`、`aarch64_user_exec`、`aarch64_user_waitpid` 及最终零失败的 END，取得证据后结束 QEMU。提交完整 VM 接入。

```sh
make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_EL0_TEST=1 SMP=1 DISPLAY=none run-aarch64-uefi
```

### 任务 4：增加产品 SMP E2E 并闭合文档

**接口** `/bin/test_map_shared` 无参数运行协调者。exec 子角色通过同一 ELF 的 argv 接收角色、路径和 pipe 端点。探针唯一成功行是 `test_map_shared: PASS`，错误输出 `test_map_shared: FAIL <stage>` 并退出非零。runner 入口为 `make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=map-shared`。

- [ ] 新增 `user/test_map_shared.c`。协调者建立两根 pipe，fork + exec 两个角色，映射用文件由各角色分别 open。先 fd 初始化 ext2 文件（例如根目录临时文件），tmpfs 文件只通过 ftruncate 扩至三页。用不同页对齐 VA、不同页偏移及固定字节值检验双向可见性与偏移隔离。一个角色先通过管道 `read` 到未驻留共享页完成实际 uaccess 写，另一个角色随后从它自己的映射读到字节；同时测试用户 store 方向。父在全部子进程 `waitpid` 正常退出、完成其余断言及清理临时文件后打印唯一 PASS。按阶段检查 pipe 实际读写字节数并关闭未用端。
- [ ] 加生命周期子用例。映射 A 后改名，另一角色从新路径打开 B 会合；A 关闭 fd 且 unlink 后仍首次读到旧 inode 未缓存页；同路径重建读到独立数据。持有共享对象的 fd 在后端修改一张已填和一张未填的页，断言前者保留映射修改、后者读到后端新值；解除最后映射后重开，重新填页读到后端数据。验证 O_WRONLY 拒绝、O_RDONLY 不能扩写、O_RDWR 降权恢复。fork 前未触碰匿名共享页，子先写后用 pipe 通知父读，两次独立匿名 mmap 保持隔离。vfat 使用 `/boot` 临时 8.3 文件，A 以 RW 映射先写标记，大小写 rename 成功后才以新拼写独立 open B 并在不同 VA 建 RO 映射，检查同一标记；另做普通 rename 到不同 8.3 名称并重开验证会合。独立验证两条正式清理入口。让一个角色持有唯一已修改的共享 VMA 直接 exec，新映像重映射应读到后端原字节。让另一个角色持有唯一已修改 VMA 直接 exit，父在 waitpid 后重映射，应读到另一份夹具的后端原字节。
- [ ] 在 `qemutests/test_aarch64_user_runtime.py` 加 RED。给解析器提供一条精确独立 PASS 的正例，以及 PASS 缺失、重复、仅命令回显、FAIL 混入、超时或 QEMU 自退的负例。验证选择该 case 时只运行 2 核产品路径，普通内核 ELF 做现有测试符号排查，并保留 shell 退出与 respawn 检查。断言该 case 使用本轮私有可写盘，原盘路径从未作为可写 drive 传给 QEMU，原有 case 的 drive 参数仍为只读。用 `python3 -m unittest qemutests.test_aarch64_user_runtime` 运行。
- [ ] 在 `mk/components/run.mk` 将 map-shared 加入合法值与产品 image/ELF 分支；使用 `USER_PROGRAMS` 的自动发现与 `AARCH64_ROOTFS_PROGRAMS` 的现有安装路径。扩展 `qemutests/aarch64_user_runtime.py` 的 CLI、产品 case 分派、运行 `/bin/test_map_shared` 和证据校验。仅为 map-shared 在本轮独有临时目录用 `shutil.copyfile` 复制 raw 产品镜像（参照 `qemutests/usb_hid.py` 的私有磁盘做法），以可写 drive 启动副本，并在 QEMU 停止后清理副本。让 `qemu_command` 的可写参数默认关闭，pflash 继续只读。复制失败按 runner 失败归档。两个待写文件系统共享这块 virtio 盘。复用既有绝对期限、排空窗口与归档。若探针使用仅 aarch64 的接口，须按现有 profile 排除模式隔离，否则保证两个 profile 均可编译。
- [ ] 跑 harness 单测及下列验收。核对新 case 的 2 核日志含独立 PASS、正常 respawn 与无 fatal；产品基线和迁移回归成功。按规格在 `docs/roadmap.md` 记录完成边界，更新 `docs/arch.md`、`docs/memory/cow-mmap.md`、`docs/build/build.md` 的行为与命令。附运行证据并提交。

```sh
make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=map-shared
make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=product
make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=smp
make PROFILE=aarch64-clang test-aarch64 MODE=user-runtime USER_RUNTIME_CASE=migration
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang test-harness
make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
```
