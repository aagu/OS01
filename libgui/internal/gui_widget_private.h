#ifndef _GUI_WIDGET_PRIVATE_H
#define _GUI_WIDGET_PRIVATE_H

/*
 * gui_widget_private.h — 原生控件注册表（libgui 内部桥接头）
 *
 * 本头文件是 libgui 内部桥接头：不进入 sysroot，公共应用看不到它。
 *
 * 它承载三样东西：
 *
 *   1. 每平台一张**固定容量**的控件表（1024 槽）：身份、归属、种类、
 *      handler/userdata、几何缓存与标志位都在槽里；
 *   2. 槽的四态生命周期：FREE → RESERVED → LIVE → DESTROYING → FREE；
 *   3. 跨模块私有入口（注册表的建立/拆除、身份计数、宿主测试注入）。
 *
 * 注册表由平台 calloc 出来，**不依赖后端内存池**：池在平台销毁时被整体
 * 释放，注册表必须活得比它更久（删除回调还要用它）。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gui/gui_event.h>
#include <gui/gui_widget.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 固定容量（全局约束：原生控件上限 1024，根与标签都计入） ──────── */

#define GUI_WIDGET_REGISTRY_CAPACITY 1024

/**
 * @brief 槽的生命周期状态
 *
 * FREE       空槽，可以被占用；
 * RESERVED   已占位但尚未发布：对象还在建，公共查询看不到它；
 * LIVE       已发布：公共 API 可以查到并使用它；
 * DESTROYING 正在销毁：公共查询立即失效，但槽**尚未**回到空闲表。
 */
typedef enum {
    GUI_WIDGET_SLOT_FREE = 0,
    GUI_WIDGET_SLOT_RESERVED,
    GUI_WIDGET_SLOT_LIVE,
    GUI_WIDGET_SLOT_DESTROYING
} gui_widget_slot_state_t;

/* ── 槽标志 ────────────────────────────────────────────────────────── */

#define GUI_WIDGET_FLAG_VISIBLE   (1u << 0) /* 可见 */
#define GUI_WIDGET_FLAG_ENABLED   (1u << 1) /* 使能 */
#define GUI_WIDGET_FLAG_CLICKABLE (1u << 2) /* 可点击 */

/**
 * @brief 注册表的一个槽
 *
 * 槽复用时**所有**字段都必须被重新初始化：旧身份、旧 handler、旧
 * userdata、旧几何缓存与旧标志一个都不许残留。
 */
typedef struct {
    gui_widget_slot_state_t state;   /* 槽状态 */
    uint64_t id;                      /* 身份号（0 = 无身份） */
    lv_obj_t *obj;                    /* 后端对象（不进入公共 API） */
    uint64_t parent_id;               /* 父控件身份（0 = 平台根） */
    gui_widget_kind_t kind;           /* 控件种类 */
    gui_event_handler_t handler;      /* 事件回调（行为在事件任务落地） */
    void *userdata;                   /* 回调上下文 */
    char *text;                       /* 文字副本（不借用调用方内存） */
    int32_t geom_x;                   /* 几何缓存：左 */
    int32_t geom_y;                   /* 几何缓存：上 */
    int32_t geom_width;               /* 几何缓存：宽 */
    int32_t geom_height;              /* 几何缓存：高 */
    uint32_t flags;                   /* 标志位 */
    bool pending_free;                /* 删除回调已发生，等待回收 */
} gui_widget_slot_t;

/**
 * @brief 每平台的控件注册表
 */
typedef struct {
    gui_widget_slot_t slots[GUI_WIDGET_REGISTRY_CAPACITY];
    uint32_t count;         /* 当前占用（RESERVED + LIVE + DESTROYING） */
    uint32_t peak;          /* 占用峰值 */
    uint32_t cursor;        /* 私有分配游标（不计入容量） */
    uint32_t callback_depth; /* 业务回调深度：>0 时禁止销毁/清空 */
} gui_widget_registry_t;

/* ── 注册表生命周期（platform.c ↔ widget.c） ─────────────────────── */

/**
 * @brief 建立平台的控件注册表
 *
 * @return 0 成功；-1 失败（errno = ENOMEM）
 */
int gui_widget_registry_init(gui_platform_t *platform);

/**
 * @brief 拆除平台的控件注册表（释放表，不碰后端对象）
 */
void gui_widget_registry_fini(gui_platform_t *platform);

/**
 * @brief 销毁平台上的整棵原生控件树
 *
 * 平台关闭时在**删除 display 之前**调用：先让原生树按公共销毁语义消失
 * （删除回调把身份立即置为失效），再删 display，最后释放注册表。
 */
void gui_widget_registry_destroy_tree(gui_platform_t *platform);

#ifdef OS01_HOST_TEST
/*
 * 宿主私有注入：身份计数的强制设置与读取。
 *
 * 只在宿主测试构建（-DOS01_HOST_TEST）里存在：目标构建不编译它，因此这个
 * 接口永远不会进入公共 sysroot，也永远不会出现在 libgui.a 里。
 * 身份耗尽契约测试在**独立子进程**里用它把计数推到末值。
 */
void gui_widget_test_set_last_id(uint64_t last_id);
uint64_t gui_widget_test_get_last_id(void);

/**
 * @brief 宿主私有表查询：把某个身份背后的槽复制出来
 *
 * 公共 API 只暴露"身份是否有效"和"种类"；槽里的 handler/userdata/几何
 * 缓存/标志这些**复用契约**必须能被宿主测试看见，否则"槽复用要全字段
 * 初始化"这条规则无法被验证。
 *
 * @return 0 成功（身份不存在时 `*out` 是全零的槽）；-1 失败
 */
int gui_widget_test_query_slot(gui_platform_t *platform, gui_widget_t widget, gui_widget_slot_t *out);

/**
 * @brief 宿主私有故障桥：往某个身份的槽里塞入"陈旧"元数据
 *
 * 复用契约测试用它证明：槽被回收再占用时，陈旧的 handler、userdata、
 * 文字、几何缓存与标志一个都不许残留。
 *
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_widget_test_plant_slot(gui_platform_t *platform, gui_widget_t widget, uint32_t stale_flags,
                               const char *stale_text);

/**
 * @brief 宿主私有表查询：注册表的占用计数与峰值
 *
 * @return 0 成功；-1 失败
 */
int gui_widget_test_get_counters(gui_platform_t *platform, uint32_t *out_count, uint32_t *out_peak);

/**
 * @brief 宿主私有后端删除：绕开注册表直接删掉原生对象
 *
 * 这模拟"后端在注册表背后自行删除对象"。删除回调照常运行，身份必须立刻
 * 失效；公开测试只通过**值句柄**判断，不碰后端指针。
 *
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_widget_test_backend_delete(gui_platform_t *platform, gui_widget_t widget);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _GUI_WIDGET_PRIVATE_H */
