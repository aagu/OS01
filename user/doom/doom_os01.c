#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gfx.h>
#include "doom_os01.h"
#include "doom_runtime.h"
#include "doomgeneric.h"
#include "i_system.h"

static doom_runtime_t g_runtime;
static doom_os01_config_t g_cfg;

int doom_os01_parse_args(int argc, char **argv, doom_os01_config_t *cfg,
                         int *out_argc, char ***out_argv)
{
    if (!cfg || !argv || argc < 1) {
        return -1;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->scale = 0; /* default auto */
    strncpy(cfg->data_dir, "/doom-data", sizeof(cfg->data_dir) - 1);

    bool scale_seen = false;
    bool data_dir_seen = false;
    bool iwad_seen = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-scaling") == 0 || strcmp(argv[i], "-gfxmode") == 0) {
            fprintf(stderr, "doom: unsafe argument '%s' is rejected\n", argv[i]);
            return -1;
        }
        if (argv[i][0] == '@') {
            fprintf(stderr, "doom: response file '%s' is rejected\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--os01-scale") == 0) {
            if (scale_seen) {
                fprintf(stderr, "doom: duplicate --os01-scale\n");
                return -1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "doom: --os01-scale requires an argument\n");
                return -1;
            }
            const char *val = argv[++i];
            char *endptr = NULL;
            long s = strtol(val, &endptr, 10);
            if (!val[0] || *endptr != '\0' || s < 1 || s > 3) {
                fprintf(stderr, "doom: invalid scale '%s', must be 1..3\n", val);
                return -1;
            }
            cfg->scale = (unsigned)s;
            scale_seen = true;
        } else if (strcmp(argv[i], "--os01-data-dir") == 0) {
            if (data_dir_seen) {
                fprintf(stderr, "doom: duplicate --os01-data-dir\n");
                return -1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "doom: --os01-data-dir requires an argument\n");
                return -1;
            }
            const char *dir = argv[++i];
            if (dir[0] != '/') {
                fprintf(stderr, "doom: data directory must be an absolute path: '%s'\n", dir);
                return -1;
            }
            strncpy(cfg->data_dir, dir, sizeof(cfg->data_dir) - 1);
            cfg->data_dir[sizeof(cfg->data_dir) - 1] = '\0';
            data_dir_seen = true;
        } else if (strcmp(argv[i], "-iwad") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "doom: -iwad requires an argument\n");
                return -1;
            }
            const char *path = argv[++i];
            if (path[0] == '/') {
                strncpy(cfg->iwad_path, path, sizeof(cfg->iwad_path) - 1);
                cfg->iwad_path[sizeof(cfg->iwad_path) - 1] = '\0';
            } else {
                char cwd[512];
                if (!getcwd(cwd, sizeof(cwd))) {
                    return -1;
                }
                snprintf(cfg->iwad_path, sizeof(cfg->iwad_path), "%s/%s", cwd, path);
            }
            iwad_seen = true;
        }
    }

    if (!iwad_seen) {
        strncpy(cfg->iwad_path, "/usr/share/games/doom/iwad.wad", sizeof(cfg->iwad_path) - 1);
        cfg->iwad_path[sizeof(cfg->iwad_path) - 1] = '\0';
    }

    if (out_argc && out_argv) {
        char **new_argv = malloc((argc + 4) * sizeof(char *));
        if (!new_argv) {
            return -1;
        }
        int new_argc = 0;
        new_argv[new_argc++] = argv[0];

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--os01-scale") == 0 ||
                strcmp(argv[i], "--os01-data-dir") == 0) {
                i++;
                continue;
            }
            if (strcmp(argv[i], "-iwad") == 0) {
                new_argv[new_argc++] = argv[i];
                new_argv[new_argc++] = cfg->iwad_path;
                i++;
                continue;
            }
            new_argv[new_argc++] = argv[i];
        }

        if (!iwad_seen) {
            new_argv[new_argc++] = "-iwad";
            new_argv[new_argc++] = cfg->iwad_path;
        }
        new_argv[new_argc] = NULL;
        *out_argc = new_argc;
        *out_argv = new_argv;
    }

    return 0;
}

int doom_os01_verify_iwad(const char *path)
{
    if (!path || access(path, R_OK) != 0) {
        fprintf(stderr, "doom: cannot access IWAD at '%s'\n"
                        "Please place a valid DOOM IWAD at '%s' or specify -iwad <path>.\n",
                path ? path : "(null)", path ? path : "(null)");
        return -1;
    }
    return 0;
}

void DG_Init(void)
{
    if (!DG_ScreenBuffer) {
        fprintf(stderr, "doom: DG_ScreenBuffer is NULL\n");
        exit(1);
    }
    if (doom_runtime_open(&g_runtime, g_cfg.scale) < 0) {
        fprintf(stderr, "doom: failed to initialize display or input: %s\n", strerror(errno));
        exit(1);
    }
}

void DG_DrawFrame(void)
{
    doom_runtime_t *rt = doom_runtime_get_active();
    if (!rt) {
        exit(1);
    }
    int rc = doom_video_draw(&rt->video, DG_ScreenBuffer);
    if (rc < 0) {
        exit(1);
    }
}

void DG_SleepMs(uint32_t ms)
{
    doom_runtime_sleep_ms(ms);
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)doom_runtime_now_ms();
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    doom_runtime_t *rt = doom_runtime_get_active();
    if (!rt) {
        exit(1);
    }
    int rc = doom_input_pop(&rt->input, pressed, key);
    if (rc < 0) {
        exit(1);
    }
    return rc;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

#ifndef OS01_HOST_TEST
int main(int argc, char **argv)
{
    int new_argc = 0;
    char **new_argv = NULL;

    if (doom_os01_parse_args(argc, argv, &g_cfg, &new_argc, &new_argv) != 0) {
        return 1;
    }

    if (doom_os01_verify_iwad(g_cfg.iwad_path) != 0) {
        return 1;
    }

    /* Create data directory if needed */
    if (mkdir(g_cfg.data_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "doom: cannot create data directory '%s': %s\n",
                g_cfg.data_dir, strerror(errno));
        return 1;
    }

    struct stat st;
    if (stat(g_cfg.data_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "doom: data directory '%s' is not a directory\n", g_cfg.data_dir);
        return 1;
    }

    if (chdir(g_cfg.data_dir) != 0) {
        fprintf(stderr, "doom: cannot chdir to '%s': %s\n",
                g_cfg.data_dir, strerror(errno));
        return 1;
    }

    doom_runtime_install_signals();
    if (atexit(doom_runtime_cleanup) != 0) {
        fprintf(stderr, "doom: cannot register exit cleanup\n");
        return 1;
    }

    doomgeneric_Create(new_argc, new_argv);
    doom_runtime_set_engine_created(1);

    while (!doom_runtime_should_exit()) {
        uint64_t now_ms = doom_runtime_now_ms();
        if (doom_input_pump(&g_runtime.input, now_ms) < 0) {
            exit(1);
        }
        doomgeneric_Tick();
    }

    I_Quit();
    return 0;
}
#endif
