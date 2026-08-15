#ifndef _KERNEL_CANON_H
#define _KERNEL_CANON_H

#include <stdint.h>
#include <stdbool.h>

// Canonical (ICANON) line discipline buffer — pure logic, no kernel deps.
// Host-testable: test/cases/test_canon.c compiles this with a small driver.

#define CANON_BUF_SIZE 256

typedef struct {
    char buf[CANON_BUF_SIZE];
    int  len;
} canon_buf_t;

// Accumulate bytes from a byte ring (head/tail indices, producer/consumer)
// into the canon buffer until '\n' or capacity.  Ring is [0, ring_size).
// Caller owns locking.  Returns new canon length.
int canon_accumulate(canon_buf_t *cb,
                     const char *ring, volatile int *head, volatile int *tail,
                     int ring_size);

// Extract one complete line (ending '\n') into out (max size).
// Remainder stays buffered.  Returns bytes copied; 0 if no complete line.
int canon_read(canon_buf_t *cb, char *out, int size);

#endif
