# 日志级别系统设计

> **日期**: 2026-07-12
> **状态**: 已批准
> **基准**: roadmap.md #5 (日志级别系统, P1)

---

## 1. 动机

当前日志有三大问题：

- **`serial_printk`/`color_printk` 无级别** — 调用即输出，无法过滤
- **`debug_<channel>` 宏仅编译期** — 运行时无法临时开启/关闭
- **ERR/WARN/INFO/DEBUG 混在一起** — 调试信息淹没过筛，关键错误丢失

## 2. 设计决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | API 风格 | `log(LEVEL, fmt, ...)` 函数风格 | 用户明确要求 |
| 2 | 实现方式 | 宏包裹 + lazy eval | 被过滤消息零 vsprintf 开销 |
| 3 | 输出目标 | 构建时选择 (serial/fb/both) | 灵活适配不同开发环境 |
| 4 | debug_<channel> 关系 | 逐步替代 | 统一系统，长期维护更简单 |
| 5 | 编译消除 | `NDEBUG` 时 `log_debug` 消失 | 生产 build 零开销 |

## 3. 日志级别

```c
#define LOG_ERR    3   // Error conditions
#define LOG_WARN   4   // Warning conditions
#define LOG_INFO   6   // Informational
#define LOG_DEBUG  7   // Debug-level messages
```

Linux 惯例：**数值越大越详细**。`log_set_level(LOG_WARN)` 只显示 ERR + WARN。

## 4. API

### 核心宏

```c
#define log(level, fmt, ...) do {                                \
    if ((level) <= g_log_level) {                                \
        _log_write(level, fmt, ##__VA_ARGS__);                   \
    }                                                            \
} while(0)
```

被过滤的消息仅执行一次整数比较，vsprintf 完全不执行。

### 便捷宏

```c
#define log_err(fmt, ...)   log(LOG_ERR,   fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log(LOG_WARN,  fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  log(LOG_INFO,  fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...) log(LOG_DEBUG, fmt, ##__VA_ARGS__)
```

### 运行时控制

```c
extern int g_log_level;      // 全局过滤级别
void log_set_level(int level);
int  log_get_level(void);
```

### 编译期消除

```c
#ifdef NDEBUG
#undef  log_debug
#define log_debug(fmt, ...) do {} while(0)
#endif
```

`make NDEBUG=1` 时 DEBUG 消息完全消失（零代码 + 零数据残留）。

## 5. 输出目标

### `_log_write()` 实现

```c
// Static buffer (NOT on stack — kernel stack is only 8KB).
// 1024 bytes is enough for any single log message; longer output
// is truncated safely by vsnprintf.
static char log_buf[1024];

void _log_write(int level, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);
    if (len < 0) return;
    if (len >= (int)sizeof(log_buf))
        len = (int)sizeof(log_buf) - 1;

#if LOG_TARGET_SERIAL
    write_serial_buf(log_buf, len);   // batch write — add if missing
#endif
#if LOG_TARGET_FB
    // Uses color_printk, which acquires Pos.lock internally.
    // Deadlock risk: if _log_write is called while Pos.lock is held
    // (e.g. from inside fb_dev_write), this would deadlock.  In
    // practice fb_dev_write does not trigger log calls, so the risk
    // is negligible.  If this changes, add a fb_write_buf() that
    // operates without Pos.lock.
    color_printk(level_to_color(level), BLACK, "%s", log_buf);
#endif
}
```

### 构建时选择

```makefile
# kernel/Makefile
# Helper: convert non-empty string to 1, empty to 0
bool = $(if $1,1,0)

LOG_TARGET ?= serial    # serial | fb | both
ALL_CFLAGS += -DLOG_TARGET_SERIAL=$(call bool,$(filter serial both,$(LOG_TARGET)))
ALL_CFLAGS += -DLOG_TARGET_FB=$(call bool,$(filter fb both,$(LOG_TARGET)))
```

### 级别→颜色映射（FB 输出）

| 级别 | 颜色 |
|------|------|
| LOG_ERR | RED |
| LOG_WARN | ORANGE |
| LOG_INFO | WHITE |
| LOG_DEBUG | LIGHT_GRAY (`0x00c0c0c0`) — 视觉上区别于 INFO |

## 6. 锁策略

`_log_write` 可能被多个上下文调用：

| 调用方 | 中断状态 | 说明 |
|--------|----------|------|
| Task 上下文 | IF 开 | 正常系统调用路径 |
| 中断 handler | IF 关 (trap gate) 或开 (int $0x80) | 时钟中断、IPI、系统调用 |

当前 `serial_printk` 使用 `spin_lock(&serial_lock)` 保护串口写缓冲区。但 `int $0x80` 使用 trap gate（IF 不变），所以 task 和 syscall handler 可以同时进入 `_log_write`。

**策略：使用 `spin_lock_irqsave` 保护输出**

```c
void _log_write(int level, const char *fmt, ...)
{
    unsigned long flags;
    spin_lock_irqsave(&log_lock, &flags);
    // ... vsnprintf + output ...
    spin_unlock_irqrestore(&log_lock, flags);
}
```

或者为 serial 和 FB 各设独立锁（复用 `serial_lock` 和 `Pos.lock`），但这要求 _log_write 不能跨上下文在持有 Pos.lock 时被调用。

**选择：统一 `spin_lock_irqsave`** — 简单、正确、不依赖调用者上下文。

### g_log_level 并发安全

`g_log_level` 是全局 int，被 `log()` 宏直接读取。多核场景下 `log_set_level()` 的写入可能与其他核心的读取并发，但日志级别变化的时序不关键 — 读到一个过渡值只会导致一两条消息多/少显示。**不做原子化或 volatile 处理**，注释注明 RELAXED 顺序可接受即可。

## 7. debug_<channel> 迁移

### 过渡期重定向

`debug.h` 的宏直接重定向到新的 `log_debug`：

```c
#define debug_sched(fmt, ...)  log_debug("[sched] " fmt, ##__VA_ARGS__)
#define debug_vfs(fmt, ...)    log_debug("[vfs] " fmt, ##__VA_ARGS__)
// ... 10 个通道全部类似
```

存量代码立刻获得运行时过滤能力，无需一次性全部修改。

### 迁移顺序

| Phase | 内容 | 文件 |
|-------|------|------|
| 1 | 创建 log.h/log.c，迁移 trap.c 异常 handler | `kernel/arch/x86_64/trap.c` |
| 2 | 迁移内存子系统、VFS 中 ERR 级消息 | `pmm.c`, `slab.c`, `vfs.c` |
| 3 | 迁移 debug_<channel> 调用 | `sched/task.c`, `sched/smp.c`, `fs/*` |
| 4 | 删除 debug.h；`serial_printk`/`color_printk` 标记 `__attribute__((deprecated))` 而非 static（它们仍被 hang.c、subsys.c、main.c 等少量路径使用，逐步淘汰） | 清理 |

## 8. 文件结构

```
新增:
  kernel/include/kernel/log.h    — 级别定义、log() 宏、便捷宏、NDEBUG 消除
  kernel/kernel/log.c            — _log_write()、g_log_level、log_set_level()

修改:
  kernel/include/kernel/debug.h  — 重定向到 log_debug (过渡)
  kernel/Makefile                — LOG_TARGET 构建选项
```

## 9. 数据流

```
log(level, fmt, ...)
  └→ if level <= g_log_level              ← 单次整数比较
       └→ _log_write(level, fmt, ...)
            ├→ spin_lock_irqsave(&log_lock)
            ├→ vsnprintf → static buf[1024]
            ├→ [serial] write_serial_buf(buf, len)
            ├→ [fb]     color_printk(level_color(level), BLACK, "%s", buf)
            ├→ [both]   两者都执行
            └→ spin_unlock_irqrestore(&log_lock)
```

## 10. 测试验证

- `make run` + selftest: ERR 消息正常输出
- `make run LOG_TARGET=both`: 串口 + FB 双写
- `make run NDEBUG=1`: DEBUG 消息完全消失
- **运行时过滤验证**: 内核内通过 `log_set_level()` 测试（编写一个简短的 log_level_test 函数在 init 序列中调用），或在 Phase 2+ 中通过 `/proc/sys/log_level` 文件暴露接口（可选，当前 P1 范围不强制）。

  ```c
  // 后续可通过 procfs 暴露（不在 P1 范围内）
  static int proc_log_level_read(char *buf, size_t size) {
      return snprintf(buf, size, "%d\n", g_log_level);
  }
  static int proc_log_level_write(const char *buf, size_t size) {
      int level;
      if (sscanf(buf, "%d", &level) == 1 &&
          level >= LOG_ERR && level <= LOG_DEBUG)
          g_log_level = level;
      return (int)size;
  }
  ```
