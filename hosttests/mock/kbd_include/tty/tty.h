/* Host shadow of <tty/tty.h> for compiling
 * kernel/driver/keyboard.c (PS/2 mouse driver Task 5).
 * keyboard.c only stores the pointer and calls tty_push_input(); the
 * mock records pushed characters so tests assert exact TTY streams
 * (implementation in test_i8042_demux.c). */
#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

typedef struct tty_struct {
    int mock_id;
} tty_t;

void tty_push_input(tty_t *tty, char c);

#endif /* _KERNEL_TTY_H */
