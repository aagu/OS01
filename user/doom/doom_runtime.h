#ifndef _DOOM_RUNTIME_H
#define _DOOM_RUNTIME_H

#include <stdint.h>
#include <gfx.h>
#include "doom_video.h"
#include "doom_input.h"

typedef struct doom_runtime {
    doom_video_t video;
    doom_input_t input;
    int initialized;
    int engine_created;
} doom_runtime_t;

int doom_runtime_open(doom_runtime_t *rt, unsigned requested_scale);
void doom_runtime_close(doom_runtime_t *rt);
void doom_runtime_cleanup(void);
uint64_t doom_runtime_now_ms(void);
void doom_runtime_sleep_ms(uint32_t ms);
void doom_runtime_install_signals(void);
int doom_runtime_should_exit(void);
void doom_runtime_request_exit(void);
void doom_runtime_set_engine_created(int created);

#endif /* _DOOM_RUNTIME_H */
