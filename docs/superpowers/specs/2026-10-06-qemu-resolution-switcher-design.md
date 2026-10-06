# QEMU 屏幕分辨率动态切换系统设计规范

## 1. 概述与背景

### 1.1 背景
OS01 在 UEFI 引导阶段通过 Graphics Output Protocol (GOP) 设置初始图形显示模式，将线性帧缓冲物理基址 `FrameBufferBase` 与尺寸 `FrameBufferSize`（通常为 16MB 连续物理显存）通过 `boot_context` 移交给内核。内核在 `0xFFFF900000000000` 建立虚实映射，并提供 `/dev/fb` 设备节点。
目前系统缺乏运行时动态调整分辨率的能力。在日常开发、测试不同 UI 布局（如 Terminal、Desktop、LVGL）以及 QEMU 交互中，需要能够随时在用户态通过命令行或图形界面热切换分辨率。

### 1.2 目标与非目标
- **目标**：
  1. 支持用户态命令行工具 `setres` 动态查询支持列表与热切换分辨率（无需重启）。
  2. 内核针对 QEMU 显卡（Standard VGA / Bochs BGA）自建硬件探测逻辑，自动生成基于可用显存上限的标准分辨率白名单。
  3. 提供统一规范的 UAPI `/dev/fb` ioctl 协议，供 CLI 与后续图形桌面（`desktop.c` / LVGL）调用。
  4. 切换后内核状态（`Pos` 全局变量、显存安全清屏、内核字符终端行宽列高）同步更新。
- **非目标**：
  1. 不修改 UEFI 阶段 `boot_context` ABI（避免跨引导边界状态膨胀与页表重构后指针失效风险）。
  2. 本阶段仅针对 QEMU 平台的 x86_64 仿真显卡（Bochs BGA），暂不引入 virtio-gpu 3D 或物理独立显卡 DRM/KMS 驱动。

---

## 2. 硬件层：QEMU BGA 适配驱动

### 2.1 Bochs Graphics Adapter (BGA) 端口与寄存器
QEMU 默认显卡（`-M q35` 下的 stdvga / bochs-display）提供 BGA 兼容接口，通过 x86 I/O 端口访问：
- **索引端口 (Index Port)**: `0x1CE` (16-bit 宽)
- **数据端口 (Data Port)**: `0x1CF` (16-bit 宽)

核心寄存器索引（`kernel/driver/bga.c` 或 `kernel/driver/fb.c` 中定义）：
```c
#define VBE_DISPI_IOPORT_INDEX          0x01CE
#define VBE_DISPI_IOPORT_DATA           0x01CF

#define VBE_DISPI_INDEX_ID              0x0
#define VBE_DISPI_INDEX_XRES            0x1
#define VBE_DISPI_INDEX_YRES            0x2
#define VBE_DISPI_INDEX_BPP             0x3
#define VBE_DISPI_INDEX_ENABLE          0x4
#define VBE_DISPI_INDEX_BANK            0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x7
#define VBE_DISPI_INDEX_X_OFFSET        0x8
#define VBE_DISPI_INDEX_Y_OFFSET        0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xA

#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID5                   0xB0C5

#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80
```

### 2.2 硬件自探测与可用分辨率生成
内核启动初始化 `fb` 时执行探测：
1. **BGA 存在性检测**：
   向 `0x1CE` 写 `VBE_DISPI_INDEX_ID`，向 `0x1CF` 写 `VBE_DISPI_ID5`，再从 `0x1CF` 读回值。若读回值在 `0xB0C0` ~ `0xB0C5` 范围内，标记 BGA 可用，否则标记不可用（后续相关 ioctl 返回 `-ENODEV`）。
2. **候选分辨率过滤表**：
   定义候选标准分辨率列表（涵盖常见 4:3、16:9、16:10 模式）：
   ```c
   static const struct {
       uint32_t width;
       uint32_t height;
   } bga_candidates[] = {
       { 640,  480  },
       { 800,  600  },
       { 1024, 768  },
       { 1280, 720  },
       { 1280, 800  },
       { 1280, 1024 },
       { 1440, 900  },
       { 1600, 900  },
       { 1920, 1080 },
   };
   ```
3. **安全容量校验**：
   遍历候选列表，满足 `candidate.width * candidate.height * 4 <= Pos.FB_length` 的项存入内核全局模式列表 `supported_modes[]`，记录有效数量 `supported_modes_count`。

### 2.3 硬件模式切换时序
```c
static void bga_set_mode(uint32_t width, uint32_t height) {
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_IOPORT_DATA, VBE_DISPI_DISABLED);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    outw(VBE_DISPI_IOPORT_DATA, (uint16_t)width);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    outw(VBE_DISPI_IOPORT_DATA, (uint16_t)height);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    outw(VBE_DISPI_IOPORT_DATA, 32);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_IOPORT_DATA, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
}
```

---

## 3. UAPI 接口与内核控制协议

### 3.1 UAPI 头文件 `kernel/include/uapi/fb.h`
```c
#ifndef _UAPI_FB_H
#define _UAPI_FB_H

#include <stdint.h>

struct fb_mode_info {
    uint32_t width;   /* 像素宽度 */
    uint32_t height;  /* 像素高度 */
    uint32_t bpp;     /* 像素位深 (32) */
    uint32_t stride;  /* 行字节数 = width * 4 */
};

#define FB_MAX_MODES 16

/* ioctl 操作码定义 */
#define FBIOSURRENDER      0x00004601  /* 转让 framebuffer 控制权 */
#define FBIOGET_MODECNT    0x00004602  /* 查询支持模式总数 (arg: uint32_t *) */
#define FBIOGET_MODES      0x00004603  /* 获取支持模式列表 (arg: struct fb_mode_info *) */
#define FBIOSET_MODE       0x00004604  /* 设置分辨率 (arg: const struct fb_mode_info *) */
#define FBIOGET_CURR_MODE  0x00004605  /* 获取当前分辨率 (arg: struct fb_mode_info *) */

#endif
```

### 3.2 `/dev/fb` ioctl 实现细节 (`kernel/driver/fb.c`)
- **并发控制**：模式变更期间获取 `Pos.lock` 自旋锁。
- **校验逻辑**：
  - 检查 BGA 硬件状态，未就绪返回 `-ENODEV`。
  - 请求的分辨率必须在 `supported_modes[]` 中匹配存在。
  - 计算 `req.width * req.height * 4`，若超出 `Pos.FB_length` 则拒绝并返回 `-EINVAL`。
- **状态同步与视觉刷新**：
  1. 调用 `bga_set_mode(width, height)` 完成硬件切换。
  2. 更新全局状态：
     - `Pos.XResolution = width;`
     - `Pos.YResolution = height;`
  3. 清屏：`memset(Pos.FB_addr, 0, width * height * 4);` 避免旧分辨率残留显存导致花屏。
  4. 若控制台终端未转让（`!fb_surrendered`）：
     - 重新计算终端字符行列数：
       `int cols = Pos.XResolution / font->width;`
       `int rows = Pos.YResolution / font->height;`
     - 重置光标坐标为 `(0, 0)`。

---

## 4. 用户态程序设计 (`user/setres.c`)

### 4.1 命令行语法
```bash
setres -l               # 查看所有可用模式与当前模式
setres <width> <height> # 切换到指定宽度与高度
setres <width>x<height> # 紧凑格式切换
setres -h               # 查看帮助信息
```

### 4.2 程序结构与流程
1. **参数解析**：识别 `-l`, `-h`, 以及单个/双个数字格式参数。
2. **打开设备**：`int fd = open("/dev/fb", O_RDWR);`。
3. **分支处理**：
   - `-l` 分支：
     - 调用 `ioctl(fd, FBIOGET_MODECNT, &count)`；
     - 动态分配内存或使用固定长度数组，调用 `ioctl(fd, FBIOGET_MODES, modes)`；
     - 调用 `ioctl(fd, FBIOGET_CURR_MODE, &curr)`；
     - 格式化输出支持的列表，在当前匹配项旁标注 `[CURRENT]`。
   - 切换模式分支：
     - 填充 `struct fb_mode_info target = { .width = w, .height = h, .bpp = 32 };`；
     - 调用 `ioctl(fd, FBIOSET_MODE, &target)`；
     - 判定返回值：`0` 表示成功，打印成功信息；非 0 打印标准错误解释。
4. **编译构建**：
   在 `user/Makefile` 中添加 `setres` 编译规则，生成静态链接 ELF 二进制，安装到 `/bin/setres`。

### 4.3 图形界面 (GUI) 对接规范
对于后续 `user/desktop.c` 或 LVGL 设置界面：
- 在初始化时调用 `FBIOGET_MODECNT` 与 `FBIOGET_MODES` 获取模式填充下拉列表。
- 用户选中后通过 `FBIOSET_MODE` 生效。
- 接收成功后，释放旧显存画布，按新 `width` 和 `height` 重新初始化 GUI backbuffer 并触发全屏重绘。

---

## 5. 测试与验证策略

### 5.1 单元测试（Host Unit Tests）
- 在 `hosttests/cases/` 增加 `test_fb_resolution.c`：
  - 测试候选模式根据显存上限过滤算法。
  - 测试参数解析器与分辨率字符串 `WxH` 提取。

### 5.2 QEMU 自动化集成测试（E2E）
在 `qemutests/` 中添加自动化测试用例：
1. **模式列表测试**：启动系统，执行 `/bin/setres -l`，断言输出包含已知候选分辨率（如 `800x600`, `1024x768`, `1280x720` 等），且初始模式被标记为 `CURRENT`。
2. **正向热切换测试**：执行 `/bin/setres 1280 720`，断言退出码为 0；随后读取 `/dev/fb` 或再次调用 `setres -l`，断言当前模式变更为 `1280x720`。
3. **异常参数测试**：执行 `/bin/setres 9999 9999`，断言程序优雅退出且退出码非 0，系统不产生内核 Panic。
