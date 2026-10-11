#ifndef MONITOR_RENDER_H
#define MONITOR_RENDER_H

/*
 * user/monitor/render.h — monitor plan Task 8: terminal dashboard.
 *
 * The renderer is the ONLY place the monitor writes bytes to a stream.
 * It consumes a finished monitor_model (Task 7) and never reads a file
 * or talks to the kernel (spec §7).  Two layouts share one code path:
 *
 *   - cols > 0: the interactive ANSI dashboard.  Every visible line is
 *     clamped to `cols` columns; a clamp truncates VISIBLE text only —
 *     an escape sequence is always written whole (it never consumes a
 *     visible column), so a narrow terminal can never cut a control
 *     sequence in half.  A line that opened a colour always closes it
 *     before the newline, so a clamp mid-line cannot leak an attribute
 *     onto the next line.
 *   - cols == 0: the plain, non-TTY layout used by `--samples` and when
 *     stdin/stdout is not a terminal.  No escape bytes are emitted at
 *     all and nothing is truncated.
 *
 * The whole program routes its output through this header (monitor.c
 * includes it for the frame control and diagnostics), which gives the
 * host fixture a single seam (the renamed vfprintf) to observe every
 * byte the monitor can produce.
 */

#include <stdio.h>

#include <monitor/model.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render one dashboard frame of `model` onto `out`.
 *   rows > 0 caps the number of lines; rows == 0 leaves it unbounded.
 *   cols > 0 selects the ANSI layout and clamps visible width;
 *   cols == 0 selects the plain layout (no escape bytes, no clamp).
 * Returns 0 on success, -1 for a NULL argument. */
int monitor_render(FILE *out, const struct monitor_model *model,
                   unsigned rows, unsigned cols);

/* Enter the interactive screen: clear, home the cursor, hide it.  Only
 * called by the loop in TTY mode. */
void monitor_term_enter(FILE *out);

/* Leave the interactive screen: reset attributes and show the cursor.
 * Only called by the loop in TTY mode, from the single exit path. */
void monitor_term_leave(FILE *out);

/* Write a literal string (no layout, no clamp).  Used by monitor.c for
 * user-facing diagnostics so every byte shares the render stream. */
void monitor_puts(FILE *out, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_RENDER_H */
