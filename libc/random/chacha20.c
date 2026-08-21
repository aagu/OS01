#include <chacha20.h>

// Rotate left. `c` is a compile-time constant in every use site.
static inline uint32_t rotl32(uint32_t v, int c)
{
    return (v << c) | (v >> (32 - c));
}

// One quarter-round: a += b; d ^= a; d <<<= 16; c += d; b ^= c; b <<<= 12;
//                      a += b; d ^= a; d <<<= 8;  c += d; b ^= c; b <<<= 7;
#define QR(a, b, c, d) do {     \
    (a) += (b); (d) ^= (a); (d) = rotl32((d), 16); \
    (c) += (d); (b) ^= (c); (b) = rotl32((b), 12); \
    (a) += (b); (d) ^= (a); (d) = rotl32((d), 8);  \
    (c) += (d); (b) ^= (c); (b) = rotl32((b), 7);  \
} while (0)

static inline uint32_t load32_le(const uint8_t p[4])
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void store32_le(uint8_t p[4], uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64])
{
    // State layout (16 little-endian 32-bit words):
    //   s[0..3]   = constant "expand 32-byte k"
    //   s[4..11]  = key (8 words)
    //   s[12]     = counter
    //   s[13..15] = nonce (3 words)
    uint32_t s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e;
    s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        s[4 + i] = load32_le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = load32_le(nonce + 4 * i);

    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = s[i];

    for (int i = 0; i < 10; i++) {
        // Column rounds
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        // Diagonal rounds
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++)
        store32_le(out + 4 * i, x[i] + s[i]);
}
