/*
 * test_user_log.c — the user-space debug log facade (user/common/user_log.c)
 *
 * The facade is the ring-3 counterpart of the kernel's log_debug(): the
 * terminal's per-second [term-present] / [term-probe] records are
 * emitted through it, and the default build (USER_DEBUG=0) must not
 * produce any serial output at all.
 *
 * Two builds of this case:
 *   * -DUSER_DEBUG=1 — the enabled path: log_debug() formats one line and
 *                      hands it to the registered sink verbatim; the sink's
 *                      return value propagates to the caller.
 *   * (default)      — the production default: log_debug() is compiled out,
 *                      so neither the format string nor its arguments are
 *                      evaluated and nothing is emitted.
 */

#include "test_framework.h"
#include "user_log.h"

#if USER_DEBUG

/* ── Capture sinks ──────────────────────────────────────────────── */
static char g_last_text[256];
static size_t g_last_len;
static int g_sink_calls;
static int g_sink_ctx_seen;

static int capture_sink(void *ctx, const char *text, size_t len)
{
    g_sink_calls++;
    g_sink_ctx_seen = (ctx == (void *)&g_sink_calls);
    if (len < sizeof g_last_text) {
        memcpy(g_last_text, text, len);
        g_last_text[len] = '\0';
        g_last_len = len;
    } else {
        g_last_text[0] = '\0';
        g_last_len = 0;
    }
    return 0;
}

static int failing_sink(void *ctx, const char *text, size_t len)
{
    (void)ctx; (void)text; (void)len;
    g_sink_calls++;
    return -1;
}

TEST_FUNC(test_log_debug_routes_to_the_sink) {
    g_sink_calls = 0;
    g_last_len = 0;
    g_last_text[0] = '\0';
    user_log_set_sink(capture_sink, (void *)&g_sink_calls);

    int rc = log_debug("[term-present] version=1 begin_ns=%llu\r\n", 42ull);
    assert_eq(0, rc);
    assert_eq(1, g_sink_calls);
    assert_str_eq("[term-present] version=1 begin_ns=42\r\n", g_last_text);
    assert_eq((long)strlen("[term-present] version=1 begin_ns=42\r\n"),
              (long)g_last_len);
    assert_true(g_sink_ctx_seen);
}

TEST_FUNC(test_log_debug_propagates_the_sink_failure) {
    g_sink_calls = 0;
    user_log_set_sink(failing_sink, NULL);
    /* The emit path relies on this: a failed write keeps the metrics
     * interval so the next emit can merge it. */
    int rc = log_debug("[term-probe] wakes/s=%llu\r\n", 7ull);
    assert_eq(-1, rc);
    assert_eq(1, g_sink_calls);
}

TEST_FUNC(test_log_debug_uses_the_registered_ctx) {
    g_sink_calls = 0;
    g_sink_ctx_seen = 0;
    user_log_set_sink(capture_sink, (void *)&g_sink_calls);
    (void)log_debug("[term-present] ctx\r\n");
    assert_eq(1, g_sink_calls);
    assert_true(g_sink_ctx_seen);
}

TEST_FUNC(test_log_debug_sink_can_be_cleared) {
    g_sink_calls = 0;
    user_log_set_sink(capture_sink, (void *)&g_sink_calls);
    user_log_set_sink(NULL, NULL);
    /* Without a sink the module falls back to its own /dev/serial
     * writer, which may or may not exist on the host — either way the
     * registered sink must NOT be consulted any more. */
    (void)log_debug("[term-present] dropped\r\n");
    assert_eq(0, g_sink_calls);
}

#else /* USER_DEBUG == 0: the production default */

TEST_FUNC(test_log_debug_is_compiled_out) {
    int evaluated = 0;
    /* The macro expands to nothing: the argument must never be
     * evaluated and nothing may reach the wire. */
    log_debug("[term-present] version=1 begin_ns=%d\r\n", ++evaluated);
    assert_eq(0, evaluated);
}

#endif /* USER_DEBUG */

TEST_LIST_BEGIN
#if USER_DEBUG
    TEST_ENTRY(test_log_debug_routes_to_the_sink),
    TEST_ENTRY(test_log_debug_propagates_the_sink_failure),
    TEST_ENTRY(test_log_debug_uses_the_registered_ctx),
    TEST_ENTRY(test_log_debug_sink_can_be_cleared),
#else
    TEST_ENTRY(test_log_debug_is_compiled_out),
#endif
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
