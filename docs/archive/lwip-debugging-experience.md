# lwIP 网络栈调试经验（2026-08-12 ~ 08-14，lwIP-dev 工作树）

> **目标**：让 busybox `wget http://example.com` 在自研内核 + lwIP + QEMU 环境下正常返回。
> **达成**：2026-08-14，纯 MSI-X 中断驱动下两次无 trace 干净运行均稳定成功——
> `saving to '/tmp/example.txt'` → `saved` → cat 显示完整 HTML → pcap 捕获 HTTP/1.1 200 响应铁证。
> 最终 HEAD：`854f5d0`（分支 `lwip-dev`）。

本文档记录本周在 lwIP-dev 工作树定位并修复问题的完整经验：根因、证据链、修复方式与可复用的调试方法论。按"层"组织——驱动 → 内核 → 用户态/libc，与排查顺序一致。

---

## 1. 背景与前置

- 环境：QEMU `q35 + OVMF + e1000e(82574L) + SLiRP user-mode net`，无 KVM（TCG）。
- 协议栈：lwIP STABLE-2_2_1（submodule），`LWIP_TCPIP_CORE_LOCKING=0`。
- 用户态：busybox 1.36.1（submodule），wget 依赖 Linux x86_64 syscall 号。
- 7 月前置（非本周）：lwIP 子模块引入（5e27463）、两阶段网络初始化（29eddfd）、virtio-net 驱动（5e51968）、e1000 MSI/link-up（b308e5c）、统一 register_irq API + MSI-X（584a584）、RCTL 混杂位（8ee21d9）。

本周（8-12 起）进入"把 wget 跑起来"的最后一公里，所有提交从 `e1be719` 到 `854f5d0` 共 12 个。

## 2. 时间线与提交链

| Commit | 日期 | 主题 | 关键内容 |
|--------|------|------|----------|
| `e1be719` | 08-12 | fix(driver): e1000 RX pipeline | MDIC autoneg、ring RXQ、RDT 语义、ethernet_input 分层 |
| `c5474e7` | 08-12 | fix(net): lwIP 集成 | core-locking off、lost-wakeup-safe mbox/sem、idle timer 直接 wake、printf 格式宏 |
| `d5e209d` | 08-12 | fix(net): 端口字节序 | SYS_connect 里 `os01_ntohs(sin_port)` |
| `34c804a` | 08-12 | refactor(libc): 去重 | 删 ENOSYS fopen/fclose/fdopen stubs |
| `39caa15` | 08-13 | thirdparty(busybox): bump | OS01 userland startup（crt0 + sigreturn） |
| `7958825` | 08-13 | fix(driver): 中断 RX（首次尝试） | MSI-X 中断驱动，drop PIT polling —— **三根因未修，中断实际不触发** |
| `69e6e19` | 08-13 | feat(net): socket poll | POLLIN 就绪 + sendto 错误上抛 |
| `5cb60ad` | 08-14 | fix(libc): stdio fd | fgets/getc/fputs/fputc/vfprintf/fileno 全部改用真实 fd；SYS_shutdown=64 映射 |
| `c5c8691` | 08-14 | fix(net): SYS_shutdown + 部分读 | netconn_shutdown 发 FIN；socket rx_nb/rx_off 缓存 |
| `e9a587e` | 08-14 | fix(timer): mbox idle timer | **三根因**：相对偏移 + UAF + one-shot 重建 |
| `c048983` | 08-14 | fix(driver): RDT re-arm | RDT 每次 poll 无条件 30→31 交替 + RXQ_DEPTH 64 + PIT 轮询兜底 |
| `854f5d0` | 08-14 | fix(driver): MSI-X 三根因 | 16 位配置写被 TCG 丢 / IMS 位不匹配 / IVAR 地址错；**最终删除 PIT 轮询，纯中断** |

**演进脉络**：8-13 首次尝试中断（7958825）失败 → 8-14 上午靠 timer 三根因 + RDT 修复配合 PIT 轮询兜底达成 wget（c048983）→ 8-14 11:42 MSI-X 三根因修复后真正纯中断化（854f5d0），两次干净运行实证。

## 3. 根因总表

| 层 | 根因 | 症状 | 修复 | Commit |
|----|------|------|------|--------|
| 驱动 | RDT 不 re-arm，QEMU 恢复接收依赖 RDT 值变化 | RDH 冻结、收包停摆 | 每次 e1000_poll_rx 无条件写 30→31 交替 | `c048983` |
| 驱动 | MSI-X 使能位从未写入（16 位配置写被 TCG 丢弃） | `icr_clear_nonmsix` 刷屏、无 notify | 32 位写整个 cap dword bit31 | `854f5d0` |
| 驱动 | IMS 掩码用 82540 位（RXT0/TXDW），QEMU e1000e 置 82574L 位（RXQ0/TXQ0） | `IMS & ICR` 恒 0，无中断 | 补 RXQ0/TXQ0/OTHER 位 | `854f5d0` |
| 驱动 | IVAR 写错地址（0xE000 应为 0xE4），向量项 VALID=0 | `wrn_msix_invalid`，notify 被拒 | 修正偏移 + 全部 cause → vec 0（VALID+vec0） | `854f5d0` |
| 内核 | create_timer 参数被双重加 jiffies（绝对 vs 相对） | timer 永不到期，tcpip_thread 睡死 | 传相对偏移 | `e9a587e` |
| 内核 | mbox idle timer 回调 data 用栈 ctx | UAF，唤醒后行为随机 | data 用 mb 本体 | `e9a587e` |
| 内核 | idle timer 是 one-shot，触发即摘链 | 第二次 sleep 无人唤醒（lost-wakeup） | sleep 前检查 active 并重建 | `e9a587e` |
| 内核 | `sin_port` 网络字节序传入 lwIP host 序接口 | DNS 发往端口 13568（0x3500）等 | connect/sendto/bind 三处 `os01_ntohs` | `d5e209d` + trap.c |
| libc | stdio stub 硬编码 fd 0/1 | wget 读终端等键盘 / 请求发到串口→400 | 全部经 fileno_unlocked→mini_file_t.fd | `5cb60ad` |
| libc/net | netbuf 一次性语义，1 字节 read 毁掉 370B 响应 | 响应截断成 1 字节 + EAGAIN | socket 缓存 rx_nb/rx_off，读完才删 | `c5c8691` |
| libc/net | SYS_shutdown 映射缺失（Linux 48） | 调用静默变成 nr 0（putchar） | 补映射 + netconn_shutdown 实现 | `5cb60ad`/`c5c8691` |

## 4. 驱动层：e1000 RX 管线与 RDT 语义

### 4.1 管线设计（e1be719 + c048983 定型）

```
e1000_handler (MSI-X 中断)
  └→ e1000_poll_rx()：检查 RXQ，收描述符入 mbox
      └→ sys_mbox_wake()：唤醒 tcpip_thread
tcpip_thread
  └→ net_poll_rx()：从 mbox 取出，调 ethernet_input() 进 lwIP
```

- **中断上下文绝不直接调 lwIP**（禁 tcpip_input/ethernet_input）——只缓冲 + 唤醒，消费在 tcpip_thread 上下文。
- RXQ 深度 16 → 64（`E1000_RXQ_DEPTH 64`）。
- PIT 轮询（net_poll_rx_irq）作为兜底经历了"删除 → 恢复 → 最终删除"的演进；854f5d0 后 RX 纯中断驱动，mbox idle timer 仅作唤醒兜底。

### 4.2 QEMU e1000e 的 RDT 语义（关键认知，c048983）

QEMU 的 e1000e 模型与真实 82574L 行为不同，三条规则直接决定驱动写法：

1. **RDT = 软件已准备好的最后描述符**（tail）。QEMU 只在 **RDT 值变化**时恢复接收（`e1000e_start_recv`）。
2. **RDH >= RDT 视为 ring 满**，停止接收。RDT 写 31 后 RDH=10 → 空闲 21，条件不成立。
3. **RDT 写必须在每次 `e1000_poll_rx` 调用无条件执行**（30→31 交替），**禁止**放进"仅当有 DD 位"的 while 循环内——否则收完一批后 RDT 不变，QEMU 永远不恢复接收，RDH 冻结。

## 5. 驱动层：MSI-X 中断三根因（854f5d0）

### 5.1 分层调试流程（永远按 device → LAPIC → CPU 顺序查）

QEMU 命令行加 trace 集：

```
-d trace:e1000e_irq_msix_notify_vec,trace:e1000e_irq_set,\
  trace:e1000e_irq_pending_interrupts,trace:e1000e_wrn_msix_invalid,\
  trace:apic_deliver_irq,trace:pci_cfg_write,trace:msix_write_config
```

（`qemu-system-x86_64 -d trace:help | grep -iE "msix|apic|e1000e_irq"` 列事件名。**事件名写错只警告不报错**，会静默拿不到输出。）

| 层 | 判据 |
|----|------|
| 设备侧 | `e1000e_irq_msix_notify_vec` 必须出现。不出现 → 查三根因 |
| APIC 侧 | `apic_deliver_irq ... vector 48`(=0x30) 必须出现。不出现 → MSI 消息地址/数据或 LAPIC SVR enable 错 |
| CPU 侧 | guest handler 真执行（IRQ 上下文用 serial_printk 低频探针，禁 write_serial） |

### 5.2 根因 #1：MSI-X 使能位从未写入

- 症状：`e1000e_irq_icr_clear_nonmsix_icr_read` 刷屏；`pci_cfg_write @0xa2` 从未出现；回读 verify=0x4。
- 原因链：`pci_make_addr` 强制 dword 对齐（`offset & 0xFC`）→ 访问 cap_ptr+2 实际落到 cap_ptr；且 **QEMU 的 PCI host 数据端口在 TCG 下丢弃 16 位端口写**（outw 0xCFE 无效）。
- 修复：**32 位写整个 cap dword 并置 bit31**：
  ```c
  pci_config_write(bus, dev, func, cap_ptr, dword | 0x80000000);
  ```
  回读 verify=0x80040011，QEMU `msix_write_config enabled 1`。（16 位配置**读**可用，只有写被丢。）

### 5.3 根因 #2：中断位不匹配（82574L vs 82540）

- 症状：`e1000e_irq_set` 显示 ICR 有 RX cause（`0x...0100000`），但 `pending_interrupts` 显示 `ICR PENDING: 0x0`，无 notify。
- 原因：QEMU e1000e（82574L）在 MSI-X 模式置 **RXQ0(bit20)/TXQ0(bit22)/OTHER(bit24)**，不是 82540 时代的 RXT0(bit7)/TXDW(bit0)。驱动 IMS=0x95 与 ICR 永不相交，`raised_causes = IMS & ICR & ~old` 恒空。
- 修复：IMS 与 handler 都改掩 RXQ0|TXQ0|OTHER（保留 LSC）。

### 5.4 根因 #3：IVAR 写错地址

- 症状：pending 非空但 notify 被拒，`e1000e_wrn_msix_invalid "Invalid entry for cause 0x100000: 0x0"`。
- 原因：82574L 的 **IVAR 寄存器在 0xE4**（0xE000 不是 IVAR）。IVAR=0 → 所有向量项 VALID=0。
- 修复：
  ```c
  e1000_write(E1000_REG_IVAR, /* 0xE4 */
      (0x8|0) | ((0x8|0) << 4) | ((0x8|0) << 8) |
      ((0x8|0) << 12) | ((0x8|0) << 16));  // RXQ0,RXQ1,TXQ0,TXQ1,OTHER → vec 0
  ```
  每 4 位域 = bit3 VALID + bits2:0 向量号；全部路由到 entry 0（pci_enable_msix 配置的那个）。**先写 vector table 再置使能位**。

### 5.5 验证链路（trace 铁证）

```
包到达 → ICR RXQ0 置位 → e1000e_irq_msix_notify_vec
→ apic_deliver_irq vector 0x30 → IDT 门 0x30 → do_IRQ
→ irq_table[16].handler → e1000_poll_rx + sys_mbox_wake → tcpip_thread → lwIP
```

验收：notify_vec > 0 **且** apic_deliver_irq > 0；删掉 PIT 轮询后 wget 仍成功；**两次无 trace 干净运行**。

## 6. 内核层：lwIP 集成决策（c5474e7）

- `LWIP_TCPIP_CORE_LOCKING=0`：tcpip_thread 单线程处理，API 层通过 mbox 通信。用 objdump 检查 api_msg.o 是否真的编译成非阻塞分支。
- **lost-wakeup-safe mbox/sem**：`sys_mbox_post` 满时**静默丢弃**（lwIP 官方行为），接收侧绝不能假设"post 必有后续 wake"；配一个 idle timer 兜底唤醒。
- `netconn_connect` 返回 ERR_OK 只表示"已发起"（do_connect 不阻塞，SYN+ACK 由 do_connected 回调 signal），用户态别提前置 SOCK_CONNECTED。
- **端口字节序**：`sockaddr_in.sin_port` 是网络序，lwIP netconn 接口要 host 序。`htons(53)=0x0035` 当 host 序 u16 读成 0x3500=13568 → DNS 包发到错端口。**grep 所有 trap.c 里读 sin_port 的 handler**（connect/sendto/bind/getsockname/getpeername）逐一 ntohs。

## 7. 内核层：timer 三根因（e9a587e）

tcpip_thread 靠 mbox idle timer 唤醒，三个独立 bug 叠加导致"睡死"：

1. **相对偏移**：`create_timer` 的 expire 是**相对 jiffies 偏移**（`expire = jiffies + arg`），不能传绝对时间。旧代码双重加 jiffies（first_exp=21291 = 2×10643+5）→ 永不到期。
2. **UAF**：idle timer 回调的 data 必须传 **mb 本体**，禁止栈 ctx。回调里 `os_mbox_t *mb = (os_mbox_t *)data;` 直接 wake。
3. **one-shot 语义**：timer 触发一次即 list_del、active=0。**每次 sleep 前必须检查 active 并重建**，否则第二次 sleep 无人唤醒。

证据：SLP 事件序列在 11980 处有 in 无 out（睡死实锤）；修复后 SLP in/out 2468 次，pcap 显示 guest ACK 持续。

配套加固（timer.c）：`timer_t.active` 标志 + add_timer 根治（tmp != head 终止）+ del_timer 先查 active。

## 8. 用户态 / libc 层（wget 最后一公里）

驱动与栈全通后，wget 仍挂——真正的拦路虎在 libc 与 syscall 映射：

1. **stdio fd 硬编码陷阱**（5cb60ad）：busybox 的 `getc→getc_unlocked`、`fileno→fileno_unlocked`。若 libc stub 硬编码：
   - `fgets_unlocked` 读 `read(0,...)` → wget 读**终端**等键盘，永久挂起（syscall trace 静默、SOCK read 永不调用）。
   - `fputs/fputc/vfprintf` 写 `write(1,...)` → HTTP 请求发到串口，服务器收到 NUL 字节 → 400。
   - 修复：所有 stdio 调用经 `fileno_unlocked(f)` → `mini_file_t.fd`，处理 (FILE*)1/2/3 魔术指针，struct 定义放公共头。
2. **netbuf 一次性语义**（c5c8691）：`netconn_recv` 返回的 netbuf 被 `netbuf_delete` 整体释放；fgets 一次读 1 字节会把 370B 响应全毁。修复：socket 缓存 `rx_nb/rx_off`，读尽剩余才删，socket_free 里释放残留。
3. **SYS_shutdown 映射缺失**（Linux 48）：映射表默认值让调用静默变成 os01 nr 0（putchar）——行为损坏而非报错。补映射 + `netconn_shutdown`（shut_rx/shut_tx 由 how 推导）。wget 发完请求即 shutdown(SHUT_WR)，服务器才回响应。
4. **getopt32 "^" POSIX 模式**：optstring 以 `^` 开头时选项解析在第一个非选项参数处停止——`wget URL -O file` 会把 `-O` 当 URL。测试命令必须 `wget -O file URL`。这是测试命令问题，不是内核 bug，但日志里看起来极像。
5. **include guard**：头文件中间一个多余的 `#endif` 会让后半截暴露在 guard 外 → 每次 include 都 typedef redefinition。

## 9. 调试方法论（可复用武器库）

### 9.1 QEMU trace 事件速查表

`-d trace:<ev1>,<ev2>,...` 一次运行直接输出 QEMU 内部条件状态，**优于猜测性驱动探针**。

| 事件 | 用途 |
|------|------|
| `e1000x_rx_can_recv_disabled` | 打印 `link_up / rx_enabled / pci_master` 三值——can_receive 的全部条件，一行终结"NIC 拒收"争论 |
| `e1000e_rx_can_recv_rings_full` / `e1000e_rx_has_buffers` | ring 空间（free descr / packet size） |
| `e1000e_rx_receive_iov` | 包进入设备接收路径的次数（对比服务器发了多少） |
| `e1000e_rx_flt_dropped` | MAC/过滤丢包 |
| `e1000e_cb_qdev_reset_hold` / `e1000e_core_ctrl_sw_reset` | 设备级/软件复位（CTRL.RST）及时间点 |
| `e1000e_wrn_regs_write_unknown` | 非法寄存器写（IVAR 地址写错会暴露） |
| `e1000e_irq_set` / `e1000e_irq_pending_interrupts` | ICR 置位 / `IMS & ICR` 是否为空 |
| `e1000e_irq_msix_notify_vec` / `e1000e_wrn_msix_invalid` | 中断通知发出 / 向量项无效被拒 |
| `apic_deliver_irq` | LAPIC 投递（vector 号） |
| `pci_cfg_write` / `pci_cfg_read` / `msix_write_config` | 配置空间读写（16 位写被丢的实证） |

**假设排除实例**：trace 首轮显示 `rx_can_recv_disabled link_up:1, rx_enabled:0, pci_master:0` → 假设"运行中设备 reset 清寄存器" → 内核无 FLR/reset 代码 → `qdev_reset_hold` trace 确认触发 → 时间序分析（parse6.py）显示 qdev_reset 只发生在初始化 369 时刻 → **假设排除**。不验证假设就改代码是浪费。

### 9.2 pcap 抓包（SLiRP 侧）

QEMU 11 拒绝 `-netdev user,logfile=...`，用 filter-dump：

```
-netdev user,id=net0 \
-object filter-dump,id=f1,netdev=net0,file=/tmp/cap.pcap \
-device e1000e,netdev=net0
```

解析用 Python struct（开发机无需 tcpdump）。**DNS 查询显示 `UDP 49276->13568` 而非 `->53`**——一行就定位了整类字节序 bug。铁律：三层验证的第三层必须是 wire capture，不是 xmit 调用返回值。

### 9.3 三计数器诊断（tcpip_thread 疑死）

| 计数器 | 位置 | 回答 |
|--------|------|------|
| mbox_fetch 入口计数 | sys_arch.c | tcpip_thread 还活着吗？ |
| e1000_process_rx 计数 | mbox_fetch 循环内 | RX 在被消费吗？ |
| PIT 10ms RDH/RDT 监控 | pit.c，变化才打印 | 包到达硬件 ring 了吗？ |

决策表：RDH 不动 → 包没到硬件（filter/SLiRP/MAC 问题），别查 lwIP；RDH 动但 fetch 计数停 → tcpip_thread 卡死（死锁/日志洪水/缺页挂起）。计数打印每 10-20 次会制造"看起来停了"的假象（线程在两次打印间跑了 12-20 次 fetch）——每 5 次或按事件打。

### 9.4 探针纪律

- **write_serial 不可靠**：`-O2` 会把它优化掉（无副作用属性的 extern 声明）。IRQ/线程上下文探针一律 `serial_printk`。
- `log_info` 在 IRQ 上下文可能不出（锁）——"无 IRQ 日志"≠"无 IRQ"。
- **日志洪水会挂起内核**：串口 FIFO 满时 log 阻塞在 transmit-empty 自旋。本会话的"死 tcpip_thread"实际卡在 log_info 里。监控保持低频，下结论前先去掉洪水探针。
- `strings` 丢 <4 字符短串；用 `grep -ao` 或原始字节搜索。

### 9.5 验证纪律

- 增量 make 会静默复用旧 .o / disk.img 里是旧 kernel.bin——**改文件后 touch 强制重编**，`stat -c "%y %n"` 看时间戳，`python3 -c "b'marker' in open('kernel.bin','rb').read()"` 验探针进产物。
- 上传远程文件后 **grep 关键行验证**（版本混淆曾致 1 小时浪费）。
- **最终验收 = 两次无 trace 干净运行**。trace 本身扰动时序（带 trace 时 QEMU 只 receive 10 次、RDH 冻结 10 的现象在干净运行下不出现）。
- 排查分层：QEMU trace 先行（内部门控）→ 驱动层 → PIT/TICK 活性 → lwIP 层。

## 10. 陷阱清单（本轮血泪）

- **inline python/heredoc 引号陷阱**：`unexpected EOF while looking for matching '` exit 2，本轮失败 4 次。统一本地写脚本 + base64 管道上传。
- **远程脚本被旧版覆盖**：parse_pcap2.py 曾 exit 1，重写恢复——上传前先 base64 拉远程最新版对比。
- **/tmp 空间**：make disk.img 泄漏可塞满 20G → 编译失败。构建后 `df -h /tmp`。
- SSH 别名解析不稳 → 用 IP 直连（`aagu@192.168.2.160`）。
- QEMU trace 与串口输出共享日志：重 trace 下 guest 小输出（echo）会丢——干净运行前别信"输出缺失"结论。
- busybox submodule dirty 状态需进子模块查（父仓库 git diff 不追踪内部改动）。
- 测试命令问题（getopt32 "^"）看起来像内核 bug——先验证命令本身。

## 11. 遗留问题（非阻塞）

- **HTTPS/TLS**：mbedtls 已在 thirdpart 但未集成（最终目标）。
- **sys_mbox_post 满时静默丢消息**：tcpip_mbox 容量值得核查。
- **运行不稳定**：同构建多次跑结果可不同——验收以"两次无 trace 干净运行"为基准（当前已达标）。
- **AHCI / 根文件系统挂载**：独立问题（kernel/driver/ahci.c）。
- e1000e EEPROM 超时 → MAC 从 RAL0/RAH0 读（既有 workaround）。
