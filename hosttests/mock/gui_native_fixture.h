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
 * @brief 宿主私有：设置/清除一个控件的**焦点视觉状态**
 *
 * 它只在 fixture 内解析真实对象并改后端的焦点状态位：**不**创建输入
 * 登记、**不**调用公共焦点 API（那一层要到 Phase 3 的唯一焦点引擎才有）。
 *
 * 这个 helper 只存在于宿主测试构建：它不安装，也不编译进目标库。
 *
 * @param fx      fixture 上下文
 * @param widget  控件身份
 * @param focused true 置上焦点视觉状态；false 清除
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_set_focus_visual(gui_native_fixture_t *fx, gui_widget_t widget, bool focused);

/**
 * @brief 宿主私有：读取焦点组的**真实接收者**
 *
 * 公开的 `gui_focus_get()` 说的是"记录里的选择"；这条查询说的是"后端组
 * 此刻真正把焦点给了谁"。两者必须一致——只看表成员是看不出分歧的。
 *
 * @param fx  fixture 上下文
 * @param out 输出参数：接收者的控件身份（没有接收者时为 NONE）
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_focus_receiver(gui_native_fixture_t *fx, gui_widget_t *out);

/**
 * @brief 宿主私有：焦点组当前是否有**真实接收者**
 *
 * @return 1 有接收者；0 没有；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_has_focus_receiver(gui_native_fixture_t *fx);

/**
 * @brief 宿主私有：第 index 个诊断(raw)对象是否正持有焦点
 *
 * @return 1 是；0 不是；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_raw_is_focused(gui_native_fixture_t *fx, unsigned index);

/**
 * @brief 宿主私有：造一个"别的平台 / 别的 display"的控件身份
 *
 * @param platform     平台上下文句柄
 * @param own_registry true = 身份在本平台的注册表里但对象在别的 display 上
 *                     （外 display）；false = 身份属于第二个注册表（外平台）
 * @param out          输出参数：造出来的身份
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_create_foreign_widget(gui_platform_t *platform, bool own_registry,
                                            gui_widget_t *out);

/**
 * @brief 宿主私有：释放上一个"外来身份"占用的资源
 */
void gui_native_fixture_free_foreign_widget(gui_platform_t *platform);

/**
 * @brief 宿主私有：读取某个原生控件的指针策略标志
 *
 * @return 0 成功（`*out_click_focusable` 给出对象是否带
 *         LV_OBJ_FLAG_CLICK_FOCUSABLE）；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_click_focusable(gui_platform_t *platform, gui_widget_t widget,
                                             bool *out_click_focusable);

/**
 * @brief 宿主私有：读回焦点登记表的**顺序**（登记顺序 = Tab 顺序）
 *
 * @param fx           fixture 上下文
 * @param out          输出数组（每个元素是一个原生身份；诊断登记的身份为 0）
 * @param out_capacity 数组容量
 * @return 登记条目数；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_focus_order(gui_native_fixture_t *fx, gui_widget_t *out,
                                         int out_capacity);

/**
 * @brief 宿主私有：读回焦点引擎的选择历史标志
 *
 * @param fx               fixture 上下文
 * @param out_ever_selected 输出：是否发生过一次成功的选择
 * @param out_explicit_clear 输出：当前是否处于"显式清空"状态
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_focus_history(gui_native_fixture_t *fx, bool *out_ever_selected,
                                           bool *out_explicit_clear);

/**
 * @brief 宿主私有：焦点组被重建过多少次
 *
 * 重建 = remove_all + 重新加入全部成员。稳定态（显式清空）必须**不再**
 * 重建组，否则每步刷新都是 O(n) 抖动。
 *
 * @param fx  fixture 上下文
 * @param out 输出参数：自上次复位以来的重建次数
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_focus_rebuild_count(gui_native_fixture_t *fx, uint32_t *out);

/**
 * @brief 宿主私有：把控件置为键盘焦点状态（FOCUS_KEY）
 *
 * 与 gui_native_fixture_set_focus_visual() 对称，但走的是**键盘**那条状态
 * 位：键盘导航的焦点反馈（FOCUS_KEY）必须与指针那条（FOCUSED）一样被
 * 单独覆盖——主题与外观层对两者的处理必须对称。
 *
 * @param fx      fixture 上下文
 * @param widget  控件身份
 * @param focused true = 置上 FOCUS_KEY；false = 撤掉
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_set_focus_key_visual(gui_native_fixture_t *fx, gui_widget_t widget,
                                            bool focused);

/**
 * @brief 宿主私有：修复待办的状态
 *
 * 观察"修复位置 + 路径属性"。路径属性（受控=有界 / 后端=绕回）在待修复
 * 期间必须**保持粘性**：候选恢复后按同一条规则继续修复。
 *
 * @param fx           fixture 上下文
 * @param out_pending  输出参数：是否处于"待修复"状态（可为空）
 * @param out_bounded  输出参数：路径属性是否为"有界"（受控路径）（可为空）
 * @param out_index    输出参数：修复起点（表内下标）（可为空）
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_focus_repair_state(gui_native_fixture_t *fx, bool *out_pending,
                                                bool *out_bounded, unsigned *out_index);

/**
 * @brief 宿主私有：控件当前状态解析出的圆角
 *
 * 用于证明"主题焦点环不得改写外观层显式表达的圆角"：`rounded=false` 的
 * 控件聚焦后仍然是零圆角。
 *
 * @param fx      fixture 上下文
 * @param widget  控件身份
 * @param out     输出参数：解析出的 radius（像素）
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_query_focus_radius(gui_native_fixture_t *fx, gui_widget_t widget,
                                          int32_t *out);

/* ── 诊断(raw)登记的宿主私有入口 ─────────────────────────────────
 *
 * 诊断(raw)登记**没有**公共 API，也不得变成公共 adopt 入口：它只是内部
 * hook。宿主测试通过下面这些 helper 触达它。
 */

/**
 * @brief 宿主私有：诊断(raw)登记一个原生 LVGL 按钮
 *
 * @return 0 成功；-1 失败（errno 已设置；errno = EBUSY 表示登记模式冲突）
 */
int gui_native_fixture_raw_focus_add_button(gui_native_fixture_t *fx);

/**
 * @brief 宿主私有：诊断(raw)登记一个原生 LVGL 单行文本框
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_raw_focus_add_textarea(gui_native_fixture_t *fx);

/**
 * @brief 宿主私有：读回第 index 个诊断(raw)对象的文字
 *
 * @return 文字指针（无效下标时返回 NULL）
 */
const char *gui_native_fixture_raw_textarea_text(gui_native_fixture_t *fx, unsigned index);

/**
 * @brief 宿主私有：在第 index 个诊断(raw)对象上**点一下**
 *
 * @return 0 成功；-1 失败（errno 已设置）
 */
int gui_native_fixture_raw_click(gui_native_fixture_t *fx, unsigned index);

/**
 * @brief 宿主私有：删掉当前**持有焦点**的诊断(raw)对象
 *
 * 它模拟"后端在回调里自行删除当前焦点对象"。
 *
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_raw_delete_focused(gui_native_fixture_t *fx);

/**
 * @brief 宿主私有：读取渲染内存池的真实字节数
 *
 * 公共统计只报告"已用"，不报告"空闲"；这条观察桥把池的**空闲**字节也
 * 露出来，测试才能独立复核 `pool_used == pool_total - pool_free` 这条
 * 等式（而不是让实现自己证明自己）。
 *
 * @param fx         fixture 上下文
 * @param out_total  输出参数：池总字节（可为 NULL）
 * @param out_free   输出参数：池空闲字节（可为 NULL）
 * @param out_max    输出参数：池历史最高占用字节（可为 NULL）
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_pool_bytes(gui_native_fixture_t *fx, uint64_t *out_total,
                                  uint64_t *out_free, uint64_t *out_max_used);

/**
 * @brief 宿主私有：在**删除回调期间**创建一个控件
 *
 * 删除事务进行时，槽不许回到空闲表：删除回调期间创建的控件必须拿到
 * **别的**槽。这个探针把"在删除回调期间创建控件"变成可观测的事实。
 *
 * 它在 `victim` 自己身上装一个删除期回调：原生删除事件送达 victim 时
 * 回调跑起来（那仍在删除事务内部），在那里通过公共 API 创建一个控件。
 *
 * @param platform 平台上下文句柄
 * @param victim   将要被删除的控件身份
 * @param parent   victim 的父控件身份
 * @param out      输出参数：删除期间创建出来的控件身份（回调发生时写入）
 * @return 0 成功；-1 失败（errno = EINVAL）
 */
int gui_native_fixture_create_during_delete(gui_platform_t *platform, gui_widget_t victim,
                                            gui_widget_t parent, gui_widget_t *out);

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
