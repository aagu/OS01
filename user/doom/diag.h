#ifndef USER_DOOM_DIAG_H
#define USER_DOOM_DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void doom_diag_init(const char *nonce);
void doom_diag_set_output_fd(int fd);
void doom_diag_set_action(const char *action);
const char *doom_diag_get_action(void);
void doom_diag_emit(const char *type, const char *fmt, ...);
void doom_diag_first_frame(void);
int  doom_diag_first_frame_done(void);
void doom_diag_present_stat(int prc, int err, uint64_t duration_us);
void doom_diag_tic(int gametic, int leveltime, int gamestate, int map,
                   int x, int y, int angle,
                   int forwardmove, int sidemove, int angleturn, int buttons);
void doom_diag_save(int slot, const char *desc, int map, int x, int y, int angle, int hp);
void doom_diag_load(int slot, int map, int x, int y, int angle, int hp);
void doom_diag_bindings(void);
void doom_diag_summary(void);
void doom_diag_self_release(void);

#ifdef __cplusplus
}
#endif

#endif /* USER_DOOM_DIAG_H */
