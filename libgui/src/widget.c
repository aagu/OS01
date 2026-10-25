#include <gui/gui_event.h>
#include <gui/gui_widget.h>
#include <gui_lvgl_private.h>
#include <gui_platform_private.h>
#include <gui_widget_private.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * widget.c — 原生控件注册表与身份（GUI-2b）
 *
 * 本文件是 libgui 里**唯一**知道"控件身份"的地方。公共面只有一个值句柄
 * （uint64_t 身份号）；后端对象指针永远不离开本文件与私有头。
 */

/* ── 进程全局身份号 ────────────────────────────────────────────────
 *
 * 身份号在**进程内**全局递增：平台销毁/重建都不重置，也绝不复用或
 * 回绕。0 保留给 GUI_WIDGET_NONE，因此发出的第一个身份号是 1。
 *
 * 计数器保存的是**最后发出的**身份号：0 表示一个都还没发出。
 */
static uint64_t g_widget_last_id = 0;

/* ── 内部辅助 ─────────────────────────────────────────────────────── */

/* 删除事务：把一棵子树标成 DESTROYING、删原生对象、回收槽位。 */
static void widget_destroy_subtree(gui_platform_t *platform, gui_widget_slot_t *root_slot);

/**
 * @brief 按身份号查找槽
 *
 * 只比较身份号，从不解引用槽里的后端指针：失效对象的指针只是被比较，
 * 不会被读。
 */
static gui_widget_slot_t *registry_find(gui_widget_registry_t *reg, uint64_t id)
{
    if (!reg || id == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *slot = &reg->slots[i];
        if (slot->state != GUI_WIDGET_SLOT_FREE && slot->id == id) {
            return slot;
        }
    }
    return NULL;
}

/**
 * @brief 复位一个槽的全部**残留元数据**
 *
 * 槽复用时必须走这里：旧身份、旧 handler、旧 userdata、旧文字、旧几何
 * 缓存与旧标志一个都不许残留。
 *
 * 清零的字段清单（32 个）：state、id、obj、parent_id、kind、handler、
 * userdata、text、geom_x、geom_y、geom_width、geom_height、
 * size_mode_w、size_value_w、size_mode_h、size_value_h、anchor、align_x、
 * align_y、absolute_pos、flow、main_align、cross_align、grow、gap、
 * pad_top、pad_bottom、pad_left、pad_right、scroll_h、scroll_v、text_mode、
 * text_align、flags、pending_free。
 *
 * 唯一**不清**的是 `index`：它是槽在表中的位置，不是上一任留下的元数据，
 * 只在注册表建立时写一次；清掉它等于毁掉槽自己的身份。
 */
static void slot_reset(gui_widget_slot_t *slot)
{
    slot->state = GUI_WIDGET_SLOT_FREE;
    slot->id = 0;
    slot->obj = NULL;
    slot->parent_id = 0;
    slot->kind = GUI_WIDGET_CONTAINER;
    slot->handler = NULL;
    slot->userdata = NULL;
    slot->text = NULL;
    slot->geom_x = 0;
    slot->geom_y = 0;
    slot->geom_width = 0;
    slot->geom_height = 0;
    slot->size_mode_w = GUI_SIZE_MODE_CONTENT;
    slot->size_mode_h = GUI_SIZE_MODE_CONTENT;
    slot->size_value_w = 0;
    slot->size_value_h = 0;
    slot->anchor = GUI_ALIGN_TOP_LEFT;
    slot->align_x = 0;
    slot->align_y = 0;
    slot->absolute_pos = false;
    slot->flow = GUI_FLOW_NONE;
    slot->main_align = GUI_FLOW_ALIGN_START;
    slot->cross_align = GUI_FLOW_ALIGN_START;
    slot->grow = 0;
    slot->gap = 0;
    slot->pad_top = 0;
    slot->pad_bottom = 0;
    slot->pad_left = 0;
    slot->pad_right = 0;
    slot->scroll_h = false;
    slot->scroll_v = false;
    slot->text_mode = GUI_TEXT_MODE_WRAP;
    slot->text_align = GUI_TEXT_ALIGN_LEFT;
    slot->flags = 0;
    slot->pending_free = false;
}

/**
 * @brief 把一个槽放回空闲表
 *
 * 只能在这个槽的删除回调**返回之后**调用：回调期间槽必须留在表里，
 * 否则槽会在删除事件还没结束时被别的控件占用。
 */
static void slot_release(gui_widget_registry_t *reg, gui_widget_slot_t *slot)
{
    if (slot->state == GUI_WIDGET_SLOT_FREE) {
        return;
    }
    free(slot->text);
    slot_reset(slot);
    /* 释放的槽立刻成为下一次分配的候选：槽复用是确定的行为，残留的
     * 元数据必须在下一次占用时被清干净。 */
    reg->cursor = (uint32_t)(slot - reg->slots);
    if (reg->count > 0) {
        reg->count--;
    }
}

/**
 * @brief 回收所有处于 DESTROYING 的槽
 *
 * 只在删除事务结束（原生删除回调全部返回）之后调用。
 */
static void registry_reap(gui_widget_registry_t *reg)
{
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *slot = &reg->slots[i];
        if (slot->state == GUI_WIDGET_SLOT_DESTROYING && slot->pending_free) {
            slot_release(reg, slot);
        }
    }
}

/**
 * @brief 占用一个空槽
 *
 * 私有游标只是分配提示，**不计入容量**：容量恒为 1024。
 *
 * @return 槽下标；-1 失败（errno = ENOSPC）
 */
static int registry_reserve(gui_widget_registry_t *reg)
{
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        uint32_t index = (reg->cursor + i) % GUI_WIDGET_REGISTRY_CAPACITY;
        gui_widget_slot_t *slot = &reg->slots[index];
        if (slot->state == GUI_WIDGET_SLOT_FREE) {
            slot_reset(slot);
            slot->state = GUI_WIDGET_SLOT_RESERVED;
            reg->cursor = (index + 1) % GUI_WIDGET_REGISTRY_CAPACITY;
            reg->count++;
            if (reg->count > reg->peak) {
                reg->peak = reg->count;
            }
            return (int)index;
        }
    }
    errno = ENOSPC;
    return -1;
}

/* ── 注册表生命周期 ───────────────────────────────────────────────── */

int gui_widget_registry_init(gui_platform_t *platform)
{
    if (!platform) {
        errno = EINVAL;
        return -1;
    }
    if (platform->registry) {
        return 0;
    }

#ifdef OS01_HOST_TEST
    /* 宿主私有故障桥：注册表表的分配失败。 */
    extern bool g_gui_test_fail_widget_registry;
    if (g_gui_test_fail_widget_registry) {
        errno = ENOMEM;
        return -1;
    }
#endif

    gui_widget_registry_t *reg = (gui_widget_registry_t *)calloc(1, sizeof(gui_widget_registry_t));
    if (!reg) {
        errno = ENOMEM;
        return -1;
    }
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        slot_reset(&reg->slots[i]);
        /* 槽的位置只写一次：它是槽自己的身份，不是可被复用的残留元数据。 */
        reg->slots[i].index = i;
    }
    platform->registry = reg;
    return 0;
}

void gui_widget_registry_fini(gui_platform_t *platform)
{
    if (!platform || !platform->registry) {
        return;
    }
    free(platform->registry);
    platform->registry = NULL;
}

void gui_widget_registry_destroy_tree(gui_platform_t *platform)
{
    if (!platform || !platform->registry) {
        return;
    }
    gui_widget_registry_t *reg = platform->registry;

    /* 树根先删：根的删除会带走它的整棵子树。 */
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *slot = &reg->slots[i];
        if (slot->state != GUI_WIDGET_SLOT_LIVE || slot->parent_id != 0) {
            continue;
        }
        widget_destroy_subtree(platform, slot);
    }
    /* 剩下的活控件（父已经不在的孤儿）也要删掉。 */
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *slot = &reg->slots[i];
        if (slot->state != GUI_WIDGET_SLOT_LIVE) {
            continue;
        }
        widget_destroy_subtree(platform, slot);
    }
}

/* ── 公共查询 ─────────────────────────────────────────────────────── */

gui_widget_slot_t *gui_widget_slot_find(gui_platform_t *platform, uint64_t id)
{
    if (!platform || !platform->registry) {
        return NULL;
    }
    return registry_find(platform->registry, id);
}

bool gui_widget_is_valid(gui_platform_t *platform, gui_widget_t widget)
{
    if (!platform || !platform->registry) {
        return false;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    /* 只有已发布的槽才算有效：RESERVED（还在建）与 DESTROYING（正在销毁）
     * 的身份对公共查询一律不可见。 */
    return slot != NULL && slot->state == GUI_WIDGET_SLOT_LIVE;
}

int gui_widget_get_kind(gui_platform_t *platform, gui_widget_t widget, gui_widget_kind_t *out)
{
    /* 先校验输出指针，再清输出：失败的调用永远不留下上一次的结果。 */
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = GUI_WIDGET_KIND_COUNT;

    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    *out = slot->kind;
    return 0;
}

/* ── 生命周期 ─────────────────────────────────────────────────────── */

/**
 * @brief 原生删除观察者
 *
 * 原生对象被删除时，这个回调是"身份失效"的第一现场：
 *   - 回调**期间**该身份立刻不可用（槽进入 DESTROYING）：公共查询再也
 *     看不到它；
 *   - 槽**不**在回调里回到空闲表：观察者还要用它，等回调返回之后由删除
 *     事务统一回收。删除事件没结束就复用槽位，会让旧身份指向新对象。
 */
static void gui_widget_on_native_delete(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);
    /* 用户数据存的是"槽下标 + 1"：下标 0 也必须与"没有用户数据"区分开。 */
    intptr_t index = (intptr_t)lv_event_get_user_data(event) - 1;

    if (!obj || index < 0 || index >= (intptr_t)GUI_WIDGET_REGISTRY_CAPACITY) {
        return;
    }

    /* 平台从它自己的 display 上取回来：原生创建从不使用默认 display。 */
    lv_display_t *disp = lv_obj_get_display(obj);
    gui_platform_t *platform =
        disp ? (gui_platform_t *)lv_display_get_user_data(disp) : NULL;
    if (!platform || !platform->registry) {
        return;
    }

    gui_widget_slot_t *slot = &platform->registry->slots[index];
    if (slot->state == GUI_WIDGET_SLOT_FREE) {
        return;
    }
    slot->state = GUI_WIDGET_SLOT_DESTROYING;
    slot->pending_free = true;
}

/**
 * @brief 按种类创建原生对象
 *
 * 父对象是本平台自己的屏幕或容器对象；**不**使用默认 display，也**不**
 * 加入任何全局组。
 */
static lv_obj_t *widget_create_native(gui_widget_kind_t kind, lv_obj_t *parent)
{
    lv_obj_t *obj = NULL;

    switch (kind) {
    case GUI_WIDGET_CONTAINER:
        obj = lv_obj_create(parent);
        break;
    case GUI_WIDGET_LABEL:
        obj = lv_label_create(parent);
        break;
    case GUI_WIDGET_BUTTON:
        obj = lv_button_create(parent);
        break;
    default:
        break;
    }
    if (!obj) {
        return NULL;
    }

    switch (kind) {
    case GUI_WIDGET_CONTAINER:
        /* 装饰性容器默认不可点击。 */
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        break;
    case GUI_WIDGET_LABEL:
        /* 标签默认不抢命中。 */
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        break;
    case GUI_WIDGET_BUTTON:
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        break;
    default:
        break;
    }
    return obj;
}

/**
 * @brief 把一棵子树整棵标成 DESTROYING
 *
 * 递归标记：整棵待删子树在原生删除**之前**就全部不可用，公共查询立刻
 * 看到它们已经没了（不只是根和直接子项）。
 */
static void widget_mark_subtree(gui_widget_registry_t *reg, uint32_t index)
{
    gui_widget_slot_t *slot = &reg->slots[index];
    if (slot->state != GUI_WIDGET_SLOT_LIVE) {
        return;
    }
    slot->state = GUI_WIDGET_SLOT_DESTROYING;

    uint64_t id = slot->id;
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *child = &reg->slots[i];
        if (child->state == GUI_WIDGET_SLOT_LIVE && child->parent_id == id) {
            widget_mark_subtree(reg, i);
        }
    }
}

/**
 * @brief 销毁一棵子树
 *
 * 顺序：先把整棵待删子树标成 DESTROYING（公共查询立刻失效），再删原生
 * 对象（删除回调在此期间运行），最后在回调全部返回之后回收槽位。
 */
static void widget_destroy_subtree(gui_platform_t *platform, gui_widget_slot_t *root_slot)
{
    gui_widget_registry_t *reg = platform->registry;

    widget_mark_subtree(reg, (uint32_t)(root_slot - reg->slots));

    lv_obj_t *obj = root_slot->obj;
    if (obj) {
        lv_obj_delete(obj);
    } else {
        /* 对象早已不在：删除回调不会再跑，这些槽直接确认死亡。 */
        for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
            gui_widget_slot_t *slot = &reg->slots[i];
            if (slot->state == GUI_WIDGET_SLOT_DESTROYING) {
                slot->pending_free = true;
            }
        }
    }

    /* 回调返回之后，槽才回到空闲表。 */
    registry_reap(reg);
}

int gui_widget_create(gui_platform_t *platform, gui_widget_t parent, gui_widget_kind_t kind,
                      gui_widget_t *out)
{
    /* 1. 参数校验：任何资源都在校验通过之后才被消耗，失败不留半对象。 */
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    out->id = 0;

    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    if ((unsigned int)kind >= (unsigned int)GUI_WIDGET_KIND_COUNT) {
        errno = EINVAL;
        return -1;
    }

    gui_widget_registry_t *reg = platform->registry;

    /* 业务回调里不允许改树：回调期间树结构必须保持稳定。 */
    if (reg->callback_depth > 0) {
        errno = EBUSY;
        return -1;
    }

    /* 后端在注册表背后删掉对象时（内部删除 fixture），那些槽停在
     * DESTROYING：它们的删除回调早就返回了，此刻回收是安全的。 */
    if (reg->callback_depth == 0) {
        registry_reap(reg);
    }

    /* 2. 父控件校验：必须是本平台上活着的控件，且种类可以拥有子项。
     *    CONTAINER 与 BUTTON 都可以拥有子项（Task 10 的卡片容器换成
     *    BUTTON，按钮的标题必须是 LABEL 子项）；LABEL 是叶子，不能作父。 */
    gui_widget_slot_t *parent_slot = NULL;
    if (parent.id != 0) {
        parent_slot = registry_find(reg, parent.id);
        if (!parent_slot || parent_slot->state != GUI_WIDGET_SLOT_LIVE) {
            errno = EINVAL;
            return -1;
        }
        if (parent_slot->kind != GUI_WIDGET_CONTAINER &&
            parent_slot->kind != GUI_WIDGET_BUTTON) {
            errno = EINVAL;
            return -1;
        }
    } else if (kind != GUI_WIDGET_CONTAINER) {
        errno = EINVAL;
        return -1;
    }

    /* 3. 先占槽：容量在造任何原生对象之前就被检查。 */
    int index = registry_reserve(reg);
    if (index < 0) {
        return -1; /* ENOSPC */
    }
    gui_widget_slot_t *slot = &reg->slots[index];

    /* 4. 取身份：进程全局递增，不复用、不回绕。失败可以消耗一个身份号，
     *    但它绝不会再被发出去。 */
    if (g_widget_last_id == UINT64_MAX) {
        slot_release(reg, slot);
        errno = EOVERFLOW;
        return -1;
    }
    uint64_t id = g_widget_last_id + 1;
    g_widget_last_id = id;

    /* 5. 造原生对象。根挂在本平台自己的活动屏幕上。 */
    lv_obj_t *parent_obj = NULL;
    if (parent_slot) {
        parent_obj = parent_slot->obj;
    } else {
        parent_obj = lv_display_get_screen_active(platform->disp);
    }
    if (!parent_obj) {
        slot_release(reg, slot);
        errno = ENOMEM;
        return -1;
    }

    lv_obj_t *obj = widget_create_native(kind, parent_obj);
    if (!obj) {
        slot_release(reg, slot);
        errno = ENOMEM;
        return -1;
    }

    if (!parent_slot) {
        /* 根铺满平台屏幕：它的尺寸来源是**固定像素**的视口，不是内容，
         * 因此它不会和任何 FILL 子项构成尺寸循环。 */
        lv_obj_set_size(obj, (int32_t)platform->width, (int32_t)platform->height);
        lv_obj_align(obj, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    /* 6. 挂注册表观察者：原生删除事件是身份失效的来源。 */
#ifdef OS01_HOST_TEST
    /* 宿主私有故障桥：观察者安装失败。 */
    extern bool g_gui_test_fail_widget_observer;
    if (g_gui_test_fail_widget_observer) {
        lv_obj_delete(obj);
        slot_release(reg, slot);
        errno = ENOMEM;
        return -1;
    }
#endif
    lv_event_dsc_t *dsc = lv_obj_add_event_cb(obj, gui_widget_on_native_delete, LV_EVENT_DELETE,
                                              (void *)(intptr_t)(index + 1));
    if (!dsc) {
        lv_obj_delete(obj);
        slot_release(reg, slot);
        errno = ENOMEM;
        return -1;
    }

    /* 7. 发布：身份此刻才对公共查询可见。 */
    slot->id = id;
    slot->obj = obj;
    slot->parent_id = parent_slot ? parent_slot->id : 0;
    slot->kind = kind;
    slot->flags = GUI_WIDGET_FLAG_VISIBLE | GUI_WIDGET_FLAG_ENABLED;
    if (kind == GUI_WIDGET_BUTTON) {
        slot->flags |= GUI_WIDGET_FLAG_CLICKABLE;
    }
    slot->state = GUI_WIDGET_SLOT_LIVE;

    /* 尺寸来源：根是视口（固定像素），子项默认由内容决定。 */
    if (!parent_slot) {
        slot->size_mode_w = GUI_SIZE_MODE_PX;
        slot->size_mode_h = GUI_SIZE_MODE_PX;
        slot->size_value_w = (int32_t)platform->width;
        slot->size_value_h = (int32_t)platform->height;
    } else {
        slot->size_mode_w = GUI_SIZE_MODE_CONTENT;
        slot->size_mode_h = GUI_SIZE_MODE_CONTENT;
        slot->size_value_w = 0;
        slot->size_value_h = 0;
    }

    out->id = id;
    return 0;
}

int gui_widget_destroy(gui_platform_t *platform, gui_widget_t widget)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_registry_t *reg = platform->registry;

    /* 业务回调里不允许改树：回调期间树结构必须保持稳定。 */
    if (reg->callback_depth > 0) {
        errno = EBUSY;
        return -1;
    }

    gui_widget_slot_t *slot = registry_find(reg, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }

    widget_destroy_subtree(platform, slot);
    return 0;
}

int gui_widget_clear(gui_platform_t *platform, gui_widget_t container)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_registry_t *reg = platform->registry;

    if (reg->callback_depth > 0) {
        errno = EBUSY;
        return -1;
    }

    gui_widget_slot_t *slot = registry_find(reg, container.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    if (slot->kind != GUI_WIDGET_CONTAINER) {
        errno = EOPNOTSUPP;
        return -1;
    }

    /* 容器留下，它的直接子项连同子树一起消失。 */
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        gui_widget_slot_t *child = &reg->slots[i];
        if (child->state != GUI_WIDGET_SLOT_LIVE) {
            continue;
        }
        if (child->parent_id != container.id) {
            continue;
        }
        widget_destroy_subtree(platform, child);
    }
    return 0;
}

/* ── 外观与状态 ───────────────────────────────────────────────────── */

int gui_widget_set_text(gui_platform_t *platform, gui_widget_t label, const char *text)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, label.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }
    if (slot->kind != GUI_WIDGET_LABEL) {
        errno = EOPNOTSUPP;
        return -1;
    }

    /* 文字是**副本**：调用方的缓冲可以在返回后立刻改掉或释放。 */
    char *copy = NULL;
    if (text) {
        size_t len = strlen(text);
        copy = (char *)malloc(len + 1);
        if (!copy) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(copy, text, len + 1);
    }

    free(slot->text);
    slot->text = copy;
    lv_label_set_text(slot->obj, copy ? copy : "");
    return 0;
}

int gui_widget_set_visible(gui_platform_t *platform, gui_widget_t widget, bool visible)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }

    if (visible) {
        lv_obj_remove_flag(slot->obj, LV_OBJ_FLAG_HIDDEN);
        slot->flags |= GUI_WIDGET_FLAG_VISIBLE;
    } else {
        lv_obj_add_flag(slot->obj, LV_OBJ_FLAG_HIDDEN);
        slot->flags &= ~GUI_WIDGET_FLAG_VISIBLE;
    }
    return 0;
}

int gui_widget_set_enabled(gui_platform_t *platform, gui_widget_t widget, bool enabled)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }

    if (enabled) {
        lv_obj_remove_state(slot->obj, LV_STATE_DISABLED);
        slot->flags |= GUI_WIDGET_FLAG_ENABLED;
    } else {
        lv_obj_add_state(slot->obj, LV_STATE_DISABLED);
        slot->flags &= ~GUI_WIDGET_FLAG_ENABLED;
    }
    return 0;
}

int gui_widget_get_bounds(gui_platform_t *platform, gui_widget_t widget, gui_rect_t *out)
{
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }

    /* 先让当前布局完成，返回的才是**布局之后**的矩形。 */
    lv_obj_update_layout(slot->obj);
    lv_area_t coords;
    lv_obj_get_coords(slot->obj, &coords);

    out->x = coords.x1;
    out->y = coords.y1;
    out->width = coords.x2 - coords.x1 + 1;
    out->height = coords.y2 - coords.y1 + 1;

    slot->geom_x = out->x;
    slot->geom_y = out->y;
    slot->geom_width = out->width;
    slot->geom_height = out->height;
    return 0;
}

#ifdef OS01_HOST_TEST

void gui_widget_test_set_last_id(uint64_t last_id)
{
    g_widget_last_id = last_id;
}

uint64_t gui_widget_test_get_last_id(void)
{
    return g_widget_last_id;
}

int gui_widget_test_query_slot(gui_platform_t *platform, gui_widget_t widget, gui_widget_slot_t *out)
{
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot) {
        return 0;
    }
    *out = *slot;
    /* 后端指针不外泄：查询结果里没有它。 */
    out->obj = NULL;
    return 0;
}

/* 一个假的业务回调：只用来证明"陈旧 handler 被清掉"，从不被调用。 */
static void gui_widget_test_stale_handler(const gui_event_t *event, void *userdata)
{
    (void)event;
    (void)userdata;
}

int gui_widget_test_plant_slot(gui_platform_t *platform, gui_widget_t widget, uint32_t stale_flags,
                               const char *stale_text)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return -1;
    }

    free(slot->text);
    slot->text = NULL;
    if (stale_text) {
        size_t len = strlen(stale_text);
        slot->text = (char *)malloc(len + 1);
        if (!slot->text) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(slot->text, stale_text, len + 1);
    }
    slot->handler = gui_widget_test_stale_handler;
    slot->userdata = (void *)&slot->handler;
    slot->flags = stale_flags;
    slot->geom_x = 11;
    slot->geom_y = 22;
    slot->geom_width = 33;
    slot->geom_height = 44;
    return 0;
}

int gui_widget_test_get_counters(gui_platform_t *platform, uint32_t *out_count, uint32_t *out_peak)
{
    if (!out_count || !out_peak) {
        errno = EINVAL;
        return -1;
    }
    *out_count = 0;
    *out_peak = 0;
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    *out_count = platform->registry->count;
    *out_peak = platform->registry->peak;
    return 0;
}

int gui_widget_test_backend_delete(gui_platform_t *platform, gui_widget_t widget)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return -1;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE || !slot->obj) {
        errno = EINVAL;
        return -1;
    }
    /* 后端在注册表背后自行删除：删除回调照常运行，身份立刻失效。 */
    lv_obj_delete(slot->obj);
    return 0;
}

lv_obj_t *gui_widget_test_get_obj(gui_platform_t *platform, gui_widget_t widget)
{
    if (!platform || !platform->registry) {
        errno = EINVAL;
        return NULL;
    }
    gui_widget_slot_t *slot = registry_find(platform->registry, widget.id);
    if (!slot || slot->state != GUI_WIDGET_SLOT_LIVE) {
        errno = EINVAL;
        return NULL;
    }
    return slot->obj;
}

#endif /* OS01_HOST_TEST */
