# 开源 OS 项目借鉴（References）

> 记录从其他教学/ Hobby OS 项目借鉴的思路与可继续拿来的部分。原始记录来自 `docs/roadmap.md`。

| 项目 | 已经用到的 | 还可以拿来的 |
|------|----------|-------------|
| **Tilck** | EEVDF 调度思路、3 层测试、hang detector | EEVDF 代码结构、load balancing、GDB helper |
| **cavOS** | lwIP、socket syscall、E1000、动态链接与 Alpine apk 路线参考 | 动态链接器加载流程、用户态包兼容 |
| **Aquila** | **ext2 R/W 核心 (~221 行)**: inode/block alloc+free | — |
| **ArvernOS** | 多架构抽象思路、aarch64 dispatch 桩模式 | 分层日志系统、UBSan、aarch64 head.S/GIC/Generic Timer |
| **opuntiaOS** | devman 子系统注册框架 | GICv2 驱动、Generic Timer（clocksource/clockevent 接口已预留）、Window Server GUI |
| **HackOS** | — | 可缩放字体渲染器、VESA 图形模式 |
