/*
 * gui_input_nonce.h — the GUI-2 input probe's nonce rule.
 *
 * The probe's CLI is `--nonce <hex>`: the nonce identifies one probe run
 * and ends up in the control file's path (/tmp/gui-input-<hex>.ctl).
 * Validating it as **hex** makes a path escape impossible by construction:
 * no '/', no '.', no whitespace and no shell metacharacter can ever reach
 * the path, so the control file can only ever be created and removed
 * inside /tmp.
 *
 * The rule lives in this header so that the guest probe
 * (user/test_gui_input/test_gui_input.c) and the host test
 * (hosttests/cases/test_gui_input_nonce.c) exercise the SAME code.
 *
 * The header is deliberately dependency-free (no LVGL, no libgui) so that
 * it can be compiled for the host as well as for the target.
 */
#ifndef _GUI_INPUT_NONCE_H
#define _GUI_INPUT_NONCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* The longest accepted nonce, in hex characters. */
#define GUI_INPUT_NONCE_MAX_CHARS 32

/* The directory the control file lives in. */
#define GUI_INPUT_CTL_DIR "/tmp"

/* The control file name template: <dir>/gui-input-<hex>.ctl */
#define GUI_INPUT_CTL_NAME "gui-input-%s.ctl"

/*
 * A valid nonce is a non-empty hex string of at most
 * GUI_INPUT_NONCE_MAX_CHARS characters.  Everything else — an empty
 * string, a path, a shell fragment, an over-long string — is rejected.
 */
static inline bool gui_input_nonce_is_valid(const char *nonce)
{
    size_t len = 0;

    if (nonce == NULL) {
        return false;
    }
    while (nonce[len] != '\0') {
        char c = nonce[len];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
        len++;
        if (len > (size_t)GUI_INPUT_NONCE_MAX_CHARS) {
            return false;
        }
    }
    return len > 0u;
}

/*
 * Builds the control file path: /tmp/gui-input-<hex>.ctl
 *
 * Returns 0 on success.  Returns -1 when the nonce is invalid or the
 * buffer is too small; the output buffer is then set to the empty string,
 * so a caller that ignores the result still cannot build a path outside
 * /tmp.
 */
static inline int gui_input_nonce_ctl_path(char *out, size_t out_len,
                                            const char *nonce)
{
    int n;

    if (out == NULL || out_len == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (!gui_input_nonce_is_valid(nonce)) {
        return -1;
    }
    n = snprintf(out, out_len, "%s/" GUI_INPUT_CTL_NAME,
                 GUI_INPUT_CTL_DIR, nonce);
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

#endif /* _GUI_INPUT_NONCE_H */
