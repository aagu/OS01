# 决策回复（host 侧 Hermes 转达，用户确认）

> 针对你上一个问题「需要你决定的两件事」。这是用户（aagu）的决策。

## 1. PIT-200HZ-ANALYSIS.md 处理
**挪到 `docs/` 归档**（`docs/pit-200hz-analysis.md`，或你喜欢的位置），作为 200Hz issue 的参考资料。不要删除——里面记录的 host 侧实测证据链（TSC=2.994GHz 换算）以后排查还用得上。

## 2. 200Hz 根因定位 vs 收尾
**先收尾**：nanosleep 修复（`2faccbc`）合入主分支，**200Hz 另立 issue 跟踪**（记录在 roadmap 或 GitHub issue，注明：影响 select/poll 超时、EEVDF 时间片、lwIP 超时、busybox sleep 长度；已排除项清单 + 矛盾焦点 QEMU 11.1.0 的 PIT timer 时钟源）。不要把 200Hz 作为 nanosleep 合并的阻塞项。

## 备注
- 200Hz issue 里建议保留「guest TSC = host TSC（`cpu_get_tsc`→`cpu_get_host_ticks`），host 实测 2,994,492,000 Hz」这条换算链，这是确认 200Hz 的决定性证据。
- 你在 memory 里记录的 `jiffies-2x-frequency-bug` 摘要可以留作 issue 正文素材。