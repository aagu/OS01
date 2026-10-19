# P3 GUI-1 LVGL 平台后端与 libgui 架构设计规范

> **日期**: 2026-10-19  
> **状态**: 待用户复审（Draft v1）  
> **基准**: OS01 `5424ea69`；LVGL v9.5.0 (`85aa60d1`)；`docs/roadmap.md` P3 GUI-1 项  
> **前置依赖**: LVGL 初步移植 ✅；libgfx ✅；显示所有权协调协议 ✅ (`docs/gui/gui.md` §4)

---

## 1. 目标与范围

本设计落实 OS01 路线图（`docs/roadmap.md`）中的 **P3 GUI 任务 1（GUI-1 LVGL 平台后端）**。

### 1.1 核心目标
1. **组件库抽象与封装（libgui）**：
   将原有的三方库简单构建（`liblvgl`）扩展为 OS01 自主掌控的图形抽象库 **`libgui`**（产物为 `libgui.a`）。对外暴露纯净的 OS01 原生图形接口（`gui.h`、`gui_platform.h`），**完全隐藏 LVGL 的底层实现类型与头文件**（不向公共 sysroot 暴露 `lvgl/lvgl.h` 或任何 `lv_*` 对象指针）。
2. **底层平台生命周期与显示后端抽象**：
   从 `user/test_lvgl/test_lvgl.c` 中抽取可复用的底层平台管理逻辑，封装统一的平台上下文（`gui_platform_t`），提供生命周期（`create`/`destroy`）、单调时钟驱动、冲刷回调（`flush_cb`）及受控显示提交。
3. **支持屏幕与离屏双运行模式**：
   - **屏幕模式（On-screen）**：自动查询 `/dev/fb` 并打开 `/dev/gfx0`，严格遵从显示所有权协调协议（`gfx_client_policy`），处理前台抢占（`gfx_acquire`）、提交（`gfx_present`）、`EAGAIN` 限频重试、`EACCES` 失去所有权注销及 `ESTALE` 分辨率变更通知。
   - **离屏模式（Off-screen）**：纯内存影子画幅（Shadow Canvas），脱离 `/dev/fb` 与物理屏幕，支持精准的像素采样与断言（`gui_platform_get_pixel`），供自动化冒烟测试（smoke test）与 CI 门禁使用。
4. **控制反转（IoC）与事件驱动步进**：
   平台后端不强占用户态主循环。应用层完全自主拥有 `while (running)` 循环及 `poll()` 监听；平台层提供 `gui_platform_step()` 钩子，计算内部定时器所需的最小等待时间并返回给应用层作为 `poll()` 超时，实现无忙等的事件驱动，并为下一阶段 GUI-2 接入鼠标/键盘输入奠定基础。
5. **内存隔离与健壮性**：
   绘制缓冲（Partial Draw Buffer）及离屏画布统一采用匿名 `mmap` 分配，严禁侵占受限的用户态堆空间；退出时执行确定性的资源回收（`munmap`、`gfx_release`、`gfx_close`）。
6. **现有测试程序迁移验证**：
   重构 `/bin/test_lvgl`，将其移植到 `libgui` 接口上，消除重复样板代码，确保 `--smoke` 离屏断言与前台屏幕展示全绿。

### 1.2 非目标 (Non-Goals)
- **GUI-2 输入接入**：本任务不直接接入 `/dev/mouse` 或 `/dev/keyboard` 的读取与事件分发（留待 GUI-2 实施；本任务保证 `poll` 架构可无缝外接输入 fd）。
- **GUI-3 控件体系全面 Facade**：本任务不封装完整的上层控件集（如 Window、Button、Label 的 Facade 封装在 GUI-2/GUI-3 逐步落地；本阶段通过内部私有头文件桥接现有 `test_lvgl` 丰富控件）。
- **多窗口服务器与合成器（GUI-5）**：本任务保持单进程全屏图形架构，不引入多进程 IPC 与复合合成。
- **硬件 2D/3D GPU 加速**：保持纯软件渲染（`LV_USE_DRAW_SW=1`），向 `/dev/gfx0` 进行 2D blit。

---

## 2. 系统架构与构建体系集成

### 2.1 依赖关系与分层拓扑

```
+-------------------------------------------------------+
|  Applications: desktop.elf, test_lvgl.elf (Userland)  |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|        libgui.a (OS01 Native GUI Facade Library)     |
|   +-----------------------------------------------+   |
|   |  gui_platform: lifecycle, mmap buffer,        |   |
|   |  flush pipeline, step, pixel sampling         |   |
|   +-----------------------------------------------+   |
|   |  LVGL Core (v9.5.0 static submodule internals)|   |
+-------------------------------------------------------+
             |                                  |
             v                                  v
+------------------------+           +------------------+
|  libgfx.a (/dev/gfx0)  |           |  libc.a (OS01)   |
+------------------------+           +------------------+
             |                                  |
             +-----------------+----------------+
                               |
                               v
               +--------------------------------+
               | Kernel UAPI & System Call ABI  |
               +--------------------------------+
```

所有依赖严格单向无环：应用仅面向 `libgui`、`libgfx` 与 `libc` 编译，不接触底层的 LVGL 私有头文件。

### 2.2 目录与文件布局

```
libgui/
├── Makefile                       # 编译 LVGL 源码与 OS01 平台适配，输出 libgui.a
├── include/
│   └── gui/
│       ├── gui.h                  # 顶层公开统一头文件
│       ├── gui_platform.h         # 平台上下文、配置、状态码与 step/lifecycle 接口
│       └── internal/
│           └── gui_lvgl_private.h # 内部桥接头文件（暴露 lv_display_t，仅供内部及过度测试使用）
└── src/
    ├── platform.c                 # gui_platform_t 实现：mmap 管理、gfx 交互、flush_cb、step
    ├── tick.c                     # CLOCK_MONOTONIC 单调时钟注册
    └── config/
        └── lv_conf.h              # 既有 LVGL 定制配置（从 config/lv_conf.h 迁移或保留符号链接）
```

### 2.3 Sysroot Staging 与原子发布
1. **替换 staging 目标**：
   在 `mk/components/sysroot.mk` 中，将原 `$(STAMPS_DIR)/lvgl-install.stamp` 替换为：
   ```make
   $(STAMPS_DIR)/gui-install.stamp: $(STAMPS_DIR)/kernel-headers-install.stamp \
                                   $(STAMPS_DIR)/libc-install.stamp \
                                   $(STAMPS_DIR)/libgfx-install.stamp FORCE
   	@mkdir -p $(dir $@)
   	$(call os01_submake,libgui,install INSTALL_ROOT=$(STAGING_DIR)/gui $(OS01_SUBMAKE_ARGS))
   	$(call stamp_check,$(STAGING_DIR)/gui)
   ```
2. **安装产物规则**：
   - 静态库：`$(INSTALL_ROOT)/usr/lib/libgui.a`
   - 公共头文件：`$(INSTALL_ROOT)/usr/include/gui/gui.h`、`$(INSTALL_ROOT)/usr/include/gui/gui_platform.h`
   - 内部头文件：`$(INSTALL_ROOT)/usr/include/gui/internal/gui_lvgl_private.h`
   - 严格不发布 `thirdpart/lvgl` 下的四百余个私有头文件到公共包含路径中。
3. **应用编译参数规范**：
   - 移除原有应用在 `user/Makefile` 中添加的 `-llvgl`，统一修改为 `-lgui -lgfx -lc`；
   - 引用路径：`-I$(TARGET_INCDIR)`，应用源码只需 `#include <gui/gui.h>`。

---

## 3. 详细接口设计 (API Specification)

### 3.1 公共头文件：`include/gui/gui_platform.h`

```c
#ifndef _GUI_PLATFORM_H
#define _GUI_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 平台运行模式枚举
 */
typedef enum {
    GUI_MODE_ONSCREEN  = 0,  /* 屏幕模式：绑定 /dev/fb 与 /dev/gfx0，受前台协调器管控 */
    GUI_MODE_OFFSCREEN = 1,  /* 离屏模式：纯内存画布，用于自动化单测、headless smoke */
} gui_mode_t;

/**
 * @brief 平台单步运行及事件状态码
 */
typedef enum {
    GUI_STATUS_OK              = 0,   /* 正常推进，画面已按需提交 */
    GUI_STATUS_EAGAIN          = 1,   /* 前台暂时被限频或正在争用，需保持视图并重试 */
    GUI_STATUS_LOST_OWNERSHIP  = 2,   /* 失去前台所有权 (EACCES)，应退出或转入后台 */
    GUI_STATUS_DISPLAY_CHANGED = 3,   /* 分辨率或内核视图失效 (ESTALE)，需重建或重启 */
    GUI_STATUS_ERROR           = -1   /* 严重底层设备或内存错误 */
} gui_status_t;

/**
 * @brief 平台初始化配置参数
 */
typedef struct {
    gui_mode_t mode;               /* 运行模式 (默认 GUI_MODE_ONSCREEN) */
    uint32_t width;                /* 宽度：离屏模式必填；屏幕模式若为 0 则自动读 /dev/fb */
    uint32_t height;               /* 高度：离屏模式必填；屏幕模式若为 0 则自动读 /dev/fb */
    uint32_t buffer_lines;         /* 局部绘制缓冲行数，若为 0 则使用默认值 40 行 */
    bool auto_acquire;             /* 屏幕模式下是否在首次 present 时自动 acquire 前台 */
} gui_config_t;

/* 不透明平台上下文句柄 */
typedef struct gui_platform gui_platform_t;

/**
 * @brief 初始化并创建 GUI 平台后端上下文
 *
 * @param cfg 配置参数指针，若为 NULL 则使用屏幕模式默认参数
 * @return 成功返回有效句柄，失败返回 NULL 并设置 errno
 */
gui_platform_t *gui_platform_create(const gui_config_t *cfg);

/**
 * @brief 销毁平台后端上下文，释放所有绘制缓冲、映射与设备句柄
 *
 * @param platform 待销毁的平台句柄指针（允许为 NULL）
 */
void gui_platform_destroy(gui_platform_t *platform);

/**
 * @brief 查询当前平台的有效像素宽度
 */
uint32_t gui_platform_get_width(const gui_platform_t *platform);

/**
 * @brief 查询当前平台的有效像素高度
 */
uint32_t gui_platform_get_height(const gui_platform_t *platform);

/**
 * @brief 查询当前平台运行模式
 */
gui_mode_t gui_platform_get_mode(const gui_platform_t *platform);

/**
 * @brief 单步迭代 GUI 定时器、脏矩形光栅化与受控帧呈现
 *
 * @param platform 平台上下文句柄
 * @param out_next_delay_ms 输出参数：建议的下一次事件 poll 等待时间（毫秒）。可为 NULL
 * @return 运行状态码 (gui_status_t)
 */
gui_status_t gui_platform_step(gui_platform_t *platform, uint32_t *out_next_delay_ms);

/**
 * @brief 在离屏模式下读取指定坐标处的像素值 (XRGB8888 格式)
 *
 * @param platform 平台上下文句柄
 * @param x 横坐标
 * @param y 纵坐标
 * @return 成功返回 32 位颜色值 (0x00RRGGBB)；越界或非离屏模式返回 0xFFFFFFFFu
 */
uint32_t gui_platform_get_pixel(const gui_platform_t *platform, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_PLATFORM_H */
```

### 3.2 顶层统一头文件：`include/gui/gui.h`

```c
#ifndef _GUI_H
#define _GUI_H

#include <gui/gui_platform.h>

#endif /* _GUI_H */
```

### 3.3 内部桥接头文件：`include/gui/internal/gui_lvgl_private.h`

```c
#ifndef _GUI_LVGL_PRIVATE_H
#define _GUI_LVGL_PRIVATE_H

#include <gui/gui_platform.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取与平台关联的内部 lv_display_t 句柄
 *
 * 注意：此函数仅供内部单元测试及 GUI-1/GUI-2 过渡期间的 test_lvgl 使用。
 * 公共业务应用禁止直接使用。
 */
lv_display_t *gui_platform_get_lv_display(gui_platform_t *platform);

#ifdef __cplusplus
}
#endif

#endif /* _GUI_LVGL_PRIVATE_H */
```

---

## 4. 平台核心实现与契约设计

### 4.1 数据结构定义（`src/platform.c` 内部）

```c
struct gui_platform {
    gui_mode_t mode;
    uint32_t width;
    uint32_t height;
    uint32_t buffer_lines;
    
    /* 绘制缓冲（匿名 mmap 分配） */
    uint32_t *draw_buf;
    size_t draw_buf_bytes;
    
    /* 离屏模式影子画幅（匿名 mmap 分配，仅离屏模式有效） */
    uint32_t *shadow_canvas;
    size_t shadow_canvas_bytes;
    
    /* 屏幕模式关联设备 */
    gfx_handle_t *gfx;
    bool acquired;
    uint64_t retry_deadline_ms;   /* EAGAIN 限频退避截止时间 (CLOCK_MONOTONIC ms) */
    
    /* 内部关联对象 */
    lv_display_t *disp;
    uint32_t flush_count;
};
```

### 4.2 缓冲区内存管理
1. **Partial Draw Buffer**：
   - 行数配置：`lines = (cfg && cfg->buffer_lines > 0) ? cfg->buffer_lines : 40;`
   - 容量计算：`draw_buf_bytes = width * lines * sizeof(uint32_t);`
   - 分配调用：`draw_buf = mmap(NULL, draw_buf_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);`
   - 绑定至 LVGL：
     `lv_display_set_buffers(disp, draw_buf, NULL, draw_buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);`
2. **Shadow Canvas**：
   - 仅在 `mode == GUI_MODE_OFFSCREEN` 时创建：
     `shadow_canvas_bytes = width * height * sizeof(uint32_t);`
     `shadow_canvas = mmap(NULL, shadow_canvas_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);`
     `memset(shadow_canvas, 0, shadow_canvas_bytes);`
3. **安全释放**：
   `gui_platform_destroy()` 中依次检查并调用 `munmap(draw_buf, draw_buf_bytes)` 与 `munmap(shadow_canvas, shadow_canvas_bytes)`，零内存泄露。

### 4.3 冲刷回调（Flush Pipeline）
冲刷回调函数 `static void platform_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)`：
1. **参数防卫与断言**：
   - 验证 `area->x1 <= area->x2 && area->y1 <= area->y2`；
   - 验证坐标在屏幕边界内：`0 <= area->x1`，`area->x2 < width`，`0 <= area->y1`，`area->y2 < height`；
   - 验证行跨度：`stride_bytes = lv_draw_buf_width_to_stride(area_w, LV_COLOR_FORMAT_XRGB8888);` 必须等于 `area_w * 4`。
2. **数据搬运**：
   - **屏幕模式**：若 `platform->gfx` 有效，调用：
     ```c
     gfx_sprite_blit(platform->gfx, area->x1, area->y1,
                     (const uint32_t *)px_map, stride_bytes,
                     (uint32_t)area_w, (uint32_t)area_h, false, 0);
     ```
   - **离屏模式**：若 `platform->shadow_canvas` 有效，逐行复制：
     ```c
     for (int32_t y = area->y1; y <= area->y2; y++) {
         const uint32_t *src_row = (const uint32_t *)(px_map + (y - area->y1) * stride_bytes);
         uint32_t *dst_row = platform->shadow_canvas + y * platform->width + area->x1;
         memcpy(dst_row, src_row, area_w * sizeof(uint32_t));
     }
     ```
3. **完成通告**：
   `platform->flush_count++;`
   `lv_display_flush_ready(disp);`

### 4.4 显示呈现与所有权协调契约
在屏幕模式下，`gui_platform_step()` 负责执行画面呈现，严格套用 `gfx_client_policy`：
1. **退避检查**：
   - 获取当前单调毫秒 `now = get_monotonic_ms();`
   - 若 `retry_deadline_ms != 0 && now < retry_deadline_ms`，直接返回 `GUI_STATUS_EAGAIN`，不向内核发起提交。
2. **前台抢占（Acquire）**：
   - 若尚未获取所有权（`!platform->acquired`），调用 `gfx_acquire(platform->gfx)`：
     - 若返回 0：标记 `platform->acquired = true`；
     - 若返回 -EBUSY：返回 `GUI_STATUS_EAGAIN`；
     - 若返回其它错误：关闭 gfx 并返回 `GUI_STATUS_ERROR`。
3. **画面呈现（Present）**：
   - 调用 `gfx_present(platform->gfx)`：
     - **返回 0（成功）**：清空 `platform->retry_deadline_ms = 0`，返回 `GUI_STATUS_OK`；
     - **返回 -EAGAIN**：调用 `gfx_client_present_policy(EAGAIN)`，设定 `platform->retry_deadline_ms = now + GFX_CLIENT_RETRY_MS`（250ms），返回 `GUI_STATUS_EAGAIN`；
     - **返回 -EACCES**：前台所有权被抢占，自动执行 `gfx_release` 与 `gfx_close`，置 `platform->gfx = NULL`，返回 `GUI_STATUS_LOST_OWNERSHIP`；
     - **返回 -ESTALE**：显示模式改变，同样执行清理后返回 `GUI_STATUS_DISPLAY_CHANGED`；
     - **其它错误**：返回 `GUI_STATUS_ERROR`。

### 4.5 定时器与 Poll 等待计算
1. 在 `gui_platform_step()` 中调用 `uint32_t lv_delay = lv_timer_handler();`。
2. 若 `out_next_delay_ms != NULL`：
   - 基础值取 `lv_delay`（若返回 `LV_NO_TIMER_READY` 则取默认 100ms）；
   - 若处于 EAGAIN 退避期，取 `min(lv_delay, retry_deadline_ms - now)`；
   - 设定安全上限（如最大 100ms），避免应用长时间不可唤醒；
   - 写入 `*out_next_delay_ms`。

---

## 5. 测试与验证计划

### 5.1 宿主单元测试 (`hosttests/cases/test_gui_platform.c`)
利用宿主 mock 环境（不依赖 QEMU）覆盖 `gui_platform` 核心逻辑：
1. **参数防卫测试**：
   - 传入非法配置（如离屏模式宽高为 0），验证返回 NULL 且 errno 为 `EINVAL`。
2. **离屏生命周期与高频往返**：
   - 连续执行 50 次 `gui_platform_create(OFFSCREEN)` 与 `gui_platform_destroy()`，验证 mmap 释放完整无内存泄漏。
3. **定时与单步推进**：
   - 运行 5 次 `gui_platform_step()`，断言返回 `GUI_STATUS_OK`，断言 `out_next_delay_ms > 0`。
4. **离屏像素采样精确断言**：
   - 在离屏画布上通过内部接口绘制基准像素，断言 `gui_platform_get_pixel()` 返回值与设定值完全一致；
   - 测试越界坐标，断言返回 `0xFFFFFFFFu`。

### 5.2 `/bin/test_lvgl` 重构与 QEMU 端到端验证
重构 `user/test_lvgl/test_lvgl.c`：
1. 编译链接由 `-llvgl` 改为 `-lgui -lgfx -lc`；
2. 消除原有的 ~200 行设备打开、mmap 缓冲分配、所有权轮询及重复的 flush 回调代码，全面改调 `gui_platform_*` 接口；
3. **双重运行验证**：
   - **离屏冒烟测试**：
     运行 `test_lvgl --smoke`，验证输出 `[TEST PASS] LVGL compatibility smoke test succeeded.`，退出码为 0，耗时不超过 1 秒；
   - **屏幕端到端测试**：
     在 QEMU 下启动 `test_lvgl`，验证界面正确呈现 Google 4 色与 Material 卡片，按 `q` 退出后控制台 Alt-screen 完美恢复。

---

## 6. 自审清单（Spec Self-Review Checklist）

- [x] **占位符扫描**：全文无 "TBD"、"TODO" 或未定实现项，所有结构体、枚举与流程均已明确给出。
- [x] **内部一致性**：所有权状态机判定与既有 `gfx_client_policy` 及 2026-10-11 显示所有权协议完全一致。
- [x] **范围聚焦**：严格限制在 GUI-1（平台后端抽取、缓冲管理、步进驱动与离屏断言），输入留给 GUI-2，控件 Facade 留给 GUI-3，不越界膨胀。
- [x] **歧义消除**：明确了 `gui_lvgl_private.h` 的定位与边界，杜绝任何外部应用感知内部 LVGL 对象指针。
