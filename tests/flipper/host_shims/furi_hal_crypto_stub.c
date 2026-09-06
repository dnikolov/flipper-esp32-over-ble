/* Host-test-only from-scratch AES-256-GCM, implementing the furi_hal_crypto.h subset
   declared in this directory's furi_hal_crypto.h -- see that header's top comment for why
   this exists (no STM32WB AES1 hardware on a desktop host). Only ever linked into
   tests/flipper/test_session.c; never linked into the real FAP.

   Assumes a 96-bit (12-byte) IV throughout (NIST SP 800-38D's simpler J0 = IV || 0^31 1
   case) -- the only IV length flipper/session_crypto.h's FEB_SESSION_NONCE_LEN (12) ever
   produces in this protocol, so the general variable-length-IV GHASH(IV) case is
   deliberately not implemented.

   The AES S-box is derived at first use from the GF(2^8) multiplicative inverse + affine
   transform per FIPS-197, not a hand-transcribed 256-entry table, matching
   tests/vectors/generate_vectors.py's own from-scratch AES-256+GCM (same
   anti-transcription-error rationale, see docs/SESSION_MEMORY.md's step 6 entry). Self-
   validated by test_session.c's AES-256-GCM known-answer check (GCM spec Test Case 16)
   before any golden end-to-end session vector is trusted. */
#include "furi_hal_crypto.h"

#include <string.h>

/* ==================== GF(2^8) S-box construction (FIPS-197) ==================== */

static uint8_t gf_mul8(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for(int i = 0; i < 8; i++) {
        if(b & 1) {
            p ^= a;
        }
        uint8_t hi = (uint8_t)(a & 0x80);
        a = (uint8_t)(a << 1);
        if(hi) {
            a = (uint8_t)(a ^ 0x1B);
        }
        b = (uint8_t)(b >> 1);
    }
    return p;
}

static uint8_t gf_pow8(uint8_t a, int e) {
    uint8_t result = 1;
    uint8_t base = a;
    while(e) {
        if(e & 1) {
            result = gf_mul8(result, base);
        }
        base = gf_mul8(base, base);
        e >>= 1;
    }
    return result;
}

static uint8_t gf_inv8(uint8_t a) {
    return a == 0 ? 0 : gf_pow8(a, 254);
}

static uint8_t g_sbox[256];
static int g_sbox_ready = 0;

static void ensure_sbox(void) {
    if(g_sbox_ready) {
        return;
    }
    for(int x = 0; x < 256; x++) {
        uint8_t s = gf_inv8((uint8_t)x);
        uint8_t rot1 = (uint8_t)((s << 1) | (s >> 7));
        uint8_t rot2 = (uint8_t)((s << 2) | (s >> 6));
        uint8_t rot3 = (uint8_t)((s << 3) | (s >> 5));
        uint8_t rot4 = (uint8_t)((s << 4) | (s >> 4));
        g_sbox[x] = (uint8_t)(s ^ rot1 ^ rot2 ^ rot3 ^ rot4 ^ 0x63);
    }
    g_sbox_ready = 1;
}

/* ==================== AES-256 (FIPS-197, Nk=8, Nr=14) ==================== */

#define AES256_ROUNDS 14
#define AES256_SCHEDULE_WORDS (4 * (AES256_ROUNDS + 1))

typedef struct {
    uint32_t w[AES256_SCHEDULE_WORDS];
} Aes256KeySchedule;

static uint8_t aes_xtime(uint8_t a) {
    return (uint8_t)((uint8_t)(a << 1) ^ ((a & 0x80) ? 0x1B : 0x00));
}

static uint32_t aes_sub_word(uint32_t w) {
    uint8_t b0 = g_sbox[(w >> 24) & 0xFFu];
    uint8_t b1 = g_sbox[(w >> 16) & 0xFFu];
    uint8_t b2 = g_sbox[(w >> 8) & 0xFFu];
    uint8_t b3 = g_sbox[w & 0xFFu];
    return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
}

static uint32_t aes_rot_word(uint32_t w) {
    return (uint32_t)((w << 8) | (w >> 24));
}

static void aes256_key_expansion(const uint8_t key[32], Aes256KeySchedule* ks) {
    ensure_sbox();
    uint32_t* w = ks->w;
    for(int i = 0; i < 8; i++) {
        w[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
               ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];
    }
    uint8_t rcon = 0x01;
    for(int i = 8; i < AES256_SCHEDULE_WORDS; i++) {
        uint32_t temp = w[i - 1];
        if(i % 8 == 0) {
            temp = aes_sub_word(aes_rot_word(temp)) ^ ((uint32_t)rcon << 24);
            rcon = aes_xtime(rcon);
        } else if(i % 8 == 4) {
            temp = aes_sub_word(temp);
        }
        w[i] = w[i - 8] ^ temp;
    }
}

static void aes_add_round_key(uint8_t state[4][4], const uint32_t* w, int round) {
    for(int c = 0; c < 4; c++) {
        uint32_t word = w[round * 4 + c];
        state[0][c] = (uint8_t)(state[0][c] ^ ((word >> 24) & 0xFFu));
        state[1][c] = (uint8_t)(state[1][c] ^ ((word >> 16) & 0xFFu));
        state[2][c] = (uint8_t)(state[2][c] ^ ((word >> 8) & 0xFFu));
        state[3][c] = (uint8_t)(state[3][c] ^ (word & 0xFFu));
    }
}

static void aes_sub_bytes(uint8_t state[4][4]) {
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
            state[r][c] = g_sbox[state[r][c]];
        }
    }
}

static void aes_shift_rows(uint8_t state[4][4]) {
    uint8_t t;
    t = state[1][0];
    state[1][0] = state[1][1];
    state[1][1] = state[1][2];
    state[1][2] = state[1][3];
    state[1][3] = t;

    uint8_t t0 = state[2][0], t1 = state[2][1];
    state[2][0] = state[2][2];
    state[2][1] = state[2][3];
    state[2][2] = t0;
    state[2][3] = t1;

    t = state[3][3];
    state[3][3] = state[3][2];
    state[3][2] = state[3][1];
    state[3][1] = state[3][0];
    state[3][0] = t;
}

static void aes_mix_columns(uint8_t state[4][4]) {
    for(int c = 0; c < 4; c++) {
        uint8_t a0 = state[0][c], a1 = state[1][c], a2 = state[2][c], a3 = state[3][c];
        uint8_t m2a0 = aes_xtime(a0);
        uint8_t m2a1 = aes_xtime(a1);
        uint8_t m2a2 = aes_xtime(a2);
        uint8_t m2a3 = aes_xtime(a3);
        state[0][c] = (uint8_t)(m2a0 ^ (uint8_t)(m2a1 ^ a1) ^ a2 ^ a3);
        state[1][c] = (uint8_t)(a0 ^ m2a1 ^ (uint8_t)(m2a2 ^ a2) ^ a3);
        state[2][c] = (uint8_t)(a0 ^ a1 ^ m2a2 ^ (uint8_t)(m2a3 ^ a3));
        state[3][c] = (uint8_t)((uint8_t)(m2a0 ^ a0) ^ a1 ^ a2 ^ m2a3);
    }
}

static void aes256_encrypt_block(const Aes256KeySchedule* ks, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[4][4];
    for(int c = 0; c < 4; c++) {
        for(int r = 0; r < 4; r++) {
            state[r][c] = in[4 * c + r];
        }
    }

    aes_add_round_key(state, ks->w, 0);
    for(int round = 1; round <= AES256_ROUNDS - 1; round++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, ks->w, round);
    }
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, ks->w, AES256_ROUNDS);

    for(int c = 0; c < 4; c++) {
        for(int r = 0; r < 4; r++) {
            out[4 * c + r] = state[r][c];
        }
    }
}

/* ==================== GCM mode (NIST SP 800-38D), 96-bit IV only ==================== */

static void gcm_inc32(uint8_t block[16]) {
    uint32_t val = ((uint32_t)block[12] << 24) | ((uint32_t)block[13] << 16) |
                   ((uint32_t)block[14] << 8) | (uint32_t)block[15];
    val += 1;
    block[12] = (uint8_t)(val >> 24);
    block[13] = (uint8_t)(val >> 16);
    block[14] = (uint8_t)(val >> 8);
    block[15] = (uint8_t)val;
}

/* GHASH's block-multiplication operation over GF(2^128), NIST SP 800-38D Algorithm 1. */
static void gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t z_out[16]) {
    uint8_t v[16];
    uint8_t z[16];
    memcpy(v, y, 16);
    memset(z, 0, 16);
    for(int i = 0; i < 128; i++) {
        int byte_idx = i / 8;
        int bit_idx = 7 - (i % 8);
        int xbit = (x[byte_idx] >> bit_idx) & 1;
        if(xbit) {
            for(int j = 0; j < 16; j++) {
                z[j] = (uint8_t)(z[j] ^ v[j]);
            }
        }
        int lsb = v[15] & 1;
        for(int j = 15; j > 0; j--) {
            v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
        }
        v[0] = (uint8_t)(v[0] >> 1);
        if(lsb) {
            v[0] = (uint8_t)(v[0] ^ 0xE1);
        }
    }
    memcpy(z_out, z, 16);
}

static void ghash_absorb_block(uint8_t y[16], const uint8_t h[16], const uint8_t block[16]) {
    for(int i = 0; i < 16; i++) {
        y[i] = (uint8_t)(y[i] ^ block[i]);
    }
    gf128_mul(y, h, y);
}

static void ghash_absorb_span(uint8_t y[16], const uint8_t h[16], const uint8_t* data, size_t len) {
    size_t full = len / 16;
    size_t rem = len % 16;
    for(size_t b = 0; b < full; b++) {
        ghash_absorb_block(y, h, data + b * 16);
    }
    if(rem > 0) {
        uint8_t block[16] = {0};
        memcpy(block, data + full * 16, rem);
        ghash_absorb_block(y, h, block);
    }
}

/* Computes AES-256-GCM. `input`/`output` may alias the same buffer content role (encrypt:
   input=plaintext, output=ciphertext; decrypt: input=ciphertext, output=plaintext) --
   authentication is always computed over the ciphertext regardless of direction. */
static void gcm_core(
    const uint8_t key[32],
    const uint8_t iv[12],
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* input,
    uint8_t* output,
    size_t length,
    uint8_t tag_out[16],
    int decrypt) {
    Aes256KeySchedule ks;
    aes256_key_expansion(key, &ks);

    static const uint8_t zero_block[16] = {0};
    uint8_t h[16];
    aes256_encrypt_block(&ks, zero_block, h);

    uint8_t j0[16];
    memcpy(j0, iv, 12);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;

    uint8_t tag_mask[16];
    aes256_encrypt_block(&ks, j0, tag_mask);

    uint8_t counter_block[16];
    memcpy(counter_block, j0, 16);
    size_t full_blocks = length / 16;
    size_t rem = length % 16;
    size_t processed = 0;
    for(size_t b = 0; b < full_blocks; b++) {
        gcm_inc32(counter_block);
        uint8_t keystream[16];
        aes256_encrypt_block(&ks, counter_block, keystream);
        for(int k = 0; k < 16; k++) {
            output[processed + (size_t)k] = (uint8_t)(input[processed + (size_t)k] ^ keystream[k]);
        }
        processed += 16;
    }
    if(rem > 0) {
        gcm_inc32(counter_block);
        uint8_t keystream[16];
        aes256_encrypt_block(&ks, counter_block, keystream);
        for(size_t k = 0; k < rem; k++) {
            output[processed + k] = (uint8_t)(input[processed + k] ^ keystream[k]);
        }
        processed += rem;
    }

    const uint8_t* ciphertext_for_auth = decrypt ? input : output;

    uint8_t y[16] = {0};
    ghash_absorb_span(y, h, aad, aad_len);
    ghash_absorb_span(y, h, ciphertext_for_auth, length);

    uint8_t len_block[16] = {0};
    uint64_t aad_bits = (uint64_t)aad_len * 8u;
    uint64_t c_bits = (uint64_t)length * 8u;
    for(int k = 0; k < 8; k++) {
        len_block[7 - k] = (uint8_t)(aad_bits >> (8 * k));
        len_block[15 - k] = (uint8_t)(c_bits >> (8 * k));
    }
    ghash_absorb_block(y, h, len_block);

    for(int k = 0; k < 16; k++) {
        tag_out[k] = (uint8_t)(y[k] ^ tag_mask[k]);
    }
}

FuriHalCryptoGCMState furi_hal_crypto_gcm_encrypt_and_tag(
    const uint8_t* key,
    const uint8_t* iv,
    const uint8_t* aad,
    size_t aad_length,
    const uint8_t* input,
    uint8_t* output,
    size_t length,
    uint8_t* tag) {
    gcm_core(key, iv, aad, aad_length, input, output, length, tag, 0);
    return FuriHalCryptoGCMStateOk;
}

FuriHalCryptoGCMState furi_hal_crypto_gcm_decrypt_and_verify(
    const uint8_t* key,
    const uint8_t* iv,
    const uint8_t* aad,
    size_t aad_length,
    const uint8_t* input,
    uint8_t* output,
    size_t length,
    const uint8_t* tag) {
    uint8_t computed_tag[16];
    gcm_core(key, iv, aad, aad_length, input, output, length, computed_tag, 1);
    uint8_t diff = 0;
    for(int i = 0; i < 16; i++) {
        diff = (uint8_t)(diff | (computed_tag[i] ^ tag[i]));
    }
    if(diff != 0) {
        memset(output, 0, length);
        return FuriHalCryptoGCMStateAuthFailure;
    }
    return FuriHalCryptoGCMStateOk;
}
