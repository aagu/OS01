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
 * @brief 复位一个槽的全部字段
 *
 * 槽复用时必须走这里：旧身份、旧 handler、旧 userdata、旧文字、旧几何
 * 缓存与旧标志一个都不许残留。
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
    slot->flags = 0;
    slot->pending_free = false;
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

    gui_widget_registry_t *reg = (gui_widget_registry_t *)calloc(1, sizeof(gui_widget_registry_t));
    if (!reg) {
        errno = ENOMEM;
        return -1;
    }
    for (uint32_t i = 0; i < GUI_WIDGET_REGISTRY_CAPACITY; i++) {
        slot_reset(&reg->slots[i]);
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

/* ── 公共查询 ─────────────────────────────────────────────────────── */

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

#ifdef OS01_HOST_TEST

void gui_widget_test_set_last_id(uint64_t last_id)
{
    g_widget_last_id = last_id;
}

uint64_t gui_widget_test_get_last_id(void)
{
    return g_widget_last_id;
}

#endif /* OS01_HOST_TEST */
