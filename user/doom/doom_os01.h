#ifndef _DOOM_OS01_H
#define _DOOM_OS01_H

typedef struct {
    unsigned scale;
    char data_dir[512];
    char iwad_path[512];
    char diag_nonce[64];
    char diag_action[64];
} doom_os01_config_t;

int doom_os01_parse_args(int argc, char **argv, doom_os01_config_t *cfg,
                         int *out_argc, char ***out_argv);
int doom_os01_verify_iwad(const char *path);

#endif /* _DOOM_OS01_H */
