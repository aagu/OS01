/* user/test_gui_fault/test_gui_fault.c — Fault injection tool for GUI QEMU suite */

#include <gui/gui.h>
#include <gui_lvgl_private.h>

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    FAULT_MODE_NONE = 0,
    FAULT_MODE_HOLD,
    FAULT_MODE_OOM,
} fault_mode_t;

int main(int argc, char *argv[])
{
    fault_mode_t mode = FAULT_MODE_NONE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hold") == 0 || strcmp(argv[i], "hold") == 0) {
            mode = FAULT_MODE_HOLD;
        } else if (strcmp(argv[i], "--oom") == 0 || strcmp(argv[i], "oom") == 0) {
            mode = FAULT_MODE_OOM;
        }
    }

    if (mode == FAULT_MODE_NONE) {
        fprintf(stderr, "Usage: %s --hold | --oom\n", argv[0]);
        return 1;
    }

    gui_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = GUI_MODE_ONSCREEN;
    cfg.width = 0;
    cfg.height = 0;
    cfg.buffer_lines = 40;

    gui_platform_t *platform = gui_platform_create(&cfg);
    if (!platform) {
        fprintf(stderr, "[gui-fault] gui_platform_create failed (errno=%d): %s\n",
                errno, strerror(errno));
        return 1;
    }

    lv_display_t *disp = gui_platform_get_lv_display(platform);
    if (!disp) {
        fprintf(stderr, "[gui-fault] failed to get lv_display\n");
        gui_platform_destroy(platform);
        return 1;
    }

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    if (!scr) {
        fprintf(stderr, "[gui-fault] failed to get active screen\n");
        gui_platform_destroy(platform);
        return 1;
    }

    /* Pure solid color UI: Magenta 0xFF00AA (R=255, G=0, B=170) */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF00AA), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Initial step to render and present the solid color */
    uint32_t delay_ms = 0;
    gui_status_t status = gui_platform_step(platform, &delay_ms);
    if (status != GUI_STATUS_OK) {
        fprintf(stderr, "[gui-fault] initial step failed (status=%d, errno=%d)\n",
                (int)status, errno);
        gui_platform_destroy(platform);
        return 1;
    }

    const char *mode_str = (mode == FAULT_MODE_HOLD) ? "hold" : "oom";
    printf("[gui-fault] acquired pid=%d mode=%s\n", (int)getpid(), mode_str);
    fflush(stdout);

    if (mode == FAULT_MODE_HOLD) {
        /* Hold ownership, stepping and polling until terminated */
        while (1) {
            uint32_t next_ms = 100;
            status = gui_platform_step(platform, &next_ms);
            if (status != GUI_STATUS_OK) {
                break;
            }
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
            int pret = poll(&pfd, 1, (int)next_ms);
            if (pret < 0 && errno != EINTR) {
                break;
            }
        }
        gui_platform_destroy(platform);
        return 0;
    }

    if (mode == FAULT_MODE_OOM) {
        /* Brief window to allow screendump before triggering OOM */
        poll(NULL, 0, 500); /* 500 ms */

        /* Exhaust memory pool using 4096, 256, 16, 1 byte chunks */
        size_t chunk_sizes[] = { 4096, 256, 16, 1 };
        for (int i = 0; i < 4; i++) {
            size_t sz = chunk_sizes[i];
            while (1) {
                void *ptr = lv_malloc(sz);
                if (!ptr) {
                    break;
                }
            }
        }

        /* Verify 1-byte allocation also fails */
        void *probe = lv_malloc(1);
        if (probe != NULL) {
            fprintf(stderr, "[gui-fault] 1-byte malloc unexpectedly succeeded\n");
            return 3;
        }

        /* Trigger real LV_ASSERT_MALLOC in lv_timer_create */
        lv_timer_create(NULL, 1, NULL);

        /* If lv_timer_create returns unexpectedly, exit 2 so driver detects failure */
        return 2;
    }

    return 0;
}
