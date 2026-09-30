# 2D 图形 API 设计

> **日期**: 2026-09-30
> **状态**: 已批准（待用户复审）
> **基准**: `docs/roadmap.md` P3 GUI "2D 图形 API" 项

---

## 1. 动机

OS01 当前 `/dev/fb0` 直接 mmap 到用户态，Tetris (`user/tetris.c`) 与 terminal (`user/terminal.c`) **手工**写像素:

- 两份代码各自实现 fill_rect / blit；同一份 Bresenham 不存在
- 未来 window server 需要**有界的 fb 视图**(每个 client 一块 sub-region),目前 `/dev/fb0` 直接 mmap 没有边界约束
- 切换 drawing 路径(从 raw fb 到 compositor)时,app 代码要全改

本次目标:提供一个**最小可用的 2D primitives 库**,既给现在 Tetris/terminal 用,也为未来 window server 的有界 sub-region 委托留好接口。文字/字体/合成器是独立 roadmap 项,本设计**不包含**。

## 2. 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 放置 | **Split:userspace `libgfx.a` + 内核 fb-handle 子系统** | `libgfx.a` 提供 primitives API;内核 `/dev/gfx0` 给 apps 一个**有界**的 fb view。未来 window server 成为 `/dev/fb0` 的 privileged owner,通过同一个 `/dev/gfx0` handle API 把 sub-region 派给 client → `libgfx.a` 接口稳定 |
| 2 | Primitives 集合 | pixel / hline / vline / line(Bresenham) / rect / fill_rect / sprite_blit(color-key) / sprite_blit_mask(1-bit alpha) | roadmap 原文 "画线/矩形/位图 blit"。sprite_blit 涵盖不透明 blit 的特例(无 mask 即等同 memcpy-style blit) |
| 3 | 颜色抽象 | **直接以 fb 原生格式为内部表示**,不做统一 RGBA32 中转 | OS01 fb 当前是单一 32 bpp raw RGB;零开销,app 不需 if-else |
| 4 | 内核↔用户态 dispatch | **mmap-fast + handle-bounded**(primitive 直接写 mmap 指针,ioctl 只用于 state/present) | Tetris 每 tick 约 600 次 fill_rect,syscall-per-primitive 不可接受;libgfx.a 几乎全是 `static inline`,ioctl 仅 `GFX_GET_INFO / GFX_SET_CLIP / GFX_PRESENT` |
| 5 | Arch 覆盖 | x86_64 only | aarch64 还没有 fb 驱动;接口预留 arch-neutral 但本阶段不实现 |
| 6 | 双缓冲/vsync | 在本项实现 | `GFX_PRESENT` ioctl;QEMU stdvga 无 vsync 则立即返回 |
| 7 | 鼠标光标 | 不在本项 | `/dev/mouse` 已有(2026-09-27 闭环),光标绘制由未来 window server 负责 |
| 8 | 库形态 | 静态 `.a` | OS01 暂无动态链接器(P5 项) |

## 3. 架构

```
┌─────────────────────────────────────────────────────────────┐
│  user/tetris.c, user/terminal.c, future GUI apps           │
└─────────────────────────────────────────────────────────────┘
                          │  link -lgfx
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  libgfx.a                                                  │
│  ┌──────────────┬──────────────┬──────────────┬──────────┐ │
│  │ gfx.h        │ gfx.c        │ line.c       │ sprite.c │ │
│  │ (inlines)    │ open/close/  │ Bresenham    │ color-key│ │
│  │ pixel/hline/ │ info/        │              │ + 1-bit  │ │
│  │ vline/       │ present/     │              │ alpha    │ │
│  │ fill_rect    │ set_clip     │              │ mask     │ │
│  └──────────────┴──────────────┴──────────────┴──────────┘ │
└─────────────────────────────────────────────────────────────┘
                          │  open("/dev/gfx0"), mmap, ioctl
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  kernel/driver/fb_handle.c   kernel/include/fb/handle.h │
│  ─ /dev/gfx0 devfs 节点                                       │
│  ─ gfx_handles[] 固定表 (max 16)                              │
│  ─ fb_handle_open / release / mmap / ioctl                   │
│  ─ fb_mmap_install_pages() 共享给 fb.c 使用                  │
└─────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  kernel/driver/fb.c  (extract fb_mmap_install_pages,         │
│                       export fb_get_info)                   │
└─────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│  QEMU stdvga MMIO  (existing)                              │
└─────────────────────────────────────────────────────────────┘
```

**关键性质**:除 `gfx_line / gfx_sprite_blit / gfx_present` 外,每个 primitive **零 syscall**(inlined 指针运算)。`gfx_present` 是每帧唯一过内核边界的路径,且只一次 ioctl。

## 4. 组件

### 4.1 Kernel

| 文件 | 状态 | 内容 |
|---|---|---|
| `kernel/include/fb/handle.h` | new | opaque type `gfx_handle_id_t`、`gfx_info_t` 结构、ioctl 编号(`GFX_GET_INFO / GFX_SET_CLIP / GFX_PRESENT`) |
| `kernel/driver/fb_handle.c` | new | `gfx_handles[16]` 表、`fb_handle_open/release/mmap/ioctl`、`devfs_ops fb_handle_ops`、`SUBSYS_INITCALL()` 注册 `/dev/gfx0` |
| `kernel/driver/fb.c` | modified | 把 `fb_mmap()` 内的 page install 循环抽成 `fb_mmap_install_pages(user_pgd, vma, fb_phys, length)`;新增并 export `fb_get_info(fb_info_t *)` |
| `kernel/include/driver/fb.h` | modified | 声明 `fb_mmap_install_pages` 和 `fb_get_info` |

### 4.2 Userspace

| 文件 | 状态 | 内容 |
|---|---|---|
| `libgfx/gfx.h` | new | public header:`gfx_info_t` 结构、`gfx_handle_t` 透传 struct(小到可直接放 header)、`gfx_pixel/hline/vline/fill_rect` 静态内联、其它 API 函数声明 |
| `libgfx/gfx.c` | new | `gfx_open / gfx_close / gfx_get_info / gfx_set_clip / gfx_present`(前两个走 open/mmap/close,后三个走 ioctl);`gfx_open` 用 `malloc` 分配 `gfx_handle_t`、`gfx_close` 释放 |
| `libgfx/line.c` | new | `gfx_line()` Bresenham 全整数实现,处理 8 个 octant,clip-aware |
| `libgfx/sprite.c` | new | `gfx_sprite_blit()`(color-key,`transparent_color == 0` 表示不透明),`gfx_sprite_blit_mask()`(1-bit alpha mask buffer) |
| `libgfx/Makefile` | new | 编译静态 archive `libgfx.a`(gfx.c + line.c + sprite.c) |
| `Makefile`(root) | modified | 暴露 `$(LIBGFX)` 变量,user apps 通过 `LDLIBS += $(LIBGFX)` 引入 |
| `user/tetris.c` | refactored | 用 `libgfx.a` 重写渲染;**保留行为完全一致**(人工回归) |

### 4.3 ioctl 集

| Cmd | Args | 行为 |
|---|---|---|
| `GFX_GET_INFO` | `struct gfx_info_t *out` | 写 width/height/stride/format |
| `GFX_SET_CLIP` | `struct gfx_clip_t {x,y,w,h} *in` | 存 advisory clip rect(供 libgfx 缓存、内联快速路径使用;**内核不 trap**) |
| `GFX_PRESENT` | none | 阻塞等待 vsync;QEMU stdvga 无 vsync 时立即返回 0;预留真实 hw double-buffer swap |

### 4.4 API(`libgfx/gfx.h` 形态)

```c
// gfx_info_t — fb 元信息
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;     // bytes per row
    uint32_t format;     // 0 = raw RGB 32 bpp (Phase 1 only)
} gfx_info_t;

// gfx_handle_t — 不透明 struct;libgfx 自管理
typedef struct {
    int             fd;
    uint8_t        *pixels;   // mmap'd fb pointer
    gfx_info_t      info;
    int32_t         clip_x, clip_y, clip_w, clip_h;
} gfx_handle_t;

// Open/close/ioctl wrappers (libgfx/gfx.c)
gfx_handle_t *gfx_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void                gfx_close(gfx_handle_t *h);
gfx_info_t         gfx_get_info(const gfx_handle_t *h);
int                 gfx_set_clip(gfx_handle_t *h,
                                  int32_t x, int32_t y,
                                  int32_t w, int32_t h);
int                 gfx_present(gfx_handle_t *h);

// Inlined primitives (gfx.h, zero-syscall)
static inline void gfx_pixel(gfx_handle_t *h,
                              int32_t x, int32_t y, uint32_t color);
static inline void gfx_hline(gfx_handle_t *h,
                              int32_t x, int32_t y,
                              uint32_t w, uint32_t color);
static inline void gfx_vline(gfx_handle_t *h,
                              int32_t x, int32_t y,
                              uint32_t h_, uint32_t color);
static inline void gfx_fill_rect(gfx_handle_t *h,
                                  int32_t x, int32_t y,
                                  uint32_t w, uint32_t h_,
                                  uint32_t color);
static inline void gfx_rect(gfx_handle_t *h,
                             int32_t x, int32_t y,
                             uint32_t w, uint32_t h_,
                             uint32_t color);   // 4 hline + 2 vline

// Function primitives (libgfx/line.c, libgfx/sprite.c)
void gfx_line(gfx_handle_t *h,
              int32_t x0, int32_t y0,
              int32_t x1, int32_t y1,
              uint32_t color);
void gfx_sprite_blit(gfx_handle_t *h,
                      int32_t dx, int32_t dy,
                      const void *src, uint32_t src_stride,
                      uint32_t src_w, uint32_t src_h,
                      uint32_t src_format,
                      uint32_t transparent_color);
void gfx_sprite_blit_mask(gfx_handle_t *h,
                           int32_t dx, int32_t dy,
                           const void *src, uint32_t src_stride,
                           const uint8_t *mask, uint32_t mask_stride,
                           uint32_t src_w, uint32_t src_h,
                           uint32_t src_format);
```

**Color format (Phase 1)**:`format == 0` 表示 raw RGB 32 bpp,`color` 是 host-endian 的 32-bit 值(逐字节写到 fb 的 4-byte pixel)。`gfx_pixel` 等不解析 color 内容,直接写 32-bit 字(匹配 fb.c 当前的 `Pos.XResolution * 4` 32 bpp 假设)。多格式 fb 支持在独立的未来工作项(取决于 fb 驱动扩展),不属本 spec 范围。

**clip-aware inlines**:`gfx_pixel/hline/vline/fill_rect/rect` 都先检查 `[x, x+w) × [y, y+h)` 与 `h->clip_*` 的交集,只写入交集内的像素。**Clip 是 advisory**:libgfx 自己检查,内核不 trap。Out-of-bounds 像素静默 drop。

## 5. 数据流

```
App libgfx.a                            Kernel
────────────────────────────────────────────────────────────────────────────
gfx_handle_t *h = gfx_open(
  0, 0, fb_w, fb_h)         ──► open("/dev/gfx0", O_RDWR) ───► fb_handle_open()
                                                                       alloc slot
                                                                       bounds = (0,0,W,H)
                                                                       ref_count = 1
                                                                       ◄── fd
                              ◄── fd
                              mmap(NULL, fb_size, PROT_READ|WRITE,
                                   MAP_SHARED, fd, 0)        ───► fb_handle_mmap()
                                                                       bounds check
                                                                       fb_mmap_install_pages(
                                                                         pgd, vma,
                                                                         fb_phys,
                                                                         vma_size)
                                                                       ◄── ptr
                              ◄── ptr → h->pixels

gfx_fill_rect(h, 10,10,50,50, RED)  ──►  inline: clip intersect
                                        memset loop over h->pixels
                                        NO syscall

gfx_line(h, 0,0,100,100, GREEN)    ──►  gfx_line() Bresenham in libgfx.a
                                        NO syscall

gfx_present(h)                     ──►  ioctl(GFX_PRESENT)   ───► kernel vsync wait
                                                                       ◄── 0

gfx_close(h)                       ──►  close(fd)           ───► fb_handle_release()
                                                                       ref_count--
                                                                       free slot
                              ◄── 0
```

**`gfx_handle_t` 是 heap-allocated struct**:`gfx_open` 通过 `malloc` 分配并填充,返回指针;`gfx_close` 释放内部 fd(close)、munmap 像素、再 `free` struct 本身。caller 不需关心大小,只持有指针。

## 6. 错误处理

| 失败 | 检测位置 | 调用方看到 |
|---|---|---|
| `/dev/gfx0` 不存在(fb 驱动未注册) | `fb_handle_open` | `gfx_open` 返回 `NULL`,`errno=ENODEV` |
| Bounds 越界(`x+w > fb_w`) | `fb_handle_open` | `NULL`,`errno=EINVAL` |
| Handle slot 表满(16) | `fb_handle_open` | `NULL`,`errno=ENOMEM` |
| mmap 失败 | `fb_handle_mmap` | 经 `mmap()` libc wrapper 传 `MAP_FAILED`,`errno` 设置;`gfx_open` 返回 `NULL` |
| ioctl 未知 cmd | `fb_handle_ioctl` | `-ENOTTY` |
| `gfx_set_clip/present` 失败 | kernel | `-1`,`errno` 由 syscall 设置 |
| Primitive 写入越界或 handle 无效 | `libgfx.a` 内联守卫 | 静默 no-op(clip 交集为空)或静默 clip;`debug` 构建(`-DGFX_DEBUG`)加 `assert` |
| 调用方传错误格式的 color | — | **不检测**(程序员错误);`gfx.h` 文档说明:调用方负责按 `gfx_get_info()` 报告的 format 给 color |
| 进程持 handle 退出 | kernel `release()` 由 fd 关闭触发 | 内核清理 |

**约定**:
- 可能失败的入口函数(`gfx_open`)返回 `NULL` + `errno`
- 其它 wrapper(`gfx_set_clip`、`gfx_present`)返回 `int`(`0` 成功,`-1` 失败 + `errno`)
- 内联 primitives `void` 返回;越界静默 clip
- 函数 primitives(`gfx_line`、`gfx_sprite_blit*`)`void` 返回;clip 由 libgfx 处理

## 7. 测试

### 7.1 Unit (hosttests)

**`hosttests/cases/test_gfx_handle.c`** — fake fb struct,无真实 MMIO:

| Suite | Asserts |
|---|---|
| `gfx_handle_open` | NULL+ENODEV 当 fb 未注册;NULL+EINVAL 越界;slot 表满返回 NULL+ENOMEM;每个 slot fd 唯一 |
| `bounds validation` | `x+w > fb_w`、`y+h > fb_h`、`w==0`、负值都拒绝 |
| `ref-count` | open+close=slot freed;两个 open 给两个独立 fd |
| `ioctl GFX_GET_INFO` | 返回与 fake fb info 一致 |
| `ioctl GFX_SET_CLIP` | 接受任意 rect;不影响其它 handle |
| `ioctl GFX_PRESENT` | QEMU-no-vsync 路径返回 0 |
| `mmap bounds` | 合法 fd 进 mmap 正常填充 VMA;非法 fd 拒绝;VMA 超出 handle bounds 拒绝 |

**`hosttests/cases/test_gfx_primitives.c`** — fake bitmap:

| Suite | Asserts |
|---|---|
| `pixel/hline/vline/fill_rect` | 像素写入正确;越界静默 clip |
| `gfx_line` Bresenham | horizontal、vertical、45°、steep+、gentle+、4 象限全覆盖;端点一致 |
| `gfx_sprite_blit` color-key | 透明像素跳过、非透明像素写入;越界 clip;sprite 完全在 fb 外 → no-op |
| `gfx_sprite_blit_mask` 1-bit alpha | mask=1 拷贝,mask=0 跳过;clip 语义同上 |

### 7.2 Integration (qemutests)

**`qemutests/gfx_api_basic.sh`** — 新增,加入 `qemutests/run_all.sh`:

`user/test_gfx.c` ring-3 app:
1. Open handle covering full fb
2. `gfx_fill_rect(0, 0, W/2, H, RED)` 左半红
3. `gfx_fill_rect(W/2, 0, W/2, H, GREEN)` 右半绿
4. `gfx_line(0, 0, W-1, H-1, WHITE)` 对角
5. `gfx_present()`
6. Open second handle,读回 fb 字节流到 serial
7. `gfx_close`

Host-side script 验证 serial output:
- RED 占 `W*H/2` 像素且 RGB 字节序正确
- GREEN 同上
- WHITE 对角线 Bresenham 像素采样位置正确

**`qemutests/gfx_api_clip.sh`** — 新增:

1. Open handle 全 fb
2. `gfx_set_clip(W/4, H/4, W/2, H/2)`
3. `gfx_fill_rect(0, 0, W, H, RED)` — 只 fill clip 内
4. 读回验证:仅中央 W/2×H/2 区域是红色

**Tetris 视觉/输入回归是手动的**(`user/tetris.c` 改用 `libgfx.a` 后):
- Developer 在 QEMU 中手工 `exec /bin/tetris`,玩几分钟
- 确认移动/旋转/下落/消行/计分与 refactor 前完全一致
- 退出终端内容恢复仍正常
- 不写自动化 tetris GUI 测试(GUI 不适合 shell-script 验证)

### 7.3 覆盖矩阵

| Component | hosttest | qemu | systest |
|---|---|---|---|
| `kernel/driver/fb_handle.c` | ✅ | ✅ basic + clip | — |
| `libgfx/gfx.c` open/close/ioctl | (via qemu) | ✅ basic | — |
| `libgfx/gfx.h` inlines | ✅ | ✅ basic | — |
| `libgfx/line.c` Bresenham | ✅ | ✅ basic | — |
| `libgfx/sprite.c` blit | ✅ | (follow-up;鼠标光标时再扩) | — |
| `user/tetris.c` 行为 | (manual) | (manual) | — |

### 7.4 合并门

- `make hosttest` 全过
- `make qemutest` 全过(`tetris_basic.sh` 等既有的继续过,新加 basic + clip)
- Tetris refactor 后:developer 手工玩一次 + 在 PR 描述附"screenshots before/after"或"no visual diff observed"

## 8. 风险

| 风险 | 缓解 |
|---|---|
| Handle layer 是 fb 之上**新**代码路径;bounds 校验 bug 可能破坏 fb 内存或崩内核 | 所有 bounds check 有 hosttest 覆盖;qemu 集成测试读回 fb 字节流验证无破坏 |
| 内联 primitives 的 clip 检查是 advisory,buggy 调用方可能写到预期外 | header 文档明示;debug 构建加 assert |
| `gfx_present` 在 QEMU 上是 no-op(无真实 vsync) | 真实 hw 验证延后;QEMU 改用 serial 读回 fb 字节流验证 |
| Phase 1 单 32 bpp 假设:从 RGB16_565 源 sprite blit 不可用 | 在 `gfx.h` 文档注明;fb 多格式支持是独立未来工作项,不属本 spec |
| Tetris refactor 引入回归风险 | 已有 `qemutests/tetris_basic.sh` 在 refactor 后必须继续过;GUI 行为由 developer 手工验证 |

## 9. 实施步骤(概要)

不是 plan;plan 由 `writing-plans` skill 在 spec 批准后单独产出。

粗略顺序(每步独立 commit,通了再推进):
1. **fb.c extract**:抽 `fb_mmap_install_pages` + export `fb_get_info`,hosttest 覆盖(`make clean && make`)
2. **fb_handle.c 骨架**:`/dev/gfx0` devfs 节点、open/release/ioctl,不实现 mmap;hosttest 覆盖
3. **fb_handle.c mmap**:完成 mmap + bounds 校验;hosttest 覆盖
4. **libgfx 骨架**:`libgfx/` 目录、Makefile、根 Makefile 接入;`gfx_open/close/info` 三个 wrapper
5. **libgfx primitives**:pixel/hline/vline/fill_rect 内联 + `line.c` Bresenham;hosttest 覆盖
6. **libgfx sprite**:color-key + 1-bit alpha;hosttest 覆盖
7. **qemutest 集成**:`test_gfx.c` + `gfx_api_basic.sh` + `gfx_api_clip.sh` 落地
8. **Tetris refactor**:`user/tetris.c` 改用 `libgfx.a`;developer 手工回归
9. **`GFX_PRESENT` 真实 hw 路径**(若 QEMU 不再够用):延后到真实硬件适配期

## 10. 不在本项范围

- 字体渲染 / 可缩放文字 → roadmap 独立项,依赖 2D API
- Window server + compositor → roadmap 独立项,依赖 2D + font + mouse
- 鼠标光标绘制 → `/dev/mouse` 已闭环,光标绘制由未来 window server 合成
- Alpha blend(>1 bit) → YAGNI;sprite_blit_mask 已覆盖 1-bit 透明
- 多格式 fb(RGB16_565 / RGB24 / ARGB32 任意一种以上) → 假设 OS01 fb 单一 32 bpp;Phase 2 再扩
- 动态链接 `libgfx.so` → 需先有 dynamic linker(P5)
- aarch64 实现 → 接口预留 arch-neutral,本阶段 x86_64 only

## 11. 文档更新

- `docs/roadmap.md` P3 GUI 表:"2D 图形 API" 行加 ✅ + 链接本 spec
- `docs/gui.md`:新增 "libgfx.a" 章节,记录 API 概述 + 链接 spec
- `docs/driver.md`:新增 "fb handle subsystem" 章节,与现有 `/dev/fb0` 章节并列
- `docs/structure.md`:`libgfx/` 加入目录列表