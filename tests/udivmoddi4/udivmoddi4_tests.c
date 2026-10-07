// lib/compiler-rt/builtins/kvasir/udivmoddi4_udiv.c (the 64-bit division of the cores with a
// 32-bit divide instruction) against the host's own 64-bit division: every pair of a set of edge
// values, and random operands of every pair of bit lengths.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#define __udivmoddi4 kvasir_udivmoddi4_under_test
#include "../../lib/compiler-rt/builtins/kvasir/udivmoddi4_udiv.c"
#undef __udivmoddi4

static uint64_t state = 0x9E3779B97F4A7C15ULL;

static uint64_t next(void) {   // xorshift64*
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

static unsigned long checked;
static unsigned long failed;

static void check(uint64_t a,
                  uint64_t b) {
    du_int       r     = 0;
    du_int const q     = kvasir_udivmoddi4_under_test(a, b, &r);
    du_int const qOnly = kvasir_udivmoddi4_under_test(a, b, 0);
    ++checked;
    if(q != a / b || r != a % b || qOnly != q) {
        if(failed++ < 20) {
            printf("FAIL %016" PRIx64 " / %016" PRIx64 ": got q=%016" PRIx64 " r=%016" PRIx64
                   " (q alone %016" PRIx64 "), expected q=%016" PRIx64 " r=%016" PRIx64 "\n",
                   a,
                   b,
                   (uint64_t)q,
                   (uint64_t)r,
                   (uint64_t)qOnly,
                   a / b,
                   a % b);
        }
    }
}

int main(void) {
    // the values around every boundary the routine branches on: 16, 32, 48 and 64 bits
    uint64_t              edges[4 * 9 + 8];
    unsigned              n      = 0;
    static unsigned const bits[] = {16, 31, 32, 33, 48, 63};
    for(unsigned i = 0; i != sizeof bits / sizeof bits[0]; ++i) {
        uint64_t const p = 1ULL << bits[i];
        edges[n++]       = p - 2;
        edges[n++]       = p - 1;
        edges[n++]       = p;
        edges[n++]       = p + 1;
        edges[n++]       = p + 2;
    }
    static uint64_t const more[] = {1,
                                    2,
                                    3,
                                    7,
                                    10,
                                    1000,
                                    1000003,
                                    0xFFFFFFFFFFFFFFFFULL,
                                    0xFFFFFFFFFFFFFFFEULL,
                                    0x8000000080000000ULL,
                                    0x00000001FFFFFFFFULL,
                                    0xFFFFFFFF00000000ULL,
                                    0x0000FFFF0000FFFFULL,
                                    0x7FFFFFFFFFFFFFFFULL};
    for(unsigned i = 0; i != sizeof more / sizeof more[0]; ++i) { edges[n++] = more[i]; }
    for(unsigned i = 0; i != n; ++i) {
        check(0, edges[i]);
        for(unsigned j = 0; j != n; ++j) { check(edges[i], edges[j]); }
    }

    // random operands of every pair of bit lengths, 2000 per pair, and near-multiples of the
    // divisor (quotient estimates one off are the algorithm's hard cases)
    for(unsigned aBits = 1; aBits <= 64; ++aBits) {
        for(unsigned bBits = 1; bBits <= 64; ++bBits) {
            for(unsigned i = 0; i != 2000; ++i) {
                uint64_t const a = (next() >> (64 - aBits)) | (1ULL << (aBits - 1));
                uint64_t const b = (next() >> (64 - bBits)) | (1ULL << (bBits - 1));
                check(a, b);
                uint64_t const q = a / b;
                check(q * b, b);
                if(q * b != 0) { check(q * b - 1, b); }
                if(q * b + (b - 1) >= q * b) { check(q * b + (b - 1), b); }
            }
        }
    }

    printf("udivmoddi4: %lu divisions checked, %lu failed\n", checked, failed);
    return failed == 0 ? 0 : 1;
}
