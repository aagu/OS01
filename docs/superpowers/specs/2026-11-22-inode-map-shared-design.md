# aarch64 跨进程 inode 级 `MAP_SHARED` 规格

对应 [路线图 P2](../../roadmap.md#-p2-aarch64-适配)。2026-11-19 的实现通过 PMM COW 引用共享 fork 时已驻留的页。独立 open/mmap 同一文件仍会各自填页。匿名共享映射若在 fork 时尚未驻留，父子也会各自分配。内核 uaccess 预实例化目前另走匿名零页路径。

## 用户可见契约

| 映射 | 共享身份与可见性 |
| --- | --- |
| aarch64 ext2、tmpfs 或 vfat 常规文件 + `MAP_SHARED` | 任何地址空间独立 open/mmap 同一挂载实例的同一 inode，同一文件页偏移共用一个 4 KiB PA。VA、fd、VFS node 地址、路径和映射先后顺序均可不同。应用使用跨进程同步后，用户 store 和 syscall 的 uaccess 写入都由其它映射读到。 |
| `MAP_SHARED \| MAP_ANONYMOUS` | 每次 mmap 创建一个独立的零填充对象。fork 继承对象和页偏移，父子任意一方在 fork 后首次触页也会会合。两次独立 mmap 产生不同对象，即使长度与 VA 相同。 |
| 独立进程共享内存 | 两方通过现有 tmpfs `/tmp` 路径打开同一文件，创建方先设置长度，随后各自使用文件 `MAP_SHARED`。这提供命名的内存对象，并沿用 mmap 对 fd 访问模式及 VMA 权限的检查。匿名映射本身没有可供独立进程会合的名称或句柄。现有 open 路径的 uid/gid/mode 检查能力及系统调用 ABI 维持原样。 |

`close`、rename、unlink 和路径重建均按 inode 身份处理。已有映射在 fd 关闭或目录项移除后继续有效。重建同名文件后映射新 inode 得到独立对象。相同 inode 号在不同挂载实例中彼此隔离。对象存活期间，后来建立的映射也看得到该对象已有页中的修改。最后一个 VMA 片段消失后销毁对象及页。此后重新映射文件会从后端重新填充。

文件对象采用映射侧内存视图。页首次填充时从 VFS 读取对应页，short read 的剩余部分及 EOF 外空页零填充，延续 aarch64 现有文件缺页语义。映射写入的可见性限于同一活跃对象的共享映射。fd read/write、truncate 和 sync 继续直接操作后端，已经缓存的页保持原内容和可访问性，随后首次填充的页观察届时的后端。磁盘回写、映射与 fd I/O 一致性、truncate 驱动的撤页和新增共享内存 ABI 另行设计。tmpfs 必须把文件大小以内的未分配块读成零，直接支持仅经 ftruncate 扩展的稀疏文件。x86_64 的 VM 接入另行设计。

普通文件 `MAP_SHARED` 需要 FD_VFS、可读 fd、VFS_FILE、有效挂载及非零 inode 标识。目录返回 `-EISDIR`，其它不具备资格的 VFS 节点或文件系统返回 `-EOPNOTSUPP`，无效 fd 遵循原有错误处理。现有 `/dev/mem` mmap 在普通文件资格检查前分流。共享写权限要求映射时持有 `O_RDWR`，并以 VMA 最大写权限记录，跨分裂、fork、关闭 fd 与降权保留。不可读 fd、以只读 fd 建立可写映射、或之后用 mprotect 扩写均返回 `-EACCES`。mprotect 扩写在验证整个范围时检查权限，失败时整个范围维持原状态。普通文件共享映射新增 offset 检查，在发布前拒绝负 `off_t`、非 4 KiB 对齐和最后一个映射字节超出 `INT64_MAX`，均返回 `-EINVAL`。匿名 fd/offset、普通映射窗口、长度、PROT 和 `MAP_FIXED` 准入沿用现有约定。私有映射的 COW 和 `/dev/mem` 的借用 PA 路径继续使用现有代码。

## 对象、首次填页与所有权

考虑过直接沿用 PMM COW 引用，它适合已知 PA 的 fork 私有页，但缺少 inode 和未驻留页的身份。完整 VFS page cache 能统一 fd I/O 与 mmap，需要脏页、回写、截断失效和反向映射。这里采用架构中立的共享对象及稀疏页注册表，aarch64 首先接入，架构层拥有 PTE、TTBR0 与 TLB 操作。

- 文件对象按 `(vfs_mount_t *, node->ino)` 查找并持有一个节点引用。ext2 使用磁盘 inode，tmpfs 使用被节点引用固定的内部 inode，vfat 常规文件使用在活跃节点期间稳定的 `fat_file_object` ID。三者均已有活跃节点与 orphan 生命周期。当前 VFS 挂载实例存活于系统运行期。三种后端均须在各自挂载锁内识别源与目标已指向同一对象的 rename，直接成功且保留目录项、链接数及活跃对象。ext2 比较 inode，tmpfs 比较内部节点，vfat 比较目录项的 LBA 与槽内偏移（包含大小写别名）。独立进程通过 rename 后的新路径打开相同 inode 时仍会取得原对象。匿名对象分配唯一对象，跳过文件对象索引。
- 每个活跃 VMA 片段持有一个对象引用。文件共享 VMA 继续持有原有 `vm_file` 引用供 VMA 元数据使用。对象拥有发布的 PA。每条 resident 记录借用它所属 VMA 对象持有的 PA，包括 PROT_NONE stash。页索引统一为 `vm_pgoff + ((page_va - vm_start) >> PAGE_4K_SHIFT)`，分裂右段继续推进偏移。
- 两级稀疏索引分别查找文件对象和对象中的页。注册表锁串行化查找、引用增减、插入及最后引用摘除。分配、填零、VFS read、页/对象释放与 `vfs_node_put` 均在锁外完成。调用者使用现有 managed-root 编辑边界，持对象引用进入查页过程，任何跨锁区间均有引用保护。共享表锁内不进入 VFS、分配器、页表或调度。VFS read 和最后一次 node put 可以等待挂载锁，执行时注册表锁已经释放、调用者仍持所需引用，涉及当前用户根的路径保持在现有脱离窗口内。恢复用户根前完成叶子修改或回滚。
- 首次触页先查表，未命中时在锁外准备候选页并填充，再在锁内二次查找并发布。每个键只发布一张完整初始化的页，竞争失败者丢弃候选页、使用获胜 PA，永远不覆盖已发布页。分配或读取失败时再查一次，已存在的获胜页优先，否则返回错误并允许重试。匿名首填为零。共享填页保留后端 `-ENOMEM`，其它负读错误映射为 `-EFAULT`，注册表分配失败返回 `-ENOMEM`。重试指对象仍存活时的新查页请求或可返回错误的 uaccess 调用。未恢复的 EL0 缺页沿用现有任务终止路径。页表安装失败只释放本次临时 resident 记录，已经发布的页可继续服务其它映射。
- 活跃对象保留所有曾发布的页，即使某页暂时没有 resident 借用。此策略让后来加入或重新映射的进程仍看到修改。开销随该活跃对象实际触碰的页数增长。最后引用摘除后在锁外回收全部页及节点引用，清理前后均不存在仍借用旧 PA 的叶子。

## VM 路径

- mmap 在发布前完成对象获取和其它可失败准备。`MAP_FIXED` 保持先验证和预分配、后撤旧映射的事务边界。新旧映射若引用同一对象，撤旧前已有新引用，避免丢失缓存内容。mmap 至多取得一次临时 fd 引用，设备探测和文件准入共用该引用，所有成功及失败出口均归还。失败路径同时释放未转移的节点和对象引用。
- CPU 缺页和 `user_vm_prepare_copy` 的 absent 分支调用同一个实例化入口。共享页按 VMA 权限直接接线。uaccess 对整个范围进行预校验、准备与回滚，失败只撤本次临时叶和 resident 记录，允许保留已发布缓存页。私有文件也从现有 VFS 路径填充，避免第一次读来自 uaccess 时落成零页。
- 区分对象借用页、PMM 私有/COW 页和 I/O 页。对象页按各 VMA 权限装叶，同一 PA 可同时有 RO 与 RW 别名。普通共享页不经 COW 写升级、PMM COW 计数或私有化。mprotect 对 resident 与 stash 保留原 PA，恢复后见到其它别名在此期间写入的内容。
- fork 先复制 VMA 并获取对象引用。已驻留共享页为子进程安装同一 PA，继承 PRESENT/STASHED 状态与逻辑权限。未驻留页仍由以后缺页获得。共享页不会触发父页降权。失败时先撤子叶再释放子 VMA 对象引用。私有页继续按现有 fork COW 事务处理。
- munmap、可替换的 `MAP_FIXED`、exec、exit、fork 回滚均先撤叶并完成架构要求的 TLB 同步，再释放借用所在的 VMA 对象引用。分裂按每个片段独立记账。未驻留、PRESENT、STASHED 都遵守同样的 VMA 生命周期。

## 验收

- 宿主测试固定同 inode 不同 node、不同 mount/ino/页偏移、匿名实例隔离、并发首次填页与回收、读取或分配失败后可重试。计数核验候选页、对象引用、节点引用和 PA 的最终清理。另测 tmpfs 仅 ftruncate 扩展到多页后的洞读与 EOF，以及三种后端已打开文件的同身份 rename，核对 inode、数据与链接数，vfat 加大小写别名。
- aarch64 VM 测试检查真正的同 PA、uaccess 首填及多页回滚、fork 前未触页及已有 RW/stash、mprotect 权限与分裂、部分 munmap、可替换的 `MAP_FIXED`、fork 发布失败及 exec/exit 清理。覆盖只读 fd 跨片段扩写原子失败、负值/非对齐/越界 offset 的 MAP_FIXED 无损失败。
- aarch64 产品 QEMU 新增 `MODE=user-runtime USER_RUNTIME_CASE=map-shared`，从 ash 启动 EL0 探针，运行 2 核配置。两个进程分别 exec，以独立 open/mmap 获取 ext2 与 tmpfs 共享对象，用继承的两根 pipe 传递阶段消息。校验不同 VA 的双向可见性、后来加入的别名、偏移隔离、rename 到新路径后重开、在 close/unlink 后首次读取尚未填充的旧页和同路径重建。另测仅 ftruncate 的 tmpfs 稀疏页、匿名晚触页 fork、独立匿名隔离、拒绝 O_WRONLY、只读 fd 扩写失败、O_RDWR 降权后恢复、后端更改只影响未填页及最后解除映射后重读后端。该端到端验证 SMP 配置上的产品路径，首次发布竞争由宿主屏障用例确定性覆盖。vfat 用产品 `/boot` 下的临时 8.3 文件验证两个独立 fd、不同 VA 的 RW/RO 别名在大小写 rename 后仍共用修改。runner 对失败、缺失证据与超时报错。
- 跑现有 host/static、aarch64 VM/COW/uaccess/SMP 和 x86_64 回归套件，覆盖私有映射及设备映射基线。

实施验收后更新路线图 P2 的完成条件，明确独立进程的命名 tmpfs 会合方式、匿名对象的 fork 继承及活跃映射期间的可见性。
