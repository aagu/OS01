/*
 * input_focus.c — 唯一焦点引擎（GUI-2b Task 7/8/9）
 *
 * 焦点引擎只有一个：原生登记（公共 API）与诊断(raw)登记（内部 hook）共用
 * **同一张登记表、同一套选择提交路径**。两套入口的差别只有三处：
 *
 *   - 原生条目带一个原生身份（诊断登记为 0），并受焦点策略约束；
 *   - 原生登记会同步指针策略（CLICK_FOCUSABLE），诊断登记保留既有 flag；
 *   - 登记模式一旦被第一次**成功**的登记锁定，两种登记不许混用。
 *
 * 引擎的几条硬规矩：
 *
 *   1. **唯一选择状态。** 当前选择、ever_selected、explicit_clear 与修复
 *      起点都记在 gui_input_focus_t 里。对象只在仍存活时解引用；删除事件
 *      里的目标在该事件结束前仍然有效。
 *   2. **一次提交选择。** 任何选择变化都经 focus_commit()：比较旧/新身份、
 *      更新选择与历史状态、投影到后端，并在焦点真的变化时显式调用取消
 *      协议（不依赖后端是否发焦点回调）。
 *   3. **投影回调保护。** 内部重建用投影深度屏蔽组回调的中间选择：先冻结、
 *      remove_all、add 建立真实 NONE，再应用最终目标。
 *   4. **稳定维护。** 提前返回要同时满足：成员集合正确、选择有效或有意
 *      NONE、真实接收者与选择一致、没有待修复标志。
 *   5. **删除事务。** 公共 destroy / clear 先把整棵待删子树标成 DESTROYING，
 *      开始批次（组整体 detach 到真实 NONE），在各删除回调里按捕获对象
 *      比较摘条目，批次结束后选一次跳过整棵子树的后继。
 *   6. **观察者所有权。** 显式注销摘掉删除观察者；input 析构卸载所有仍在册
 *      条目的观察者；已注销的不得残留。
 *   7. **指针选择。** 原生登记/更新策略即同步 CLICK_FOCUSABLE；诊断登记
 *      保留既有 flag。
 *   8. **视觉/输入边界。** 输入层**只**设置状态，主题负责绘制。
 */

#include <gui/gui_focus.h>
#include <gui/gui_input.h>
#include <gui_input_private.h>

#include <errno.h>
#include <string.h>

/* ── 记录操作 ──────────────────────────────────────────────────── */

/* 在记录里查找控件：只比较指针值，从不解引用。 */
static int focus_index(gui_input_t *input, lv_obj_t *obj)
{
    unsigned i;

    if (!input || !obj) {
        return -1;
    }
    for (i = 0; i < input->focus.count; i++) {
        if (input->focus.entries[i].obj == obj) {
            return (int)i;
        }
    }
    return -1;
}

/* 按原生身份查找条目：诊断(raw)登记没有身份，永远查不到。 */
static int focus_index_by_id(gui_input_t *input, uint64_t native_id)
{
    unsigned i;

    if (!input || native_id == 0) {
        return -1;
    }
    for (i = 0; i < input->focus.count; i++) {
        if (input->focus.entries[i].native && input->focus.entries[i].native_id == native_id) {
            return (int)i;
        }
    }
    return -1;
}

bool gui_input_focus_alive(gui_input_t *input, lv_obj_t *obj)
{
    return focus_index(input, obj) >= 0;
}

/*
 * 自身或祖先被隐藏 / 禁用 → 不是导航候选。
 *
 * LVGL 只在对象自身上检查 DISABLED（禁用父对象不会禁用子对象），隐藏则沿
 * 祖先链判断；这里两条链都查，隐藏 / 禁用的祖先同样排除后代。
 *
 * 焦点候选与滚轮目标共用这一条判定（gui_input_focus_object_usable）：两处
 * 对"可用"的定义必须一致，否则"隐藏 / 禁用的控件不占导航位置"与"滚轮不得
 * 命中隐藏 / 禁用的控件"就会分叉。
 */
bool gui_input_focus_object_usable(lv_obj_t *obj)
{
    lv_obj_t *node = obj;

    while (node) {
        if (lv_obj_has_flag(node, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
        if (lv_obj_get_state(node) & LV_STATE_DISABLED) {
            return false;
        }
        node = lv_obj_get_parent(node);
    }
    return true;
}

/*
 * 可用性：在册、存活、自身/祖先可见且未禁用。
 *
 * 保活、显式 set 与点击都用它；只有 Tab 搜索额外排除原生 CLICK。
 */
static bool focus_usable(gui_input_t *input, const gui_input_focus_entry_t *entry)
{
    if (!input || !entry || !entry->obj) {
        return false;
    }
    if (focus_index(input, entry->obj) < 0) {
        return false;
    }
    return gui_input_focus_object_usable(entry->obj);
}

/* Tab 谓词 = 可用性，且原生 CLICK 策略不参与 Tab 导航。 */
static bool focus_tab_ok(gui_input_t *input, const gui_input_focus_entry_t *entry)
{
    if (!focus_usable(input, entry)) {
        return false;
    }
    if (entry->native && entry->policy == (uint8_t)GUI_FOCUS_CLICK) {
        return false;
    }
    return true;
}

/* 按对象判断可用性（不越界：对象不在册就是不可用）。 */
static bool focus_obj_usable(gui_input_t *input, lv_obj_t *obj)
{
    int idx = focus_index(input, obj);

    if (idx < 0) {
        return false;
    }
    return focus_usable(input, &input->focus.entries[idx]);
}

/* ── 焦点视觉 ──────────────────────────────────────────────────── */

/*
 * 焦点视觉：**输入层只设置状态，主题负责绘制**。
 *
 * Material 主题在 FOCUSED / FOCUS_KEY 状态下画 primary 色的焦点环；输入层
 * 不再覆盖宽度与颜色（那曾经是一条绿色的局部覆盖）。
 */
static void focus_visual_set(lv_obj_t *obj, bool on)
{
    if (!obj) {
        return;
    }
    if (on) {
        lv_obj_add_state(obj, LV_STATE_FOCUS_KEY);
    } else {
        lv_obj_remove_state(obj, LV_STATE_FOCUS_KEY);
    }
}

/* ── 唯一的选择提交路径 ────────────────────────────────────────── */

/*
 * 提交一次选择。
 *
 * 旧/新身份比较、选择记录更新、历史状态更新与后端投影都在这里。任何一次
 * 焦点变化（包含 → NONE）都会显式标记取消协议：旧键不得继续作用到新的
 * 接收者上。
 *
 * **提交本身不碰修复待办**：目标为 NULL 只说明"此刻没有选择"，它可能是
 * 显式清空（稳定态），也可能是焦点失效（待修复）。这两种状态由各自的
 * 路径分别记录，不在这里合并。
 */
static void focus_commit(gui_input_t *input, lv_obj_t *target)
{
    lv_obj_t *old = input->focus.focus;
    int idx;

    if (old == target) {
        /* 选择没有变：不动视觉，也不重发取消。 */
        return;
    }

    /* 旧焦点可能已被删除：记录里查不到就不去碰它。 */
    if (old && gui_input_focus_alive(input, old)) {
        focus_visual_set(old, false);
    }
    input->focus.focus = target;
    input->focus.focus_id = 0;
    if (target) {
        idx = focus_index(input, target);
        if (idx >= 0) {
            input->focus.focus_id = input->focus.entries[idx].native_id;
        }
        focus_visual_set(target, true);
    }

    /* 焦点真的变了 → 取消当前键与它的重复。 */
    gui_input_lvgl_mark_cancel(input);
}

/* ── LVGL 回调 ─────────────────────────────────────────────────── */

/*
 * 组焦点变化（无论来自适配层还是应用直接调用 lv_group_focus_obj）。
 *
 * 只有在册且可用的目标才被采纳：投影事务中的中间选择、后端析构时的
 * 自动转焦都只标脏，真正的选择在下一个外层维护里提交。
 */
static void focus_group_cb(lv_group_t *group)
{
    gui_input_t *input = (gui_input_t *)lv_group_get_user_data(group);
    lv_obj_t *now;
    int idx;

    if (!input) {
        return;
    }

    now = lv_group_get_focused(group);

    if (input->focus.projection_depth > 0) {
        /* 内部重建期间的中间选择不是真选择：只标脏。 */
        input->focus.dirty = true;
        return;
    }

    if (now) {
        idx = focus_index(input, now);
        if (idx < 0 || !focus_usable(input, &input->focus.entries[idx])) {
            /* 不是在册且可用的目标：不采纳（可能是中间态或无效目标）。 */
            input->focus.dirty = true;
            return;
        }
    }

    focus_commit(input, now);
    if (now) {
        input->focus.ever_selected = true;
        input->focus.explicit_clear = false;
        input->focus.repair_pending = false;
    }
}

/* 进入"待修复"：记住修复起点与**路径**。 */
static void focus_repair_arm(gui_input_t *input, unsigned start, bool bounded);

/*
 * 控件被删除：从记录里摘掉（只比较指针值），并通知适配层。
 */
static void focus_obj_event_cb(lv_event_t *e)
{
    gui_input_t *input = (gui_input_t *)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target_obj(e);
    int idx;
    unsigned i;
    bool was_current;

    if (!input || !obj) {
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    idx = focus_index(input, obj);
    if (idx < 0) {
        return;
    }
    was_current = (input->focus.focus == obj);

    /* 顺序保持：后面的项整体前移一位。 */
    for (i = (unsigned)idx; i + 1u < input->focus.count; i++) {
        input->focus.entries[i] = input->focus.entries[i + 1u];
    }
    input->focus.count--;

    if (was_current) {
        /*
         * 移除前的位置：后继从同一个下标开始（移除后同下标即原后继）。
         * 选择此刻失效：清掉，之后不再解引用它；旧键必须取消。
         */
        input->focus.repair_index = (unsigned)idx;
        if (input->focus.delete_depth == 0u) {
            /*
             * 后端自删（不在受控删除批次内）：修复搜索绕回（保既有 raw
             * 契约），找不到候选时保留修复位置，之后候选恢复可继续修复。
             *
             * 受控删除批次内**不**在这里置待修复标志：批次收尾按受控路径
             * （有界）决定修复规则，在这里置标志会盖掉它。
             */
            focus_repair_arm(input, (unsigned)idx, false);
        }
        focus_commit(input, NULL);
    }

    /* 观察者随条目一起消失：已注销的不得残留。 */
    lv_obj_remove_event_cb(obj, focus_obj_event_cb);

    /* LVGL 随后会把对象移出焦点组；旧键不能继续作用于自动选中的新控件。 */
    gui_input_focus_lost(input, obj);
}

/* ── 成员同步与导航 ────────────────────────────────────────────── */

/* 组内成员是否已经等于"当前可用项集合"。 */
static bool focus_members_match(gui_input_t *input)
{
    unsigned i;

    for (i = 0; i < input->focus.count; i++) {
        lv_obj_t *obj = input->focus.entries[i].obj;
        bool want = focus_usable(input, &input->focus.entries[i]);
        bool have = (obj && lv_obj_get_group(obj) == input->lvgl.group);

        if (want != have) {
            return false;
        }
    }
    return true;
}

/*
 * 选择是否与后端一致：真实接收者必须就是记录里的选择。
 *
 * 「显式清空」与「修复待办」是两个**独立**状态：
 *   - 显式清空（explicit_clear）是**稳定态**：没有待修复的事，选择一致，
 *     提前返回因此可命中（每步不重建组）。
 *   - 修复待办（repair_pending）表示"焦点失效后还没找到后继"：此时一致
 *     性不成立，维护路径要继续找后继。
 */
static bool focus_selection_consistent(gui_input_t *input, lv_obj_t *target)
{
    lv_obj_t *group_focus = lv_group_get_focused(input->lvgl.group);

    if (input->focus.repair_pending) {
        return false;
    }
    if (!target) {
        return group_focus == NULL && input->focus.focus == NULL;
    }
    return group_focus == target && input->focus.focus == target;
}

/* 从 start 开始（含）找第一个可用 Tab 候选；到末尾后绕回开头。 */
static lv_obj_t *focus_tab_candidate(gui_input_t *input, unsigned start, bool forward)
{
    unsigned n = input->focus.count;
    unsigned i;

    if (n == 0) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        unsigned idx = forward ? ((start + i) % n) : ((start + n - (i % n)) % n);

        if (focus_tab_ok(input, &input->focus.entries[idx])) {
            return input->focus.entries[idx].obj;
        }
    }
    return NULL;
}

/*
 * 从 start 开始（含）向后扫到**表尾**找第一个可用 Tab 候选；**不回绕**。
 *
 * 受控路径（公共注销、原生删除批次）用它选后继：移除前的位置决定后继，
 * 移除后同下标即原后继；表尾之后没有候选就是没有候选——不许绕回表头
 * 抢一个计划没有授权的选择。
 */
static lv_obj_t *focus_tab_candidate_bounded(gui_input_t *input, unsigned start)
{
    unsigned n = input->focus.count;
    unsigned i;

    for (i = start; i < n; i++) {
        if (focus_tab_ok(input, &input->focus.entries[i])) {
            return input->focus.entries[i].obj;
        }
    }
    return NULL;
}

/* 修复搜索：bounded = true 有界（受控），false 绕回（后端）。 */
static lv_obj_t *focus_repair_search(gui_input_t *input, unsigned start, bool bounded);

/* 按失效时记录的路径修复：受控=有界，后端=绕回。 */
static lv_obj_t *focus_repair_by_mode(gui_input_t *input, unsigned start);

/*
 * 把组整体 detach 到真实 NONE。
 *
 * lv_group_remove_obj() 在删除当前项时会**解除冻结并自动 refocus**：不能靠
 * freeze 阻止它。受控删除因此先把整个组摘空（remove_all_objs 不会 refocus），
 * 再修改表，最后重建。
 */
static void focus_detach_group(gui_input_t *input)
{
    lv_group_t *group = input->lvgl.group;

    if (!group) {
        return;
    }
    input->focus.projection_depth++;
    lv_group_focus_freeze(group, true);
    lv_group_remove_all_objs(group);
    lv_group_focus_freeze(group, false);
    input->focus.projection_depth--;
}

/*
 * 重建组：成员 = 当前可用项（顺序 = 登记顺序），并应用最终目标。
 *
 * 重建期间组回调的中间选择被投影深度屏蔽，因此只有一次真正的选择提交。
 */
static void focus_rebuild(gui_input_t *input, lv_obj_t *target)
{
    lv_group_t *group = input->lvgl.group;
    unsigned i;

    if (!group) {
        return;
    }

#ifdef OS01_HOST_TEST
    /* 宿主私有观察：组重建次数（"稳定态不重建组"的断言用）。 */
    extern uint32_t g_gui_test_focus_rebuild_count;
    g_gui_test_focus_rebuild_count++;
#endif

    input->focus.projection_depth++;
    lv_group_focus_freeze(group, true);
    lv_group_remove_all_objs(group);
    for (i = 0; i < input->focus.count; i++) {
        if (focus_usable(input, &input->focus.entries[i])) {
            lv_group_add_obj(group, input->focus.entries[i].obj);
        }
    }
    lv_group_focus_freeze(group, false);
    input->focus.projection_depth--;

    if (target && focus_obj_usable(input, target)) {
        /* 最终目标：组回调走同一条选择提交路径。 */
        lv_group_focus_obj(target);
        if (lv_group_get_focused(group) != target) {
            focus_commit(input, NULL);
        }
    } else if (input->focus.focus != NULL) {
        /* 真实 NONE：组的焦点已经被 remove_all_objs 清掉。 */
        focus_commit(input, NULL);
    }
}

/* 把焦点落到目标控件（NULL = 无焦点）。 */
static void focus_apply(gui_input_t *input, lv_obj_t *obj)
{
    lv_group_t *group = input->lvgl.group;

    if (!group) {
        return;
    }
    if (!obj) {
        /* 清空：组必须真的没有焦点（真实 NONE）。 */
        if (lv_group_get_focused(group) == NULL && input->focus.focus == NULL) {
            /* 已经是真实 NONE：重建组没有意义（每步重建是 O(n) 抖动）。 */
            return;
        }
        focus_rebuild(input, NULL);
        return;
    }
    if (lv_obj_get_group(obj) != group) {
        focus_rebuild(input, obj);
        return;
    }
    lv_group_focus_obj(obj);
    if (lv_group_get_focused(group) != obj) {
        focus_rebuild(input, NULL);
    }
}

/* 是否应当自动建立首次选择。 */
static bool focus_should_establish_initial(gui_input_t *input)
{
    if (input->focus.ever_selected) {
        return false;
    }
    if (input->focus.explicit_clear) {
        return false;
    }
    return input->focus.initial == (uint8_t)GUI_FOCUS_INITIAL_AUTO_FIRST;
}

/*
 * 让组内成员 = 当前可用项（顺序 = 登记顺序），并保持选择有效。
 */
static void focus_sync(gui_input_t *input)
{
    lv_group_t *group = input->lvgl.group;
    lv_obj_t *old;
    lv_obj_t *target = NULL;
    bool members_ok;
    int old_idx;

    if (!group) {
        return;
    }

    old = input->focus.focus;
    old_idx = old ? focus_index(input, old) : -1;
    members_ok = focus_members_match(input);

    /* 1. 计算目标选择。 */
    if (input->focus.explicit_clear) {
        /* 显式清空保持到下一次成功选择：后续登记或刷新都不复活它。 */
        target = NULL;
    } else if (old && old_idx >= 0 && focus_usable(input, &input->focus.entries[old_idx])) {
        target = old;
    } else if (old == NULL && !input->focus.repair_pending) {
        /* 从未选择过：AUTO_FIRST 建立首次选择，MANUAL 保持 NONE。 */
        target = focus_should_establish_initial(input) ? focus_tab_candidate(input, 0, true) : NULL;
    } else if (old_idx >= 0) {
        /*
         * 焦点对象还在册但不可用（隐藏/禁用）：**后端路径**，本次修复用
         * **绕回**搜索（`focus_repair_search(..., false)`）——待修复状态里
         * 记下的规则不参与（此时并未处于待修复状态）。
         */
        target = focus_repair_search(input, (unsigned)old_idx, false);
    } else {
        /*
         * 焦点已失效：**自动修复续修**，按待修复状态里记下的那条规则走
         * （受控来源 = 有界，后端自删来源 = 绕回）——候选恢复后按**同一条**
         * 规则继续修复（plan:167）。
         */
        target = focus_repair_by_mode(input, input->focus.repair_index);
    }

    /* 2. 应用。提前返回要同时满足四个条件。 */
    if (members_ok && !input->focus.dirty && focus_selection_consistent(input, target)) {
        return;
    }
    input->focus.dirty = false;

    if (!members_ok) {
        focus_rebuild(input, target);
    } else {
        focus_apply(input, target);
    }
}

/* 按登记顺序找出 from 之后的下一个候选（循环）。 */
static lv_obj_t *focus_next_candidate(gui_input_t *input, lv_obj_t *from, bool forward)
{
    int cur = from ? focus_index(input, from) : -1;
    unsigned n = input->focus.count;

    if (cur < 0) {
        /* 没有当前项：正向取第一个，反向取最后一个。 */
        if (n == 0) {
            return NULL;
        }
        return forward ? focus_tab_candidate(input, 0, true)
                       : focus_tab_candidate(input, n - 1u, false);
    }
    return focus_tab_candidate(input, forward ? ((unsigned)cur + 1u) % n
                                               : ((unsigned)cur + n - 1u) % n,
                              forward);
}

/* ── 指针策略 ──────────────────────────────────────────────────── */

/*
 * 同步指针策略：原生登记/更新 policy 时**立即**同步 CLICK_FOCUSABLE。
 *
 * TAB 关闭（点击不夺焦），CLICK / STRONG 开启（点击夺焦）。诊断(raw)登记
 * 保留既有 flag，不重写。
 */
static void focus_sync_click_flag(const gui_input_focus_entry_t *entry)
{
    if (!entry || !entry->obj || !entry->native) {
        return;
    }
    if (entry->policy == (uint8_t)GUI_FOCUS_CLICK || entry->policy == (uint8_t)GUI_FOCUS_STRONG) {
        lv_obj_add_flag(entry->obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    } else {
        lv_obj_remove_flag(entry->obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    }
}

/* ── 组的建立与拆除 ────────────────────────────────────────────── */

/*
 * 建立本 display 的私有焦点组。
 *
 * 组是私有的：keypad 显式绑定它，不使用 LVGL 全局默认组。
 */
int gui_input_focus_create(gui_input_t *input)
{
    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (input->lvgl.group) {
        return 0;   /* 已经建立 */
    }

    input->lvgl.group = lv_group_create();
    if (!input->lvgl.group) {
        errno = ENOMEM;
        return -1;
    }
    lv_group_set_user_data(input->lvgl.group, input);
    lv_group_set_focus_cb(input->lvgl.group, focus_group_cb);
    input->focus.mode = (uint8_t)GUI_FOCUS_MODE_UNSET;
    input->focus.initial = (uint8_t)GUI_FOCUS_INITIAL_AUTO_FIRST;
    return 0;
}

/* 删除焦点组并清空记录。 */
void gui_input_focus_destroy(gui_input_t *input)
{
    unsigned i;

    if (!input) {
        return;
    }

    /*
     * 先摘掉每个控件上的 LV_EVENT_DELETE 回调。
     *
     * 控件比 input 活得久：gui_input_destroy() 释放 input 之后，平台/屏幕
     * 才被销毁，控件随之收到 LV_EVENT_DELETE。回调里会解引用 input
     * （记录与适配层状态），因此必须在这里注销，否则就是释放后使用。
     * 已注销的条目早已摘掉自己的观察者，不会残留。
     */
    for (i = 0; i < input->focus.count; i++) {
        lv_obj_t *obj = input->focus.entries[i].obj;

        if (obj) {
            lv_obj_remove_event_cb(obj, focus_obj_event_cb);
        }
    }

    if (input->lvgl.group) {
        lv_group_delete(input->lvgl.group);
        input->lvgl.group = NULL;
    }
    memset(&input->focus, 0, sizeof(input->focus));
}

/* ── 诊断(raw)登记：内部 hook ──────────────────────────────────── */

/*
 * 登记一个诊断(raw)对象。
 *
 * 这是**内部**入口（guest 探针与宿主诊断测试用它）。它与原生登记共用同一张
 * 表与同一套选择提交路径，差别只在：没有原生身份、不重写 CLICK_FOCUSABLE。
 */
int gui_input_focus_add(gui_input_t *input, lv_obj_t *obj)
{
    lv_display_t *disp;

    if (!input || !obj || !input->lvgl.group || !input->lvgl.keypad) {
        errno = EINVAL;
        return -1;
    }

    /* 混用两种登记被拒绝：模式由第一次成功的登记锁定。 */
    if (input->focus.mode == (uint8_t)GUI_FOCUS_MODE_NATIVE) {
        errno = EBUSY;
        return -1;
    }

    /* 属于别的 display 的控件不是本组的成员。 */
    disp = lv_indev_get_display(input->lvgl.keypad);
    if (disp && lv_obj_get_display(obj) != disp) {
        errno = EINVAL;
        return -1;
    }

    /* 重复登记：返回成功但不添加第二份。 */
    if (focus_index(input, obj) >= 0) {
        return 0;
    }

    if (input->focus.count >= GUI_INPUT_FOCUS_CAPACITY) {
        errno = ENOSPC;
        return -1;
    }

#ifdef OS01_HOST_TEST
    /* 宿主私有故障桥：删除观察者安装失败。 */
    extern bool g_gui_test_fail_focus_observer;
    if (g_gui_test_fail_focus_observer) {
        errno = ENOMEM;
        return -1;
    }
#endif
    if (!lv_obj_add_event_cb(obj, focus_obj_event_cb, LV_EVENT_DELETE, input)) {
        errno = ENOMEM;
        return -1;
    }

    input->focus.entries[input->focus.count].obj = obj;
    input->focus.entries[input->focus.count].native_id = 0;
    input->focus.entries[input->focus.count].policy = (uint8_t)GUI_FOCUS_TAB;
    input->focus.entries[input->focus.count].native = false;
    input->focus.count++;

    /* 第一次成功的登记锁定模式。 */
    input->focus.mode = (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC;

    focus_sync(input);
    return 0;
}

/* 摘除一个登记条目（诊断(raw)与原生共用）。 */
static void focus_drop_entry(gui_input_t *input, unsigned index)
{
    unsigned i;

    for (i = index; i + 1u < input->focus.count; i++) {
        input->focus.entries[i] = input->focus.entries[i + 1u];
    }
    input->focus.count--;
}

/*
 * 进入"待修复"：记住修复起点与**路径**。
 *
 * 路径属性（repair_bounded）记的是"这条待修复属于哪条路径"：
 *   - 受控来源（公共注销 / 原生删除批次）→ 有界；
 *   - 后端自删来源 → 绕回。
 *
 * 粘性：**自动修复续修**永远按这里记下的那条规则走，续修失败时也按**同一条**
 * 规则重新记下（候选恢复后按同一条规则继续修复，plan:167）。只有**新的
 * 事件**（一次新的受控移除 / 一次新的后端自删）才写入它自己的规则。
 */
static void focus_repair_arm(gui_input_t *input, unsigned start, bool bounded)
{
    input->focus.repair_bounded = bounded;
    input->focus.repair_pending = true;
    input->focus.repair_index = start;
}

/*
 * 修复搜索：找不到候选时保留修复起点，之后候选恢复可继续修复。
 *
 * bounded = true → **有界**前向搜索（受控路径）：自起点到表尾，不回绕；表尾
 * 之后没有候选就是没有候选，不许绕回表头抢一个计划没有授权的选择。
 * bounded = false → **绕回**搜索（后端路径）：保既有 raw 契约。
 *
 * 规则由**调用方**决定：一次新的受控移除一律有界 —— 待修复状态里记下的
 * 规则**不得**改写它；只有"焦点失效后的自动修复"续修才使用记下的那条规则。
 */
static lv_obj_t *focus_repair_search(gui_input_t *input, unsigned start, bool bounded)
{
    lv_obj_t *target;

    if (bounded) {
        target = focus_tab_candidate_bounded(input, start);
    } else {
        target = focus_tab_candidate(input, start, true);
    }

    if (target) {
        /* 修复完成：待修复标志与路径属性都复位。 */
        input->focus.repair_pending = false;
        input->focus.repair_bounded = false;
        return target;
    }

    focus_repair_arm(input, start, bounded);
    return NULL;
}

/*
 * 自动修复续修：按待修复状态里记下的那条规则走。
 *
 * 这是唯一使用"记下的规则"的入口（受控来源 = 有界，后端自删 = 绕回）。
 */
static lv_obj_t *focus_repair_by_mode(gui_input_t *input, unsigned start)
{
    return focus_repair_search(input, start, input->focus.repair_bounded);
}

/*
 * 受控移除一个条目：先 detach 组，再改表，再重建并提交一次选择。
 *
 * 移除前的位置决定后继：移除后同下标即原后继。
 *
 * 受控路径的后继搜索是**有界**的（自移除下标到表尾，不回绕）：表尾之后
 * 没有候选就是没有候选——不许绕回表头抢一个计划没有授权的选择。没有候选
 * 时选择保持 NONE，并保留修复位置。
 */
static void focus_remove_controlled(gui_input_t *input, unsigned index)
{
    lv_obj_t *obj = input->focus.entries[index].obj;
    bool was_native = input->focus.entries[index].native;
    bool was_current = (input->focus.focus == obj);
    unsigned repair_index = index;
    lv_obj_t *target = NULL;

    /* 先把组整体 detach 到真实 NONE：后端不得在删除期间转焦。 */
    focus_detach_group(input);

    /* 被移除的对象还活着：它的焦点视觉必须撤掉。 */
    if (was_current && obj) {
        focus_visual_set(obj, false);
    }

    focus_drop_entry(input, index);

    /* 观察者归属：显式注销必须移除删除观察者。 */
    if (obj) {
        lv_obj_remove_event_cb(obj, focus_obj_event_cb);
    }
    /* 未登记的原生对象：指针策略关闭。 */
    if (was_native && obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    }

    if (was_current) {
        input->focus.repair_index = repair_index;
        focus_commit(input, NULL);
    }

    if (input->focus.explicit_clear) {
        /* 显式清空是稳定态：没有待修复的事。 */
        input->focus.repair_pending = false;
        target = NULL;
    } else if (was_current) {
        /* 受控移除自己的后继：本次移除的规则 = 有界前向搜索，不回绕。 */
        target = focus_repair_search(input, repair_index, true);
    } else if (input->focus.focus != NULL) {
        if (focus_obj_usable(input, input->focus.focus)) {
            target = input->focus.focus;
        } else {
            /* 当前选择失效：自动修复续修，按记下的那条规则走。 */
            target = focus_repair_by_mode(input, input->focus.repair_index);
        }
    } else if (input->focus.repair_pending) {
        /*
         * 已在待修复状态：本次移除仍然是**受控路径**，用**自己的**规则
         * （有界前向搜索）——待修复状态里记下的规则**不得**改写它。
         */
        target = focus_repair_search(input, input->focus.repair_index, true);
    } else {
        /* 从未选择过：注销不建立新的选择（MANUAL 语义）。 */
        target = NULL;
    }

    focus_rebuild(input, target);
}

/* 诊断(raw)注销：内部 hook。 */
void gui_input_focus_remove(gui_input_t *input, lv_obj_t *obj)
{
    int idx;

    if (!input || !obj) {
        return;
    }
    if (input->focus.mode != (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        return;
    }
    idx = focus_index(input, obj);
    if (idx < 0) {
        /* 未登记：不碰它（它可能属于别的焦点组）。 */
        return;
    }
    focus_remove_controlled(input, (unsigned)idx);
}

/* 诊断(raw)显式设置选择：内部 hook。 */
int gui_input_focus_set(gui_input_t *input, lv_obj_t *obj)
{
    if (!input || !obj || !input->lvgl.group) {
        errno = EINVAL;
        return -1;
    }
    /* 未登记的控件不能成为焦点。 */
    if (!gui_input_focus_alive(input, obj)) {
        errno = EINVAL;
        return -1;
    }

    focus_sync(input);
    focus_apply(input, obj);
    return 0;
}

lv_obj_t *gui_input_focus_current(gui_input_t *input)
{
    if (!input) {
        return NULL;
    }
    return input->focus.focus;
}

bool gui_input_focus_navigate(gui_input_t *input, bool forward)
{
    lv_obj_t *target;

    if (!input || !input->lvgl.group) {
        return false;
    }

    target = focus_next_candidate(input, input->focus.focus, forward);
    focus_sync(input);

    if (!target) {
        /* 没有可用项：无焦点（这仍然是一次成功）。 */
        focus_apply(input, NULL);
        return false;
    }
    focus_apply(input, target);
    return true;
}

bool gui_input_focus_is_text_input(gui_input_t *input)
{
    lv_obj_t *focus;

    if (!input) {
        return false;
    }
    focus = input->focus.focus;
    if (!focus || !gui_input_focus_alive(input, focus)) {
        return false;
    }
#if LV_USE_TEXTAREA
    return lv_obj_has_class(focus, &lv_textarea_class);
#else
    return false;
#endif
}

/*
 * 每步一次的焦点检查：成员集合与焦点都必须仍然有效。
 * 返回 true 表示焦点发生了变化（适配层据此取消当前键）。
 *
 * 修复也在这里完成（focus_sync）：维护路径**不**建立新的选择——只有
 * 程序、键盘与鼠标的**成功选择**才建立选择。
 */
bool gui_input_focus_refresh(gui_input_t *input)
{
    lv_obj_t *before;

    if (!input || !input->lvgl.group) {
        return false;
    }

    before = input->focus.focus;
    focus_sync(input);
    return before != input->focus.focus;
}

/* ── 原生登记桥（focus.c 使用） ────────────────────────────────── */

uint8_t gui_input_focus_mode(gui_input_t *input)
{
    if (!input) {
        return (uint8_t)GUI_FOCUS_MODE_UNSET;
    }
    return input->focus.mode;
}

int gui_input_focus_register_native(gui_input_t *input, lv_obj_t *obj, uint64_t native_id,
                                    gui_focus_policy_t policy)
{
    int idx;

    if (!input || !obj || !input->lvgl.group || !input->lvgl.keypad || native_id == 0) {
        errno = EINVAL;
        return -1;
    }

    /* 混用两种登记被拒绝。 */
    if (input->focus.mode == (uint8_t)GUI_FOCUS_MODE_RAW_DIAGNOSTIC) {
        errno = EBUSY;
        return -1;
    }

    /* 重复登记：更新策略，保留位置（不占新容量）。 */
    idx = focus_index_by_id(input, native_id);
    if (idx >= 0) {
        input->focus.entries[idx].policy = (uint8_t)policy;
        focus_sync_click_flag(&input->focus.entries[idx]);
        return 0;
    }

    if (input->focus.count >= GUI_INPUT_FOCUS_CAPACITY) {
        errno = ENOSPC;
        return -1;
    }

#ifdef OS01_HOST_TEST
    /*
     * 宿主私有故障桥：第 N 次原生登记失败（倒计数）。
     *
     * 它让宿主测试能覆盖「提交的每一个登记步骤」。
     */
    extern int g_gui_test_fail_focus_register_countdown;
    if (g_gui_test_fail_focus_register_countdown > 0) {
        g_gui_test_fail_focus_register_countdown--;
        if (g_gui_test_fail_focus_register_countdown == 0) {
            errno = ENOSPC;
            return -1;
        }
    }
#endif

#ifdef OS01_HOST_TEST
    /* 宿主私有故障桥：删除观察者安装失败。 */
    extern bool g_gui_test_fail_focus_observer;
    if (g_gui_test_fail_focus_observer) {
        errno = ENOMEM;
        return -1;
    }
#endif
    if (!lv_obj_add_event_cb(obj, focus_obj_event_cb, LV_EVENT_DELETE, input)) {
        errno = ENOMEM;
        return -1;
    }

    input->focus.entries[input->focus.count].obj = obj;
    input->focus.entries[input->focus.count].native_id = native_id;
    input->focus.entries[input->focus.count].policy = (uint8_t)policy;
    input->focus.entries[input->focus.count].native = true;
    input->focus.count++;

    /* 第一次成功的登记锁定模式。 */
    input->focus.mode = (uint8_t)GUI_FOCUS_MODE_NATIVE;

    /* 指针策略立即同步。 */
    focus_sync_click_flag(&input->focus.entries[input->focus.count - 1u]);

    focus_sync(input);
    return 0;
}

int gui_input_focus_unregister_native(gui_input_t *input, uint64_t native_id)
{
    int idx;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (input->focus.mode != (uint8_t)GUI_FOCUS_MODE_NATIVE) {
        errno = EOPNOTSUPP;
        return -1;
    }
    idx = focus_index_by_id(input, native_id);
    if (idx < 0) {
        errno = EINVAL;
        return -1;
    }

    focus_remove_controlled(input, (unsigned)idx);

    return 0;
}

int gui_input_focus_set_native(gui_input_t *input, uint64_t native_id)
{
    int idx;

    if (!input) {
        errno = EINVAL;
        return -1;
    }
    if (input->focus.mode != (uint8_t)GUI_FOCUS_MODE_NATIVE) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (native_id == 0) {
        /*
         * 显式清空：保持 NONE，直到下一次成功选择。
         *
         * 显式清空**不是**修复待办：它不置 repair_pending，反而把它清掉
         * （此前的修复待办作废：用户明确说了"不要焦点"）。清空是稳定态，
         * 后续刷新可以提前返回，不必每步重建组。
         */
        input->focus.explicit_clear = true;
        input->focus.repair_pending = false;
        focus_sync(input);
        return 0;
    }

    idx = focus_index_by_id(input, native_id);
    if (idx < 0) {
        errno = EINVAL;
        return -1;
    }
    if (!focus_usable(input, &input->focus.entries[idx])) {
        /* 不可用目标设焦点：EACCES。选择与清空标志都不变。 */
        errno = EACCES;
        return -1;
    }

    focus_sync(input);
    focus_apply(input, input->focus.entries[idx].obj);
    return 0;
}

uint64_t gui_input_focus_current_native_id(gui_input_t *input)
{
    if (!input) {
        return 0;
    }
    return input->focus.focus_id;
}

void gui_input_focus_set_initial(gui_input_t *input, gui_focus_initial_t initial)
{
    if (!input) {
        return;
    }
    input->focus.initial = (uint8_t)initial;
}

/* ── 删除批次 ──────────────────────────────────────────────────── */

void gui_input_focus_delete_batch_begin(gui_input_t *input)
{
    if (!input) {
        return;
    }
    if (input->focus.delete_depth++ > 0u) {
        return;
    }
    /*
     * 组整体 detach 到真实 NONE：批次期间后端不得转焦，中间的删除回调
     * 只摘条目，不提交选择。
     */
    focus_detach_group(input);
}

void gui_input_focus_delete_batch_end(gui_input_t *input)
{
    lv_obj_t *target = NULL;

    if (!input || input->focus.delete_depth == 0u) {
        return;
    }
    if (--input->focus.delete_depth > 0u) {
        return;
    }

    /*
     * 批次结束：选跳过整棵待删子树的后继，提交一次最终投影。
     *
     * 原生删除批次是**受控路径**：后继搜索**有界**（自修复位置到表尾，不回绕），
     * 且不继承待修复状态里记下的规则。没有候选 → 选择 NONE + 保留修复位置。
     */
    if (!input->focus.explicit_clear) {
        target = focus_repair_search(input, input->focus.repair_index, true);
    }
    focus_rebuild(input, target);
}
