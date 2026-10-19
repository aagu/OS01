# P3 GUI-1 LVGL 平台后端与 libgui 架构设计规范

> **日期**: 2026-10-19  
> **状态**: 独立子 agent 三轮评审通过（Draft v2，2026-10-19）；待用户确认
> **基准**: OS01 `5424ea69`；LVGL v9.5.0 (`85aa60d1`)；`docs/roadmap.md` P3 GUI-1 项  
> **前置依赖**: LVGL 初步移植 ✅；libgfx ✅；显示所有权协调协议 ✅ (`docs/gui/gui.md`「显示所有权协调协议」)

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

所有依赖严格单向无环。公共应用仅面向 `libgui`、`libgfx` 与 `libc` 编译；GUI-1 尚不提供控件 Facade，`test_lvgl` 是唯一允许使用仓库内私有桥接接口的过渡应用。图中的 `desktop.elf` 是后续迁移目标，本次不迁移 desktop。

### 2.2 目录与文件布局

```text
libgui/
├── Makefile                       # LVGL + 平台源码 → libgui.a
├── include/gui/
│   ├── gui.h                      # 安装到 sysroot
│   └── gui_platform.h             # 安装到 sysroot
├── internal/
│   └── gui_lvgl_private.h         # 仓库内部桥接；禁止安装
└── src/
    ├── platform.c                 # 生命周期、flush、step、像素采样
    └── tick.c                     # 单调时钟适配
```

`config/lv_conf.h` 保持唯一配置源，libgui、过渡测试和宿主真实 LVGL 测试使用相同配置；不复制、不迁移，不修改 LVGL 子模块源码。保留现有断言开关，仅调整 fatal handler，使致命断言退出进程，不再永久循环；适用范围及测试见 §4.2 和 §5.1。LVGL 的断言宏不补分号，必须使用以下完整定义（末尾分号是宏值的一部分）：

```c
#define LV_ASSERT_HANDLER_INCLUDE <unistd.h>
#define LV_ASSERT_HANDLER do { _exit(1); } while (0);
```

### 2.3 Sysroot Staging 与原子发布

迁移必须覆盖完整组件链路：

1. `mk/components/sysroot.mk` 中用 `gui-install.stamp` 替换 `lvgl-install.stamp`；依赖 kernel headers、libc、libgfx staging 完成，调用 `libgui install INSTALL_ROOT=$(STAGING_DIR)/gui`，沿用 `stamp_check`。同时替换 `SYSROOT_STAMP` 的对应依赖，以及 generation 发布循环中的组件名 `lvgl → gui`。仍由 sysroot 发布器单写最终 generation，libgui 禁止写最终 sysroot。
2. `libgui/Makefile` 沿用 profile-only、目标工具链、manifest 和依赖文件机制；使用 staging headers 编译，先验证 `thirdpart/lvgl.manifest` 的版本。profile 定义 `LIBGUI_BUILD_DIR := $(BUILD_DIR)/libgui`，不复用旧 archive 或对象目录。LVGL、平台源码均编入同一个 `libgui.a`；对象路径保留各自目录前缀，避免同名对象冲突。
3. install 每次清空自己的 `staging/gui`，只安装 `usr/lib/libgui.a`、`usr/include/gui/gui.h`、`usr/include/gui/gui_platform.h`，生成排序 manifest。LVGL 的许可证作为 `usr/share/licenses/libgui/LVGL-LICENCE.txt` 随组件安装。不得安装桥接头、LVGL 头或 `lv_conf.h`。新 generation 不包含旧 `liblvgl.a` 或旧 LVGL include 树；保留的历史 generation 不修改。
4. 实际应用链接声明在 `user/test_lvgl/app.mk`：改为 `APP_LDLIBS := -lgui -lgfx`，最终 libc 仍由 user 链接规则追加。删除该应用原有 `APP_SHARED_SOURCES := common/gfx_client_policy.c`。共享策略仍位于 `user/common/gfx_client_policy.{c,h}`，本次由 libgui 编入 archive 并使用私有包含路径；其他现有应用继续按原规则链接共享策略，不改变其行为，也不把策略头安装进 sysroot。
5. 移除 `user/Makefile` 的全局 `-I$(TARGET_INCDIR)/lvgl -DLV_CONF_INCLUDE_SIMPLE`。仅 `test_lvgl/app.mk` 的 `APP_CFLAGS` 增加 `-I$(OS01_ROOT)/libgui/internal -I$(OS01_ROOT)/thirdpart -I$(OS01_ROOT)/thirdpart/lvgl -I$(OS01_ROOT)/config -DLV_CONF_INCLUDE_SIMPLE`；这样私有桥接头中的 `<lvgl/lvgl.h>` 及配置可解析。普通应用只需 `#include <gui/gui.h>`，不得继承上述私有路径。
6. 更新组件相关构建入口和审计脚本中仍作为活动依赖的旧名字；历史文档无需批量改名。验收同时覆盖干净构建和已有 lvgl staging 的增量迁移，确认发布内容、库链接顺序及私有 include 隔离。

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
    GUI_STATUS_EAGAIN          = 1,   /* present 暂时失败，保留视图，按等待时间重试 */
    GUI_STATUS_LOST_OWNERSHIP  = 2,   /* 终止态：失去前台所有权 (EACCES)，需销毁 */
    GUI_STATUS_DISPLAY_CHANGED = 3,   /* 分辨率或内核视图失效 (ESTALE)，需重建或重启 */
    GUI_STATUS_ERROR           = -1   /* 严重底层设备或内存错误 */
} gui_status_t;

/**
 * @brief 平台初始化配置参数
 */
typedef struct {
    gui_mode_t mode;               /* 运行模式 (默认 GUI_MODE_ONSCREEN) */
    uint32_t width;                /* 离屏必填；屏幕模式必须为 0，自动查询 */
    uint32_t height;               /* 离屏必填；屏幕模式必须为 0，自动查询 */
    uint32_t buffer_lines;         /* 局部绘制缓冲行数，若为 0 则使用默认值 40 行 */
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

### 3.3 内部桥接头文件：`libgui/internal/gui_lvgl_private.h`（不安装）

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
    bool pending_present;       /* flush 后置位，present 成功后清除 */
    gui_status_t terminal_status; /* OK 表示仍活跃，其余终止态锁存 */
    int terminal_errno;
    int flush_error;            /* flush 无返回值，供 step 收敛错误 */
    uint64_t retry_deadline_ms;   /* EAGAIN 限频退避截止时间 (CLOCK_MONOTONIC ms) */
    
    /* 内部关联对象 */
    lv_display_t *disp;
    uint32_t flush_count;
};
```

### 4.2 生命周期、时钟与内存

**进程模型**：GUI-1 仅允许同进程同时存在一个平台上下文，所有调用和私有 LVGL 操作在同一线程串行执行；已有上下文（包括终止态未销毁）时第二次 create 返回 NULL/`EBUSY`。私有使用者不得自行调用 `lv_init/lv_deinit`、删除 display、替换缓冲/flush/tick 回调，也不得从 LVGL 回调递归调用 step 或 destroy。

**配置及容量校验**：

- `cfg == NULL` 等效于 `{GUI_MODE_ONSCREEN, 0, 0, 0}`。无效 mode、离屏宽高为 0、屏幕模式传非零宽高均返回 `EINVAL`。屏幕模式每次 create 读取并关闭 `/dev/fb`，短读、零尺寸或非支持格式按 `EINVAL` 失败；读取系统调用失败保留原 errno。只支持 32bpp XRGB8888，使用 `gfx_open(0, 0, width, height)`；核对返回 view 的宽高、格式和 stride 与本库约束一致，失败清理并返回 `EINVAL`。
- 宽高必须可表示为正的 LVGL 坐标（不超过 `LV_COORD_MAX`），并满足 `width <= 16383`、`height <= 65535`。这是 LVGL v9.5.0 的 `lv_image_header_t.w/h/stride` 为 16 位所要求的保守统一上限：XRGB8888 的 `width * 4 <= UINT16_MAX`，同时 header 的宽高不可缩窄；超限返回 `EOVERFLOW`。行数 `lines = min(buffer_lines ? buffer_lines : 40, height)`。先检查所有乘法，再计算 `(size_t)width * sizeof(uint32_t)` 的 stride、`stride * lines` 的 draw bytes，以及 `stride * height` 的 canvas bytes。有效 lines 必须不超过 `UINT16_MAX`。容量必须非零；拒绝超过 `SIZE_MAX`、页对齐溢出，或传给 LVGL `uint32_t buf_size` 时超过 `UINT32_MAX` 的 draw bytes，返回 `EOVERFLOW`。校验发生在分配和 LVGL 创建前；禁止先作 uint32 乘法后转换。
- `LV_COLOR_DEPTH=32`、`LV_DRAW_BUF_STRIDE_ALIGN=1` 保持现有配置；创建 display 后显式设置 `LV_COLOR_FORMAT_XRGB8888`，再绑定 partial buffer。绑定后通过内部桥接可访问的真实 draw buffer header 校验 `w == width`、`h == lines`、`stride == width * 4`，不依赖可被关闭的 assert 来保障容量。发现不一致进入创建回滚并返回 `EINVAL`。
- draw buffer 和离屏 shadow canvas 均用匿名 `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)`；失败判断 `MAP_FAILED`，不把它当有效映射。像素区不使用 malloc。平台小结构可使用 calloc；离屏 canvas 初始化为零，get_pixel 成功时返回 `canvas[y * width + x] & 0x00ffffffu`，屏蔽 X 字节，避免白色与错误哨兵混淆。索引及行偏移均先提升为 `size_t`。NULL、越界或非离屏查询返回 `0xffffffffu` 并设 `EINVAL`。

**创建顺序与回滚**：

1. 校验配置、进程单实例限制及 `CLOCK_MONOTONIC` 可读性；分配平台结构，查询屏幕元数据（离屏不得打开显示设备）。时钟失败返回 NULL，保留 errno。
2. 完成容量检查、gfx view 创建（屏幕模式）及像素映射分配；此时不 acquire，create 不改变物理显示所有权。
3. `lv_init()`，注册基于 `CLOCK_MONOTONIC` 的 tick callback，创建 display。若 API 实际返回 NULL，以 `ENOMEM` 进入平台侧回滚；这不代表 LVGL 内部 OOM 可恢复（见下文边界）。设置格式、user_data、flush callback、partial buffer 并校验真实 header。全部成功后注册为唯一活动上下文并返回。
4. 对于能够返回的平台/系统调用失败，保存原 errno，按销毁顺序回收已经获得的资源，清除单实例占用，再恢复 errno。仅在本次已初始化 LVGL 时执行 `lv_deinit()`，不对未初始化状态调用它。

**LVGL 内部失败边界**：平台自身 calloc/mmap、libgfx 可返回的内存/设备错误、时钟失败保证上述回滚；不承诺把 LVGL 内部 pool 耗尽及其他致命断言转换为 `NULL/ENOMEM` 或 `GUI_STATUS_ERROR`。保留 `LV_USE_ASSERT_MALLOC=1` 等现有开关，将 fatal handler 从 `while(1)` 改为 §2.2 给出的完整退出宏；此路径不返回、不调用 destroy，不尝试从可能已部分变更的 LVGL 状态恢复。由内核进程退出回收用户映射及 fd，gfx 最终关闭恢复 fallback。宿主对应测试必须隔离到子进程验证退出码 1；shim 返回 NULL 的回滚测试仅证明平台分支，不证明真实 LVGL 内部 OOM 恢复。正常 create/destroy 仍执行确定性用户态回收，不依赖退出回收。

Tick callback 按 LVGL 接口返回单调毫秒的低 32 位，允许自然回绕；退避截止时间使用独立 uint64 单调毫秒。callback 中 clock_gettime 失败时锁存时钟错误并返回上次成功的 tick；step 在定时器调用前后检查错误并转为 `GUI_STATUS_ERROR`，禁止以恒定 0 时钟继续重试。错误状态只属于当前上下文，在重建时清零。

**销毁顺序**（同时适用于创建回滚）：

1. 停止使用上下文，先 `lv_display_delete(disp)`，清空指针，以删除 screens、刷新 timer 和关联对象；再 `lv_deinit()` 清理本进程 LVGL 全局状态。此时绘制映射及平台结构仍有效。
2. 若有 gfx，且 acquired，尽力 `gfx_release()`；随后始终 `gfx_close()` 并置 NULL、acquired=false。release 失败不得跳过 close。
3. `munmap` 有效 draw/shadow 映射，使用记录的原始长度；清除单实例状态，释放平台结构。destroy(NULL) 无操作；非 NULL 句柄只可销毁一次。

### 4.3 冲刷回调（Flush Pipeline）

`platform_flush_cb(disp, area, px_map)` 从 display user_data 获取上下文，仅进行内存复制，不 acquire、不 present、不释放资源。

1. 验证矩形非空、坐标非负且在画布内、px_map 非 NULL，按当前 XRGB8888 配置计算 `area_w`、`area_h`、源 stride，并检查其等于 `area_w * 4`，源大小不超过 draw buffer 容量。失败锁存 `flush_error=EINVAL`，不得读写像素。
2. 屏幕模式使用 `gfx_sprite_blit(gfx, x1, y1, px_map, stride, area_w, area_h, false, 0)` 更新 libgfx 私有像素缓冲，并设置 `pending_present=true`。该复制不需要显示所有权。离屏模式按行复制到 shadow canvas，源/目标偏移均用 size_t。
3. 每次回调包括校验失败均恰好调用一次 `lv_display_flush_ready(disp)`，防止 LVGL 等待未完成 flush；成功时增加 flush_count。step 在 handler 返回后检查 flush_error，统一进入错误终止态。

### 4.4 显示所有权与错误状态机

所有 libgfx 调用遵循用户态约定：**成功为 0；失败为 -1，并设置 errno**。必须在失败后立即保存 errno，按保存值分类，清理不可覆盖原始错误。`gfx_client_present_policy(saved_errno)` 只分类 present 结果；不用于 acquire。

| 操作及结果 | 行为 | step 状态 |
|---|---|---|
| 首次有待提交内容，acquire 成功 | acquired=true，再 present | 取决于 present |
| acquire 失败，errno=EBUSY | 不等待另一个 GUI，不重试抢占；关闭 view，锁存 EBUSY | ERROR |
| acquire 其他失败 | 关闭 view，锁存原 errno | ERROR |
| present 成功 | 清 pending_present、retry_deadline_ms | OK |
| present 失败，策略返回 1（EAGAIN） | 保留 view、acquired 及 pending_present；在失败调用返回后重新读时钟，设置 deadline=now+250ms | EAGAIN |
| present 失败，errno=EACCES | release/close，清 gfx/acquired，锁存原 errno | LOST_OWNERSHIP |
| present 失败，errno=ESTALE | 同上 | DISPLAY_CHANGED |
| present 其他失败 | 同上 | ERROR |

GUI-1 固定为首次有待提交内容时自动 acquire，不提供 auto_acquire 开关或手动 acquire API。EBUSY 采用现有 test_lvgl 的“退出并报错”策略，避免新应用无界等待后意外接管显示。

清理 gfx 的统一辅助路径先保存原 errno，尽力 release（仅 acquired 时），始终 close，清 acquired/pending_present/deadline。终止状态及原 errno 锁存到平台上下文；display、draw buffer 留待 destroy。后续 step 返回相同状态及 errno，既不运行 timer 也不 acquire/present，不会访问已关闭 gfx，不会自动重新抢占。应用收到终止状态后 destroy 并退出；如需恢复，显式重新 create。时钟或 flush 错误也使用相同终止路径。

`gui_platform_step(NULL, out)` 返回 ERROR/`EINVAL`；有效上下文的 ERROR 保留明确 errno。成功与可重试 EAGAIN 不要求调用者读取 errno。宽高查询 NULL 返回 0/`EINVAL`；mode 查询 NULL 返回 `GUI_MODE_ONSCREEN`/`EINVAL`，调用者应先验证句柄。

### 4.5 step 顺序与 Poll 等待计算

每个 step 的顺序唯一：

1. 若 out 非 NULL，先写 `*out=100`；无效句柄或已锁存终止态按 §4.4 返回。检查时钟可读性。
2. 执行 `lv_delay=lv_timer_handler()`，包括 present 退避期；检查 tick/flush 错误。离屏到此完成绘制，不访问 gfx。
3. 屏幕模式若无 pending_present，返回 OK；有 pending_present 且 deadline 尚未到达时返回 EAGAIN，不发 acquire/present；否则执行 §4.4。成功提交后才清 pending_present，退避期间新 flush 保留最新画面。每个 step 最多一次 present，首帧全部 partial flush 完成后才提交，避免半帧。
4. 所有正常和 EAGAIN 路径在返回前再次读取 uint64 单调时间。若失败，进入 ERROR。`lv_delay==LV_NO_TIMER_READY` 时基础等待为 100ms；否则从 lv_delay 中减去 handler 返回后到计算等待之间经过的毫秒，饱和到 0，再限制到最多 100ms。若 pending_present 且存在未来 retry deadline，取基础等待与 deadline 剩余时间的较小值。deadline 已到但本轮刚好尚未提交时等待为 0，下一轮即可提交。
5. 若计算等待为 0，允许应用立即再 step；不强制钳制为 1ms，因为 LVGL 可能确有已到期任务。无到期任务的稳定画面应返回正等待时间；应用使用返回值执行 poll，不额外忙轮询。终止、参数错误路径统一保留 out=100，不留下未初始化输出。out=NULL 同样执行完整状态机。

示例主循环（省略 UI 创建与错误打印）：

```c
for (;;) {
    uint32_t delay_ms;
    gui_status_t status = gui_platform_step(platform, &delay_ms);
    if (status != GUI_STATUS_OK && status != GUI_STATUS_EAGAIN) break;
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int rc = poll(&pfd, 1, (int)delay_ms);
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
    /* 有输入则读取并处理 q/ESC；GUI-2 后续在这里增加输入 fd。 */
}
gui_platform_destroy(platform);
```

---

## 5. 测试与验证计划

### 5.1 宿主测试

在 `hosttests/Makefile` 注册 `hosttests/cases/test_gui_platform.c`，纳入 `make test-host`。分两组，不以 mock LVGL 的像素输出证明真实渲染正确：

1. **确定性状态机测试**：使用可控单调时钟及 gfx/open/close/ioctl/mmap 失败注入；LVGL handler 可用 shim 控制返回等待时间及触发 flush。mock libgfx 必须返回 -1 并设置 errno。覆盖：
   - 所有非法配置、宽高/行数边界、乘法及 uint32 缩窄溢出；尺寸边界明确覆盖 width=16383/16384、height=65535/65536、buffer_lines 大于画高时的裁剪及 16 位字段匹配；失败时不调用后续分配/LVGL 接口。
   - 平台自身及 libgfx 可返回的每个分配/设备/时钟失败位置的逆序回滚，保存原 errno，MAP_FAILED 不被 munmap；destroy(NULL)、单实例 EBUSY 以及失败后可重新 create。LVGL shim 返回 NULL 单独标注为平台分支测试，不宣称覆盖真实内部 OOM 回滚。
   - acquire EBUSY 直接终止；present EAGAIN/EACCES/ESTALE/EIO 的状态、句柄计数及 errno；模拟清理覆盖 errno，验证原值仍可见。
   - EAGAIN 后至少 250ms 才再次提交，以失败调用结束时间为起点；退避期间 handler 继续运行且无 present；覆盖新 flush、deadline 到期、handler 返回 0/无 timer、输出 NULL 和每条提前返回路径。
   - 多个 partial flush 合并成一次 present、无脏帧不 present、失败保留 pending、终止后重复 step 无 timer/gfx 调用、flush 校验失败仍 ready 一次。
2. **真实 LVGL 集成测试**：宿主编译同版本 LVGL + libgui（相同 config），仅替换 OS 设备/时钟边界，不替换渲染器。使用私有桥接得到 display 的 active screen，绘制固定背景和纯色矩形，经 step 渲染，检查坐标像素（含白色、高字节掩码、边缘和跨 partial buffer 行边界），不要直接写 shadow canvas。稳定时钟下验证正等待；允许有到期任务时返回 0。
3. **LVGL fatal 验证**：用同一 config 编译，在隔离宿主子进程中实际耗尽内存池并触发分配断言，验证退出码 1、不会永久循环；QEMU 中用独立故障测试程序在持有显示所有权时触发同一 fatal 路径，确认内核关闭资源、恢复 terminal fallback。不得在主测试进程里触发 fatal，也不把此结果标为 create 返回 ENOMEM。
4. **真实生命周期测试**：50 次 create → 构造控件 → step → destroy，再创建并 step，确认无残留 display/timer、mmap 成对释放、LVGL pool 恢复。每轮 destroy 后继续推进测试时钟，下一轮 timer 不得访问旧映射；使用宿主 ASan（环境支持时）及映射计数共同验证。离屏整个路径 assert 从未打开 `/dev/fb`、`/dev/gfx0` 或调用所有权 ioctl。

### 5.2 构建与接口隔离验收

- `make PROFILE=x86_64-clang clean` 后正常构建；另在存在旧 staging/lvgl 的构建目录验证增量迁移。检查新 generation manifest 只发布规定的 gui 公共头、archive 和许可证，无 LVGL include 树与旧 archive。
- 用仅含 sysroot include 路径的最小 C 和 C++ consumer 编译公共头，确认不依赖 LVGL 私有头。确认 test_lvgl 使用 app 局部私有路径可编译，其他应用不获得这些路径。
- 检查 libgui archive 同时包含平台、LVGL 和共享策略实现，最终应用链接为 `-lgui -lgfx -lc`（由既有规则追加 libc），不会因迁移漏掉策略符号。验收该私有源码及配置的依赖文件，使修改后能触发重编译。

### 5.3 `/bin/test_lvgl` 与 QEMU 验收

1. 将平台样板代码替换为 create/step/destroy，保留既有丰富 UI 和 TLSF 检查。私有桥接仅用来构造控件与取得 display 的 active screen；tick/flush/buffer/ownership 不再由应用管理。成功、失败与 q 退出均经过 destroy。
2. 增加 `--smoke`（保留现有 `smoke` 别名），纯离屏 320×240。在应用启动后的 1 秒单调预算内推进 step+poll，实际渲染背景/按钮后调用 get_pixel 断言，并保留 `[TEST PASS] LVGL compatibility smoke test succeeded.` 与退出码 0。从最终像素采样，不依赖原 callback 的 sampled 标志。超时或任何终止错误返回非零。
3. 屏幕模式正常查询实际分辨率；`test_lvgl --wait` 保持显示并响应 q/ESC（保留既有 wait 别名），缺省展示后退出。应用负责现有 stdin/终端行为，平台不写 ANSI 序列、不改变 termios。通过显示所有权 release/close 验证 terminal fallback 恢复，不能把“Alt-screen 恢复”假定为 libgui 自动完成。
4. 将离屏 smoke 与屏幕启动/退出场景加入 `qemutests/` 中合适的现有 GUI 测试入口或独立 GUI suite 并注册到顶层测试 harness；验证 SMP 1/2 的启动、像素断言、正常退出、q 退出、已有前台导致 EBUSY，以及异常退出后的终端恢复。模式切换 ESTALE 和 transient EAGAIN 的精确分支由宿主失败注入门禁覆盖，QEMU 模式切换场景可复用既有显示测试基础设施。

以上是后续实现验收要求；本 spec 的评审通过不表示这些构建/运行测试已执行。

---

## 6. 评审记录

| 轮次 | 修订与验证 | 结论 |
|---|---|---|
| 初审 | 核对 libgfx 返回约定、私有头发布、LVGL 销毁、sysroot 发布链、acquire 配置、退避顺序及容量计算 | 已在 Draft v2 修订 |
| 子 agent 第 1 轮 | REQUEST CHANGES：补齐 LVGL header 的 16 位宽高/stride 边界；纠正内部 OOM 回滚承诺，fatal handler 改为退出进程并测试资源恢复 | 已修订，第 2 轮确认解决 |
| 子 agent 第 2 轮 | REQUEST CHANGES：退出 handler 宏须自带分号，否则 LVGL 断言展开无法编译 | 已给出完整定义；真实 LVGL 断言头的 clang 语法检查通过 |
| 子 agent 第 3 轮 | 复核完整退出宏、16 位缓冲字段、OOM 边界及失败回收契约 | APPROVE；无剩余阻塞项 |

自审要求：公共/私有接口隔离、用户态 errno 契约、单实例生命周期、终止态锁存、退避期 timer 推进、容量校验和失败回滚均有明确规定及对应验收项。2026-10-19，独立子 agent 第 3 轮明确 APPROVE。本次只修订 spec；未执行后续实现的完整构建、宿主测试或 QEMU 验收。
