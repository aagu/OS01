#include "udivti3_vectors.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

typedef union {
    unsigned __int128 value;
    struct { uint64_t lo, hi; } limb;
} u128_bits;

extern unsigned __int128 __udivti3(unsigned __int128, unsigned __int128);

static int check_vector(const struct udivti3_vector *v, size_t index)
{
    u128_bits n = { .limb = { v->n_lo, v->n_hi } };
    u128_bits d = { .limb = { v->d_lo, v->d_hi } };
    u128_bits q = { .value = __udivti3(n.value, d.value) };
    if (q.limb.lo != v->q_lo || q.limb.hi != v->q_hi) {
        fprintf(stderr, "vector %zu mismatch\n", index);
        return 0;
    }
    return 1;
}

int main(void)
{
    for (size_t i = 0; i < OS01_UDIVTI3_VECTOR_COUNT; ++i)
        if (!check_vector(&udivti3_vectors[i], i)) return 1;

    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        u128_bits n = { .limb = { 1, 0 } };
        u128_bits zero = { .value = 0 };
        (void)__udivti3(n.value, zero.value);
        _exit(0);
    }
    int status;
    if (waitpid(child, &status, 0) < 0) return 1;
    if (!WIFSIGNALED(status) && (!WIFEXITED(status) || WEXITSTATUS(status) == 0)) {
        fprintf(stderr, "zero divisor did not fail\n");
        return 1;
    }
    return 0;
}
