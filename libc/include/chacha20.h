#ifndef _CHACHA20_H
#define _CHACHA20_H

#include <stdint.h>

// RFC 8439 ChaCha20 block function — stateless, no allocation, no globals.
// Generates one 64-byte block of keystream:
//   key     : 32-byte key
//   counter : 32-bit block counter (little-endian word 12 of the state)
//   nonce   : 12-byte nonce (three little-endian 32-bit words, state 13..15)
//   out     : 64 bytes of keystream written here
//
// The caller (kernel PRNG) is responsible for monotonic (counter, nonce)
// pairs so keystream never repeats.  All 32-bit words are serialized
// little-endian, per RFC 8439 §2.3.
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);

#endif // _CHACHA20_H
