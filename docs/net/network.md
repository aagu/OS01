# 网络栈 — lwIP + 驱动 + socket

> 本文档归集网络子系统已完成工作的实施总结。网络调试经验单独收录于 `docs/lwip-debugging-experience.md`。

---

## lwIP 网络栈 + E1000 驱动（已完成，合并于 7416f40）

**状态:** 已合并到主线。具备 lwIP 2.2.1、E1000/virtio-net、DHCP/DNS、TCP/UDP socket、poll/select 网络就绪通知和 HTTP wget。

| 任务 | 工作量 | 借鉴 |
|------|--------|------|
| lwIP 移植到内核 | ✅ | tcpip_thread + OS01 sys_arch |
| E1000/virtio-net 驱动 | ✅ | PCI、MSI-X、IRQ RX 缓冲 |
| socket syscall 层 | ✅ | TCP/UDP、DNS、shutdown、部分读缓存 |
| poll/select 网络集成 | ✅ | `FD_SOCKET` readiness 回调 |
| HTTPS/TLS | ⏳ | mbedTLS 已引入，尚未集成到 BusyBox wget |

**收益:** 网络栈是从"玩具 OS"到"可用 OS"的最大一步。

下一步重点不是继续堆 socket API，而是把 DNS、TCP、UDP、DHCP 和 wget 的成功路径纳入稳定的 QEMU 自动化回归。

---

## 网络正确性加固（已完成）

| 项 | 内容 | 日期 |
|----|------|------|
| 网络正确性加固 | DNS 超时、端口字节序、部分读缓存、shutdown、UDP readiness、响应 hang | 08-12~08-15 |
| E1000 RX ring 所有权串行化 | tcpip 线程独占硬件 ring 消费，IRQ 仅 ack + wake | 08-16 |
| DHCP ACD 关闭 | `LWIP_DHCP_DOES_ACD_CHECK=0`，消除 ~10.6s ACD 竞态导致的偶发不绑定 | 08-16 |
| 网络回归 harness | `make test-network` + `OS01_TCP_ECHO_DELAY_MS` delayed-reply + 20/20 no-delay + 10/10 delay250 cohort | 08-16 |

---

## 网络相关依赖链（规划中，见 roadmap P5）

```
P5: socket ✅ → AF_UNIX/socketpair（原 P2#11）
    mbedTLS ✅ → HTTPS/TLS（BusyBox wget，原 P2#10）
```
