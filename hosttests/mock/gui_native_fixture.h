#ifndef _GUI_NATIVE_FIXTURE_H
#define _GUI_NATIVE_FIXTURE_H

/*
 * gui_native_fixture.h — 宿主私有的原生控件 fixture（GUI-2b）
 *
 * 这是**宿主专用**的私有 fixture：它可以出现后端类型（LVGL、鼠标事件），
 * 但它不进入公共头，也不进入目标库。公共契约测试自身只包含
 * gui 目录下的公共头、标准库、测试框架和本头。
 *
 * fixture 建在既有的 `gui_test_runtime` mock 之上（假时钟、分配故障注入），
 * 不复制第二份 mock。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gui/gui_input.h>
#include <gui/gui_platform.h>
#include <gui/gui_widget.h>
#include <uapi/mouse.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 宿主原生控件 fixture
 */
typedef struct {
    gui_platform_t *platform; /* 离屏平台（640x480） */
    gui_input_t *input;       /* 可选：合成输入 */
    gui_widget_t root;        /* 根控件身份；由测试显式创建，初始为 NONE */
    uint32_t delay;           /* 最近一次 step 给出的建议等待时间 */
} gui_native_fixture_t;

/**
 * @brief 建立 fixture
 *
 * 先复位宿主 mock（`gui_test_reset()`），再建一个明确的 OFFSCREEN
 * 640x480 平台；`with_input` 为真时再挂一个 SYNTHETIC 输入。
 *
 * `root` 初始化为 `GUI_WIDGET_NONE`：**注册表测试不会偷偷多出一个根**，
 * 需要根的测试自己显式创建。
 *
 * @return 0 成功；-1 失败（errno 已设置；失败时反向清理并保留 errno）
 */
int gui_native_fixture_create(gui_native_fixture_t *fx, bool with_input);

/**
 * @brief 拆除 fixture（幂等）
 *
 * 释放顺序：输入 → 平台；随后清空所有指针。
 */
void gui_native_fixture_destroy(gui_native_fixture_t *fx);

/**
 * @brief 推进假时钟并跑一次真实的 `gui_platform_step()`
 *
 * @return 0 成功；-1 失败（平台状态不是 OK）
 */
int gui_native_fixture_step(gui_native_fixture_t *fx, uint64_t advance_ms);

/**
 * @brief 把合成的 Set 1 字节喂进真实的解码器/FIFO 路径
 *
 * 注入之后由测试显式 step。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_keys(gui_native_fixture_t *fx, const uint8_t *bytes, size_t len);

/**
 * @brief 把合成的鼠标事件喂进真实的 FIFO/适配层路径
 *
 * 注入之后由测试显式 step。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_mouse(gui_native_fixture_t *fx, const mouse_event_t *events, size_t count);

/* ── 宿主私有表查询 ─────────────────────────────────────────────────
 *
 * 公共 API 只暴露"身份是否有效"和"种类"；槽里的 handler/userdata/几何
 * 缓存/标志这些**复用契约**必须能被看见，否则"槽复用要全字段初始化"
 * 这条规则无法被验证。下面这些查询走的是真实注册表，不是模拟。
 */

/* 槽的四态（与 libgui 内部状态一一对应）。 */
#define GUI_NATIVE_SLOT_FREE       0
#define GUI_NATIVE_SLOT_RESERVED   1
#define GUI_NATIVE_SLOT_LIVE       2
#define GUI_NATIVE_SLOT_DESTROYING 3

/* 槽标志（与 libgui 内部标志一一对应）。 */
#define GUI_NATIVE_FLAG_VISIBLE   (1u << 0)
#define GUI_NATIVE_FLAG_ENABLED   (1u << 1)
#define GUI_NATIVE_FLAG_CLICKABLE (1u << 2)

/**
 * @brief 一个身份背后的槽（宿主私有视图）
 */
typedef struct {
    int state;         /* GUI_NATIVE_SLOT_* */
    uint32_t index;    /* 槽在表中的下标：据此断言"同一个槽被复用" */
    uint64_t id;       /* 槽里的身份 */
    uint64_t parent_id;/* 父身份（0 = 平台根） */
    int kind;          /* gui_widget_kind_t 的值 */
    bool has_handler;  /* 槽里是否还挂着业务回调 */
    bool has_userdata; /* 槽里是否还挂着回调上下文 */
    bool has_text;     /* 槽里是否还留着文字副本 */
    uint32_t flags;    /* 槽标志 */
    int32_t geom_x;    /* 几何缓存 */
    int32_t geom_y;
    int32_t geom_width;
    int32_t geom_height;
} gui_native_slot_info_t;

/**
 * @brief 注册表的占用计数（宿主私有视图）
 */
typedef struct {
    uint32_t count;    /* 当前占用的槽 */
    uint32_t peak;     /* 占用峰值 */
    uint32_t capacity; /* 固定容量 */
} gui_native_registry_info_t;

/**
 * @brief 查询一个身份背后的槽
 *
 * @return 0 成功（身份不存在时 `*out` 全零）；-1 失败
 */
int gui_native_fixture_query_slot(gui_platform_t *platform, gui_widget_t widget,
                                  gui_native_slot_info_t *out);

/**
 * @brief 查询注册表的占用计数与峰值
 *
 * @return 0 成功；-1 失败
 */
int gui_native_fixture_query_registry(gui_platform_t *platform, gui_native_registry_info_t *out);

/**
 * @brief 往某个身份的槽里塞入"陈旧"元数据（复用契约的探针）
 *
 * 宿主私有故障桥：它模拟"槽曾经装过别的东西"的残留状态，用来证明槽被
 * 回收再占用时这些字段全部被清干净。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_plant_slot(gui_platform_t *platform, gui_widget_t widget,
                                  uint32_t stale_flags, const char *stale_text);

/**
 * @brief 把进程全局的身份计数器推到末值（身份耗尽契约的探针）
 *
 * 计数器是进程全局的：它只在**独立子进程**里被推到末值，主进程不受
 * 影响。
 */
void gui_native_fixture_set_identity_last_id(uint64_t last_id);

/**
 * @brief 读回进程全局的身份计数器
 */
uint64_t gui_native_fixture_get_identity_last_id(void);

/**
 * @brief 后端内存池的空闲字节数（分配/释放配对的探针）
 *
 * @return 0 成功；-1 失败
 */
int gui_native_fixture_pool_free(gui_platform_t *platform, size_t *out_free_bytes);

/**
 * @brief 后端删除 fixture：绕开注册表直接删掉原生对象
 *
 * 这是**内部** fixture：公开测试仍然只通过值句柄判断身份是否还有效，
 * 不碰任何后端指针。
 *
 * @return 0 成功；-1 失败
 */
int gui_native_fixture_backend_delete(gui_platform_t *platform, gui_widget_t widget);

/**
 * @brief 后端状态（宿主私有视图）
 *
 * fixture 头允许出现后端类型（既有约定），但这些后端类型**不进入**任何
 * 公共头，也**不进入**目标 `libgui.a`：这里只是把后端状态抄成宿主可读
 * 的布尔量。
 */
typedef struct {
    bool clickable;        /* 对象带 LV_OBJ_FLAG_CLICKABLE */
    bool platform_display; /* 对象所属的 display 是本平台的 display */
    bool default_display;  /* 对象所属的 display 是 LVGL 的默认 display */
    bool in_group;         /* 对象属于某个 lv_group */
} gui_native_backend_info_t;

/**
 * @brief 查询一个身份背后的后端状态（命中标志 / display / 组）
 *
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_backend(gui_platform_t *platform, gui_widget_t widget,
                                     gui_native_backend_info_t *out);

/**
 * @brief 宿主私有：把 LVGL 的默认 display 清空
 *
 * 这样"原生创建没有取 lv_display_get_default()"就变成可观测的事实：默认
 * display 为空时创建仍然成功，说明实现取的是本平台自己的 display。
 *
 * @return 0 成功；-1 失败
 */
int gui_native_fixture_set_default_display_none(void);

/**
 * @brief 宿主私有：把 LVGL 的默认 display 恢复成本平台的 display
 *
 * @return 0 成功；-1 失败
 */
int gui_native_fixture_restore_default_display(gui_platform_t *platform);

/* 固定容量（与 libgui 内部一致；fixture 的 .c 里有静态断言守着）。 */
#define GUI_NATIVE_CAPACITY 1024u

#ifdef __cplusplus
}
#endif

#endif /* _GUI_NATIVE_FIXTURE_H */
