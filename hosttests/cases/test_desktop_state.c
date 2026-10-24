/*
 * test_desktop_state.c — 桌面外壳状态机契约测试（GUI-3 Task 3）
 *
 * 编译**真实的** user/desktop/desktop_state.c（纯 C，无 LVGL 依赖），
 * 穷举 (状态 × 意图) 全组合，验证设计规范 §9.2 的第 1–7 条：
 *   - 只允许规范列出的迁移，其余一律 DESKTOP_STATE_INVALID
 *   - HOME + OPEN_APP(id) → APP
 *   - APP + OPEN_APP(other) → INVALID（应用不允许叠加）
 *   - HOME + ESC → HOME（不改变状态）
 *   - BACK / ESC 在 DRAWER / SETTINGS / APP 上一律 → HOME
 *   - OPEN_APP 缺 app_id 或 app_id 为空串 → INVALID
 *   - 每个合法状态都有互不相同的名字
 */

#include "test_framework.h"
#include "desktop_state.h"

#include <stdbool.h>
#include <stddef.h>

#define STATE_COUNT 4

static const desktop_state_t g_states[STATE_COUNT] = {
    DESKTOP_STATE_HOME,
    DESKTOP_STATE_DRAWER,
    DESKTOP_STATE_SETTINGS,
    DESKTOP_STATE_APP,
};

static const desktop_intent_t g_intents[] = {
    DESKTOP_INTENT_NONE,
    DESKTOP_INTENT_OPEN_DRAWER,
    DESKTOP_INTENT_OPEN_SETTINGS,
    DESKTOP_INTENT_OPEN_APP,
    DESKTOP_INTENT_BACK,
    DESKTOP_INTENT_ESC,
};

#define INTENT_COUNT (sizeof(g_intents) / sizeof(g_intents[0]))

static bool is_expected_state(desktop_state_t st)
{
    for (size_t i = 0; i < STATE_COUNT; i++) {
        if (g_states[i] == st) {
            return true;
        }
    }
    return false;
}

/* ── 断言 1：全组合的结果只能是合法状态或 INVALID ───────────────── */

TEST_FUNC(test_every_combination_yields_a_known_state)
{
    for (size_t i = 0; i < STATE_COUNT; i++) {
        for (size_t j = 0; j < INTENT_COUNT; j++) {
            desktop_state_t next = desktop_state_next(g_states[i], g_intents[j], "files");
            assert_true(next == DESKTOP_STATE_INVALID || is_expected_state(next));
        }
    }
}

/* ── 断言 2：HOME + OPEN_APP → APP ──────────────────────────────── */

TEST_FUNC(test_home_opens_an_app)
{
    assert_eq(DESKTOP_STATE_APP,
              desktop_state_next(DESKTOP_STATE_HOME, DESKTOP_INTENT_OPEN_APP, "files"));
    assert_eq(DESKTOP_STATE_APP,
              desktop_state_next(DESKTOP_STATE_HOME, DESKTOP_INTENT_OPEN_APP, "terminal"));
}

/* ── 断言 3：应用不允许叠加 ─────────────────────────────────────── */

TEST_FUNC(test_app_cannot_stack_another_app)
{
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_APP, DESKTOP_INTENT_OPEN_APP, "files"));
    /* 抽屉与设置同理：不能从非主屏直接跳到另一个表面。 */
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_DRAWER, DESKTOP_INTENT_OPEN_APP, "files"));
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_SETTINGS, DESKTOP_INTENT_OPEN_APP, "files"));
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_DRAWER, DESKTOP_INTENT_OPEN_SETTINGS, NULL));
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_SETTINGS, DESKTOP_INTENT_OPEN_DRAWER, NULL));
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_APP, DESKTOP_INTENT_OPEN_DRAWER, NULL));
}

/* ── 断言 4：ESC 在主屏上无副作用 ───────────────────────────────── */

TEST_FUNC(test_escape_on_home_changes_nothing)
{
    assert_eq(DESKTOP_STATE_HOME,
              desktop_state_next(DESKTOP_STATE_HOME, DESKTOP_INTENT_ESC, NULL));
    assert_eq(DESKTOP_STATE_HOME,
              desktop_state_next(DESKTOP_STATE_HOME, DESKTOP_INTENT_BACK, NULL));
}

/* ── 断言 5：BACK / ESC 一律回主屏 ──────────────────────────────── */

TEST_FUNC(test_back_returns_to_home_from_every_surface)
{
    const desktop_state_t others[] = {
        DESKTOP_STATE_DRAWER,
        DESKTOP_STATE_SETTINGS,
        DESKTOP_STATE_APP,
    };

    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        assert_eq(DESKTOP_STATE_HOME,
                  desktop_state_next(others[i], DESKTOP_INTENT_BACK, NULL));
        assert_eq(DESKTOP_STATE_HOME,
                  desktop_state_next(others[i], DESKTOP_INTENT_ESC, NULL));
    }
}

/* ── 断言 6：OPEN_APP 缺少有效 app_id 时非法 ────────────────────── */

TEST_FUNC(test_open_app_requires_an_app_id)
{
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_HOME, DESKTOP_INTENT_OPEN_APP, NULL));
    assert_eq(DESKTOP_STATE_INVALID,
              desktop_state_next(DESKTOP_STATE_HOME, DESKTOP_INTENT_OPEN_APP, ""));
}

/* ── 断言 7：NONE 意图不改变状态 ────────────────────────────────── */

TEST_FUNC(test_none_intent_keeps_the_state)
{
    for (size_t i = 0; i < STATE_COUNT; i++) {
        assert_eq(g_states[i],
                  desktop_state_next(g_states[i], DESKTOP_INTENT_NONE, NULL));
    }
}

/* ── 断言 8：状态名互不相同；非法状态有名字 ─────────────────────── */

TEST_FUNC(test_state_names_are_distinct)
{
    const char *home = desktop_state_name(DESKTOP_STATE_HOME);
    const char *drawer = desktop_state_name(DESKTOP_STATE_DRAWER);
    const char *settings = desktop_state_name(DESKTOP_STATE_SETTINGS);
    const char *app = desktop_state_name(DESKTOP_STATE_APP);

    assert_not_null(home);
    assert_not_null(drawer);
    assert_not_null(settings);
    assert_not_null(app);

    assert_true(home != drawer);
    assert_true(home != settings);
    assert_true(home != app);
    assert_true(drawer != settings);
    assert_true(drawer != app);
    assert_true(settings != app);

    /* 非法状态也有名字，便于诊断输出。 */
    assert_not_null(desktop_state_name(DESKTOP_STATE_INVALID));

    /* 合法性判定与枚举一致。 */
    for (size_t i = 0; i < STATE_COUNT; i++) {
        assert_true(desktop_state_is_valid(g_states[i]));
    }
    assert_false(desktop_state_is_valid(DESKTOP_STATE_INVALID));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_every_combination_yields_a_known_state),
    TEST_ENTRY(test_home_opens_an_app),
    TEST_ENTRY(test_app_cannot_stack_another_app),
    TEST_ENTRY(test_escape_on_home_changes_nothing),
    TEST_ENTRY(test_back_returns_to_home_from_every_surface),
    TEST_ENTRY(test_open_app_requires_an_app_id),
    TEST_ENTRY(test_none_intent_keeps_the_state),
    TEST_ENTRY(test_state_names_are_distinct),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
