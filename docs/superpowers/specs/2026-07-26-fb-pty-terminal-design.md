# /dev/fb mmap + PTY + terminal 用户态渲染 — 设计规格

**日期**: 2026-07-26
**状态**: 已确认
**分支**: 待创建

## 概述

将终端渲染从内核移入用户态程序 `terminal.elf`。内核保留原始数据传输
（TTY ring buffer、PTY pipe），VT100 解析、PSF2 字型渲染、行编辑全部
在用户态实现。`/dev/fb` 新增 mmap 支持，允许用户态直接写入 framebuffer。

### 目标

1. 实现 Unix98 风格 PTY（`/dev/ptmx` + `/dev/pts/N`），复用现有 pipe
2. `/dev/fb` 新增 mmap（`VM_IO` 映射）+ read（元数据）
3. tty.c 退化为 raw ring buffer + 阻塞读写（删除 canonical/line discipline）
4. console.c 退化为应急 panic 输出（删除 VT100 CSI 状态机、光标闪烁）
5. `/dev/tty` 实现 Linux 风格 `current->ctty` 指针（每进程控制终端）
6. 新建 `user/terminal.c`：VT100 终端 + PSF2 渲染 + 行编辑
7. busybox ash 无需改动（PTY slave 兼容 isatty/tcgetattr/tcgetpgrp）

---

## 架构

```
┌───────────────────────────────────────────────────────┐
│  Userspace                                             │
│                                                        │
│  ┌─────────────┐   PTY pair    ┌──────────────────┐   │
│  │terminal.elf  │◄════════════►│  ash (busybox)    │   │
│  │              │ master slave │                   │   │
│  │ • VT100 解析 │              │ stdin/stdout/stderr│   │
│  │ • PSF2 渲染  │              │                   │   │
│  │ • 行编辑     │              └──────────────────┘   │
│  │ • echo/^C   │                                      │
│  └──┬──────┬───┘                                      │
│     │      │                                           │
│  mmap│   read(fd=/dev/tty)                             │
│ ┌───▼──┐ ┌─▼──────┐                                   │
│ │/dev/fb│ │/dev/tty│  ← open 时查 current->ctty       │
│ └──────┘ └────────┘                                   │
├───────────────────────────────────────────────────────┤
│  Kernel                                               │
│                                                        │
│  ┌────────────┐ ┌───────────┐ ┌──────────────────┐   │
│  │ fb mmap    │ │ tty (raw) │ │ pty (双 pipe)    │   │
│  │ handler    │ │ ring+wait │ │ + termios stub   │   │
│  │ +read meta │ │ ~100 行   │ │ ~250 行          │   │
│  └────────────┘ └─────▲─────┘ └──────────────────┘   │
│                       │                                │
│            ┌──────────┴─────────┐                     │
│            │ 键盘 IRQ            │ 串口 IRQ          │
│            │ translate_and_push │                     │
│            └────────────────────┘                     │
│                                                        │
│  /dev/tty open → 查 current->ctty → 返回 PTY slave    │
│  (物理 TTY 通过其他路径访问: 应急 console + IRQ)       │
│                                                        │
│  应急 console: putchar_at + 简单滚动 (无 VT100 解析)   │
│  仅 panic 和 terminal.elf 启动前短暂使用              │
└───────────────────────────────────────────────────────┘
```

### 内核保留 vs. 移出

| 保留在内核 | 移到用户态 |
|---|---|
| 键盘 scancode → ASCII 转换 | VT100/CSI 转义序列解析 |
| 原始 ring buffer + 阻塞读写 | 行编辑 (readline) |
| 等待队列 + poll | echo（回显） |
| 应急 panic 输出到 fb | ^C → SIGINT 生成 |
| PTY 数据传输管道 | PSF2 字型渲染 |
| /dev/fb mmap | 终端颜色管理 |
| termios 属性存储 (stub) | 物理光标渲染 |

### 设备节点

| 设备 | 现况 | 改造后 |
|---|---|---|
| `/dev/tty` | 有 (canonical mode + console 渲染) | 有 (current->ctty 魔数设备) |
| `/dev/fb` | **无** | **新增** (mmap + read metadata) |
| `/dev/ptmx` | **无** | **新增** (Unix98 PTY master 克隆设备) |
| `/dev/pts/0..N` | **无** | **新增** (动态注册 slave) |
| `/dev/keyboard` | 有 | 不变 |

### 启动流程

```
init
 ├─ fork → terminal.elf   (负责 fb 渲染 + 键盘/串口输入分发)
 │    ├─ open /dev/tty → 物理 TTY fd (ctty==NULL → 回退键盘/串口)
 │    ├─ open /dev/fb, read metadata, mmap
 │    ├─ open /dev/ptmx → master fd (/dev/pts/0 自动创建)
 │    ├─ open /dev/pts/0 → slave fd（使 slave 成为当前 session ctty）
 │    ├─ fork → ash
 │    │    └─ dup2(slave, 0/1/2) → close(slave) → exec("/bin/ash")
 │    │         ash 的 current->ctty 已继承自 fork = slave
 │    │         ash 内 open("/dev/tty") → ctty!=NULL → 返回 slave
 │    ├─ close(slave) → 释放 terminal 持有的 slave fd
 │    └─ loop: poll(tty_fd + pty_master) → render → write to pty
 │
 └─ 应急 console 保持活跃，仅 panic/early boot 使用
```

---

## 模块设计

---

### 1. PTY (Unix98 风格)

#### 设备注册

- `/dev/ptmx` — 字符设备。`open()` 时创建一对新的 PTY，返回 master fd。
- `/dev/pts/<N>` — 字符设备。由 `ptmx_open` 动态注册到 devfs。`open()` 返回 slave fd。
- 最大同时 PTY 数量：`PTY_MAX = 8`。

#### 数据结构

```c
#define PTY_MAX  8

typedef struct pty_struct {
    int         index;              // 0..7
    bool        allocated;

    pipe_t     *master_to_slave;    // terminal.elf → ash
    pipe_t     *slave_to_master;    // ash → terminal.elf

    // termios stub — 存储终端属性供 tcgetattr/tcsetattr
    struct termios  term;

    // 窗口大小
    uint16_t    ws_row;
    uint16_t    ws_col;

    // 前台进程组
    pid_t       pgrp;

    // 等待队列（slave 端 blocking read）
    list_t      read_wait;
    spinlock_T   read_wait_lock;
} pty_t;
```

#### 数据流

Master fd 操作：
- `read(master)` → 从 `slave_to_master` pipe 读（ash 的输出）
- `write(master)` → 写到 `master_to_slave` pipe（ash 的输入）
- `poll(master)` → POLLIN = slave 端有数据可读，POLLOUT = pipe 有空间
- `ioctl` → `-ENOTTY`（master 不是 TTY）

Slave fd 操作：
- `read(slave)` → 从 `master_to_slave` pipe 读（terminal.elf 的输入）
- `write(slave)` → 写到 `slave_to_master` pipe（ash 的输出）
- `poll(slave)` → 对应 pipe 的 POLLIN/POLLOUT
- `ioctl(slave, TCGETS)` → 返回 `pty->term`
- `ioctl(slave, TCSETS/TCSETSW)` → 存储到 `pty->term`（不改变实际行为）
- `ioctl(slave, TIOCGWINSZ)` → 返回 `pty->ws_row/ws_col`
- `ioctl(slave, TIOCSWINSZ)` → 设置后发 SIGWINCH 到 pty->pgrp
- `ioctl(slave, TIOCGPGRP)` → 返回 `pty->pgrp`
- `ioctl(slave, TIOCSPGRP)` → 设置 `pty->pgrp`

#### termios 初始化

PTY slave 预设为合理的"标准终端"值：
- `c_lflag`: ICANON | ECHO | ECHOE | ECHOK | ISIG
- `c_iflag`: ICRNL | IXON
- `c_oflag`: OPOST | ONLCR
- `c_cflag`: B38400 | CS8 | CREAD

#### 涉及文件

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/driver/pty.c` | 新建：PTY alloc/free、master/slave 读写、ioctl | ~250 |
| `kernel/include/kernel/pty.h` | 新建：pty_t + PTY_MAX + API 声明 | ~30 |
| `kernel/fs/devfs.c` | ptmx open 时注册 /dev/pts/N | ~20 |
| `kernel/kernel/main.c` | 调用 pty_init() | ~2 |

---

### 2. /dev/fb mmap

#### 设备注册

`/dev/fb` 按字符设备注册到 devfs，提供 `read`（元数据）+ `mmap`（像素缓冲区）。

#### VM_IO 映射类型

新增 `VM_IO` 标志（`vma.h`）：

```c
#define VM_IO  0x80   // 设备 MMIO — 页面不由 pmm/slab 管理
```

`do_mmap` 中校验：`VM_IO | MAP_PRIVATE` 的组合拒绝（设备映射必须 SHARED）。

#### fb_mmap handler

mmap 时一次性填入所有 PTE——因为 fb 物理地址已知且连续，不需要惰性映射：

```c
int fb_mmap(vfs_node_t *node, vma_t *vma)
{
    uint64_t length = vma->vm_end - vma->vm_start;
    uint64_t fb_size = Pos.XResolution * Pos.YResolution * 4;

    if (vma->vm_pgoff != 0)               return -EINVAL;
    if (length > fb_size)                  return -EINVAL;
    if (!(vma->vm_flags & VM_SHARED))      return -EINVAL;

    vma->vm_flags |= VM_IO;
    vma->vm_page_prot = PAGE_USER_4K | PAGE_PWT | PAGE_PCD;

    uint64_t *user_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
    for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_4K_SIZE) {
        uint64_t pa = Pos.Phy_addr + (va - vma->vm_start);
        vmm_map_4k_page(user_pml4, va, pa, vma->vm_page_prot);
    }
    return 0;
}
```

#### read 接口（元数据）

```c
struct fb_info {
    uint32_t width;          // 像素
    uint32_t height;
    uint32_t stride;         // 每行字节数 (= width * 4)
    uint32_t bpp;            // 32
    uint32_t format;         // 0 = XRGB8888
} __attribute__((packed));
```

`fb_read(node, offset, size, buf)`：`offset=0` 返回 `sizeof(fb_info)` 字节，`offset>0` 返回 0（EOF）。

#### #PF handler

无需特殊处理——PTE 已在 mmap 时填入，不会因惰性映射触发 #PF。越界访问正常 SIGSEGV。

#### devfs_ops 重构

`devfs_register_chrdev` 改为接收 ops struct：

```c
struct devfs_ops {
    int (*read)(vfs_node_t *, uint64_t offset, uint64_t size, void *buf);
    int (*write)(vfs_node_t *, uint64_t offset, uint64_t size, void *buf);
    uint32_t (*poll)(void *priv, struct poll_table *pt);
    int (*mmap)(vfs_node_t *, vma_t *);
    int (*ioctl)(vfs_node_t *, int cmd, void *arg);
};

int devfs_register_chrdev(const char *name, void *private_data,
                          const struct devfs_ops *ops);
```

#### 涉及文件

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/driver/fb.c` | 新建：fb_mmap + fb_read + 注册 | ~60 |
| `kernel/fs/devfs.c` | devfs_mmap 分派 + ops struct 重构 | ~30 |
| `kernel/include/fs/devfs.h` | devfs_ops struct | ~15 |
| `kernel/include/kernel/vma.h` | VM_IO 定义 | +2 |
| `kernel/memory/vma.c` | VM_IO | MAP_PRIVATE 校验 | ~5 |
| `kernel/kernel/main.c` | 注册 /dev/fb | ~3 |

---

### 3. tty.c 退化

#### 现状 (~350 行)
1. cooked ring buffer — `tty_cooked_push/pop`
2. canonical processing — `tty_canon_process`（行缓冲、退格、^C、echo）
3. blocking read — `tty_read`（cooked ring → canonical → 用户缓冲区）

#### 目标 (~100 行)
1. raw ring buffer — 不变（cooked 改名 raw）
2. canonical processing — **完全移除**
3. blocking read — **简化**为直接从 ring 拷贝到用户缓冲区

#### 退化后的 tty_t

```c
typedef struct tty_struct {
    // Raw ring buffer
    spinlock_T   ring_lock;
    char         ring[TTY_BUF_SIZE];
    volatile int head;
    volatile int tail;

    // Read wait queue
    spinlock_T   read_wait_lock;
    list_t       read_wait;

    // Poll
    list_t       read_poll;

    // Output callbacks（应急 console 用）
    void (*output_char)(char c);
    void (*echo_char)(char c);

    // ── 移除的字段 ──
    // char  line[TTY_BUF_SIZE];
    // int   line_len, read_pos;
    // bool  line_ready;
    // uint8_t lflag;
    // int64_t pgrp;
} tty_t;
```

#### 退化后的 tty_read

简化为从 ring buffer 直接拷贝，不做 canonical：

```
tty_read(tty, buf, size, nonblock):
  loop:
    // Phase 1: ring → buf（直接拷）
    while ring 非空 && copied < size:
      *buf++ = ring[tail]; tail++; copied++;
    if copied > 0: return copied

    if nonblock: return 0
    if signal_pending_fatal(): return 0

    // Phase 2: sleep
    enqueue read_wait
    double-check ring 非空
    schedule()
    dequeue read_wait
    check signals
```

#### 移除的功能

| 函数/字段 | 去向 |
|---|---|
| `tty_canon_process()` | terminal.elf 行编辑 |
| `tty->line[]`, `line_len`, `read_pos`, `line_ready` | terminal.elf 行缓冲 |
| `tty->lflag` (ICANON/ECHO/ISIG) | 概念上移到 terminal.elf |
| `tty->pgrp` | PTY slave 的 `pty->pgrp` |
| `TTY_L_ICANON/ECHO/ISIG` 宏 | 删除 |

#### 键盘模块适配

`keyboard.c` 中 `translate_and_push()` 的方向键分支不再检查 `kbd_tty->lflag & TTY_L_ICANON`——始终推送 VT100 转义序列到 TTY：

```c
if (c >= 0x100) {
    // 始终发 VT100 序列，不再检查 ICANON
    switch (c) {
    case K_UP:    push_vt100_seq(kbd_tty, 'A'); break;
    // ...
    }
    return;
}
```

#### 涉及文件

| 文件 | 改动 |
|---|---|
| `kernel/include/kernel/tty.h` | 删除 canonical 字段 + lflag + pgrp |
| `kernel/tty/tty.c` | tty_read 简化，删除 tty_canon_process/cooked pop/push |
| `kernel/driver/keyboard.c` | 方向键不再检查 ICANON |

---

### 4. console.c 退化

#### 移除
- VT100 CSI 状态机全部（`CSI_NORMAL/ESC/BRACKET/PARAM/QMARK`）
- 光标闪烁（`console_blink_tick`、`console_draw_blink`）
- PIT tick 回调

#### 保留
- `putchar_at()` — 字符渲染（在 `printk.c` 中，不动）
- `console_scroll()` — 简单滚动
- `console_putchar()` — 退化版本

#### 退化后的 console_putchar

```c
void console_putchar(char c)
{
    if (!term_initialized) return;

    switch (c) {
    case '\n':
        term_cursor_col = 0;
        term_cursor_row++;
        break;
    case '\r':
        term_cursor_col = 0;
        break;
    case '\b': case 0x7F:
        if (term_cursor_col > 0) term_cursor_col--;
        break;
    case '\t':
        term_cursor_col = (term_cursor_col + 8) & ~7;
        break;
    default:
        if ((unsigned char)c >= ' ') {
            putchar_at(term_cursor_col, term_cursor_row, term_fg, term_bg, c);
            term_cursor_col++;
        }
        break;
    }

    int max_cols = Pos.XResolution / font->width;
    if (term_cursor_col >= max_cols) {
        term_cursor_col = 0;
        term_cursor_row++;
    }
    int max_rows = Pos.YResolution / font->height;
    if (term_cursor_row >= max_rows)
        console_scroll();
}
```

#### 生存期管理

```c
void console_surrender_fb(void)    // terminal.elf 启动后调用
{
    term_initialized = false;       // console_putchar → no-op
}

void console_force_enable(void)    // panic 路径调用
{
    term_initialized = true;        // 恢复 fb 输出
}
```

#### 涉及文件

| 文件 | 改动 |
|---|---|
| `kernel/tty/console.c` | 删除 CSI 状态机 + 光标闪烁，console_putchar 退化，新增生存期函数 |
| `kernel/include/kernel/console.h` | 更新声明 |
| `kernel/driver/pit.c` | 移除 console_blink_tick 调用 |

---

### 5. /dev/tty 魔数设备（current->ctty）

#### 实现

- `task_struct`（或 `sched.h` 的 task 结构体）新增 `void *ctty` 字段，指向 `pty_t`
- `fork()` 时子进程继承父进程的 `ctty`
- `/dev/tty` 注册为魔数字符设备——`open` 时查 `current->ctty`：
  - 非 NULL → 返回对应 PTY slave 的 fd
  - NULL → 返回物理 TTY（`kbd_tty`）的 fd（回退机制）
- session leader 首次打开 PTY slave 时：`current->ctty = pty`
- PTY 释放时：遍历所有 task，`ctty == this → ctty = NULL`
- `/dev/tty` fd 在 open 时解析 ctty，之后无论 ctty 如何变化，fd 指向的设备不变

#### 时序与行为

terminal.elf 的启动顺序是关键——读键盘的 `/dev/tty` fd 必须在设置 ctty 之前打开：

```
terminal.elf:
  1. open("/dev/tty")        → ctty==NULL → 回退到物理 TTY fd (键盘/串口)
  2. open("/dev/ptmx")       → master fd，创建 pty[0]
  3. open("/dev/pts/0")      → slave fd，current->ctty = &ptys[0]
  4. fork()                  → ash 继承 ctty = pty[0]
  5. close(slave)            // terminal 不需要 slave fd（只用 master）
  
  // terminal.elf 的 tty_fd 仍指向物理 TTY（在步骤 1 已解析完毕）
  // ash 的 /dev/tty → ctty!=NULL → PTY slave

ash (fork 后):
  open("/dev/tty")           → ctty==&ptys[0] → PTY slave fd
  tcgetpgrp/tcsetpgrp        → 操作 PTY slave（兼容）
```

#### 涉及文件

| 文件 | 改动 | 行数 |
|---|---|---|
| `kernel/include/kernel/sched.h` | task 结构体加 `void *ctty` | +2 |
| `kernel/sched/task.c` | fork 继承 ctty | +2 |
| `kernel/driver/pty.c` | slave open 设 ctty，free 清 ctty | ~10 |
| `kernel/fs/devfs.c` | /dev/tty 魔数设备注册 + open handler | ~20 |

---

### 6. terminal.elf

#### 职责

1. 从 `/dev/tty` 读键盘/串口输入（统一字符流）
2. 从 PTY master 读 ash 输出
3. 解析 ash 输出中的 VT100 转义序列
4. PSF2 字型渲染到 framebuffer
5. 行编辑（echo、backspace、^C、方向键透传）
6. 处理后输入写入 PTY master → ash

#### 主循环

```c
int tty_fd  = open("/dev/tty", O_RDONLY);   // ctty==NULL → 回退物理 TTY
int fb_fd   = open("/dev/fb", O_RDWR);
int pty_fd  = open("/dev/ptmx", O_RDWR);    // 返回 master
// /dev/pts/0 被自动创建

// 读取 fb 元数据
struct fb_info info;
read(fb_fd, &info, sizeof(info));

// mmap framebuffer
uint32_t *fb = mmap(NULL, info.height * info.stride,
                    PROT_WRITE, MAP_SHARED, fb_fd, 0);

// 使 PTY slave 成为控制终端
int slave = open("/dev/pts/0", O_RDWR);    // current->ctty = slave

// Fork ash
int ash_pid = fork();
if (ash_pid == 0) {
    dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
    close(slave); close(pty_fd); close(tty_fd); close(fb_fd);
    exec("/bin/ash", NULL);
}

close(slave);  // terminal.elf 不需要在 master 端持有 slave fd

// 主循环
struct pollfd fds[2] = {
    {.fd = tty_fd, .events = POLLIN},
    {.fd = pty_fd, .events = POLLIN},
};
char buf[256];

while (1) {
    poll(fds, 2, -1);

    if (fds[0].revents & POLLIN) {    // /dev/tty: 键盘输入
        int n = read(tty_fd, buf, sizeof(buf));
        handle_input(buf, n, pty_fd, ash_pid, fb, &info);
    }
    if (fds[1].revents & POLLIN) {    // pty master: ash 输出
        int n = read(pty_fd, buf, sizeof(buf));
        handle_output(buf, n, fb, &info);
    }
}
```

#### 输入处理（handle_input）

```
对于每个字符 c：
  if c == '\r' || c == '\n':
      line_buf[line_len++] = '\n'
      write(pty_fd, line_buf, line_len)
      line_len = 0
      if echo: render_char('\r\n') → 光标移到下一行行首
  else if c == '\x7f' || c == '\b':
      if line_len > 0:
          line_len--
          if echo: render_backspace() → fb 上覆盖空格
  else if c == '\x03':                   // ^C
      kill(ash_pid, SIGINT)
  else if c == '\x04' && line_len == 0: // ^D → EOF
      传送 EOF 到 PTY master（close master write 端或发零长度）
  else if c == '\x1b':                  // ESC 开始 → 方向键
      读后续 2 字节构造 VT100 序列
      透传给 PTY master（raw 模式，ash 的 readline 自己处理）
  else if c >= ' ':
      line_buf[line_len++] = c
      if echo: render_char(c)
```

#### VT100 状态机（handle_output）

```
状态: NORMAL | ESC | CSI_PARAM

支持序列：
  ESC [ n A      上移光标
  ESC [ n B      下移光标
  ESC [ n C      右移光标
  ESC [ n D      左移光标
  ESC [ n K      清到行尾 (n=0: 光标到行尾, n=1: 行首到光标, n=2: 整行)
  ESC [ 2 J      清屏
  ESC [ H        光标归位 (0,0)
  ESC [ ? 25 h   显示光标
  ESC [ ? 25 l   隐藏光标

普通字符 → render_char_at_cursor(c)
\b → 光标左移 1
\t → 跳下一个 8 列对齐
\r → 光标列 = 0
\n → 光标行+1，必要时 scroll up
```

#### 渲染器

```c
// PSF2 字型到 fb——移植内核 printk.c 的 putchar_at 逻辑
void render_char(uint32_t *fb, struct fb_info *info,
                 psf2_t *font, int col, int row,
                 uint32_t fg, uint32_t bg, char c);

// 滚动：memmove fb 行 1..N-1 到 0..N-2，清最末行
void fb_scroll(uint32_t *fb, struct fb_info *info);
```

PSF2 字体内嵌到 terminal.elf（`objcopy -B i386 -I binary -O elf64-x86-64`）。

#### 文件布局

```
user/terminal.c      ~400 行 (新建)
  ├── vt100 parser    ~80 行
  ├── renderer        ~80 行 (PSF2 渲染 + scroll)
  ├── input handler   ~80 行 (行编辑 + echo)
  ├── main loop       ~50 行 (poll + dispatch)
  └── PTY/ash setup   ~40 行
```

---

### 7. busybox ash

#### 无需改动

ash 的 `setjobctl()` 正常运行：

```c
setjobctl(on):
    fd = open("/dev/tty", O_RDWR);   // → 查 current->ctty → PTY slave
    fcntl(fd, F_DUPFD_CLOEXEC, 10);  // → PTY slave 支持
    tcgetpgrp(fd);                   // → PTY slave TIOCGPGRP
    tcsetpgrp(fd, pgrp);             // → PTY slave TIOCSPGRP
```

ash 的读输入路径 `preadfd()` 不变——fd 0 已 dup2 到 PTY slave，`read(0, ...)` 等价于从 master→slave pipe 读。

目前 ash 的 `_PATH_TTY` 宏（`/dev/tty`）需要确保编译进 busybox 时是这个路径。检查现有 busybox 配置，如已是 `/dev/tty` 则无需改动。

---

## 总计

```
内核改动:
  kernel/driver/pty.c         | ~250  (新建：PTY alloc/free + master/slave I/O)
  kernel/driver/fb.c          |  ~60  (新建：fb mmap + read)
  kernel/include/kernel/pty.h |  ~30  (新建：pty_t + 宏)
  kernel/fs/devfs.c           |  ~50  (mmap 分派 + ops struct + ptmx/pts + /dev/tty 魔数)
  kernel/include/fs/devfs.h   |  ~15  (devfs_ops struct)
  kernel/include/kernel/vma.h |   +2  (VM_IO)
  kernel/memory/vma.c         |   +5  (VM_IO | MAP_PRIVATE 拒绝)
  kernel/tty/console.c        | -120  (删除 CSI 状态机 + 光标)
  kernel/tty/tty.c            | -250  (删除 canonical + 简化 read)
  kernel/include/kernel/tty.h |  -20  (精简字段)
  kernel/include/kernel/console.h | +8 (更新声明)
  kernel/include/kernel/sched.h | +2 (ctty 指针)
  kernel/sched/task.c         |   +2  (fork 继承 ctty)
  kernel/driver/keyboard.c    |   +2  (方向键不再检查 ICANON)
  kernel/kernel/main.c        |  ~10  (注册 fb + PTY init)
  ─────────────────────────────────
  内核净增                     | ~300 行 (+430 新增, -390 删除)

用户态改动:
  user/terminal.c             | ~400  (新建)
  user/Makefile               |   +5  (terminal.elf 构建规则)
  ─────────────────────────────────
  用户态净增                   | ~400 行

总计净增                       | ~700 行
```

---

## 实现顺序

1. **devfs_ops struct 重构** — 所有后续模块的基础接口
2. **VM_IO + /dev/fb mmap** — terminal.elf 的渲染基础
3. **tty.c 退化** — 键盘输入路径简化
4. **console.c 退化** — 移除 VT100 解析
5. **PTY 实现** — 包括 current->ctty、/dev/tty 魔数
6. **terminal.elf** — 用户态终端
7. **busybox ash 对接 + init 编排** — 集成测试
8. **测试 + 文档**
