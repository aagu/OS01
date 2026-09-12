#ifndef _KERNEL_FUTEX_H
#define _KERNEL_FUTEX_H

void futex_init(void);
int do_futex_wait(int *uaddr, int val);
int do_futex_wake(int *uaddr, int val);

#endif
