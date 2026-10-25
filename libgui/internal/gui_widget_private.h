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
#include <gui/gui_layout.h>
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
 * 槽复用时**所有残留元数据**都必须被重新初始化：旧身份、旧 handler、旧
 * userdata、旧文字、旧几何缓存与旧标志一个都不许残留。
 *
 * 唯一的例外是 `index`：它是槽在表中的**位置**，不是上一任留下的元数据，
 * 只在注册表建立时写一次；清掉它等于毁掉槽自己的身份。
 */
typedef struct {
    gui_widget_slot_state_t state;   /* 槽状态 */
    uint32_t index;                  /* 槽在表中的下标（位置属性，不是残留元数据） */
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

    /* GUI-2b Task 4：尺寸来源缓存。两轴各有一个模式（PX/CONTENT/FILL），
     * 模式决定 `size_value_*` 是否被使用。CONTENT 父 + 同轴 FILL 子构成
     * 尺寸循环，这两组字段就是判定它的事实依据。 */
    gui_size_mode_t size_mode_w;
    gui_size_mode_t size_mode_h;
    int32_t size_value_w;
    int32_t size_value_h;

    /* 定位方式：absolute_pos 为真表示绝对坐标（set_geometry），为假表示
     * 锚点定位（set_alignment）。两套定位互不覆盖尺寸来源。 */
    gui_align_t anchor;
    int32_t align_x;
    int32_t align_y;
    bool absolute_pos;

    /* 布局缓存（Task 4）。 */
    gui_flow_t flow;
    gui_flow_align_t main_align;
    gui_flow_align_t cross_align;
    uint32_t grow;
    int32_t gap;
    int32_t pad_top;
    int32_t pad_bottom;
    int32_t pad_left;
    int32_t pad_right;

    /* 滚动与文本模式缓存（Task 4）。 */
    bool scroll_h;
    bool scroll_v;
    gui_text_mode_t text_mode;
    gui_text_align_t text_align;

    /* 外观缓存（Task 5）：形态 + 色彩角色 + 令牌层级 + 三个开关。 */
    gui_appearance_t appearance;

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
    uint32_t delete_depth;  /* 删除事务深度：>0 时槽不许回到空闲表 */
} gui_widget_registry_t;

/* ── 注册表生命周期（platform.c ↔ widget.c） ─────────────────────── */

/**
 * @brief 事件桥接：给一个新控件装上原生点击的桥接
 *
 * 每个新对象**发布前**装一次。桥接的槽下标放在事件回调自己的 user data
 * 里：后端对象的 user data 不被占用。
 *
 * @return 0 成功；-1 失败（errno = ENOMEM）
 */
int gui_widget_bridge_install(lv_obj_t *obj, uint32_t slot_index);

/**
 * @brief 外观：按控件种类给出创建时的默认外观
 *
 * 根容器是透明的全屏底板，标签是素面，按钮取主题里按钮的现有默认。
 *
 * @param kind   控件种类
 * @param is_root 该控件是不是平台根容器
 * @param out    输出参数：默认外观，不可为 NULL
 */
void gui_widget_appearance_init(gui_widget_kind_t kind, bool is_root, gui_appearance_t *out);

/**
 * @brief 外观：把一份外观写进后端对象（普通状态）
 *
 * 只写普通状态：主题的 hover / pressed / focused / disabled 样式因此
 * 原样生效，焦点环不会被普通局部边框遮掉。
 */
void gui_widget_appearance_apply(lv_obj_t *obj, const gui_appearance_t *appearance);

/**
 * @brief 按身份号查找**已发布**的槽
 *
 * 布局、外观与事件这三个模块都要按身份找到槽，但注册表的查找逻辑只有
 * 一份：它住在 widget.c 里，这里只把入口暴露给同库的其它编译单元。
 *
 * 查找只比较身份号，从不解引用槽里的后端指针。
 *
 * @param platform 平台上下文句柄
 * @param id       控件身份号（0 = 无身份）
 * @return 槽指针；身份不存在或未发布时返回 NULL
 */
gui_widget_slot_t *gui_widget_slot_find(gui_platform_t *platform, uint64_t id);

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
 * 初始化"这条规则无法被验证。复制出来的槽带有它的**槽下标**（`index`），
 * 宿主测试据此断言"同一个槽真的被复用了"。
 *
 * @return 0 成功（身份不存在时 `*out` 是全零的槽）；-1 失败
 */
int gui_widget_test_query_slot(gui_platform_t *platform, gui_widget_t widget, gui_widget_slot_t *out);

/**
 * @brief 宿主私有观察桥：把身份解析成后端对象
 *
 * 它让宿主测试能观察后端状态（命中标志、对象所属的 display、对象所属的
 * 组）。它只存在于宿主测试构建：目标构建不编译它，因此它既不进公共头，
 * 也不进目标 `libgui.a`。
 *
 * @return 后端对象；NULL 表示身份无效（errno = EINVAL）
 */
lv_obj_t *gui_widget_test_get_obj(gui_platform_t *platform, gui_widget_t widget);

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

/**
 * @brief 宿主私有：造一个"别的平台 / 别的 display"的控件身份
 *
 * 生产平台是单例（第二个平台建不出来）。归属契约因此用宿主私有面构造：
 *
 *   mode 0  身份属于**第二个注册表**（外平台）：公共焦点 API 必须在解析层
 *           拒绝它。
 *   mode 1  身份在本平台的注册表里，但对象在**第二个 display** 上（外
 *           display）：公共焦点 API 必须在归属层拒绝它。
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_widget_test_create_foreign(gui_platform_t *platform, int mode, gui_widget_t *out);

/**
 * @brief 宿主私有：释放上一个"外来身份"占用的资源
 */
void gui_widget_test_free_foreign(gui_platform_t *platform);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _GUI_WIDGET_PRIVATE_H */
