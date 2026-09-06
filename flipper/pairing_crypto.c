/* Implements flipper/pairing_crypto.h (frozen contract, do not change the header).
   docs/PLAN.md step 5: the Flipper firmware ABI exports no X25519/SHA-256/HMAC/HKDF (see
   docs/STANDALONE_FAP.md), so every primitive below is hand-rolled from spec, except
   X25519 itself, which is a faithful port of a small, audited reference implementation
   rather than an in-house derivation (field arithmetic over 2^255-19 has well-known
   correctness pitfalls that testing alone won't reliably catch).

   ---- X25519 port provenance ----
   The section between the "curve25519-donna port begins" and "...ends" markers below is
   ported from curve25519-donna.c (the portable/generic 32/64-bit build, not the
   64-bit-asm-tuned curve25519-donna-c64.c variant), by Adam Langley
   <agl@imperialviolet.org>, derived from public-domain code by Daniel J. Bernstein:
     https://github.com/agl/curve25519-donna/blob/master/curve25519-donna.c
     (commit referenced via the master branch, fetched 2026-09-03)
   License: 3-clause BSD, Copyright 2008 Google Inc., reproduced in full below as required
   by that license's redistribution terms.

     Copyright 2008, Google Inc.
     All rights reserved.

     Redistribution and use in source and binary forms, with or without
     modification, are permitted provided that the following conditions are
     met:

         * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
         * Redistributions in binary form must reproduce the above
     copyright notice, this list of conditions and the following disclaimer
     in the documentation and/or other materials provided with the
     distribution.
         * Neither the name of Google Inc. nor the names of its
     contributors may be used to endorse or promote products derived from
     this software without specific prior written permission.

     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
     A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
     OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
     SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
     LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
     DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
     THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
     (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
     OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   Porting notes (everything else in this file is this project's own code, not from
   curve25519-donna): the algorithm, structure, comments, and identifier names (including
   the deliberately terse `u8`/`s32`/`limb` typedefs and single/double-letter locals like
   `i`, `j`, `t0`, `t1`) are kept as close to upstream as possible on purpose, so this port
   can be diffed against the original source line-by-line for a correctness audit -- that
   auditability is worth more here than this project's usual naming conventions. The only
   changes made: renaming the public entry point `curve25519_donna` to the file-local
   static `x25519_donna_scalarmult` (avoids exporting a stray global symbol from a firmware
   binary that doesn't need one), and the pre-existing `_MSC_VER` `inline` shim (needed
   because this project's host test build uses MSVC's C compiler, which does not recognize
   C99 `inline` by default). No field-arithmetic logic was altered. */
#include "pairing_crypto.h"

#include <string.h>

/* ==================== curve25519-donna port begins ==================== */

#ifdef _MSC_VER
#define inline __inline
#endif

typedef uint8_t u8;
typedef int32_t s32;
typedef int64_t limb;

/* Sum two numbers: output += in */
static void fsum(limb* output, const limb* in) {
    unsigned i;
    for(i = 0; i < 10; i += 2) {
        output[0 + i] = output[0 + i] + in[0 + i];
        output[1 + i] = output[1 + i] + in[1 + i];
    }
}

/* Find the difference of two numbers: output = in - output
 * (note the order of the arguments!). */
static void fdifference(limb* output, const limb* in) {
    unsigned i;
    for(i = 0; i < 10; ++i) {
        output[i] = in[i] - output[i];
    }
}

/* Multiply a number by a scalar: output = in * scalar */
static void fscalar_product(limb* output, const limb* in, const limb scalar) {
    unsigned i;
    for(i = 0; i < 10; ++i) {
        output[i] = in[i] * scalar;
    }
}

/* Multiply two numbers: output = in2 * in
 *
 * output must be distinct to both inputs. The inputs are reduced coefficient
 * form, the output is not.
 *
 * output[x] <= 14 * the largest product of the input limbs. */
static void fproduct(limb* output, const limb* in2, const limb* in) {
    output[0] = ((limb)((s32)in2[0])) * ((s32)in[0]);
    output[1] = ((limb)((s32)in2[0])) * ((s32)in[1]) + ((limb)((s32)in2[1])) * ((s32)in[0]);
    output[2] = 2 * ((limb)((s32)in2[1])) * ((s32)in[1]) +
                ((limb)((s32)in2[0])) * ((s32)in[2]) + ((limb)((s32)in2[2])) * ((s32)in[0]);
    output[3] = ((limb)((s32)in2[1])) * ((s32)in[2]) + ((limb)((s32)in2[2])) * ((s32)in[1]) +
                ((limb)((s32)in2[0])) * ((s32)in[3]) + ((limb)((s32)in2[3])) * ((s32)in[0]);
    output[4] = ((limb)((s32)in2[2])) * ((s32)in[2]) +
                2 * (((limb)((s32)in2[1])) * ((s32)in[3]) + ((limb)((s32)in2[3])) * ((s32)in[1])) +
                ((limb)((s32)in2[0])) * ((s32)in[4]) + ((limb)((s32)in2[4])) * ((s32)in[0]);
    output[5] = ((limb)((s32)in2[2])) * ((s32)in[3]) + ((limb)((s32)in2[3])) * ((s32)in[2]) +
                ((limb)((s32)in2[1])) * ((s32)in[4]) + ((limb)((s32)in2[4])) * ((s32)in[1]) +
                ((limb)((s32)in2[0])) * ((s32)in[5]) + ((limb)((s32)in2[5])) * ((s32)in[0]);
    output[6] = 2 * (((limb)((s32)in2[3])) * ((s32)in[3]) + ((limb)((s32)in2[1])) * ((s32)in[5]) +
                     ((limb)((s32)in2[5])) * ((s32)in[1])) +
                ((limb)((s32)in2[2])) * ((s32)in[4]) + ((limb)((s32)in2[4])) * ((s32)in[2]) +
                ((limb)((s32)in2[0])) * ((s32)in[6]) + ((limb)((s32)in2[6])) * ((s32)in[0]);
    output[7] = ((limb)((s32)in2[3])) * ((s32)in[4]) + ((limb)((s32)in2[4])) * ((s32)in[3]) +
                ((limb)((s32)in2[2])) * ((s32)in[5]) + ((limb)((s32)in2[5])) * ((s32)in[2]) +
                ((limb)((s32)in2[1])) * ((s32)in[6]) + ((limb)((s32)in2[6])) * ((s32)in[1]) +
                ((limb)((s32)in2[0])) * ((s32)in[7]) + ((limb)((s32)in2[7])) * ((s32)in[0]);
    output[8] = ((limb)((s32)in2[4])) * ((s32)in[4]) +
                2 * (((limb)((s32)in2[3])) * ((s32)in[5]) + ((limb)((s32)in2[5])) * ((s32)in[3]) +
                     ((limb)((s32)in2[1])) * ((s32)in[7]) + ((limb)((s32)in2[7])) * ((s32)in[1])) +
                ((limb)((s32)in2[2])) * ((s32)in[6]) + ((limb)((s32)in2[6])) * ((s32)in[2]) +
                ((limb)((s32)in2[0])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[0]);
    output[9] = ((limb)((s32)in2[4])) * ((s32)in[5]) + ((limb)((s32)in2[5])) * ((s32)in[4]) +
                ((limb)((s32)in2[3])) * ((s32)in[6]) + ((limb)((s32)in2[6])) * ((s32)in[3]) +
                ((limb)((s32)in2[2])) * ((s32)in[7]) + ((limb)((s32)in2[7])) * ((s32)in[2]) +
                ((limb)((s32)in2[1])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[1]) +
                ((limb)((s32)in2[0])) * ((s32)in[9]) + ((limb)((s32)in2[9])) * ((s32)in[0]);
    output[10] = 2 * (((limb)((s32)in2[5])) * ((s32)in[5]) + ((limb)((s32)in2[3])) * ((s32)in[7]) +
                      ((limb)((s32)in2[7])) * ((s32)in[3]) + ((limb)((s32)in2[1])) * ((s32)in[9]) +
                      ((limb)((s32)in2[9])) * ((s32)in[1])) +
                 ((limb)((s32)in2[4])) * ((s32)in[6]) + ((limb)((s32)in2[6])) * ((s32)in[4]) +
                 ((limb)((s32)in2[2])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[2]);
    output[11] = ((limb)((s32)in2[5])) * ((s32)in[6]) + ((limb)((s32)in2[6])) * ((s32)in[5]) +
                 ((limb)((s32)in2[4])) * ((s32)in[7]) + ((limb)((s32)in2[7])) * ((s32)in[4]) +
                 ((limb)((s32)in2[3])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[3]) +
                 ((limb)((s32)in2[2])) * ((s32)in[9]) + ((limb)((s32)in2[9])) * ((s32)in[2]);
    output[12] = ((limb)((s32)in2[6])) * ((s32)in[6]) +
                 2 * (((limb)((s32)in2[5])) * ((s32)in[7]) + ((limb)((s32)in2[7])) * ((s32)in[5]) +
                      ((limb)((s32)in2[3])) * ((s32)in[9]) + ((limb)((s32)in2[9])) * ((s32)in[3])) +
                 ((limb)((s32)in2[4])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[4]);
    output[13] = ((limb)((s32)in2[6])) * ((s32)in[7]) + ((limb)((s32)in2[7])) * ((s32)in[6]) +
                 ((limb)((s32)in2[5])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[5]) +
                 ((limb)((s32)in2[4])) * ((s32)in[9]) + ((limb)((s32)in2[9])) * ((s32)in[4]);
    output[14] = 2 * (((limb)((s32)in2[7])) * ((s32)in[7]) + ((limb)((s32)in2[5])) * ((s32)in[9]) +
                      ((limb)((s32)in2[9])) * ((s32)in[5])) +
                 ((limb)((s32)in2[6])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[6]);
    output[15] = ((limb)((s32)in2[7])) * ((s32)in[8]) + ((limb)((s32)in2[8])) * ((s32)in[7]) +
                 ((limb)((s32)in2[6])) * ((s32)in[9]) + ((limb)((s32)in2[9])) * ((s32)in[6]);
    output[16] = ((limb)((s32)in2[8])) * ((s32)in[8]) +
                 2 * (((limb)((s32)in2[7])) * ((s32)in[9]) + ((limb)((s32)in2[9])) * ((s32)in[7]));
    output[17] = ((limb)((s32)in2[8])) * ((s32)in[9]) + ((limb)((s32)in2[9])) * ((s32)in[8]);
    output[18] = 2 * ((limb)((s32)in2[9])) * ((s32)in[9]);
}

/* Reduce a long form to a short form by taking the input mod 2^255 - 19.
 *
 * On entry: |output[i]| < 14*2^54
 * On exit: |output[0..8]| < 280*2^54 */
static void freduce_degree(limb* output) {
    output[8] += output[18] << 4;
    output[8] += output[18] << 1;
    output[8] += output[18];
    output[7] += output[17] << 4;
    output[7] += output[17] << 1;
    output[7] += output[17];
    output[6] += output[16] << 4;
    output[6] += output[16] << 1;
    output[6] += output[16];
    output[5] += output[15] << 4;
    output[5] += output[15] << 1;
    output[5] += output[15];
    output[4] += output[14] << 4;
    output[4] += output[14] << 1;
    output[4] += output[14];
    output[3] += output[13] << 4;
    output[3] += output[13] << 1;
    output[3] += output[13];
    output[2] += output[12] << 4;
    output[2] += output[12] << 1;
    output[2] += output[12];
    output[1] += output[11] << 4;
    output[1] += output[11] << 1;
    output[1] += output[11];
    output[0] += output[10] << 4;
    output[0] += output[10] << 1;
    output[0] += output[10];
}

#if (-1 & 3) != 3
#error "This code only works on a two's complement system"
#endif

/* return v / 2^26, using only shifts and adds.
 *
 * On entry: v can take any value. */
static inline limb div_by_2_26(const limb v) {
    const uint32_t highword = (uint32_t)(((uint64_t)v) >> 32);
    const int32_t sign = ((int32_t)highword) >> 31;
    const int32_t roundoff = ((uint32_t)sign) >> 6;
    return (v + roundoff) >> 26;
}

/* return v / (2^25), using only shifts and adds.
 *
 * On entry: v can take any value. */
static inline limb div_by_2_25(const limb v) {
    const uint32_t highword = (uint32_t)(((uint64_t)v) >> 32);
    const int32_t sign = ((int32_t)highword) >> 31;
    const int32_t roundoff = ((uint32_t)sign) >> 7;
    return (v + roundoff) >> 25;
}

/* Reduce all coefficients of the short form input so that |x| < 2^26.
 *
 * On entry: |output[i]| < 280*2^54 */
static void freduce_coefficients(limb* output) {
    unsigned i;

    output[10] = 0;

    for(i = 0; i < 10; i += 2) {
        limb over = div_by_2_26(output[i]);
        output[i] -= over << 26;
        output[i + 1] += over;

        over = div_by_2_25(output[i + 1]);
        output[i + 1] -= over << 25;
        output[i + 2] += over;
    }
    output[0] += output[10] << 4;
    output[0] += output[10] << 1;
    output[0] += output[10];

    output[10] = 0;

    {
        limb over = div_by_2_26(output[0]);
        output[0] -= over << 26;
        output[1] += over;
    }
}

/* A helpful wrapper around fproduct: output = in * in2.
 *
 * On entry: |in[i]| < 2^27 and |in2[i]| < 2^27.
 *
 * output must be distinct to both inputs. The output is reduced degree
 * (indeed, one need only provide storage for 10 limbs) and |output[i]| < 2^26. */
static void fmul(limb* output, const limb* in, const limb* in2) {
    /* static, not stack-local: this file's whole X25519 ladder runs on the Flipper's
       1280-byte BleEventWorker thread (see cmult()/fmonty() below for the dominant
       offenders); every scratch buffer in the ladder is moved off that stack. Safe
       because BLE event dispatch is synchronous/single-in-flight, so there is never a
       concurrent or reentrant call into fmul(). Always fully written by fproduct()
       before use, so no cross-call stale-value hazard. */
    static limb t[19];
    fproduct(t, in, in2);
    freduce_degree(t);
    freduce_coefficients(t);
    memcpy(output, t, sizeof(limb) * 10);
}

/* Square a number: output = in**2
 *
 * output must be distinct from the input. The inputs are reduced coefficient
 * form, the output is not.
 *
 * output[x] <= 14 * the largest product of the input limbs. */
static void fsquare_inner(limb* output, const limb* in) {
    output[0] = ((limb)((s32)in[0])) * ((s32)in[0]);
    output[1] = 2 * ((limb)((s32)in[0])) * ((s32)in[1]);
    output[2] = 2 * (((limb)((s32)in[1])) * ((s32)in[1]) + ((limb)((s32)in[0])) * ((s32)in[2]));
    output[3] = 2 * (((limb)((s32)in[1])) * ((s32)in[2]) + ((limb)((s32)in[0])) * ((s32)in[3]));
    output[4] = ((limb)((s32)in[2])) * ((s32)in[2]) + 4 * ((limb)((s32)in[1])) * ((s32)in[3]) +
                2 * ((limb)((s32)in[0])) * ((s32)in[4]);
    output[5] = 2 * (((limb)((s32)in[2])) * ((s32)in[3]) + ((limb)((s32)in[1])) * ((s32)in[4]) +
                     ((limb)((s32)in[0])) * ((s32)in[5]));
    output[6] = 2 * (((limb)((s32)in[3])) * ((s32)in[3]) + ((limb)((s32)in[2])) * ((s32)in[4]) +
                     ((limb)((s32)in[0])) * ((s32)in[6]) + 2 * ((limb)((s32)in[1])) * ((s32)in[5]));
    output[7] = 2 * (((limb)((s32)in[3])) * ((s32)in[4]) + ((limb)((s32)in[2])) * ((s32)in[5]) +
                     ((limb)((s32)in[1])) * ((s32)in[6]) + ((limb)((s32)in[0])) * ((s32)in[7]));
    output[8] = ((limb)((s32)in[4])) * ((s32)in[4]) +
                2 * (((limb)((s32)in[2])) * ((s32)in[6]) + ((limb)((s32)in[0])) * ((s32)in[8]) +
                     2 * (((limb)((s32)in[1])) * ((s32)in[7]) + ((limb)((s32)in[3])) * ((s32)in[5])));
    output[9] = 2 * (((limb)((s32)in[4])) * ((s32)in[5]) + ((limb)((s32)in[3])) * ((s32)in[6]) +
                     ((limb)((s32)in[2])) * ((s32)in[7]) + ((limb)((s32)in[1])) * ((s32)in[8]) +
                     ((limb)((s32)in[0])) * ((s32)in[9]));
    output[10] = 2 * (((limb)((s32)in[5])) * ((s32)in[5]) + ((limb)((s32)in[4])) * ((s32)in[6]) +
                      ((limb)((s32)in[2])) * ((s32)in[8]) +
                      2 * (((limb)((s32)in[3])) * ((s32)in[7]) + ((limb)((s32)in[1])) * ((s32)in[9])));
    output[11] = 2 * (((limb)((s32)in[5])) * ((s32)in[6]) + ((limb)((s32)in[4])) * ((s32)in[7]) +
                      ((limb)((s32)in[3])) * ((s32)in[8]) + ((limb)((s32)in[2])) * ((s32)in[9]));
    output[12] = ((limb)((s32)in[6])) * ((s32)in[6]) +
                 2 * (((limb)((s32)in[4])) * ((s32)in[8]) +
                      2 * (((limb)((s32)in[5])) * ((s32)in[7]) + ((limb)((s32)in[3])) * ((s32)in[9])));
    output[13] = 2 * (((limb)((s32)in[6])) * ((s32)in[7]) + ((limb)((s32)in[5])) * ((s32)in[8]) +
                      ((limb)((s32)in[4])) * ((s32)in[9]));
    output[14] = 2 * (((limb)((s32)in[7])) * ((s32)in[7]) + ((limb)((s32)in[6])) * ((s32)in[8]) +
                      2 * ((limb)((s32)in[5])) * ((s32)in[9]));
    output[15] = 2 * (((limb)((s32)in[7])) * ((s32)in[8]) + ((limb)((s32)in[6])) * ((s32)in[9]));
    output[16] =
        ((limb)((s32)in[8])) * ((s32)in[8]) + 4 * ((limb)((s32)in[7])) * ((s32)in[9]);
    output[17] = 2 * ((limb)((s32)in[8])) * ((s32)in[9]);
    output[18] = 2 * ((limb)((s32)in[9])) * ((s32)in[9]);
}

/* fsquare sets output = in^2.
 *
 * On entry: The |in| argument is in reduced coefficients form and |in[i]| <
 * 2^27.
 *
 * On exit: The |output| argument is in reduced coefficients form (indeed, one
 * need only provide storage for 10 limbs) and |out[i]| < 2^26. */
static void fsquare(limb* output, const limb* in) {
    /* static: see fmul()'s comment above -- same BleEventWorker stack-budget rationale,
       same single-in-flight safety argument, always fully written before use. */
    static limb t[19];
    fsquare_inner(t, in);
    freduce_degree(t);
    freduce_coefficients(t);
    memcpy(output, t, sizeof(limb) * 10);
}

/* Take a little-endian, 32-byte number and expand it into polynomial form */
static void fexpand(limb* output, const u8* input) {
#define F(n, start, shift, mask)                                                        \
    output[n] = ((((limb)input[start + 0]) | ((limb)input[start + 1]) << 8 |             \
                  ((limb)input[start + 2]) << 16 | ((limb)input[start + 3]) << 24) >>     \
                 shift) &                                                                \
                mask;
    F(0, 0, 0, 0x3ffffff);
    F(1, 3, 2, 0x1ffffff);
    F(2, 6, 3, 0x3ffffff);
    F(3, 9, 5, 0x1ffffff);
    F(4, 12, 6, 0x3ffffff);
    F(5, 16, 0, 0x1ffffff);
    F(6, 19, 1, 0x3ffffff);
    F(7, 22, 3, 0x1ffffff);
    F(8, 25, 4, 0x3ffffff);
    F(9, 28, 6, 0x1ffffff);
#undef F
}

#if (-32 >> 1) != -16
#error "This code only works when >> does sign-extension on negative numbers"
#endif

/* s32_eq returns 0xffffffff iff a == b and zero otherwise. */
static s32 s32_eq(s32 a, s32 b) {
    a = ~(a ^ b);
    a &= a << 16;
    a &= a << 8;
    a &= a << 4;
    a &= a << 2;
    a &= a << 1;
    return a >> 31;
}

/* s32_gte returns 0xffffffff if a >= b and zero otherwise, where a and b are
 * both non-negative. */
static s32 s32_gte(s32 a, s32 b) {
    a -= b;
    return ~(a >> 31);
}

/* Take a fully reduced polynomial form number and contract it into a
 * little-endian, 32-byte array.
 *
 * On entry: |input_limbs[i]| < 2^26 */
static void fcontract(u8* output, limb* input_limbs) {
    int i;
    int j;
    s32 input[10];
    s32 mask;

    for(i = 0; i < 10; i++) {
        input[i] = (s32)input_limbs[i];
    }

    for(j = 0; j < 2; ++j) {
        for(i = 0; i < 9; ++i) {
            if((i & 1) == 1) {
                const s32 m = input[i] >> 31;
                const s32 carry = -((input[i] & m) >> 25);
                input[i] = input[i] + (carry << 25);
                input[i + 1] = input[i + 1] - carry;
            } else {
                const s32 m = input[i] >> 31;
                const s32 carry = -((input[i] & m) >> 26);
                input[i] = input[i] + (carry << 26);
                input[i + 1] = input[i + 1] - carry;
            }
        }

        {
            const s32 m = input[9] >> 31;
            const s32 carry = -((input[9] & m) >> 25);
            input[9] = input[9] + (carry << 25);
            input[0] = input[0] - (carry * 19);
        }
    }

    {
        const s32 m = input[0] >> 31;
        const s32 carry = -((input[0] & m) >> 26);
        input[0] = input[0] + (carry << 26);
        input[1] = input[1] - carry;
    }

    for(j = 0; j < 2; j++) {
        for(i = 0; i < 9; i++) {
            if((i & 1) == 1) {
                const s32 carry = input[i] >> 25;
                input[i] &= 0x1ffffff;
                input[i + 1] += carry;
            } else {
                const s32 carry = input[i] >> 26;
                input[i] &= 0x3ffffff;
                input[i + 1] += carry;
            }
        }

        {
            const s32 carry = input[9] >> 25;
            input[9] &= 0x1ffffff;
            input[0] += 19 * carry;
        }
    }

    mask = s32_gte(input[0], 0x3ffffed);
    for(i = 1; i < 10; i++) {
        if((i & 1) == 1) {
            mask &= s32_eq(input[i], 0x1ffffff);
        } else {
            mask &= s32_eq(input[i], 0x3ffffff);
        }
    }

    input[0] -= mask & 0x3ffffed;

    for(i = 1; i < 10; i++) {
        if((i & 1) == 1) {
            input[i] -= mask & 0x1ffffff;
        } else {
            input[i] -= mask & 0x3ffffff;
        }
    }

    input[1] <<= 2;
    input[2] <<= 3;
    input[3] <<= 5;
    input[4] <<= 6;
    input[6] <<= 1;
    input[7] <<= 3;
    input[8] <<= 4;
    input[9] <<= 6;
#define F(idx, s)                                    \
    output[s + 0] |= input[idx] & 0xff;               \
    output[s + 1] = (input[idx] >> 8) & 0xff;         \
    output[s + 2] = (input[idx] >> 16) & 0xff;        \
    output[s + 3] = (input[idx] >> 24) & 0xff;
    output[0] = 0;
    output[16] = 0;
    F(0, 0);
    F(1, 3);
    F(2, 6);
    F(3, 9);
    F(4, 12);
    F(5, 16);
    F(6, 19);
    F(7, 22);
    F(8, 25);
    F(9, 28);
#undef F
}

/* Input: Q, Q', Q-Q'
 * Output: 2Q, Q+Q'
 *
 *   x2 z3: long form
 *   x3 z3: long form
 *   x z: short form, destroyed
 *   xprime zprime: short form, destroyed
 *   qmqp: short form, preserved
 *
 * On entry and exit, the absolute value of the limbs of all inputs and outputs
 * are < 2^26. */
static void fmonty(
    limb* x2,
    limb* z2, /* output 2Q */
    limb* x3,
    limb* z3, /* output Q + Q' */
    limb* x,
    limb* z, /* input Q */
    limb* xprime,
    limb* zprime, /* input Q' */
    const limb* qmqp /* input Q - Q' */) {
    /* static, not stack-local: this is the single largest stack-overflow offender in the
       X25519 port -- 9 arrays (153 limbs, ~1224 bytes) in one frame, called from inside
       cmult()'s 256-iteration ladder loop so it stacks on top of cmult()'s own ~1216-byte
       frame (2.4 KB combined) against the Flipper's 1280-byte BleEventWorker stack
       (docs/SESSION_MEMORY.md's stack-overflow root cause). Safe as static: BLE event
       dispatch is synchronous/single-in-flight, fmonty() is not reentrant or recursive,
       and every array here is fully overwritten (memcpy/fsum/fproduct/fsquare) before
       being read on each call -- no cross-call stale-value hazard. */
    static limb origx[10], origxprime[10], zzz[19], xx[19], zz[19], xxprime[19], zzprime[19],
        zzzprime[19], xxxprime[19];

    memcpy(origx, x, 10 * sizeof(limb));
    fsum(x, z);
    fdifference(z, origx); /* does x - z */

    memcpy(origxprime, xprime, sizeof(limb) * 10);
    fsum(xprime, zprime);
    fdifference(zprime, origxprime);
    fproduct(xxprime, xprime, z);
    fproduct(zzprime, x, zprime);
    freduce_degree(xxprime);
    freduce_coefficients(xxprime);
    freduce_degree(zzprime);
    freduce_coefficients(zzprime);
    memcpy(origxprime, xxprime, sizeof(limb) * 10);
    fsum(xxprime, zzprime);
    fdifference(zzprime, origxprime);
    fsquare(xxxprime, xxprime);
    fsquare(zzzprime, zzprime);
    fproduct(zzprime, zzzprime, qmqp);
    freduce_degree(zzprime);
    freduce_coefficients(zzprime);
    memcpy(x3, xxxprime, sizeof(limb) * 10);
    memcpy(z3, zzprime, sizeof(limb) * 10);

    fsquare(xx, x);
    fsquare(zz, z);
    fproduct(x2, xx, zz);
    freduce_degree(x2);
    freduce_coefficients(x2);
    fdifference(zz, xx); /* does zz = xx - zz */
    memset(zzz + 10, 0, sizeof(limb) * 9);
    fscalar_product(zzz, zz, 121665);
    freduce_coefficients(zzz);
    fsum(zzz, xx);
    fproduct(z2, zz, zzz);
    freduce_degree(z2);
    freduce_coefficients(z2);
}

/* Conditionally swap two reduced-form limb arrays if 'iswap' is 1, but leave
 * them unchanged if 'iswap' is 0.  Runs in data-invariant time to avoid
 * side-channel attacks.
 *
 * NOTE that this function requires that 'iswap' be 1 or 0; other values give
 * wrong results.  Also, the two limb arrays must be in reduced-coefficient,
 * reduced-degree form: the values in a[10..19] or b[10..19] aren't swapped,
 * and all all values in a[0..9],b[0..9] must have magnitude less than
 * INT32_MAX. */
static void swap_conditional(limb a[19], limb b[19], limb iswap) {
    unsigned i;
    const s32 swap = (s32)-iswap;

    for(i = 0; i < 10; ++i) {
        const s32 x = swap & (((s32)a[i]) ^ ((s32)b[i]));
        a[i] = ((s32)a[i]) ^ x;
        b[i] = ((s32)b[i]) ^ x;
    }
}

/* Calculates nQ where Q is the x-coordinate of a point on the curve
 *
 *   resultx/resultz: the x coordinate of the resulting curve point (short form)
 *   n: a little endian, 32-byte number
 *   q: a point of the curve (short form) */
static void cmult(limb* resultx, limb* resultz, const u8* n, const limb* q) {
    /* static, not stack-local: the single largest offender here (8 arrays, 152 limbs,
       ~1216 bytes in one frame) -- called once per feb_x25519()/feb_x25519_base(), and
       calls fmonty() (also static, ~1224 bytes) 256 times from its inner loop, so the two
       frames stack to ~2.4 KB combined against the Flipper's 1280-byte BleEventWorker
       stack (docs/SESSION_MEMORY.md's stack-overflow root cause) -- more than the entire
       thread stack on its own. Safe as static: BLE event dispatch is synchronous/
       single-in-flight, cmult() is not reentrant or recursive. Unlike fmonty()'s arrays,
       these carry an initial value (a/c/e/g = 0, b/d/f/h = 1) that a `static` initializer
       would only apply once at startup, not on every call -- so the reset below is done
       explicitly at the top of every call instead, preserving the original per-call
       semantics. */
    static limb a[19], b[19], c[19], d[19], e[19], f[19], g[19], h[19];
    limb *nqpqx = a, *nqpqz = b, *nqx = c, *nqz = d, *t;
    limb *nqpqx2 = e, *nqpqz2 = f, *nqx2 = g, *nqz2 = h;

    unsigned i, j;

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    b[0] = 1;
    memset(c, 0, sizeof(c));
    c[0] = 1;
    memset(d, 0, sizeof(d));
    memset(e, 0, sizeof(e));
    memset(f, 0, sizeof(f));
    f[0] = 1;
    memset(g, 0, sizeof(g));
    memset(h, 0, sizeof(h));
    h[0] = 1;

    memcpy(nqpqx, q, sizeof(limb) * 10);

    for(i = 0; i < 32; ++i) {
        u8 byte = n[31 - i];
        for(j = 0; j < 8; ++j) {
            const limb bit = byte >> 7;

            swap_conditional(nqx, nqpqx, bit);
            swap_conditional(nqz, nqpqz, bit);
            fmonty(nqx2, nqz2, nqpqx2, nqpqz2, nqx, nqz, nqpqx, nqpqz, q);
            swap_conditional(nqx2, nqpqx2, bit);
            swap_conditional(nqz2, nqpqz2, bit);

            t = nqx;
            nqx = nqx2;
            nqx2 = t;
            t = nqz;
            nqz = nqz2;
            nqz2 = t;
            t = nqpqx;
            nqpqx = nqpqx2;
            nqpqx2 = t;
            t = nqpqz;
            nqpqz = nqpqz2;
            nqpqz2 = t;

            byte <<= 1;
        }
    }

    memcpy(resultx, nqx, sizeof(limb) * 10);
    memcpy(resultz, nqz, sizeof(limb) * 10);
}

/* -----------------------------------------------------------------------------
 * Shamelessly copied from djb's code
 * ----------------------------------------------------------------------------- */
static void crecip(limb* out, const limb* z) {
    /* static, not stack-local: 10 arrays (~800 bytes in one frame) against the Flipper's
       1280-byte BleEventWorker stack -- same rationale as fmonty()/cmult() above. Safe as
       static: single-in-flight BLE event dispatch, not reentrant/recursive, and every
       array is fully written (fsquare/fmul) before being read on each call. */
    static limb z2[10];
    static limb z9[10];
    static limb z11[10];
    static limb z2_5_0[10];
    static limb z2_10_0[10];
    static limb z2_20_0[10];
    static limb z2_50_0[10];
    static limb z2_100_0[10];
    static limb t0[10];
    static limb t1[10];
    int i;

    /* 2 */ fsquare(z2, z);
    /* 4 */ fsquare(t1, z2);
    /* 8 */ fsquare(t0, t1);
    /* 9 */ fmul(z9, t0, z);
    /* 11 */ fmul(z11, z9, z2);
    /* 22 */ fsquare(t0, z11);
    /* 2^5 - 2^0 = 31 */ fmul(z2_5_0, t0, z9);

    /* 2^6 - 2^1 */ fsquare(t0, z2_5_0);
    /* 2^7 - 2^2 */ fsquare(t1, t0);
    /* 2^8 - 2^3 */ fsquare(t0, t1);
    /* 2^9 - 2^4 */ fsquare(t1, t0);
    /* 2^10 - 2^5 */ fsquare(t0, t1);
    /* 2^10 - 2^0 */ fmul(z2_10_0, t0, z2_5_0);

    /* 2^11 - 2^1 */ fsquare(t0, z2_10_0);
    /* 2^12 - 2^2 */ fsquare(t1, t0);
    /* 2^20 - 2^10 */ for(i = 2; i < 10; i += 2) {
        fsquare(t0, t1);
        fsquare(t1, t0);
    }
    /* 2^20 - 2^0 */ fmul(z2_20_0, t1, z2_10_0);

    /* 2^21 - 2^1 */ fsquare(t0, z2_20_0);
    /* 2^22 - 2^2 */ fsquare(t1, t0);
    /* 2^40 - 2^20 */ for(i = 2; i < 20; i += 2) {
        fsquare(t0, t1);
        fsquare(t1, t0);
    }
    /* 2^40 - 2^0 */ fmul(t0, t1, z2_20_0);

    /* 2^41 - 2^1 */ fsquare(t1, t0);
    /* 2^42 - 2^2 */ fsquare(t0, t1);
    /* 2^50 - 2^10 */ for(i = 2; i < 10; i += 2) {
        fsquare(t1, t0);
        fsquare(t0, t1);
    }
    /* 2^50 - 2^0 */ fmul(z2_50_0, t0, z2_10_0);

    /* 2^51 - 2^1 */ fsquare(t0, z2_50_0);
    /* 2^52 - 2^2 */ fsquare(t1, t0);
    /* 2^100 - 2^50 */ for(i = 2; i < 50; i += 2) {
        fsquare(t0, t1);
        fsquare(t1, t0);
    }
    /* 2^100 - 2^0 */ fmul(z2_100_0, t1, z2_50_0);

    /* 2^101 - 2^1 */ fsquare(t1, z2_100_0);
    /* 2^102 - 2^2 */ fsquare(t0, t1);
    /* 2^200 - 2^100 */ for(i = 2; i < 100; i += 2) {
        fsquare(t1, t0);
        fsquare(t0, t1);
    }
    /* 2^200 - 2^0 */ fmul(t1, t0, z2_100_0);

    /* 2^201 - 2^1 */ fsquare(t0, t1);
    /* 2^202 - 2^2 */ fsquare(t1, t0);
    /* 2^250 - 2^50 */ for(i = 2; i < 50; i += 2) {
        fsquare(t0, t1);
        fsquare(t1, t0);
    }
    /* 2^250 - 2^0 */ fmul(t0, t1, z2_50_0);

    /* 2^251 - 2^1 */ fsquare(t1, t0);
    /* 2^252 - 2^2 */ fsquare(t0, t1);
    /* 2^253 - 2^3 */ fsquare(t1, t0);
    /* 2^254 - 2^4 */ fsquare(t0, t1);
    /* 2^255 - 2^5 */ fsquare(t1, t0);
    /* 2^255 - 21 */ fmul(out, t1, z11);
}

/* Renamed from upstream's `curve25519_donna` -- kept file-local (static) since this
   firmware only needs it behind feb_x25519()/feb_x25519_base(). Applies RFC 7748's
   decodeScalar25519 clamping to a local copy of `secret`; never mutates the caller's
   buffer. */
static int x25519_donna_scalarmult(u8* mypublic, const u8* secret, const u8* basepoint) {
    /* static, not stack-local: same BleEventWorker stack-budget rationale as
       cmult()/fmonty()/crecip() above -- this frame (~360 bytes) is the outermost one in
       the ladder, so it's always present underneath whichever of those is active. Safe as
       static: single-in-flight BLE event dispatch, not reentrant/recursive; all of
       bp/x/z/zmone are fully written by fexpand()/cmult()/crecip()/fmul() before being
       read, and `e` is fully written by the clamping loop below before use. */
    static limb bp[10], x[10], z[11], zmone[10];
    static uint8_t e[32];
    int i;

    for(i = 0; i < 32; ++i) e[i] = secret[i];
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fexpand(bp, basepoint);
    cmult(x, z, e, bp);
    crecip(zmone, z);
    fmul(z, x, zmone);
    fcontract(mypublic, z);

    feb_secure_zero(e, sizeof(e));
    feb_secure_zero(bp, sizeof(bp));
    feb_secure_zero(x, sizeof(x));
    feb_secure_zero(z, sizeof(z));
    feb_secure_zero(zmone, sizeof(zmone));
    return 0;
}

/* ==================== curve25519-donna port ends ==================== */

void feb_x25519(
    uint8_t out[FEB_X25519_KEY_LEN],
    const uint8_t scalar[FEB_X25519_KEY_LEN],
    const uint8_t point[FEB_X25519_KEY_LEN]) {
    (void)x25519_donna_scalarmult(out, scalar, point);
}

void feb_x25519_base(uint8_t out[FEB_X25519_KEY_LEN], const uint8_t scalar[FEB_X25519_KEY_LEN]) {
    static const uint8_t basepoint[FEB_X25519_KEY_LEN] = {9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                            0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    (void)x25519_donna_scalarmult(out, scalar, basepoint);
}

void feb_x25519_keypair(
    uint8_t out_private[FEB_X25519_KEY_LEN],
    uint8_t out_public[FEB_X25519_KEY_LEN],
    const uint8_t random_32[FEB_X25519_KEY_LEN]) {
    memcpy(out_private, random_32, FEB_X25519_KEY_LEN);
    feb_x25519_base(out_public, out_private);
}

int feb_is_all_zero(const uint8_t* data, size_t len) {
    uint8_t acc = 0;
    size_t i;
    for(i = 0; i < len; i++) {
        acc |= data[i];
    }
    return acc == 0;
}

int feb_consttime_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    size_t i;
    if(len == 0) {
        return 0;
    }
    for(i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

void feb_secure_zero(void* buf, size_t len) {
    volatile uint8_t* p = (volatile uint8_t*)buf;
    size_t i;
    if(buf == NULL) {
        return;
    }
    for(i = 0; i < len; i++) {
        p[i] = 0;
    }
}

/* ==================== SHA-256 (FIPS 180-4) ==================== */

#define SHA256_BLOCK_LEN 64u

typedef struct {
    uint32_t state[8];
    uint8_t buffer[SHA256_BLOCK_LEN];
    size_t buffer_len;
    uint64_t total_len;
} sha256_ctx_t;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32u - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_BSIG0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_BSIG1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SSIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SSIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static void sha256_compress(uint32_t state[8], const uint8_t block[SHA256_BLOCK_LEN]) {
    /* static, not stack-local: reachable from handle_pair_init/confirm/complete via
       feb_pairing_derive_kconfirm/feb_pairing_derive_secret/feb_hmac_sha256, all on the
       Flipper's 1280-byte BleEventWorker stack (docs/SESSION_MEMORY.md's stack-overflow
       root cause). Safe as static: single-in-flight BLE event dispatch, not
       reentrant/recursive, and fully written by the loop below before use. */
    static uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    size_t i;

    for(i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | ((uint32_t)block[4 * i + 3]);
    }
    for(i = 16; i < 64; i++) {
        w[i] = SHA256_SSIG1(w[i - 2]) + w[i - 7] + SHA256_SSIG0(w[i - 15]) + w[i - 16];
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for(i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_BSIG1(e) + SHA256_CH(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = SHA256_BSIG0(a) + SHA256_MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;

    feb_secure_zero(w, sizeof(w));
}

static void sha256_init(sha256_ctx_t* ctx) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(ctx->state, iv, sizeof(iv));
    ctx->buffer_len = 0;
    ctx->total_len = 0;
}

static void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    ctx->total_len += (uint64_t)len;

    if(ctx->buffer_len > 0) {
        size_t fill = SHA256_BLOCK_LEN - ctx->buffer_len;
        if(fill > len) {
            fill = len;
        }
        memcpy(ctx->buffer + ctx->buffer_len, data, fill);
        ctx->buffer_len += fill;
        data += fill;
        len -= fill;
        if(ctx->buffer_len == SHA256_BLOCK_LEN) {
            sha256_compress(ctx->state, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }

    while(len >= SHA256_BLOCK_LEN) {
        sha256_compress(ctx->state, data);
        data += SHA256_BLOCK_LEN;
        len -= SHA256_BLOCK_LEN;
    }

    if(len > 0) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

static void sha256_final(sha256_ctx_t* ctx, uint8_t out[FEB_SHA256_LEN]) {
    /* static: same BleEventWorker stack-budget rationale as sha256_compress() above;
       always fully overwritten by memset() immediately below before use. */
    static uint8_t pad[SHA256_BLOCK_LEN * 2];
    size_t pad_len;
    uint64_t bit_len = ctx->total_len * 8u;
    size_t i;

    memset(pad, 0, sizeof(pad));
    memcpy(pad, ctx->buffer, ctx->buffer_len);
    pad[ctx->buffer_len] = 0x80;
    pad_len = (ctx->buffer_len < 56) ? SHA256_BLOCK_LEN : SHA256_BLOCK_LEN * 2;
    for(i = 0; i < 8; i++) {
        pad[pad_len - 1 - i] = (uint8_t)(bit_len >> (8 * i));
    }

    sha256_compress(ctx->state, pad);
    if(pad_len == SHA256_BLOCK_LEN * 2) {
        sha256_compress(ctx->state, pad + SHA256_BLOCK_LEN);
    }

    for(i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(ctx->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)(ctx->state[i]);
    }

    feb_secure_zero(pad, sizeof(pad));
}

void feb_sha256(const uint8_t* data, size_t len, uint8_t out[FEB_SHA256_LEN]) {
    /* static: same BleEventWorker stack-budget rationale as hmac_sha256_2part()'s ctx
       above; always re-initialized via sha256_init() immediately below before use. */
    static sha256_ctx_t ctx;
    sha256_init(&ctx);
    if(len > 0) {
        sha256_update(&ctx, data, len);
    }
    sha256_final(&ctx, out);
    feb_secure_zero(&ctx, sizeof(ctx));
}

/* ==================== HMAC-SHA-256 (RFC 2104) ==================== */

/* Computes HMAC-SHA-256(key, data1 || data2) -- `data2` may be NULL/zero-length. Used
   directly by feb_hmac_sha256() (data2 empty) and by feb_hkdf_sha256()'s Expand step
   (info || counter_byte) without needing a caller-sized concatenation buffer. */
static void hmac_sha256_2part(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data1,
    size_t data1_len,
    const uint8_t* data2,
    size_t data2_len,
    uint8_t out[FEB_HMAC_SHA256_LEN]) {
    /* static, not stack-local: same BleEventWorker stack-budget rationale as
       sha256_compress()/sha256_final() above -- this frame (~360 bytes combined) sits
       directly underneath those. Safe as static: single-in-flight BLE event dispatch, not
       reentrant/recursive; key_block/ipad/opad are fully written before use (memset+loop),
       key_hash/inner_hash are fully written before being read, and ctx is always
       re-initialized via sha256_init() before each use within this function. */
    static uint8_t key_block[SHA256_BLOCK_LEN];
    static uint8_t key_hash[FEB_SHA256_LEN];
    static uint8_t ipad[SHA256_BLOCK_LEN];
    static uint8_t opad[SHA256_BLOCK_LEN];
    static uint8_t inner_hash[FEB_SHA256_LEN];
    const uint8_t* actual_key = key;
    size_t actual_key_len = key_len;
    static sha256_ctx_t ctx;
    size_t i;

    if(key_len > SHA256_BLOCK_LEN) {
        feb_sha256(key, key_len, key_hash);
        actual_key = key_hash;
        actual_key_len = FEB_SHA256_LEN;
    }

    memset(key_block, 0, sizeof(key_block));
    if(actual_key_len > 0) {
        memcpy(key_block, actual_key, actual_key_len);
    }
    for(i = 0; i < SHA256_BLOCK_LEN; i++) {
        ipad[i] = (uint8_t)(key_block[i] ^ 0x36);
        opad[i] = (uint8_t)(key_block[i] ^ 0x5c);
    }

    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    if(data1_len > 0) {
        sha256_update(&ctx, data1, data1_len);
    }
    if(data2_len > 0) {
        sha256_update(&ctx, data2, data2_len);
    }
    sha256_final(&ctx, inner_hash);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner_hash, sizeof(inner_hash));
    sha256_final(&ctx, out);

    feb_secure_zero(key_block, sizeof(key_block));
    feb_secure_zero(key_hash, sizeof(key_hash));
    feb_secure_zero(ipad, sizeof(ipad));
    feb_secure_zero(opad, sizeof(opad));
    feb_secure_zero(inner_hash, sizeof(inner_hash));
    feb_secure_zero(&ctx, sizeof(ctx));
}

void feb_hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[FEB_HMAC_SHA256_LEN]) {
    hmac_sha256_2part(key, key_len, data, data_len, NULL, 0, out);
}

/* ==================== HKDF-SHA-256 (RFC 5869) ==================== */

int feb_hkdf_sha256(
    const uint8_t* salt,
    size_t salt_len,
    const uint8_t* ikm,
    size_t ikm_len,
    const uint8_t* info,
    size_t info_len,
    uint8_t* out,
    size_t out_len) {
    /* static: same BleEventWorker stack-budget rationale as this file's other primitives;
       both are fully written by hmac_sha256_2part() before being read. */
    static uint8_t prk[FEB_SHA256_LEN];
    static uint8_t t1[FEB_HMAC_SHA256_LEN];
    uint8_t counter = 0x01;

    if(out == NULL || out_len == 0 || out_len > FEB_HKDF_MAX_LEN) {
        return -1;
    }

    /* Extract: PRK = HMAC-SHA-256(salt, IKM). A NULL/zero-length salt is handled
       correctly by hmac_sha256_2part's key_len==0 path (zero-padded key block),
       which is byte-identical to using an explicit 32-byte all-zero salt. */
    hmac_sha256_2part(salt, salt_len, ikm, ikm_len, NULL, 0, prk);

    /* Expand, first block only: T(1) = HMAC-SHA-256(PRK, info || 0x01). This
       protocol never requests more than FEB_HKDF_MAX_LEN (32) bytes, i.e. never
       more than one HMAC block of output, so T(0) (empty) never needs to be
       prepended and no second iteration is required. */
    hmac_sha256_2part(prk, sizeof(prk), info, info_len, &counter, 1, t1);

    memcpy(out, t1, out_len);

    feb_secure_zero(prk, sizeof(prk));
    feb_secure_zero(t1, sizeof(t1));
    return 0;
}
