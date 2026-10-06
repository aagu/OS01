# QEMU 屏幕分辨率动态切换系统设计规范 (v2)

## 1. 概述与设计边界

### 1.1 背景与问题定位
OS01 在 UEFI 引导阶段通过 Graphics Output Protocol (GOP) 设置初始图形显示模式。但存在以下关键技术事实：
1. **GOP 帧缓冲长度 ≠ 真实显存容量**：OVMF 的 `QemuVideoDxe/Gop.c` 实现中，`Mode->FrameBufferSize` 仅按**当前模式像素数 × 4（按页对齐）**计算（例如 800×600 模式下仅约 1.92MB，1024×768 模式下仅约 3.1MB）。若将其直接作为显存上限，会错误阻断切换到更高分辨率（如 1024×768 或 1280×720）。
2. **内核虚地址映射范围限制**：当前 `frame_buffer_init()` 仅按 `Pos.FB_length` 循环映射 2MB 页。若不建立完整显存映射，分辨率放大访问超出初始长度的显存将引发内核缺页崩溃。
3. **硬件类型明确**：QEMU 中 `bochs-display` 的寄存器暴露在 PCI BAR2 MMIO 中，而 Standard VGA (`-vga std`) 暴露在 ISA I/O 端口 `0x1CE/0x1CF`。v1 必须显式约束硬件目标。
4. **终端热切换保护**：当前 `terminal.elf` 在运行中若遭遇分辨率变小，旧 gfx view 写入越界导致 `gfx_present` 连续 5 次失败会直接致命退出杀死 Shell。必须建立完整的终端自适应恢复机制。

### 1.2 设计目标与范围
- **显式支持硬件**：QEMU Standard VGA (`-vga std`，PCI Device 0x1234:0x1111)。通过 PCI 配置空间验证物理显存基址与 GOP 移交的 `Pos.Phy_addr` 一致。
- **显存与映射模型**：解耦 `vram_capacity`（真实显存物理容量）、`fb_mapped_size`（内核虚拟映射长度）与当前模式的 `active_framebuffer_size`。在初始化时完整映射 VRAM Aperture（16MB）。
- **事务性模式切换**：写入硬件后执行完整读回校验（Enable、宽高、bpp、virtual width 与 offsets），校验失败自动回滚旧模式，杜绝虚假成功。
- **并发与显示同步规则**：引入分层锁与模式序列号，解耦用户态内存拷贝与内核自旋锁，防止一帧画面跨越两种 stride 撕裂或写入越界。
- **终端与图形层联动**：`terminal.elf` 捕获分辨率变更，重新创建 `gfx` view，重置终端缓冲区与 PTY 窗口尺寸（`TIOCSWINSZ`），保留交互 Shell 会话。
- **防御性 UAPI 契约**：使用定宽安全结构体，输入校验前置，采用 `copy_from_user_ft` / `copy_to_user_ft`，无效指针返回 `-EFAULT`。

---

## 2. 硬件层与显存映射架构

### 2.1 硬件探测与身份验证
在系统初始化阶段（`bga_pci_init`）：
1. **PCI 配置空间扫描**：
   - 检索 Vendor ID = `0x1234`，Device ID = `0x1111`（QEMU Standard VGA）。
   - 读取 PCI BAR0（Prefetchable 32-bit/64-bit Memory BAR），获取其物理基址 `bar0_base` 与 BAR 空间容量（写入全 1 探测大小，标准 QEMU VGA 为 16MB）。
   - 强断言匹配：`bar0_base == (uint64_t)Pos.Phy_addr`。若不一致或未找到，说明并非由当前 BGA 控制 GOP 帧缓冲，标记 BGA 不可用（后续 ioctl 返回 `-ENODEV`）。
2. **BGA 端口握手与容量核验**：
   - 保存 `0x1CE/0x1CF` 当前寄存器状态。
   - 写入 `VBE_DISPI_INDEX_ID` 为 `VBE_DISPI_ID5 (0xB0C5)`，读回若处于 `0xB0C2 ~ 0xB0C5` 则确认支持 32bpp 与 LFB。
   - 读取 `VBE_DISPI_INDEX_VIDEO_MEMORY_64K (0xA)`，得到 64KB 块数，计算 `bga_vram_bytes = reg_val * 64 * 1024`。
   - 最终物理显存容量取保守交集：`vram_capacity = min(pci_bar_size, bga_vram_bytes)`（通常为 16MB）。

### 2.2 虚拟显存映射扩展机制
在 `kernel/core/printk.c`（或 `kernel/driver/fb.c` 早期初始化）：
- 不再仅按 GOP 的 `Pos.FB_length` 循环映射，而是将 `Pos.FB_length` 修正为经过核验的完整物理容量 `vram_capacity`（16MB）。
- 将 `VIRT_FRAMEBUFFER_OFFSET` 起始的 `vram_capacity` 范围全部通过 `vmm_map_page`（2MB 大页对齐）映射至内核地址空间，标记 `PAGE_KERNEL_PMD | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE`。
- 保证任意合法分辨率（如 1920×1080×4 ≈ 8.29MB）在显存内任何有效偏移写入时，均位于已建立的页表范围内，杜绝内核缺页。

### 2.3 可用分辨率白名单生成
驱动维护标准候选模式表，并在启动时完成能力过滤：
```c
struct bga_mode_entry {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
};

static const struct bga_mode_entry bga_candidates[] = {
    { 640,  480,  32 },
    { 800,  600,  32 },
    { 1024, 768,  32 },
    { 1280, 720,  32 },
    { 1280, 800,  32 },
    { 1280, 1024, 32 },
    { 1440, 900,  32 },
    { 1600, 900,  32 },
    { 1920, 1080, 32 },
};
```
过滤条件：
1. `(uint64_t)candidate.width * candidate.height * 4 <= vram_capacity`。
2. `candidate.width <= 16000 && candidate.height <= 12000`（QEMU 最大限值）。
满足条件者进入 `supported_modes[]` 数组，最大条目数限制为 `FB_MAX_MODES (16)`。

### 2.4 事务性模式切换与读回校验
模式切换遵循严格事务流程，禁止单向无条件发布：
```c
int bga_set_mode_transaction(uint32_t width, uint32_t height) {
    /* 1. 保存旧寄存器状态 */
    uint16_t old_enable = bga_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t old_xres   = bga_read(VBE_DISPI_INDEX_XRES);
    uint16_t old_yres   = bga_read(VBE_DISPI_INDEX_YRES);
    uint16_t old_bpp    = bga_read(VBE_DISPI_INDEX_BPP);

    /* 2. 编程写入新模式 */
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bga_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bga_write(VBE_DISPI_INDEX_BPP, 32);
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* 3. 读回校验 (Read-back Verification) */
    uint16_t rb_enable = bga_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t rb_xres   = bga_read(VBE_DISPI_INDEX_XRES);
    uint16_t rb_yres   = bga_read(VBE_DISPI_INDEX_YRES);
    uint16_t rb_bpp    = bga_read(VBE_DISPI_INDEX_BPP);

    if ((rb_enable & (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED)) !=
        (VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED) ||
        rb_xres != width || rb_yres != height || rb_bpp != 32) {
        /* 硬件拒绝或修正失配：执行回滚 */
        bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        bga_write(VBE_DISPI_INDEX_XRES, old_xres);
        bga_write(VBE_DISPI_INDEX_YRES, old_yres);
        bga_write(VBE_DISPI_INDEX_BPP, old_bpp);
        bga_write(VBE_DISPI_INDEX_ENABLE, old_enable);
        return -EIO;
    }
    return 0;
}
```

---

## 3. 并发控制与显示同步规则

### 3.1 锁层次与保护范畴
为杜绝混合状态读写及跨 stride 撕裂，定义显示子系统锁规则：
1. **`display_mutex` (内核互斥锁)**：
   - 保护模式切换事务的全过程。
   - 确保同一时刻只有一个进程在执行分辨率切换，防止硬件端口写竞争。
2. **`Pos.lock` (自旋锁)**：
   - 仅保护 `Pos.XResolution`、`Pos.YResolution`、`Pos.FB_addr` 元数据及控制台光标状态的原子发布。
   - **绝不在持有自旋锁时调用 `copy_from_user_ft`/`copy_to_user_ft`，绝不在锁内执行耗时的大显存 `memset` 或复杂日志打印**。
3. **`display_mode_seq` (模式代数序列号)**：
   - 每次成功切换分辨率递增 `display_mode_seq`。
   - `/dev/gfx0` 在 `GFX_CREATE_VIEW` 时记录分配视图时的 `view->mode_seq`。
   - 在 `gfx_present` 逐行 blit 前校验 `view->mode_seq == display_mode_seq` 以及 `x + w <= Pos.XResolution && y + h <= Pos.YResolution`。若代数失配或尺寸越界，立即拒绝写入并返回 `-EINVAL`，阻断撕裂和越界。

### 3.2 模式切换时的清屏与同步流程
在持有 `display_mutex` 下执行：
1. 调用 `bga_set_mode_transaction(new_w, new_h)`。若返回错误，立即退出。
2. 此时硬件已切换窗口，但在更新全局尺寸前，先对显存区域执行局部清零（`memset(Pos.FB_addr, 0, new_w * new_h * 4)`），避免旧模式像素残留。
3. 获取 `Pos.lock`：
   - 更新 `Pos.XResolution = new_w;`
   - 更新 `Pos.YResolution = new_h;`
   - `display_mode_seq++;`
   - 释放 `Pos.lock`。
4. 调用 `console_notify_resize(new_w, new_h)` 同步内核字符终端。

---

## 4. 控制台与用户态终端自适应

### 4.1 内核控制台同步 (`console_notify_resize`)
在 `kernel/tty/console.c` 中显式提供：
```c
void console_notify_resize(uint32_t width, uint32_t height) {
    if (!font) return;
    /* 保持 console_fb_active / fb_surrendered 状态不变，严禁意外重新激活已让渡的控制台 */
    term_cursor_row = 0;
    term_cursor_col = 0;
    Pos.XPosition = 0;
    Pos.YPosition = 0;
}
```

### 4.2 用户态终端 (`terminal.elf`) 热切换与会话保留
在 `user/terminal.c` 的主循环事件处理中建立自适应机制：
1. **尺寸失配探测**：
   - 当 `gfx_present(gfx)` 首次返回失败（返回非 0）时，不立即计入 fatal 计数。
   - 终端调用 `ioctl(fb_fd, FBIOGET_CURR_MODE, &new_info)` 检测当前物理分辨率。
2. **动态重建流程 (Reconfigure)**：
   - 若检测到 `new_info.width != fb_info.width || new_info.height != fb_info.height`：
     1. 关闭旧图形视图：`gfx_close(gfx);`
     2. 更新本地参数：`fb_info = new_info;`
     3. 重新计算行列：`term_cols = fb_info.width / font->width; term_rows = fb_info.height / font->height;`
     4. 重建图形视图：`gfx = gfx_open(0, 0, fb_info.width, fb_info.height);`
     5. 重建渲染缓存与终端状态：`term_render_resize(&render, term_cols, term_rows); term_core_resize(&core, term_cols, term_rows);`
     6. 通知 Shell 窗口变化：构造 `struct winsize ws = { .ws_col = term_cols, .ws_row = term_rows };` 并调用 `ioctl(pty_fd, TIOCSWINSZ, &ws);`（内核向 ash 进程发送 `SIGWINCH`）。
     7. 触发全屏重绘，重置 `present_failures = 0`。
   - 若尺寸未变但 present 仍然失败，才递增 `present_failures` 走既有的容灾逻辑。
3. **效果保证**：无论是在 Shell 中执行 `setres` 还是外部修改分辨率，终端窗口自动拉伸/收缩填充全屏，正在运行的交互会话与前台程序不中断。

---

## 5. UAPI 接口与内核契约

### 5.1 跨边界头文件 `kernel/include/uapi/fb.h`
统一用户态与内核结构体定义，消除各程序各自声明 `struct fb_info` 的混乱：
```c
#ifndef _UAPI_FB_H
#define _UAPI_FB_H

#include <stdint.h>

#define FB_MAX_MODES 16

/* 像素格式枚举 */
#define FB_FORMAT_RGB32 0 /* XRGB8888 32-bit (与 GFX_FORMAT_RGB32 保持一致) */

/* 基础元数据结构 (兼容现有 fb_read ABI) */
struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;  /* 字节行跨度 = width * 4 */
    uint32_t bpp;     /* 必须为 32 */
    uint32_t format;  /* 固定为 FB_FORMAT_RGB32 */
} __attribute__((packed));

/* 分辨率模式列表查询请求 */
struct fb_modes_req {
    uint32_t capacity; /* 输入：用户提供的 modes 数组最大容量 */
    uint32_t count;    /* 输出：实际填入 modes 数组的模式条目数 */
    uint32_t total;    /* 输出：硬件支持的模式总数 */
    struct fb_info modes[FB_MAX_MODES]; /* 模式列表 */
};

/* 分辨率切换请求 */
struct fb_set_mode_req {
    uint32_t width;   /* 目标宽度 */
    uint32_t height;  /* 目标高度 */
    uint32_t bpp;     /* 必须为 32 或 0 (0 默认按 32 处理) */
};

/* ioctl 命令号定义 */
#define FBIOSURRENDER      0x00004601  /* 让渡控制台 */
#define FBIOGET_MODES      0x00004602  /* 查询模式列表 (arg: struct fb_modes_req *) */
#define FBIOSET_MODE       0x00004603  /* 切换分辨率 (arg: const struct fb_set_mode_req *) */
#define FBIOGET_CURR_MODE  0x00004604  /* 获取当前生效分辨率 (arg: struct fb_info *) */

#endif
```

### 5.2 安全契约与错误码规范
所有 ioctl 分支严格遵守指针防御规则：
- **`access_ok` 与容错拷贝**：
  - 用户指针必须经 `access_ok(arg, sizeof(req))` 校验。
  - 请求参数必须通过 `copy_from_user_ft` 载入内核局部栈变量，失败返回 `-EFAULT`。
  - 查询结果必须通过 `copy_to_user_ft` 写回用户空间，失败返回 `-EFAULT`。
- **参数校验规则**：
  - `FBIOSET_MODE`：
    - 若 BGA 硬件未探测成功，返回 `-ENODEV`。
    - 若 `bpp != 0 && bpp != 32`，返回 `-EINVAL`。
    - 遍历 `supported_modes[]`，若目标 `width/height` 不在支持列表内，返回 `-EINVAL`。
    - 若 `(uint64_t)width * height * 4 > vram_capacity`，返回 `-EINVAL`。
  - `FBIOGET_MODES`：
    - 若用户态传入 `capacity == 0`，仅回填 `total`，`count = 0`，返回 0（支持两段式查询）。
    - 将 `min(req.capacity, supported_modes_count)` 项模式拷回。

---

## 6. 用户态程序设计 (`user/setres.c`)

### 6.1 命令行语法与参数解析
```bash
setres -l               # 列出支持的所有模式，标注 [CURRENT]
setres <width> <height> # 切换指定分辨率
setres <width>x<height> # 支持紧凑格式（如 1280x720）
setres -h               # 打印使用帮助
```

### 6.2 健壮性参数解析
严格过滤非法输入：
- 拒绝负数、0、包含非法非数字字符的参数。
- 拒绝参数过少或多余参数。
- 检测整数溢出（数值必须 $\le 16000$）。
- 若参数不合法，打印友好使用提示并返回退出码 1。

### 6.3 模式列表格式化输出
```text
Supported screen resolutions (QEMU Standard VGA):
  [0]  640x480   (32 bpp, 4:3)
  [1]  800x600   (32 bpp, 4:3)
  [2] 1024x768   (32 bpp, 4:3)  [CURRENT]
  [3] 1280x720   (32 bpp, 16:9)
  [4] 1280x800   (32 bpp, 16:10)
  [5] 1280x1024  (32 bpp, 5:4)
  [6] 1440x900   (32 bpp, 16:10)
  [7] 1600x900   (32 bpp, 16:9)
  [8] 1920x1080  (32 bpp, 16:9)
```

---

## 7. 构建、安装与子系统集成路径

### 7.1 头文件与 Sysroot 发布
1. 创建 `kernel/include/uapi/fb.h`。
2. `kernel/Makefile` 的 `install-headers` 规则已包含 `cp -R include/. $(INSTALL_ROOT)/usr/include/.`，自动发布至 Sysroot `usr/include/uapi/fb.h`。
3. `kernel/include/driver/fb.h` 引用 `<uapi/fb.h>`，移除冗余的私有 `struct fb_info` 定义。
4. 重构 `user/desktop.c` 与 `user/terminal.c`，统一引用 `<uapi/fb.h>`。

### 7.2 驱动模块与架构边界
- 在 `kernel/driver/bga.c` 实现 QEMU BGA 核心驱动，在 `kernel/include/driver/bga.h` 暴露接口。
- 端口 I/O 通过 `<arch/io.h>` 的 `io_inw` / `io_outw` 访问；在非 x86 架构（如 aarch64）提供 stub 实现，探测恒返回 `-ENODEV`。
- 在 `kernel/arch/x86_64/platform/boot.c` 的设备注册阶段增加 `bga_init()` 硬件探测入口。

### 7.3 用户态程序构建与 Rootfs 打包
1. `user/Makefile`：新增 `setres.elf` 构建目标，链接 `crt0.o`、`sigreturn.o`、`libc`。
2. `config/rootfs.mk`：在 `ROOTFS_FILES` 中注册：
   ```makefile
   /bin/setres=$(USER_ARTIFACT_DIR)/setres.elf:0755
   ```

---

## 8. 测试与质量验收标准

### 8.1 单元测试 (`test-host`)
在 `hosttests/cases/test_fb_resolution.c` 中覆盖：
1. **模式过滤算法测试**：模拟给定不同容量 VRAM（2MB、8MB、16MB），断言过滤出的白名单符合数学计算边界。
2. **命令行解析测试**：覆盖合法与非法格式（负数、字符后缀、零、极值）。
3. **UAPI 结构大小与对齐静态断言**：`sizeof(struct fb_info) == 20`，`sizeof(struct fb_modes_req)` 固定定长验证。

### 8.2 QEMU 自动化集成测试 (`test-qemu`)
编写 QEMU 自动化测试套件 `qemutests/test_resolution_switcher.py`：
1. **硬件一致性验证**：读取 BGA I/O 端口与 `/dev/fb` 当前状态，核对初始模式与硬件读回一致。
2. **多阶段热切换验证**：
   - 放大切换：`setres 1280 720`，验证硬件端口读回为 1280×720，退出码为 0。
   - 缩小切换：`setres 640 480`，验证硬件端口读回为 640×480，退出码为 0。
   - 恢复切换：`setres 1024 768`，验证恢复初态。
   - 幂等切换：连续两次执行 `setres 1024 768`，验证正常返回且不崩溃。
3. **边界与异常测试**：
   - 执行 `setres 9999 9999`，验证返回非 0，系统不产生 Panic。
   - 传入无效指针给 ioctl，验证返回 `-EFAULT`。
4. **终端自适应与 Shell 存活测试**：
   - 在交互式终端下执行 `setres 800 600`，等待 1 秒后输入 `echo TERMINAL_ALIVE`，验证终端正常捕获并回显，无进程退出或崩溃。
