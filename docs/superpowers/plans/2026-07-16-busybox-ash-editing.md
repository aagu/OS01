# Busybox ash 方向键 + 光标闪烁 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 busybox ash 支持 VT100 方向键导航、行编辑和终端光标闪烁。

**Architecture:** 四层改动——键盘驱动输出 VT100 转义序列（raw mode 下），TTY ioctl 打通 tcsetattr 允许切 raw mode，新增 console.c 在写路径上解析 VT100 CSI 序列并管理终端光标，busybox 启用 FEATURE_EDITING。光标闪烁由 PIT 100Hz 回调直接驱动（framebuffer 是内核恒等映射，IRQ 上下文写入安全）。

**Tech Stack:** C (kernel + libc)，busybox kconfig，PS/2 Scancode Set 1，VT100/ANSI CSI 转义序列，PSF2 字体渲染

## Global Constraints

- x86_64 syscall ABI：3 个参数（rdi、rsi、rdx），第 4 个参数在 r10 不会被内核读取
- ISIG 必须强制保持启用（^C → SIGINT 不能丢）
- framebuffer 映射是 2MB 恒等映射（PAGE_KERNEL_Page | PWT | PCD），IRQ 上下文可安全写入
- `Pos.lock` 只在 `color_printk` 内使用，`putchar_at` 不应获取该锁
- keyboard_init Phase 5 先于 keyboard_set_tty Phase 7——kbd_tty 有一段时间是 NULL（已有安全检查）
- 内核源码目录结构：`kernel/`（tty、driver、kernel、fs、arch、include）
- `thirdpart/busybox-1.36.1/` 重新编译：`make -C thirdpart/busybox-1.36.1`

---

### Task 1: `putchar_at()` — 帧缓冲定位写入

**Files:**
- Modify: `kernel/include/kernel/printk.h:46-47`（新增声明）
- Modify: `kernel/kernel/printk.c:31-54`（新增函数体）

**Interfaces:**
- Produces: `void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor, unsigned char c)` — 在指定字符格坐标 (col, row) 写入字符 `c`。坐标以字符格为单位（非像素）。依赖全局 `extern psf2_t *font` 和 `extern position Pos`（只读其 `FB_addr`、`XResolution`、`YResolution`、`font` 引用）。

- [ ] **Step 1: 新增 `putchar_at` 声明**

在 `kernel/include/kernel/printk.h` 的 `putchark` 声明后新增：

```c
// Write a character to a specific character-cell position on the
// framebuffer.  (col, row) are in character cells; pixel address
// is computed from font->width/height and Pos.FB_addr.
// Does NOT touch Pos.XPosition/YPosition or Pos.lock.
void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c);
```

- [ ] **Step 2: 在 `putchark` 之后新增 `putchar_at` 函数体**

在 `putchark` 函数结束（`kernel/kernel/printk.c:54`，`}` 之后）插入：

```c
void putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor,
                unsigned char c)
{
    int i = 0, j = 0;
    uint32_t *addr = NULL;
    int testval = 0;
    unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start
        + font->headersize
        + (c > 0 && c < font->numglyph ? c : 0) * font->bytesperglyph;

    int pixel_row_start = row * (int)font->height;
    int pixel_col_start = col * (int)font->width;
    int max_rows = (int)(Pos.YResolution / font->height);
    int max_cols = (int)(Pos.XResolution / font->width);

    // Clamp to framebuffer bounds
    if (row < 0 || row >= max_rows) return;
    if (col < 0 || col >= max_cols) return;

    for (i = 0; i < (int)font->height; i++) {
        addr = Pos.FB_addr + Pos.XResolution * (pixel_row_start + i)
               + pixel_col_start;
        testval = 0x100;
        for (j = 0; j < (int)font->width; j++) {
            testval = testval >> 1;
            if (*glyph & testval)
                *addr = FRcolor;
            else
                *addr = BKcolor;
            addr++;
        }
        glyph++;
    }
}
```

- [ ] **Step 3: 验证编译**

```bash
make -j$(nproc) 2>&1 | tail -5
```

预期：编译成功，无警告。

- [ ] **Step 4: 验证 `putchark` 回归 —— 内核 printk 仍正常**

启动 QEMU，观察内核启动日志在屏幕上正常显示，无异常。

- [ ] **Step 5: 验证 `putchar_at` 基本功能 —— 在固定位置画一个字符**

临时在 `kernel_main` 的某个位置（在 `devfs_init` 之后，framebuffer 已映射）插入测试代码：

```c
putchar_at(0, 0, WHITE, RED, 'X');
```

启动 QEMU，验证屏幕左上角出现白字红底的 `X`。

- [ ] **Step 6: 删除临时测试代码，提交**

```bash
git add kernel/kernel/printk.c kernel/include/kernel/printk.h
git commit -m "feat: add putchar_at() for cell-positioned framebuffer writes

putchar_at writes a glyph to a specific (col,row) character cell
without touching Pos.XPosition/YPosition or Pos.lock.  Safe to call
from IRQ context (uses only kernel-identity-mapped FB address).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: `dev_tty` 从 devfs.c 迁移到 tty.c

**Files:**
- Modify: `kernel/tty/tty.c:1-299`（新增 `dev_tty` + `get_dev_tty` + `tty_set_dev_tty`）
- Modify: `kernel/include/kernel/tty.h:47-67`（新增导出声明）
- Modify: `kernel/fs/devfs.c:27-32`（删除 `dev_tty` 和 `devfs_set_tty`，改为通过 `get_dev_tty()` 获取）
- Modify: `kernel/kernel/main.c:222-226`（`devfs_set_tty` → `tty_set_dev_tty`）

**Interfaces:**
- Produces: `tty_t *get_dev_tty(void)` — tty.h 导出，供 trap.c ioctl 路由和 devfs.c 使用
- Produces: `void tty_set_dev_tty(tty_t *)` — tty.h 导出，供 main.c 注册
- Removes: `void devfs_set_tty(tty_t *)` — 从 devfs.h/devfs.c 删除

- [ ] **Step 1: 在 tty.c 末尾新增 `dev_tty` 及其访问函数**

在 `kernel/tty/tty.c` 末尾（`tty_write` 的 `}` 之后，第 298 行之后）追加：

```c
// ── Console TTY singleton ────────────────────────
// Set by main.c during init, consumed by dev_tty_read/write and trap.c ioctl.

static tty_t *dev_tty = NULL;

void tty_set_dev_tty(tty_t *tty)
{
    dev_tty = tty;
}

tty_t *get_dev_tty(void)
{
    return dev_tty;
}
```

- [ ] **Step 2: 在 tty.h 新增声明**

在 `kernel/include/kernel/tty.h` 的 `tty_write` 声明之后、`#endif` 之前追加：

```c
// Set and get the console TTY singleton — used by devfs, ioctl, and main.c.
void tty_set_dev_tty(tty_t *tty);
tty_t *get_dev_tty(void);
```

- [ ] **Step 3: 修改 devfs.c —— 删除 `dev_tty` 和 `devfs_set_tty`**

删除 `kernel/fs/devfs.c:27-32`（整个 `dev_tty` 变量和 `devfs_set_tty` 函数），并在 `dev_tty_read` 和 `dev_tty_write` 中把 `dev_tty` 引用改为 `get_dev_tty()`：

```c
// 删除这两行：
static tty_t *dev_tty = NULL;

void devfs_set_tty(tty_t *tty)
{
    dev_tty = tty;
}

// dev_tty_read (line 62): dev_tty → get_dev_tty()
static int dev_tty_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    tty_t *tty = get_dev_tty();
    if (!tty || !buffer || size == 0) return 0;
    return tty_read(tty, (char *)buffer, (int)size, false);
}

// dev_tty_write (line 69): dev_tty → get_dev_tty()
static int dev_tty_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    tty_t *tty = get_dev_tty();
    if (!tty || !buffer || size == 0) return 0;
    return tty_write(tty, (const char *)buffer, (int)size);
}
```

**重要**：检查 `kernel/fs/devfs.c` 顶部是否有 `#include <kernel/tty.h>`——确认已有（line 8），否则新增。

- [ ] **Step 4: 检查并更新 devfs.h**

检查 `kernel/include/fs/devfs.h` 中是否声明了 `devfs_set_tty()`，如存在则删除。

- [ ] **Step 5: 修改 main.c**

`kernel/kernel/main.c:226` 中 `devfs_set_tty(console)` 改为 `tty_set_dev_tty(console)`。

- [ ] **Step 6: 编译验证**

```bash
make -j$(nproc) 2>&1 | grep -E "error:|undefined reference" || echo "OK"
```

预期：编译成功，无链接错误。

- [ ] **Step 7: 启动 QEMU，验证 /dev/tty 读写正常**

启动 QEMU，登录 shell，输入命令回车执行。预期：`/dev/tty` 通过新的 `get_dev_tty()` 路径正常工作。

- [ ] **Step 8: 提交**

```bash
git add kernel/tty/tty.c kernel/include/kernel/tty.h kernel/fs/devfs.c \
        kernel/kernel/main.c
# 如果有 devfs.h 改动也加上
git commit -m "refactor: move dev_tty singleton from devfs.c to tty.c

- New: tty_set_dev_tty() / get_dev_tty() in tty.c, exported via tty.h
- Removed: devfs_set_tty() and static dev_tty from devfs.c
- devfs.c dev_tty_read/write now use get_dev_tty()
- main.c now calls tty_set_dev_tty() directly

Consolidates all TTY references in tty.c; no functional change.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: `tty_ioctl()` — TTY 层的 ioctl 处理

**Files:**
- Modify: `kernel/tty/tty.c:290`（在 `tty_write` 之后新增 `tty_ioctl`）
- Modify: `kernel/include/kernel/tty.h`（新增声明）

**Interfaces:**
- Produces: `int tty_ioctl(tty_t *tty, int cmd, void *arg)` — 处理 TCGETS/TCSETS/TCSETSW
- Consumes: `get_dev_tty()` from Task 2
- Consumes: `struct termios` from `libc/include/termios.h`

- [ ] **Step 1: 在 tty.h 中新增声明**

在 `kernel/include/kernel/tty.h` 顶部 `#include` 区域新增 `#include <termios.h>`（使 `struct termios` 可用），并在 `get_dev_tty()` 声明后新增：

```c
// TTY ioctl — handles TCGETS/TCSETS/TCSETSW for line discipline control.
// Returns 0 on success or -errno on failure.
int tty_ioctl(tty_t *tty, int cmd, void *arg);
```

- [ ] **Step 2: 在 tty.c 中实现 `tty_ioctl`**

在 `kernel/tty/tty.c` 末尾（`tty_write` 之后，`get_dev_tty` 之前）新增：

```c
// ── ioctl — line discipline control ─────────────
// Maps between kernel lflag bits and POSIX termios flags.
// Only ICANON and ECHO are live; ISIG is mandatory.

int tty_ioctl(tty_t *tty, int cmd, void *arg)
{
    if (!tty || !arg)
        return -EINVAL;

    struct termios *tio = (struct termios *)arg;

    switch (cmd) {
    case TCGETS: {
        // Read current line discipline into termios
        tio->c_iflag = ICRNL | IXON | BRKINT;
        tio->c_oflag = OPOST | ONLCR;
        tio->c_cflag = CS8 | CREAD | CLOCAL;
        tio->c_lflag = ISIG;  // ISIG is always on
        if (tty->lflag & TTY_L_ICANON) tio->c_lflag |= ICANON;
        if (tty->lflag & TTY_L_ECHO)   tio->c_lflag |= ECHO;
        tio->c_lflag |= ECHOE | ECHOK | ECHOCTL;
        tio->c_line = 0;
        tio->c_cc[VINTR]    = 0x03;
        tio->c_cc[VQUIT]    = 0x1c;
        tio->c_cc[VERASE]   = 0x7f;
        tio->c_cc[VKILL]    = 0x15;
        tio->c_cc[VEOF]     = 0x04;
        tio->c_cc[VTIME]    = 0;
        tio->c_cc[VMIN]     = 1;
        tio->c_cc[VSTART]   = 0x11;
        tio->c_cc[VSTOP]    = 0x13;
        tio->c_cc[VSUSP]    = 0x1a;
        tio->c_cc[VEOL]     = 0;
        tio->c_cc[VREPRINT] = 0x12;
        tio->c_cc[VDISCARD] = 0x0f;
        tio->c_cc[VWERASE]  = 0x17;
        tio->c_cc[VLNEXT]   = 0x16;
        tio->c_cc[VEOL2]    = 0;
        tio->__c_ispeed = 38400;
        tio->__c_ospeed = 38400;
        return 0;
    }

    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        // Write: apply ICANON and ECHO; ISIG is force-enabled
        tty->lflag = TTY_L_ISIG;
        if (tio->c_lflag & ICANON) tty->lflag |= TTY_L_ICANON;
        if (tio->c_lflag & ECHO)   tty->lflag |= TTY_L_ECHO;

        // Reset canonical line buffer when mode changes
        tty->line_len = 0;
        tty->read_pos = 0;
        tty->line_ready = false;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}
```

**头文件**: 在 `kernel/tty/tty.c` 顶部已有 include 区中新增 `#include <termios.h>`。`ICANON`/`ECHO`/`ISIG` 等宏在 `libc/include/termios.h` 中定义，`trap.c:24` 已经这样用且正常工作。内核 `TTY_L_ICANON` 等宏（`tty.h` 中的 `1<<0` 格式）与 POSIX 的 `ICANON`（`0000002` 格式）数值完全不同，没有命名冲突。

- [ ] **Step 3: 编译验证**

```bash
make -j$(nproc) 2>&1 | grep -E "error:|undefined reference" || echo "OK"
```

预期：编译成功。

- [ ] **Step 4: 提交**

```bash
git add kernel/tty/tty.c kernel/include/kernel/tty.h
git commit -m "feat: add tty_ioctl() for TCGETS/TCSETS line discipline control

Maps TTY_L_ICANON/ECHO to POSIX termios c_lflag. ISIG is force-enabled.
tty_ioctl() is called from trap.c SYS_ioctl when fd refers to /dev/tty.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: ioctl 路由 — trap.c 中 TCSETS/TCSETSW 连接到 tty_ioctl

**Files:**
- Modify: `kernel/arch/x86_64/trap.c:1456-1534`（修改 TCSETS/TCSETSW 分支，TCGETS 改为调用 tty_ioctl）

**Interfaces:**
- Consumes: `tty_ioctl()` from Task 3，`get_dev_tty()` from Task 2

- [ ] **Step 1: 修改 trap.c 中 TCSETS/TCSETSW 分支**

将 `kernel/arch/x86_64/trap.c:1518-1523`：

```c
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            // tcsetattr — no-op
            regs->rax = 0;
            break;
```

替换为：

```c
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            struct termios *tio = (struct termios *)arg;
            if ((uint64_t)tio >= current->addr_limit) {
                regs->rax = -EFAULT;
                break;
            }
            tty_t *tty = get_dev_tty();
            if (!tty) { regs->rax = -ENOTTY; break; }
            regs->rax = tty_ioctl(tty, (int)request, tio);
            break;
        }
```

- [ ] **Step 2: 修改 trap.c 中 TCGETS 分支**

将 `kernel/arch/x86_64/trap.c:1485-1517`（整个 TCGETS 分支，硬编码返回值）替换为：

```c
        case TCGETS: {
            struct termios *tio = (struct termios *)arg;
            if ((uint64_t)tio >= current->addr_limit) {
                regs->rax = -EFAULT;
                break;
            }
            tty_t *tty = get_dev_tty();
            if (!tty) { regs->rax = -ENOTTY; break; }
            regs->rax = tty_ioctl(tty, (int)request, tio);
            break;
        }
```

- [ ] **Step 3: 添加缺失的 include**

`trap.c` 当前不包含 `<kernel/tty.h>`——但 `get_dev_tty()` 和 `tty_t` 类型都需要它。在 `trap.c` 顶部 include 区（`#include <termios.h>` 附近）新增：

```c
#include <kernel/tty.h>
```

- [ ] **Step 4: 编译验证**

```bash
make -j$(nproc) 2>&1 | grep -E "error:|undefined reference" || echo "OK"
```

- [ ] **Step 5: 提交**

```bash
git add kernel/arch/x86_64/trap.c
git commit -m "feat: route TCSETS/TCSETSW/TCGETS ioctl to tty_ioctl()

Replaces the hardcoded TCGETS return and no-op TCSETS/TCSETSW in trap.c
with calls to tty_ioctl() via get_dev_tty().  This makes tcsetattr()
actually toggle ICANON/ECHO on the console TTY.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: libc stub — tcsetattr 改为走 SYS_ioctl

**Files:**
- Modify: `libc/unistd/busybox_stubs.c:65-66`

**Interfaces:**
- Consumes: `SYS_ioctl` (already defined at `kernel/include/uapi/syscall.h:33`)

- [ ] **Step 1: 修改 tcsetattr stub**

将 `libc/unistd/busybox_stubs.c:66`：

```c
int tcsetattr(int fd, int a, const struct termios *tio) { (void)fd; (void)a; (void)tio; return 0; }
```

替换为：

```c
int tcsetattr(int fd, int a, const struct termios *tio) {
    (void)a;
    return (int)syscall(SYS_ioctl, (uint64_t)fd, (uint64_t)TCSETS, (uint64_t)tio);
}
```

- [ ] **Step 2: 编译验证**

```bash
make -j$(nproc) 2>&1 | grep -E "error:|undefined reference" || echo "OK"
```

- [ ] **Step 3: 提交**

```bash
git add libc/unistd/busybox_stubs.c
git commit -m "fix: tcsetattr stub now issues SYS_ioctl instead of no-op

tcsetattr(fd, a, tio) calls syscall(SYS_ioctl, fd, TCSETS, tio).
The 'a' (action) parameter is ignored in v1 — the kernel doesn't
differentiate TCSETS/TCSETSW/TCSANOW yet.

Important: x86_64 syscall ABI uses rdi/rsi/rdx only, so only 3
params are passed — tio lands in rdx → regs->rdx = arg in trap.c.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: console.c — VT100 CSI 解析器 + 光标闪烁（核心新模块）

**Files:**
- Create: `kernel/tty/console.c`
- Create: `kernel/include/kernel/console.h`
- Modify: `kernel/tty/Makefile`（如果没有自动通配符则需新增 console.o）

**Interfaces:**
- Produces: `void console_init(void)` — 注册光标闪烁回调和设置终端初始状态
- Produces: `void console_putchar(char c)` — 单字符驱动的 VT100 状态机 + 字符输出（用作 TTY 的 output_char 回调）
- Consumes: `putchar_at()` from Task 1
- Consumes: `jiffies` (extern) from `kernel/time/timer.c`
- Consumes: `font` (extern psf2_t *) and `Pos` (extern position) from printk.h

- [ ] **Step 1: 创建 console.h**

`kernel/include/kernel/console.h`：

```c
#ifndef _KERNEL_CONSOLE_H
#define _KERNEL_CONSOLE_H

// Initialize the software terminal — sets up cursor blink timer.
// Must be called after framebuffer is mapped and PIT is running.
void console_init(void);

// Feed one output character to the terminal.
// Implements a VT100 CSI subset state machine.  Characters that are
// not part of a recognized escape sequence are rendered to the
// framebuffer via putchar_at().
// Use as TTY output_char callback.
void console_putchar(char c);

// Called from PIT handler (100 Hz) to drive cursor blink.
// Safe to call from IRQ context.
void console_blink_tick(void);

#endif
```

- [ ] **Step 2: 创建 console.c 骨架 + 状态机**

`kernel/tty/console.c`：

```c
#include <kernel/console.h>
#include <kernel/printk.h>
#include <font.h>
#include <stdint.h>
#include <stdbool.h>

// ═══════════════════════════════════════════════════════
//  Terminal cursor state
// ═══════════════════════════════════════════════════════

static int term_cursor_row = 0;
static int term_cursor_col = 0;
static bool term_cursor_visible = true;   // ?25h/?25l
static bool term_blink_on = false;        // current blink phase
static int  term_blink_counter = 0;       // ticks since last toggle
static unsigned int term_fg = WHITE;
static unsigned int term_bg = BLACK;
static bool term_initialized = false;

// ═══════════════════════════════════════════════════════
//  VT100 CSI state machine
// ═══════════════════════════════════════════════════════

enum csi_state {
    CSI_NORMAL,
    CSI_ESC,
    CSI_BRACKET,    // after ESC [
    CSI_PARAM,      // accumulating digit(s)
    CSI_QMARK,      // after ESC [ ?
};

static enum csi_state cs = CSI_NORMAL;
static int cs_param = 0;
static bool cs_qmark = false;

// ═══════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════

// Erase from current cursor column to end of line (inclusive).
static void console_clear_to_eol(void)
{
    int max_cols = (int)(Pos.XResolution / font->width);
    for (int x = term_cursor_col; x < max_cols; x++) {
        putchar_at(x, term_cursor_row, term_fg, term_bg, ' ');
    }
}

// Draw or erase the blink cursor block at current position.
// blink_on=true → reverse video; blink_on=false → draw space.
static void console_draw_blink(bool on)
{
    if (!term_cursor_visible || !term_initialized)
        return;

    int max_cols = (int)(Pos.XResolution / font->width);
    int max_rows = (int)(Pos.YResolution / font->height);

    if (term_cursor_row < 0 || term_cursor_row >= max_rows) return;
    if (term_cursor_col < 0 || term_cursor_col >= max_cols) return;

    if (on) {
        // Reverse video: swap fg/bg, write a space block
        putchar_at(term_cursor_col, term_cursor_row, term_bg, term_fg, ' ');
    } else {
        // Restore: draw a space with normal colors
        putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, ' ');
    }
}

// Advance cursor one column; wrap to next line.
static void console_advance(void)
{
    int max_cols = (int)(Pos.XResolution / font->width);
    int max_rows = (int)(Pos.YResolution / font->height);

    term_cursor_col++;
    if (term_cursor_col >= max_cols) {
        term_cursor_col = 0;
        term_cursor_row++;
        if (term_cursor_row >= max_rows) {
            // Scroll: TODO for now just clamp
            term_cursor_row = max_rows - 1;
        }
    }
}

// Move cursor left by n (clamped).
static void console_cursor_left(int n)
{
    if (n <= 0) n = 1;
    term_cursor_col -= n;
    if (term_cursor_col < 0) term_cursor_col = 0;
}

// Move cursor right by n (clamped).
static void console_cursor_right(int n)
{
    if (n <= 0) n = 1;
    int max_cols = (int)(Pos.XResolution / font->width);
    term_cursor_col += n;
    if (term_cursor_col >= max_cols) term_cursor_col = max_cols - 1;
}

// Commit a normal (non-escape) character to the framebuffer.
static void console_put_normal(char c)
{
    if (c == '\n') {
        // Erase blink before moving cursor
        if (term_blink_on) console_draw_blink(false);
        term_cursor_col = 0;
        term_cursor_row++;
        int max_rows = (int)(Pos.YResolution / font->height);
        if (term_cursor_row >= max_rows) {
            // TODO: scroll — for v1 just clamp
            term_cursor_row = max_rows - 1;
        }
        return;
    }

    if (c == '\r') {
        if (term_blink_on) console_draw_blink(false);
        term_cursor_col = 0;
        return;
    }

    if (c == '\b' || c == 0x7F) {
        if (term_blink_on) console_draw_blink(false);
        if (term_cursor_col > 0) term_cursor_col--;
        // Erase the character at the new position
        putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, ' ');
        return;
    }

    if (c == '\t') {
        if (term_blink_on) console_draw_blink(false);
        int max_cols = (int)(Pos.XResolution / font->width);
        term_cursor_col = (term_cursor_col + 8) & ~7;
        if (term_cursor_col >= max_cols) term_cursor_col = max_cols - 1;
        return;
    }

    // Printable characters (space and above) and anything else
    if (c >= ' ' || c < 0) {
        if (term_blink_on) console_draw_blink(false);
        putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, c);
        console_advance();
    }
}

// ═══════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════

// Feed one character through the VT100 state machine + render.
void console_putchar(char c)
{
    if (!term_initialized) return;

    switch (cs) {
    case CSI_NORMAL:
        if (c == '\x1b') {
            cs = CSI_ESC;
            return;
        }
        console_put_normal(c);
        break;

    case CSI_ESC:
        if (c == '[') {
            cs = CSI_BRACKET;
            cs_param = 0;
            cs_qmark = false;
        } else {
            // Unrecognized ESC sequence — discard, return to normal
            cs = CSI_NORMAL;
        }
        break;

    case CSI_BRACKET:
        if (c == '?') {
            cs = CSI_QMARK;
            cs_qmark = true;
        } else if (c >= '0' && c <= '9') {
            cs = CSI_PARAM;
            cs_param = c - '0';
        } else {
            // Terminal character with no parameter
            switch (c) {
            case 'D':  // Cursor Left (default 1)
                if (term_blink_on) console_draw_blink(false);
                console_cursor_left(1);
                break;
            case 'C':  // Cursor Right (default 1)
                if (term_blink_on) console_draw_blink(false);
                console_cursor_right(1);
                break;
            case 'K':  // Erase to end of line
                if (term_blink_on) console_draw_blink(false);
                console_clear_to_eol();
                break;
            default:
                break;
            }
            cs = CSI_NORMAL;
        }
        break;

    case CSI_PARAM:
        if (c >= '0' && c <= '9') {
            cs_param = cs_param * 10 + (c - '0');
        } else {
            // Terminal character with parameter
            switch (c) {
            case 'D':  // Cursor Left n
                if (term_blink_on) console_draw_blink(false);
                console_cursor_left(cs_param);
                break;
            case 'C':  // Cursor Right n
                if (term_blink_on) console_draw_blink(false);
                console_cursor_right(cs_param);
                break;
            case 'K':  // Erase to end of line (param ignored)
                if (term_blink_on) console_draw_blink(false);
                console_clear_to_eol();
                break;
            default:
                break;
            }
            cs = CSI_NORMAL;
        }
        break;

    case CSI_QMARK:
        if (c >= '0' && c <= '9') {
            cs_param = cs_param * 10 + (c - '0');
            cs = CSI_PARAM;  // continue parsing param after ?
        } else {
            // Terminal character with ? prefix (private mode)
            switch (c) {
            case 'h':  // DECSET — enable mode
                if (cs_param == 25) {
                    // Show cursor
                    term_cursor_visible = true;
                    term_blink_on = false;
                    term_blink_counter = 0;
                }
                break;
            case 'l':  // DECRST — disable mode
                if (cs_param == 25) {
                    // Hide cursor
                    if (term_blink_on) console_draw_blink(false);
                    term_cursor_visible = false;
                    term_blink_on = false;
                }
                break;
            default:
                break;
            }
            cs = CSI_NORMAL;
        }
        break;
    }
}

// ═══════════════════════════════════════════════════════
//  Cursor blink — called from PIT handler (100 Hz)
// ═══════════════════════════════════════════════════════

void console_blink_tick(void)
{
    if (!term_initialized) return;
    if (!term_cursor_visible) return;

    term_blink_counter++;
    if (term_blink_counter >= 50) {  // 500ms @ 100Hz
        term_blink_counter = 0;
        term_blink_on = !term_blink_on;
        console_draw_blink(term_blink_on);
    }
}

// ═══════════════════════════════════════════════════════
//  Init
// ═══════════════════════════════════════════════════════

void console_init(void)
{
    // Start terminal cursor at the current kernel printk position row
    term_cursor_row = Pos.YPosition;
    term_cursor_col = 0;
    term_cursor_visible = true;
    term_blink_on = false;
    term_blink_counter = 0;
    term_fg = WHITE;
    term_bg = BLACK;
    term_initialized = true;
    cs = CSI_NORMAL;
    cs_param = 0;
    cs_qmark = false;
}
```

- [ ] **Step 3: 检查 kernel/tty/Makefile**

```bash
cat /home/aagu/OS01/kernel/tty/Makefile 2>/dev/null || echo "No Makefile"
```

如果 Makefile 使用通配符 `*.c` 则无需修改。如果需要显式列出，新增 `console.o`。

- [ ] **Step 4: 编译验证**

```bash
make -j$(nproc) 2>&1 | grep -E "error:|undefined reference" || echo "OK"
```

- [ ] **Step 5: 提交**

```bash
git add kernel/tty/console.c kernel/include/kernel/console.h
git commit -m "feat: add VT100 CSI terminal emulator (console.c)

console_putchar() implements a VT100 CSI subset state machine:
  ESC [ D    — cursor left
  ESC [ n D  — cursor left n
  ESC [ C    — cursor right
  ESC [ n C  — cursor right n
  ESC [ K    — clear to end of line
  ESC [ ?25h — show cursor
  ESC [ ?25l — hide cursor

console_blink_tick() toggles reverse-video cursor every 500ms
(50 PIT ticks at 100Hz).  Safe in IRQ context — framebuffer is
kernel-identity mapped and putchar_at() uses no locks.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: 键盘驱动 — 方向键 → VT100 转义序列

**Files:**
- Modify: `kernel/driver/keyboard.c:65-188`（扩展 kbd_key、ext_scancode_tbl、translate_and_push）
- Modify: `kernel/include/driver/keyboard.h:1-26`（新增 `keyboard_get_tty` 声明）

**Interfaces:**
- Produces: `tty_t *keyboard_get_tty(void)` — keyboard.h 导出，供 translate_and_push 读取 lflag
- Consumes: `kbd_tty` (existing static in keyboard.c), TTY lflag bits

- [ ] **Step 1: 扩展 kbd_key 结构体 + 定义特殊键码**

在 `kernel/driver/keyboard.c` 中将 `struct kbd_key` 的字段类型从 `char` 改为 `int`（65-68 行）：

```c
// Special key codes (> 0xFF, to avoid collision with ASCII).
// These are expanded to multi-byte VT100 escape sequences in
// translate_and_push when the TTY is in raw mode.
#define K_UP    0x100
#define K_DOWN  0x101
#define K_LEFT  0x102
#define K_RIGHT 0x103
#define K_HOME  0x104
#define K_END   0x105

struct kbd_key {
    int base;
    int shift;
};
```

- [ ] **Step 2: 更新 ext_scancode_tbl**

将 `kernel/driver/keyboard.c:101-108` 的 `ext_scancode_tbl` 替换为：

```c
static const struct kbd_key ext_scancode_tbl[128] = {
    [0x1C] = { '\n', '\n' },         // KP Enter
    [0x35] = { '/',  '/'  },         // KP /
    [0x47] = { K_HOME,  K_HOME  },   // E0 47 = Home
    [0x48] = { K_UP,    K_UP    },   // E0 48 = ↑
    [0x49] = { K_UP,    K_UP    },   // E0 49 = Page Up (as ↑ for v1)
    [0x4B] = { K_LEFT,  K_LEFT  },   // E0 4B = ←
    [0x4D] = { K_RIGHT, K_RIGHT },   // E0 4D = →
    [0x4F] = { K_END,   K_END   },   // E0 4F = End
    [0x50] = { K_DOWN,  K_DOWN  },   // E0 50 = ↓
    [0x51] = { K_DOWN,  K_DOWN  },   // E0 51 = Page Down (as ↓ for v1)
    [0x52] = { K_HOME,  K_HOME  },   // E0 52 = Insert (as Home for v1)
    [0x53] = { '\x7f', '\x7f' },     // E0 53 = Delete
};
```

- [ ] **Step 3: 新增 VT100 序列推入函数 + 修改 translate_and_push**

在 `translate_and_push` 函数之前新增辅助函数：

```c
// Push a VT100 CSI escape sequence to the TTY.
// seq is the final character: 'A'=UP, 'B'=DOWN, 'C'=RIGHT, 'D'=LEFT
static void push_vt100_seq(tty_t *tty, char seq)
{
    tty_push_input(tty, '\x1b');   // ESC
    tty_push_input(tty, '[');      // [
    tty_push_input(tty, seq);      // final
}
```

然后修改 `translate_and_push` 的变量类型和逻辑顺序：

**关键修改 1**：将 `char c = 0;`（line 153）改为 `int c = 0;`。否则 `K_UP = 0x100` 赋值给 `char` 被截断为 `0`，后面 `if (c == 0) return;` 会把特殊键码直接丢弃。

**关键修改 2**：在 `if (c == 0) return;`（line 163）之后、Ctrl modifier 处理之前（line 165 之前），插入以下代码：

```c
    // ── Expand VT100 escape sequences for navigation keys ──
    if (c >= 0x100) {
        // Only emit escape sequences in raw mode; discard in canonical.
        if (!(kbd_tty->lflag & TTY_L_ICANON)) {
            switch (c) {
            case K_UP:    push_vt100_seq(kbd_tty, 'A'); break;
            case K_DOWN:  push_vt100_seq(kbd_tty, 'B'); break;
            case K_LEFT:  push_vt100_seq(kbd_tty, 'D'); break;
            case K_RIGHT: push_vt100_seq(kbd_tty, 'C'); break;
            case K_HOME:  push_vt100_seq(kbd_tty, 'H'); break;
            case K_END:   push_vt100_seq(kbd_tty, 'F'); break;
            default: break;
            }
        }
        return;
    }
```

**为什么不放在 Ctrl modifier 之后**：`c >= 0x100` 的高位不匹配任何 Ctrl 映射规则，但 `kbd_ctrl()` 分支里有 `if (c == 0) return;`（line 184），若 `K_UP` 先经过了奇怪的 Ctrl 转换可能变成其他值。放在 Ctrl 之前保证直接命中退出。

- [ ] **Step 4: 新增 keyboard_get_tty() 并导出**

在 `kernel/driver/keyboard.c` 中已有 `static tty_t *kbd_tty = NULL;`（line 115）。在文件末尾新增：

```c
tty_t *keyboard_get_tty(void)
{
    return kbd_tty;
}
```

在 `kernel/include/driver/keyboard.h` 末尾（`#endif` 之前）新增声明：

```c
// Get the TTY that receives translated input (for reading lflag, etc.).
tty_t *keyboard_get_tty(void);
```

- [ ] **Step 5: 编译验证**

```bash
make -j$(nproc) 2>&1 | grep -E "error:|undefined reference" || echo "OK"
```

- [ ] **Step 6: 提交**

```bash
git add kernel/driver/keyboard.c kernel/include/driver/keyboard.h
git commit -m "feat: map arrow keys to VT100 escape sequences in raw mode

Navigation keys (↑↓←→ Home End PgUp PgDn) now emit ESC [ X sequences
when the TTY is in raw mode (ICANON unset).  In canonical mode they
are silently dropped (current behavior preserved).

kbd_key.base/shift widened to int to hold K_UP/DOWN/LEFT/RIGHT/HOME/END
special key codes (> 0xFF).  keyboard_get_tty() exported for read access.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: 串联 — main.c 中注册 console_putchar + console_init

**Files:**
- Modify: `kernel/kernel/main.c:220-228`

**Interfaces:**
- Consumes: `console_init()`, `console_putchar()` from Task 6
- Consumes: `console_blink_tick()` — 需要注册到 PIT 回调

- [ ] **Step 1: 将 console_putchar 设为 TTY 的 output_char**

`kernel/kernel/main.c:222`，将 `tty_alloc(NULL, NULL)` 改为使用 `console_putchar`：

```c
    // ═══ 7. Console TTY ═════════════════════════════════════
    // tty_alloc with console_putchar as output — routes all
    // user-space writes through the VT100 CSI terminal emulator.
    // NULL for echo_char → defaults to same as output_char.
    tty_t *console = tty_alloc(console_putchar, NULL);
    if (console) {
        serial_set_tty(console);         // serial IRQ → TTY
        keyboard_set_tty(console);       // keyboard IRQ → TTY
        tty_set_dev_tty(console);        // /dev/tty read/write → TTY
        serial_printk("tty: console TTY created\n");
    }

    // Initialize the software terminal (cursor, VT100 state)
    console_init();
```

- [ ] **Step 2: 将 console_blink_tick 注册到 PIT 处理链**

`console_blink_tick` 需要在每个 PIT tick 被调用（IRQ 上下文，简单函数，只检查计数器+翻转像素）。最简单的方式是在 `kernel/driver/pit.c` 的 `pit_handler` 中直接调用。在 `pit_handler` 的 `serial_poll()` 调用之后添加：

```c
    // After serial_poll() in pit_handler (pit.c line 36):
    console_blink_tick();
```

这需要在 `kernel/driver/pit.c` 顶部新增 `#include <kernel/console.h>`。

- [ ] **Step 3: 在 main.c 顶部新增 include**

在 `kernel/kernel/main.c` 顶部新增：

```c
#include <kernel/console.h>
```

- [ ] **Step 4: 编译验证**

```bash
make -j$(nproc) 2>&1 | grep -E "error:|undefined reference" || echo "OK"
```

- [ ] **Step 5: 提交**

```bash
git add kernel/kernel/main.c kernel/driver/pit.c
git commit -m "feat: wire console_putchar as TTY output_char + blink timer

- tty_alloc(console_putchar, NULL): all user-space writes go through
  the VT100 CSI terminal emulator instead of raw tty_def_output.
- console_init() initializes terminal cursor after TTY creation.
- console_blink_tick() called from pit_handler (100Hz IRQ) for
  cursor blink — 500ms period, safe in IRQ context.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: Busybox 配置 — 启用 FEATURE_EDITING

**Files:**
- Modify: `thirdpart/busybox-1.36.1/.config`

**Interfaces:**
- None — kconfig only

- [ ] **Step 1: 修改 .config 中的编辑相关选项**

```bash
cd /home/aagu/OS01/thirdpart/busybox-1.36.1
# Enable line editing
sed -i 's/^# CONFIG_FEATURE_EDITING is not set/CONFIG_FEATURE_EDITING=y/' .config
sed -i 's/^CONFIG_FEATURE_EDITING_MAX_LEN=0/CONFIG_FEATURE_EDITING_MAX_LEN=1024/' .config
sed -i 's/^CONFIG_FEATURE_EDITING_HISTORY=0/CONFIG_FEATURE_EDITING_HISTORY=15/' .config
# Enable fancy prompt (PS1 escape sequences)
sed -i 's/^# CONFIG_FEATURE_EDITING_FANCY_PROMPT is not set/CONFIG_FEATURE_EDITING_FANCY_PROMPT=y/' .config
```

- [ ] **Step 2: 验证 .config 改动**

```bash
grep -E "FEATURE_EDITING|HISTORY" .config | grep -v "^#"
```

预期输出：
```
CONFIG_FEATURE_EDITING=y
CONFIG_FEATURE_EDITING_MAX_LEN=1024
CONFIG_FEATURE_EDITING_HISTORY=15
CONFIG_FEATURE_EDITING_FANCY_PROMPT=y
```

- [ ] **Step 3: 重新编译 busybox**

```bash
make -C /home/aagu/OS01/thirdpart/busybox-1.36.1 -j$(nproc) 2>&1 | tail -3
```

预期：编译成功，输出 `busybox_unstripped` 和最终链接成功。

- [ ] **Step 4: 重新编译整个 OS（kernel + busybox 镜像）

```bash
cd /home/aagu/OS01 && make -j$(nproc) 2>&1 | tail -5
```

- [ ] **Step 5: 提交**

```bash
git add thirdpart/busybox-1.36.1/.config
git commit -m "feat: enable CONFIG_FEATURE_EDITING=y and history in busybox

Enables libbb line editing (readline-like) in ash shell:
- FEATURE_EDITING=y + EDITING_MAX_LEN=1024
- FEATURE_EDITING_HISTORY=15 (↑/↓ recall)
- FEATURE_EDITING_FANCY_PROMPT=y (PS1 escape sequences)

Requires kernel-side VT100 CSI terminal + raw mode TTY to work.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: 构建 + 手动验证

**Files:** None (test only)

- [ ] **Step 0: 完整构建 OS 镜像**

```bash
cd /home/aagu/OS01 && make clean && make -j$(nproc) 2>&1 | tail -10
```

预期：编译成功，生成 boot image。

- [ ] **Step 1: 启动 QEMU + 验证 ash 方向键**

启动 QEMU（带 framebuffer）。进入 busybox ash 后：

1. 输入 `echo hello`，不要回车
2. 按 ← 方向键 3 次 → 光标移动到 'h' 上
3. 按 → 方向键 2 次 → 光标移动到 'l' 上
4. 验证光标在行内正确移动，不出现乱码（ESC [ D 序列被正确解释）

- [ ] **Step 2: 验证历史命令**

1. 回车执行当前命令（或 Ctrl-C 取消后输入 `ls` 并回车）
2. 按 ↑ → 显示上一条命令 (`ls`)
3. 按 ↑ 再按 ↑ → 显示更早的命令

- [ ] **Step 3: 验证行编辑**

1. 输入 `ecoh hello`
2. 按 ← 直到光标在 'o' 上
3. 按 Backspace 删除 'o'
4. 输入 `ho` 使单词变成 `echo hello`
5. 回车 → 输出 `hello`

- [ ] **Step 4: 验证光标闪烁**

1. 在 ash 提示符下，观察光标是否以约 2Hz 的频率闪烁
2. 等待 5 秒不按键 → 光标持续闪烁（不冻结）

- [ ] **Step 5: 验证 /bin/sh 回归（canonical mode）**

1. 运行 `/bin/sh`（非 busybox 的 shell）
2. 按方向键 → 不应有任何输出（静默丢弃）
3. 输入正常命令回车执行 → 正常

- [ ] **Step 6: 验证内核 printk 回归**

1. 观察启动日志在 framebuffer 上正常显示
2. 确认 `color_printk` 不受影响（Pos 结构未被 console 破坏）

- [ ] **Step 7: 回归测试套件**

如果存在自动化测试：

```bash
cd /home/aagu/OS01 && make test 2>&1 | tail -20
```

- [ ] **Step 8: 提交**

```bash
# 这个 task 不产生代码变更——所有改动已在前面提交
git log --oneline -10
```

---

## 依赖关系

```
Task 1 (putchar_at)     ← 完全独立，可首个执行
  ↓
Task 2 (dev_tty refactor)  ← 改 tty.c + tty.h
  ↓                          ┐
Task 3 (tty_ioctl)          │ 顺序执行：共享 tty.c/tty.h
  ↓                          ┘
Task 4 (trap ioctl)    ← 依赖 Task 2(get_dev_tty) + Task 3(tty_ioctl)
  ↓
Task 5 (libc stub)     ← 独立，可与 Task 3-4 并行
  ↓
Task 6 (console.c)     ← 依赖 Task 1(putchar_at)
  ↓
Task 7 (keyboard kbd_key) ← 独立，读 kbd_tty->lflag（已有代码保证非 NULL 时有意义）
  ↓
Task 8 (main.c wire-up)   ← 依赖 Tasks 2,3,6,7
  ↓
Task 9 (busybox config)   ← 独立（纯 .config edit）
  ↓
Task 10 (build + test)    ← 依赖所有
```
