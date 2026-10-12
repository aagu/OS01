# Terminal partial-present 设计规范

日期：2026-10-12。状态：待评审；本文定义方案与验收，不授权开始实现。

输入：[调查 handoff](../../terminal-cpu-handoff-2026-10-12.md)，基线 commit `bfeb0b0e`（已合入 `c644b3d2`）。相关契约：[2D graphics API](2026-09-30-2d-graphics-api-design.md)、[分辨率切换](2026-10-06-qemu-resolution-switcher-design.md)、[构建与测试入口](../../build/build.md)。

## 1. 目标与范围

在 x86_64 QEMU TCG、2 vCPU、terminal 内运行交互式 monitor 的场景中，通过减少 framebuffer 写入像素，降低 terminal 的持续 CPU 占用。保留默认 ash、PTY 会话、33 ms 合帧策略、250 ms 恢复策略、显示所有权与模式 generation 语义。

交付范围：新增部分矩形 present UAPI 与 libgfx API；terminal renderer 的待提交脏区；display 层的提交与重试；对应 hosttests、QEMU 正确性及性能验收。旧完整帧接口、其他 gfx 客户端继续可用。

不包含 framebuffer mmap、DMA、GPU 加速、内核滚屏 ioctl、QEMU 参数调优、调度器或 CPU 统计修改、monitor 输出协议修改。不得用降低 monitor 刷新频率替代减少写入量。

## 2. 已知事实与证据限度

handoff 记录的 baseline：约 5 wakes/s、2 presents/s，`present_avg_us=85277`；kernel present 平均 83.8 ms；单行写入平均 47.9 µs、约 33 ns/pixel。idle terminal 回到 0%。整帧 bulk memcpy 尝试约 196 ms/present、40% CPU，已回退。现有逐行拷贝与一次容错上下文必须保留。

这些是历史实测输入，本文未重新跑 QEMU。主要成本位于 framebuffer 写入路径有测量支持；QEMU 内部 dirty-tracking 的具体机制和裸机加速比仍是调查推断，不能作为跨机器性能保证。wall time 也不能直接等同于 scheduler 计费的 CPU time，验收分别采集两者。

handoff 的分辨率/字节数不一致：1280×960×4 为 4,915,200 B，1920×1080×4 为 8,294,400 B。不能把“1280×960、8.1 MB、1080 行”当作同一模式。新基线必须记录实际 width、height、stride、QEMU 版本、加速器和 vCPU；完整帧写入量为 `width * height * 4`，源缓冲区跨度与 framebuffer stride 分开记。

当前代码事实：

- `GFX_PRESENT=0x4703`，请求为 16 B；`reserved` 必须为零，stride 必须等于 view width×4。
- `gfx_ioctl_present` 快照 view，验证用户范围，获取 `fb_gfx_writer_begin` 租约，逐行经过 kernel bounce 写入；失败可以已经显示前几行。
- core 已有逐 cell dirty bitmap，`set_glyph`/`blank_cell` 对相同 glyph 不重复标记；不存在 handoff 所称的 `term_core_mark_dirty` 公共入口。
- renderer 还直接标记旧/新光标 cell，处理像素滚屏和 alt-screen，并在 flush 中清掉 core dirty；真正的 present 入口在 `terminal_display.c`。
- monitor 启动时清屏，常规采样用 home + 内容 + erase-to-EOL；数值、历史与进程排序变化可能分散在多个字符行。

## 3. 方案选择

| 方案 | 收益与限制 | 决策 |
|---|---|---|
| 单一 dirty bbox + 新矩形 ioctl | 最简单；分散变化间的空白也会上传，可能超出 300 KB | 不作为 terminal 唯一策略 |
| 每字符行一个 dirty span + 新矩形 ioctl | 复用现有 cell bitmap，避免跨行包围空白；代价是一个逻辑帧可能多个 ioctl | 采用 |
| 矩形数组 ioctl / 像素 shadow diff | 可减少 syscall 或过滤重复绘制，但增加 UAPI 或额外内存、生命周期 | 本阶段不引入 |

第一版不增加 `fb_write_rect_leased` 或 bulk memcpy。gfx 层已有必要的用户容错、bounce 与租约逻辑，矩形路径继续调用 `fb_write_row_leased` 即可。新 helper 若把用户指针直接交给 fb 层，会模糊 uaccess 边界，也不是减少写入字节的必要条件。

## 4. UAPI 与 libgfx 契约

新增 `GFX_PRESENT_RECT=0x4709`，保留全部旧命令号、旧结构尺寸和旧 reserved 校验。不得以旧 reserved 区分请求版本。

```c
typedef struct {
    uint64_t pixels;
    uint32_t stride;
    uint32_t reserved;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} gfx_present_rect_req_t; /* 32 B */

int gfx_present_rect(gfx_handle_t *handle,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h);
```

ABI 使用固定宽度类型；添加 sizeof 与关键字段 offsetof 静态断言：pixels=0、stride=8、reserved=12、x=16、y=20、w=24、h=28。

所有坐标为 view-local 像素；目的 framebuffer 坐标为 view origin 加 x/y。`pixels` 指向完整 view 私有缓冲区的首字节，不能指向一个 packed rect；stride 仍严格等于 view width×4。第 i 行源地址为 `pixels + (y+i)*stride + x*4`，拷贝长度为 `w*4`。绘制 clip 只约束绘制，不裁剪 present 请求。

- w/h 必须非零，且 `x <= view_w && w <= view_w-x`，y/h 同理；不隐式裁剪，越界和空矩形返回 EINVAL。
- reserved 非零或 stride 不符返回 EINVAL；用户请求不可读、pixels 为零、源范围非法/读取 fault 返回 EFAULT。
- 完整 view 矩形合法。libgfx 可直接调用旧 `gfx_present`，但其结果、错误与逐行写入行为必须等价。
- libgfx 的 NULL handle / 几何非法在发 ioctl 前返回 -1/EINVAL；非完整帧合法矩形恰发一次新 ioctl；每次重建请求，不缓存用户指针。成功不改 errno，失败透传 ioctl errno。
- 不自动探测旧 kernel，也不在 ENOTTY 时静默整帧 fallback；仓库内 kernel 与 libgfx 同步发布，旧客户端在新 kernel 上兼容。

## 5. Kernel 执行与安全边界

新命令遵循旧 present 的阶段顺序：容错复制请求 → 短锁下快照 configured view/模式序号 → 无锁校验几何、stride、用户源范围 → 获取 gfx writer 租约 → bounce 逐行写入 → 清理资源。

全部乘加使用 uint64_t 并显式检查溢出。源验证可使用连续包络：

```text
first = pixels + y*stride + x*4
length = (h-1)*stride + w*4
```

检查 first、length 与结束地址的可表示性，随后 `syscall_check_user_range(first, length, false)`；包络内的行间字节也必须可读，但实际只拷贝每行 w×4 B，不拷贝 padding 或 rect 外的像素。几何合法但地址算术越界视为 EFAULT。合法请求的 bounce 大小仅 w×4 B。

整次矩形持有一份租约，目的 stride/地址来自 lease snapshot，不读实时 Pos；调用 `fb_write_row_leased(lease, view_x+x, view_y+y+i, bounce, w*4)`。不得持有 table/state 自旋锁进行用户 range check、setjmp 或 memcpy。

复用 v1 的单次 fault 上下文，恢复 `fault_jmp`、`fault_cleanup` 和 `fault_cleanup_arg`；正常、ENOMEM、用户 fault 与写行失败全部归还租约并释放 bounce。对 fault 分支使用 setjmp 后仍有效的变量，避免 longjmp 后读取未定义的局部状态。

EACCES、EAGAIN、ESTALE、EIO 由现有 `fb_gfx_writer_begin` 原样返回。矩形内和多个矩形间均不承诺画面原子性；失败可能已写入部分像素。成功不保留用户指针或待执行任务。旧 `GFX_PRESENT` 可以共享内部实现，但不得改变其错误契约、验证范围、probe 与完整帧路径。

## 6. Renderer：绘制 dirty 与提交 damage 分离

core dirty 表示需要重绘的 cell；renderer pending damage 表示私有 pixels 已修改、尚未全部提交成功的区域。flush 可以继续清除 core dirty，但必须先把修改区域并入 pending damage。不得在 flush 或任一矩形成功后提前清除整个 pending damage。

`term_render_t` 增加固定容量 64 的矩形集合、计数与 `full_pending`；矩形采用半开区间并限制在实际 view 内。每次 flush 对每个字符行的 dirty cells 取最左/最右列，转换为 font 像素边界，加入一个 span。相同 y/h 的 span 合并 x 区间；相邻字符行仅在 x/w 相同时纵向合并。保留距离较远的字符行独立，不能把它们无条件合成一个 bbox。

容量达到上限而仍需加入新矩形时，置 `full_pending` 并丢弃列表；只增加保守写入量，绝不遗漏修改。固定容量无需新的 malloc，尺寸变化不会增加 renderer 资源准备失败点。任意时刻 `full_pending` 覆盖所有列表内容，完整帧成功后才清除。

必要的 damage 来源：

| 操作 | 必须提交的范围 |
|---|---|
| glyph 绘制、erase line、erase screen | 所有实际重绘 cell 的 span，包括被擦除的旧内容 |
| 光标移动/显隐 | 被擦除的旧 cell 与当前光标 cell；绘制 underline 也产生 damage |
| `term_render_clear` | 完整 view，含字符网格外的底部/右侧余量 |
| 像素 scroll，包括不足一屏的滚屏 | 完整 view；pixels 的移动改变了上方全部目的像素 |
| alt-screen 切换、resize、新 view、前台恢复 | clear + 标记所有 cell dirty + 完整 view |
| 无 cell/光标/其他像素变化 | 无 present ioctl |

本阶段不修改 core parser 或所有 dirty 写入点；renderer 收集实际绘制的 cell，避免遗漏直接写 dirty bitmap 的光标路径。原有可能多绘一次相同 underline 的行为可保留并计入 damage，优化光标另行评估。

新增 renderer 的查询/ack 内部接口：display 可以取 full 标志和矩形快照；只有整个逻辑帧提交成功才 ack。单线程 terminal 主循环是这一约束的前提；提交期间不得调用会新增 damage 的回调。

初次绑定 renderer 默认 `full_pending=true`。terminal.c 启动/退出直接调用的旧 full present 保持原样；启动未 ack 的 pending full 允许导致随后多一次整帧更新，不能为了消除此一次性成本改动主循环。

## 7. Display 提交与恢复

`terminal_display_ops_t` 增加矩形 present hook，真实实现调用 libgfx；测试 fake 明确提供新 hook。内部缺失 hook 时保守走旧 full present，成功后 ack，以兼容既有注入测试，不用于掩盖生产 ENOTTY。

前台时先 flush，再根据 pending damage 选择：full → 旧 full present；列表 → 按 y/x 固定顺序逐个 rect present；为空 → 返回 0 且不发 ioctl。只有所有调用成功才 ack、清除待提交区域。任一个失败立即停止本轮，整个列表保留，重试允许覆盖此前成功区域。

- EAGAIN：保留 damage，沿用 250 ms deadline；无新 PTY 输出也必须能重试。
- ESTALE：保留当前 damage 直到现有 refresh/rebind 完成；新 view 必须 full redraw，旧 view 坐标不能复制到新 view。
- EACCES：进入 background/skipped，保留 damage，继续处理 PTY；前台恢复后必须 full redraw，覆盖其他 owner 留下的内容。
- EIO 与其他错误：沿用既有恢复/失败计数与 serial-only 策略，不伪装成功。

pending damage 的重试由 display 的现有 deadline/refresh 路径驱动，即使 generation 未变也不能直接清 deadline 而跳过未成功的提交。须在 `terminal_display.c` 内完成这一修正，不改 terminal.c 的调度逻辑。失败后的 errno 在返回时必须仍是失败 present 的 errno。

一轮多 ioctl 可以遇到 owner/mode 在矩形之间改变；这是允许的部分提交失败，靠保留 damage 和后续 full 恢复收敛，不把整轮矩形扩大为一个持锁事务。

## 8. 观测与性能验收

保留 terminal.c 的四段 `[term-probe]`、kernel gfx present probe 和 fb row probe。handoff 的“长期保留”优先于代码内写着“上线后移除”的旧注释；可以修正文案，不删除观测。

旧 `gfx_present` probe 不足以单独比较多个 rect：必须让矩形路径也计入成功 present 的时间/字节统计，区分 full/rect。另在 display/renderer 侧增加低频逻辑帧汇总，包含 full_frames、partial_frames、rect_calls、成功逻辑帧写入字节、失败次数；不改变已有 term-probe 格式。可用新增 `log_debug`/debug channel 控制 kernel 新统计，新代码不新增 serial_printk。

fb row 原 probe 不含直接 byte 输出，单靠 ns/pixel 不能证明字节减少；新增统计或 host mock 的总 bytes/rect 几何必须提供独立证据。固定“每 256 次”也不能假设 10 秒必有输出：2 calls/s 需要约 128 秒。短验收窗口使用每秒汇总，或明确延长采样到获得完整数据。

在同一宿主、相同 QEMU 参数与实际模式下采集 baseline/实现后各至少 30 秒稳态数据，排除启动、monitor 入场清屏、退出、切换恢复的 full redraw。主性能场景为普通镜像、TCG、2 vCPU、terminal 内输入 `monitor`；`monitor --samples` 是非交互路径，不能替代此场景。

通过条件：

1. 稳态平均每个成功逻辑帧写入量 <300,000 B；统计一轮所有 rect 的总和，而非单个 ioctl。确定性稀疏变化 fixture 同时证明只写指定区域。
2. 原 `[term-probe]` 的稳态聚合 `present_avg_us <= 10000`，按 presents 数加权，涵盖整轮 display present 的多个 ioctl；保留每秒值和长尾以便分析。
3. terminal 在 monitor 中的稳态 CPU 目标为 CPU0 的约 1–3%，报告 scheduler 实际计费结果。主门槛是字节数和时间，不能把 wall time 换算值当作 CPU 达标证据；CPU 未达目标仍需解释原因。
4. idle terminal 持续无 full/rect 更新（排除初始化），无忙轮询；视觉正确性与恢复测试全部通过。

若时间仍 ≥50 ms 或字节门槛未满足，不能宣称优化完成；保留输出、rect 分布、模式和 probe 数据，定位重复绘制/过宽 span/列表溢出。10–50 ms 也未达到本 spec 的性能门槛。不得重新引入已回退的整帧 bulk helper。裸机可采同格式 baseline，但不作为本阶段必须具备的环境或加速保证。

## 9. 验证矩阵

| 层 | 必须覆盖 |
|---|---|
| UAPI/libgfx host | 结构布局、命令号、旧 reserved 校验；rect 请求完整字段与源基址；NULL/零尺寸/越界/完整 view；clip 无关；成功 errno 不变、失败 errno 透传 |
| kernel gfx host | 非零 view origin 与 x/y；单像素、最右下角、完整 rect；stride 错误、溢出、无效用户地址；请求 copy fault 与中间行 fault；只写 w×h×4 B，rect 外 sentinel 不变；资源/lease/fault 状态清理；EACCES/EAGAIN/ESTALE/EIO |
| renderer host | 分散行不合 bbox、同行合并、相邻同跨度合并；光标擦除/显隐；清屏、scroll、alt-screen、余量像素；64 容量溢出 full；dirty 清除后 pending 仍保留；无变化无 ioctl |
| display host | 多 rect 全成功才 ack；第 N 个失败后全列表重试；无新输出的 deadline 重试；background drain/恢复 full；mode rebind 丢弃旧几何；缺失注入 hook full fallback；新 present 错误沿用恢复策略 |
| QEMU surface/session | sparse 更新前后截图像素差，rect 外不变；光标、滚屏、清屏、alt-screen；GUI owner 退出/释放/被杀后的完整恢复；setres 往返、后台切换和故障恢复；monitor 正常退出及 Ctrl-C 后 shell 存活 |

稀疏像素 sentinel/截图测试必须实际验证内核写入范围；只有 libgfx ioctl mock 不能证明 kernel 边界安全。QEMU 截图应沿用既有 QMP surface 获取路径，不能因截图失败跳过。性能 runner 基于已有 monitor suite 扩展或注册聚焦用例，具体入口在 implementation plan 钉死；不把尚不存在的 target 写成现有命令。

已有验证入口：`make test-host`、`make test-static`、`make test-qemu SUITE=gfx`、`make test-qemu SUITE=monitor`、`make test-qemu SUITE=resolution`、`make FB_RESOLUTION_TEST=1 test-qemu SUITE=resolution`。display ownership 的既有 host case 和 `qemutests/test_display_owner.py` 必须全部运行，其独立脚本参数由 plan 按当前 runner 核对。monitor suite 保留原 1/2/8 vCPU 功能矩阵，性能门槛以 2 vCPU 为主。

## 10. 文件边界与落地约束

- `kernel/include/uapi/gfx.h`：唯一 UAPI 定义；sysroot 同步传播，不复制 ABI 到 libgfx。
- `kernel/driver/gfx.c`：新 dispatch、矩形容错逐行路径、统计；可做必要内部共用。
- `kernel/driver/fb.c` 与 `kernel/include/driver/fb.h`：不新增写入 API，不改 row probe 或 lease 协议。
- `libgfx/gfx.h`、`libgfx/gfx.c`：公开矩形函数与 wrapper，handle 继续 opaque。
- `user/terminal/terminal_render.[ch]`、`terminal_display.[ch]`：pending damage、提交/ack、retry、注入 hook。
- `hosttests/cases/`、`qemutests/` 及现有构建注册：新增测试必须接入真实 harness，无孤儿测试。
- `user/terminal/terminal.c`：本阶段不修改；保留全部 probe 和 `/bin/busybox` ash 启动。不得临时 exec monitor 作为交付结果。

不新增 kernel 子系统，不改变 boot_context 或 syscall 编号。任何结构变化后必须 `make clean` 再重建；clean 后重新取得 profile firmware/image 路径，确保性能和功能测试确实运行新镜像。本文只交付 spec，评审后再生成 implementation plan。
