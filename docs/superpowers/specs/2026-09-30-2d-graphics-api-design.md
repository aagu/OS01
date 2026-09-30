# 2D 图形 API 设计

> **日期**: 2026-09-30
> **状态**: 待用户复审
> **基准**: `docs/roadmap.md` P3 GUI「2D 图形 API」项

## 1. 目标与范围

OS01 当前通过 `/dev/fb` 把帧缓冲映射到用户态；`user/tetris.c` 和 `user/terminal.c` 各自写像素。本项提供静态 `libgfx.a`，使应用在局部坐标系里画点、线、矩形和位图，并提供 `/dev/gfx0` 把一个矩形视图呈现到屏幕。Tetris 迁移到新 API；terminal 的字体与 VT100 渲染留在独立迁移项，继续使用 `/dev/fb`。本项不实现字体、window server、合成器或硬件 vsync。

成功标准：Tetris 保持现有输入和画面行为；绘图 primitive 不进入内核；每帧最多一次 `GFX_PRESENT` ioctl；内核不把超出已配置视图的像素从该 handle 写入帧缓冲。

## 2. 设计边界

| 决策 | 本阶段选择 |
|---|---|
| 部署 | x86_64 上的 `libgfx.a` 与内核 `/dev/gfx0`；aarch64 暂无 fb 驱动 |
| 绘制 | `libgfx` 在普通用户态私有缓冲绘制，`GFX_PRESENT` 才将像素交给内核 |
| 坐标 | 创建视图使用全屏坐标 `(x,y,w,h)`；primitive、clip、buffer 索引使用视图局部坐标，左上角为 `(0,0)` |
| 格式 | 仅现有 32 bpp、`format=0` 的 host-endian 像素值；buffer 行跨度为 `w*4` 字节 |
| 呈现 | 按视图逐行拷贝全部 `w*h` 像素到 fb 对应矩形；QEMU stdvga 同步返回，不承诺 vsync 或真正双缓冲 |
| 隔离 | 内核拷贝限制**单个 handle 的输出范围**；本阶段任何进程仍可打开 `/dev/fb` 或创建重叠视图，不提供进程间图形权限隔离 |
| 未来演进 | window server 可沿用 `libgfx` 的局部坐标与绘图 API；可信 owner、授权委托、关闭 `/dev/fb` 旁路及合成策略须另行设计 |

选择私有缓冲是因为页表按 4 KiB 授权：直接 mmap 任意 fb 矩形会暴露同页的区外像素。`libgfx` 的 clip 只防误用，不能作为安全边界。`GFX_PRESENT` 按内核保存的视图边界拷贝，不信任用户态 handle 的坐标。

## 3. 组件与内核接口

| 文件 | 职责 |
|---|---|
| `kernel/include/uapi/gfx.h` | 唯一的跨边界 ABI：定宽结构、格式和 ioctl 号；libgfx 从 sysroot 引用同一头 |
| `kernel/include/driver/gfx.h`, `kernel/driver/gfx.c` | `/dev/gfx0` 实现；每个 open file 一个 `gfx_view`，保存矩形与配置状态 |
| `kernel/include/driver/fb.h`, `kernel/driver/fb.c` | 导出 fb 元信息及内核专用的按行写入辅助函数；现有 `/dev/fb` 行为不变 |
| `kernel/include/fs/devfs.h`, `kernel/fs/devfs.c` | 增加基于 `file_t *` 的设备 ioctl/release 回调，保留已有 node 回调 |
| `kernel/include/fs/file.h`, `kernel/fs/file.c` | `file_t` 保存设备私有指针；`fd_ioctl` 优先按 file 分发；最后一次 `file_put` 触发 release |
| `kernel/arch/x86_64/intr/trap.c` | `SYS_ioctl` 通过现有 `files_get_file()` 取得调用期间的引用，分发结束后 `file_put()`；不能直接从 fd 表取裸指针 |
| `kernel/arch/x86_64/platform/boot.c` | 与现有 `/dev/fb` 一起，在 devfs 初始化后注册 `/dev/gfx0`；不使用早于 devfs 的 `SUBSYS_INITCALL` |
| `libgfx/gfx.h`, `gfx.c`, `line.c`, `sprite.c` | 公有 API、缓冲管理和绘图 |
| `libgfx/Makefile`, `mk/components/sysroot.mk`, `user/Makefile`, `mk/components/user.mk` | 构建、暂存并发布库和头，链接 Tetris/测试程序，放入镜像 |
| `user/tetris.c` | 用新 API 代替直接 `/dev/fb` mmap；保留按差异重绘和游戏逻辑 |

分发契约：`devfs_ops` 新增 `ioctl_file(file_t *, cmd, arg)`、`release_file(file_t *)`。`devfs_open_node()` 的自定义 open 返回 `FD_DEV` 的 `file_t`，其 `node` 保留 devfs 节点引用，`dev_private` 指向本次打开的 `gfx_view`。`SYS_ioctl` 使用 `files_get_file()` 在 fd 表锁内增加引用，调用 `fd_ioctl()` 后 `file_put()`；这样并发 close 不能释放正在 present 的视图。`dup`/`fork` 共享同一 `file_t` 和视图；仅引用计数归零释放。由于像素缓冲属于普通用户态内存且没有设备 mmap，`munmap`/VMA 生命周期不持有视图；present 每次接收当前进程的用户指针，失效指针返回 `EFAULT`。`exec` 后 fd 遵循现有继承语义，新进程须自行提供有效缓冲。

`/dev/gfx0` 的 open 只分配未配置视图。`gfx_open(x,y,w,h)` 依次 open、`GFX_CREATE_VIEW`、`GFX_GET_INFO`、分配 `w*h*4` 缓冲；任一步失败均清理资源。创建只允许一次；宽高非零，并用 `x <= fb_w && w <= fb_w-x` 等减法形式防溢出。固定视图表最多 16 个；加锁保护分配、配置和最终释放。重叠视图按 present 顺序覆盖，不保证合成。

## 4. ABI 与用户态 API

```c
/* kernel/include/uapi/gfx.h；所有字段定宽，reserved 必须传 0 */
#define GFX_FORMAT_RGB32 0u
#define GFX_CREATE_VIEW  0x4701
#define GFX_GET_INFO     0x4702
#define GFX_PRESENT      0x4703

typedef struct { uint32_t x, y, w, h; } gfx_view_desc_t;
typedef struct {
    uint32_t width, height;  /* 已配置视图的局部尺寸 */
    uint32_t stride;         /* width * 4，字节 */
    uint32_t format;         /* GFX_FORMAT_RGB32 */
} gfx_info_t;
typedef struct {
    uint64_t pixels;         /* 本进程用户指针；每次调用重新提供 */
    uint32_t stride;         /* 必须等于 info.stride */
    uint32_t reserved;       /* 必须为 0 */
} gfx_present_req_t;
```

`GFX_CREATE_VIEW` 输入 `gfx_view_desc_t`；`GFX_GET_INFO` 输出 `gfx_info_t`；`GFX_PRESENT` 输入 `gfx_present_req_t`。未配置视图的 get-info/present 返回 `EINVAL`，未知命令返回 `ENOTTY`。结构入/出先调用 `syscall_check_user_range(..., writable)` 校验属于用户地址空间，再通过 `copy_from_user_ft`/`copy_to_user_ft` 传递，不直接解引用。present 验证地址、行跨度和总长度乘加不会溢出；**每一行**先以 `syscall_check_user_range(row_addr, row_bytes, false)` 拒绝内核地址或不可读区域，再用 `copy_from_user_ft` 读入内核临时行缓冲并写入 fb。校验是快照，fault-tolerant copy 仍处理并发 unmap。若中途读取失败，返回 `EFAULT`；前面行可能已更新，下一次完整 present 可恢复。内核不保留用户指针。

视图表锁只覆盖分配、配置和释放，不能跨 `syscall_check_user_range` 或 fault-tolerant copy 持有：present 在受锁保护下取得不可变的视图矩形快照，然后解锁进行逐行读取及写入；调用期 `file_t` 引用保证视图和设备状态仍有效。`copy_*_ft` 可能经 fault longjmp 返回，持有 spinlock 调用会泄漏锁。

```c
/* libgfx/gfx.h；handle 由库分配，调用方只持有指针 */
typedef struct gfx_handle gfx_handle_t;
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void gfx_close(gfx_handle_t *h);
gfx_info_t gfx_get_info(const gfx_handle_t *h);
int gfx_set_clip(gfx_handle_t *h, int32_t x, int32_t y, uint32_t w, uint32_t h_);
int gfx_present(gfx_handle_t *h);
void gfx_pixel(gfx_handle_t *h, int32_t x, int32_t y, uint32_t color);
void gfx_hline(gfx_handle_t *h, int32_t x, int32_t y, uint32_t w, uint32_t color);
void gfx_vline(gfx_handle_t *h, int32_t x, int32_t y, uint32_t h_, uint32_t color);
void gfx_line(gfx_handle_t *h, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
void gfx_rect(gfx_handle_t *h, int32_t x, int32_t y, uint32_t w, uint32_t h_, uint32_t color);
void gfx_fill_rect(gfx_handle_t *h, int32_t x, int32_t y, uint32_t w, uint32_t h_, uint32_t color);
void gfx_sprite_blit(gfx_handle_t *h, int32_t dx, int32_t dy,
                     const uint32_t *src, uint32_t src_stride,
                     uint32_t src_w, uint32_t src_h,
                     bool use_color_key, uint32_t color_key);
void gfx_sprite_blit_mask(gfx_handle_t *h, int32_t dx, int32_t dy,
                          const uint32_t *src, uint32_t src_stride,
                          const uint8_t *mask, uint32_t mask_stride,
                          uint32_t src_w, uint32_t src_h);
```

`gfx.h` 将点、水平/垂直线、矩形与填充矩形定义为 `static inline`，上表只展示签名。`gfx_line` 用整数 Bresenham，包含两端点和 8 个 octant。sprite 源仅接受 RGB32；`src_stride` 是字节数且至少 `src_w*4`；mask 每行至少 `ceil(src_w/8)` 字节，最高位对应最左像素，1 表示拷贝。`use_color_key=false` 是不透明 blit；为 true 时，包括黑色 `0` 在内的任意值都可作为透明色。调用方须保证源缓冲覆盖声明的尺寸；源指针无效属于程序错误。

库内缓冲 `pixels[local_y * stride + local_x * 4]` 从视图 `(0,0)` 开始，不含全屏偏移。所有 primitive 与 `[0,w)×[0,h)` 和库缓存的 clip 求交，空交集静默返回；用 64 位中间值处理 `x+w`。`gfx_set_clip` 只更新库内 clip，无 ioctl；越出视图的 clip 取交集，空 clip 合法。`gfx_present` 是唯一每帧 ioctl，内核始终呈现完整缓冲，不依据 clip 限制拷贝。缓冲初始全零。`gfx_close` 释放缓冲、关闭 fd、释放 handle。公有 API 不暴露可绕过 clip 的像素指针；即使进程篡改私有缓冲，内核仍限制输出矩形。

## 5. 错误与生命周期

| 情况 | 结果 |
|---|---|
| `/dev/gfx0` 不存在 | `gfx_open` 返回 `NULL`，`errno=ENODEV`（库把 `ENOENT` 归一化） |
| 零尺寸、越界或尺寸乘法溢出 | `gfx_open` 返回 `NULL`，`errno=EINVAL` |
| 16 个视图已占满 | `gfx_open` 返回 `NULL`，`errno=EMFILE` |
| 进程 fd 表已满 | `gfx_open` 返回 `NULL`，`errno=ENFILE`（沿用当前 `SYS_open` 的返回值） |
| 内核/用户态分配失败 | `gfx_open` 返回 `NULL`，`errno=ENOMEM` |
| ioctl 指针无效、present 缓冲不可读 | `-1`，`errno=EFAULT` |
| 未知 ioctl | `-1`，`errno=ENOTTY` |
| `gfx_set_clip` 收到空 handle | `-1`，`errno=EINVAL` |

现有 `libc/unistd/ioctl.c` 返回原始负 errno，不设置 `errno`；实现计划须修复该 wrapper，并回归既有 ioctl 调用。`gfx_present`/`gfx_set_clip` 成功返回 0，失败返回 -1 且设置 `errno`。绘图函数不返回错误；超出视图的部分被裁掉。`gfx_get_info(NULL)` 返回全零结构并设置 `errno=EINVAL`。

## 6. 构建与测试

`libgfx.a` 和公有 `gfx.h` 由新增 staging 组件暂存、写入 manifest，再由 `mk/components/sysroot.mk` **单一发布者**纳入不可变 sysroot generation；禁止库 Makefile 或根 Makefile 直接写最终 sysroot。UAPI 头走现有 kernel headers 安装路径。`user/Makefile` 对 Tetris 和 `test_gfx.elf` 显式链接 `-lgfx -lc`（静态库在使用它的对象之后）；`mk/components/user.mk` 的 `USER_PROGRAMS` 加入 `test_gfx`，镜像配方纳入它。terminal 链接规则本阶段不变。

**Hosttests**（接入现有 `make test-host`）：

- fake fb 验证零尺寸/越界/溢出、最多 16 个视图、独立状态、重叠视图顺序、dup/fork 共享及最后关闭释放；无效用户指针不能使内核崩溃。
- fake bitmap 验证点、水平/垂直线、矩形、Bresenham 八方向、完全/部分裁剪、透明黑色 color-key、mask 位序和源 stride。
- present 验证只修改视图内像素，左右相邻像素及同行 padding 不变；无效 stride/reserved/指针返回约定错误。

**QEMU**：在 `qemutests/run_test.py` 与 `mk/components/run.mk` 加 `gfx` suite，使用正常镜像，执行 `make test-qemu SUITE=gfx`。ring-3 测试程序创建全屏视图，绘制左红右绿及白色对角线；通过第二个 `/dev/fb` 映射读取指定坐标，并将紧凑 PASS/FAIL 标记输出到 serial，由 runner 验证。白线覆盖的采样点应为白，线外两侧分别为红/绿；不可把白线覆盖后的红绿数量写成各 `W*H/2`。另创建中央小视图并 present，读取其四周哨兵像素，确认区外不变；覆盖未配置和越界视图负例。测试不输出整帧字节流。

**回归门**：`make test-host`、`make test-qemu SUITE=phase-0`、`make test-qemu SUITE=gfx`；在 QEMU 手工验证 Tetris 移动、旋转、下落、消行、计分和退出后 terminal 恢复。terminal 仍直接使用 `/dev/fb`，需验证 Tetris 前后其绘制正常。修改 `file_t` 或 ioctl ABI 后先 `make clean` 再构建，避免无 header dependency 导致旧对象混用。

## 7. 实施顺序与风险

实施计划由 `writing-plans` 在用户确认本 spec 后单独产出。建议顺序：UAPI 与 fd 私有状态/释放分发 → `/dev/gfx0` 创建及 present → libgfx 与 sysroot 集成 → primitives 和 hosttests → QEMU suite → Tetris 迁移与回归。

主要风险是每次 present 拷贝整个视图，尤其 terminal 高频重绘时有开销；本阶段只迁移 Tetris，并在 QEMU 实测响应。后续可在保持绘图 API 的前提下加入 dirty-rect present。另一个风险是现有 `/dev/fb` 旁路和任意重叠视图：真实 window-server 权限模型须在合成器阶段一并解决，本阶段不得宣称完成安全隔离。

## 8. 后续文档

实现时更新 `docs/roadmap.md`、`docs/gui.md`、`docs/driver.md`、`docs/structure.md`，准确记录本阶段 API、测试入口和未实现的 owner/delegation 机制。文字/字体、alpha blend、多格式 fb、window server、鼠标光标与 aarch64 仍属独立后续工作。
