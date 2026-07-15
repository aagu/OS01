# Busybox ash 方向键 + 光标闪烁支持

**日期**: 2026-07-16
**状态**: 已批准
**范围**: OS01 内核 + busybox 配置

## 概述

让 busybox ash shell 支持方向键导航、行编辑和光标闪烁。这是四层缺失叠加的结果——三层在 OS01 内核，一层在 busybox 配置。四个层都需要改动。

## 动机

当前在 QEMU 中运行 busybox ash 时：
- 方向键被键盘驱动静默丢弃（`ext_scancode_tbl` 全为 `{0,0}`）
- TTY 永远运行在 canonical 模式（`TCSETS` ioctl 是 no-op），无法切换到 raw mode
- 帧缓冲没有光标概念——`putchark` 只是把像素写到显存
- busybox `CONFIG_FEATURE_EDITING=n`，即使有 raw mode 也做不了行编辑

用户只能逐字输入命令，无法回退编辑或使用方向键。

## 架构

```
                    ┌─────────────────────────────────┐
                    │  ash (CONFIG_FEATURE_EDITING=y)  │
                    │  tcsetattr(fd, TCSANOW, &raw)    │
                    │  read() ← VT100 seqs from kbd    │
                    │  write() → "echo\033[K\033[1C"  │
                    └──────────┬───────────┬──────────┘
                               │ read      │ write
                    ┌──────────▼───────────▼──────────┐
                    │     /dev/tty (devfs.c)           │
                    │  dev_tty_read → tty_read         │
                    │  dev_tty_write → tty_write       │
                    └──────────┬───────────┬──────────┘
                               │           │
              ┌────────────────▼──┐   ┌────▼──────────────┐
              │  TTY Line Disc.   │   │  TTY output path   │
              │  tty_canon_process│   │  tty->output_char  │
              │  (raw: pass-thru) │   │       ↓            │
              │  ESC [ A → TTY    │   │  【NEW】console.c  │
              └───────────────────┘   │  VT100 CSI parser  │
                                      │  terminal cursor    │
                                      │  putchar_at(col,row)│
                                      │  cursor blink timer │
                                      └────────┬───────────┘
                                               │
                                      ┌────────▼───────────┐
                                      │  putchark (printk.c)│
                                      │  纯像素写入，无状态 │
                                      │  Pos.FB_addr[...]=c │
                                      └────────────────────┘
```

**模块总览：**

| 模块 | 文件 | 改动性质 | 预估行数 |
|------|------|----------|----------|
| 键盘驱动 | `kernel/driver/keyboard.c` | 扩展转义序列表 | ~30 |
| 键盘头文件 | `kernel/include/kernel/keyboard.h` | 添加 `keyboard_get_tty()` 声明 | ~5 |
| TTY ioctl | `kernel/tty/tty.c` + `kernel/include/kernel/tty.h` | 新增 `tty_ioctl()` | ~50 |
| ioctl 路由 | `kernel/arch/x86_64/trap.c` | TCSETS/TCSETSW → tty_ioctl | ~40 |
| libc stub | `libc/unistd/busybox_stubs.c` | tcsetattr/ioctl syscall 转型 | ~10 |
| 软件终端 | **新文件** `kernel/tty/console.c` + `kernel/include/kernel/console.h` | VT100 CSI 解析 + 光标 | ~250 |
| 帧缓冲辅助 | `kernel/kernel/printk.c` + `kernel/include/kernel/printk.h` | 新增 `putchar_at()` | ~30 |
| Busybox 配置 | `thirdpart/busybox-1.36.1/.config` | 启用 FEATURE_EDITING | ~5 |

**总预估：~420 行新增/修改代码。**

## 模块设计

### 1. 键盘驱动 — VT100 转义序列映射

**文件**: `kernel/driver/keyboard.c`

**当前问题**: `ext_scancode_tbl[128]` 的 `struct kbd_key` 只存 `char base; char shift;`。导航键（Home、End、方向键、PgUp、PgDn）全用 `{0,0}` 占位 → 按了没反应。

**方案**: 引入特殊键码（> 0xFF），`translate_and_push` 在 raw mode 下将它们展开为 3 字节 VT100 序列：

```c
// 特殊键码（值 > 0xFF 避免与 ASCII 冲突）
#define K_UP    0x100
#define K_DOWN  0x101
#define K_LEFT  0x102
#define K_RIGHT 0x103
#define K_HOME  0x104
#define K_END   0x105

// ext_scancode_tbl 改为 unsigned int 类型
// 示例：
// [0x48] = { K_UP,    K_UP    },   // E0 48 = ↑
// [0x4B] = { K_LEFT,  K_LEFT  },   // E0 4B = ←
// [0x4D] = { K_RIGHT, K_RIGHT },   // E0 4D = →
// [0x50] = { K_DOWN,  K_DOWN  },   // E0 50 = ↓
```

`translate_and_push` 的行为：
- **raw mode**（`ICANON` 关闭）：推入对应的 VT100 序列（`ESC [ A` / `ESC [ B` / `ESC [ C` / `ESC [ D` 等）
- **canonical mode**（`ICANON` 开启）：静默丢弃（如当前行为），避免回显乱码

`translate_and_push` 需要通过 `kbd_tty` 查询当前 `lflag` 来判断模式。

**边界情况**:
- Shift+方向键不发转义序列（已经由 `{K_UP, K_UP}` 保证 shift 和 base 相同）
- Ctrl+方向键不单独处理（与 Linux 行为一致，Ctrl+方向键生成不同序列，v1 不实现）

### 2. TTY ioctl 打通

**涉及文件**: `libc/unistd/busybox_stubs.c`，`kernel/arch/x86_64/trap.c`，`kernel/tty/tty.c`，`kernel/include/kernel/tty.h`

#### 2a. libc stub 转型

**文件**: `libc/unistd/busybox_stubs.c`

当前 `tcsetattr` 是空壳。改为走 `SYS_ioctl`：

```c
int tcsetattr(int fd, int a, const struct termios *tio) {
    return (int)syscall(SYS_ioctl, (uint64_t)fd, (uint64_t)TCSETS,
                        (uint64_t)a, (uint64_t)tio);
}
```

`tcgetattr` 同理改为走 ioctl（当前 trap.c 中 `TCGETS` 的硬编码返回值也会改为调用 `tty_ioctl`，见 §2c）。

#### 2b. 内核 ioctl 路由（trap.c）

**文件**: `kernel/arch/x86_64/trap.c`

当前 `TCSETS`/`TCSETSW` 直接返回 0（no-op）。改为调用 TTY 层：

```c
case TCSETS:
case TCSETSW: {
    struct termios *tio = (struct termios *)arg;
    if ((uint64_t)tio >= current->addr_limit) {
        regs->rax = -EFAULT;
        break;
    }
    // 通过 tty.c 导出的 get_dev_tty() 获取控制台 tty
    extern tty_t *get_dev_tty(void);
    tty_t *tty = get_dev_tty();
    if (!tty) { regs->rax = -ENOTTY; break; }
    regs->rax = tty_ioctl(tty, (int)request, tio);
    break;
}
```

`get_dev_tty()` 是 `kernel/tty/tty.c` 中新增的导出函数（当前 `dev_tty` 全局变量只在 `main.c` 中可见，需要暴露接口或将其移入 tty.c）。

同样 `TCGETS` 从硬编码返回值改为调用 `tty_ioctl` 读取实际 TTY 状态。

#### 2c. TTY 层 tty_ioctl()

**文件**: `kernel/tty/tty.c`（新增函数），`kernel/include/kernel/tty.h`（新增声明）

```c
int tty_ioctl(tty_t *tty, int cmd, void *arg);
```

实现：
- `TCGETS`：从 `tty->lflag` 读取，映射 `TTY_L_ICANON → ICANON`，`TTY_L_ECHO → ECHO`，其余填默认值
- `TCSETS`：从 `arg` 写入 `tty->lflag`，映射 `ICANON → TTY_L_ICANON`，`ECHO → TTY_L_ECHO`。`ISIG` 强制保持启用（`^C` 必须总能工作）。其他 flag 位静默接受但不做任何事。
- `TCSETSW`：行为同 TCSETS（当前无输出缓冲区可排空，简化为直接 set）

**边界情况**:
- `fd` 不是 tty 类型文件 → `-ENOTTY`
- `arg` 指针越界 → `-EFAULT`

### 3. 软件终端 VT100 解析器

**新文件**: `kernel/tty/console.c` + `kernel/include/kernel/console.h`

这是整个改动的核心新模块。它位于 TTY write 路径和 framebuffer 之间——插入 TTY 的 `output_char` 回调中。

#### 3a. 职责

1. 维护独立的终端光标状态（`term_cursor_row` / `term_cursor_col`），区别于 `Pos` 结构的"内核 printk 打印头"
2. 扫描输出字节流，识别 `ESC [` CSI（Control Sequence Introducer）转义序列
3. 将普通字符转发到 `putchar_at(col, row)`
4. 通过 PIT 回调驱动光标闪烁

#### 3b. VT100 CSI 子集（v1）

| 序列 | 语义 | 实现 |
|------|------|------|
| `ESC [ D` | 光标左移 1 | 存 |
| `ESC [ n D` | 光标左移 n | 存 |
| `ESC [ C` | 光标右移 1 | 存 |
| `ESC [ n C` | 光标右移 n | 存 |
| `ESC [ K` | 清到行尾 | 存 |
| `ESC [ ?25h` | 显示光标 | 存 |
| `ESC [ ?25l` | 隐藏光标 | 存 |
| `ESC [ n @` | 插入 n 个空格 | **延后** |
| `ESC [ n P` | 删除 n 个字符 | **延后** |
| `ESC [ A` / `ESC [ n A` | 光标上移 | **延后** |
| `ESC [ B` / `ESC [ n B` | 光标下移 | **延后** |

**决策理由**: 光标左/右 + 清行 + 光标显示/隐藏 是 ash 行编辑的最小必要集。上/下移用于历史命令展开（history），但 v1 先不做（history 在上方回显而非移动物理光标）。插入/删除字符的 `@`/`P` 序列也是行编辑的重要部分，但 v1 可以先尝试验证架构是否正确再扩展。

#### 3c. 状态机

```
Bytes → [Normal] ── ESC ──→ [Escaped]
                             │
                             ├── '[' ──→ [CSI_Entry]
                             │            │ num=0, qmark=false
                             │            ├── digit ──→ [CSI_Param]
                             │            │              num = num*10 + (c-'0')
                             │            ├── '?' ──→ [CSI_QMark]
                             │            │            → 返回 CSI_Param
                             │            └── 终结字符 ──→ dispatch → [Normal]
                             │
                             └── 其他 ──→ [Normal]（丢弃，或 future 扩展）
```

状态机实现在一个函数 `console_putchar(char c)` 中，维护少量静态变量（state、param、qmark）。这个函数替代 `tty_def_output` 作为 TTY 的 `output_char` 回调。

#### 3d. 普通字符转发

字符 `>= ' '`（空格及以上）、`\r`、`\n`、`\b` 等控制字符直接驱动光标移动和 `putchar_at()`。行为与当前 `color_printk` 中对 `\n`/`\r`/`\b` 的处理一致，但操作的是终端光标而非全局 `Pos`。

#### 3e. 光标闪烁

**方案**: PIT 回调 + dirty flag（混合懒渲染）。

1. `console_init()` 中注册 `console_blink_tick()` 到已有的定时器回调链
2. 每 50 个 tick（500ms @ 100Hz），`console_blink_tick` 设置一个 volatile flag `console_blink_dirty = true`
3. 下次 `console_putchar()` 被调用时（无论哪个字符），检查 dirty flag：
   - 若 `console_cursor_visible && !console_cursor_hidden`：反转当前光标格的视频属性（交换前景色/背景色），调用 `putchar_at` 重绘该格
   - 清除 dirty flag
4. `ESC [ ?25l` 隐藏光标 → 先擦除当前闪烁块（恢复原始颜色），再设 `console_cursor_hidden = true`
5. `ESC [ ?25h` 显示光标 → 设 `console_cursor_hidden = false`，设 dirty flag 触发立即绘制

**为什么不用纯 PIT IRQ 回调直接画光标**: `putchar_at` 需要遍历 glyph 位图写入显存像素，而显存映射可能不在中断上下文中被安全访问（某些条件下会触发 page fault）。所以用 dirty flag 将实际渲染推迟到 task context（`tty_write` 路径）。

### 4. 帧缓冲辅助函数

**文件**: `kernel/kernel/printk.c`

新增 `putchar_at(int col, int row, unsigned int FRcolor, unsigned int BKcolor, char c)`，基于 `putchark` 但接受行列参数，计算像素地址时用 `col * font->width` 和 `row * font->height` 替代 `Pos.XPosition/YPosition`。

在 `console.c` 中字符写入时使用 `putchar_at`；内核 `color_printk` 仍然使用 `putchark`（操作全局 `Pos`）。两者互不干扰，但共享同一个 framebuffer 区域。即 `color_printk` 写入的区域可能被 `console.c` 覆盖（如果行列重叠）。这是预期行为——`/dev/fb` 和终端输出应该共享同一视觉空间。

不对 `Pos` 结构做修改。

### 5. Busybox 配置

**文件**: `thirdpart/busybox-1.36.1/.config`

```
-CONFIG_FEATURE_EDITING=n → CONFIG_FEATURE_EDITING=y
-CONFIG_FEATURE_EDITING_MAX_LEN=0 → CONFIG_FEATURE_EDITING_MAX_LEN=1024
-CONFIG_FEATURE_EDITING_HISTORY=0 → CONFIG_FEATURE_EDITING_HISTORY=15
-CONFIG_FEATURE_EDITING_FANCY_PROMPT → 启用（ash 会输出 PS1 转义序列）
```

改完后需要 `make -C thirdpart/busybox-1.36.1` 重新编译。

## 数据流追踪（raw mode，用户按 → 键）

```
1. 用户按 →（Scancode Set 1: E0 4D）
2. 键盘 ISR 收到 E0（设 ext=true），再收到 4D（keydown）
3. translate_and_push(0x4D, ext=true):
   - ext_scancode_tbl[0x4D] = {K_RIGHT, K_RIGHT}
   - lflag & ICANON == 0 → raw mode → 推入 3 字节
   - tty_push_input(kbd_tty, '\x1b')  // ESC
   - tty_push_input(kbd_tty, '[')     // [
   - tty_push_input(kbd_tty, 'C')     // C
4. ash 调用 read(0, buf, 64):
   - tty_read → cooked ring 里逐个弹出 ESC,[,C
   - canonical==false → 直接返回 1 字节 (ESC)
   - ash 需要再读 2 次才能拿到完整序列
5. ash libbb/lineedit.c 解析 ESC [ C → 光标右移
6. ash write(1, "\033[1C", 4):
   - tty_write → console_putchar('\x1b') → 状态机进 Escaped
   - console_putchar('[') → 状态机进 CSI_Entry
   - console_putchar('1') → 状态机进 CSI_Param, num=1
   - console_putchar('C') → dispatch: 光标右移 1 → term_cursor_col++
   - 刷新：重绘光标位置
```

## 测试策略

**手动验证清单**:

1. **方向键**：在 ash 中输入文本，按 ← → → 光标在行内移动
2. **历史命令**：按 ↑ 调出上一条命令
3. **行编辑完整流程**：输入 `ecoh hello`，按 ← ← ← 到 'o'，退格删除 → 输入 `ho` → 回车 → 输出 `hello` 正确
4. **光标闪烁**：进入 ash 后光标在命令提示符后可见且以 ~2Hz 闪烁
5. **回归**：`color_printk`（内核日志）在 framebuffer 上的输出不受破坏
6. **回归**：旧 `/bin/sh`（非 busybox shell）仍能正常工作（canonical mode 未受影响）
7. **canonical mode 下方向键**：在 `/bin/sh` 中按方向键不产生任何输出（静默丢弃，不输出乱码）

## 不做的事（v1 范围外）

- 不实现 `ESC [ A` / `ESC [ B`（上/下移动）
- 不实现 `ESC [ n @` / `ESC [ n P`（插入/删除字符）
- 不改变 `color_printk` 的行为或 `Pos` 结构
- 不实现 VT100 中除 CSI 以外的转义（如 OSC、DCS 等）
- 不实现 framebuffer 滚动（由 `color_printk` 的现有 memmove 逻辑处理，`console.c` 不需要独立的滚动——kernel log 输出很少）
