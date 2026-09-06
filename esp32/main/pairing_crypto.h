/* Shared contract, mirrored byte-for-byte in flipper/pairing_crypto.h. Cryptographic
   primitives used by pairing.c (docs/PLAN.md step 5) and, from step 6 onward, runtime
   session authentication. The ESP32 backs these with mbedtls (mbedtls_ecdh_*,
   mbedtls_md_hmac, mbedtls_hkdf, mbedtls_gcm_* -- all Kconfig-default-enabled except
   CONFIG_MBEDTLS_HKDF_C, which esp32/sdkconfig.defaults must set). The Flipper has no
   exported crypto beyond raw-key AES-GCM (furi_hal_crypto_gcm_*) -- see docs/PLAN.md
   step 5 "Step 5 implementation decisions" -- and hand-rolls SHA-256/HMAC/HKDF from
   spec plus a ported X25519 (e.g. curve25519-donna) behind this same interface.

   Every function is one-shot over a fully-buffered input; no streaming context is
   exposed. Every message this protocol hashes/HMACs/derives from is small and already
   fully assembled in memory (transcript T, proof strings, at most a few hundred bytes),
   so this keeps the header portable across a context-object-based backend (mbedtls) and
   a hand-rolled one without exposing either backend's internal state layout. */
#ifndef FEB_PAIRING_CRYPTO_H
#define FEB_PAIRING_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define FEB_X25519_KEY_LEN 32u
#define FEB_SHA256_LEN 32u
#define FEB_HMAC_SHA256_LEN 32u
#define FEB_HKDF_MAX_LEN 32u /* this protocol never derives more than 32 bytes at once */

/* ---- X25519 (RFC 7748) ----
   feb_x25519() is the RFC 7748 X25519(k, u) function directly: `scalar` is clamped
   internally (decodeScalar25519) on every call, matching RFC 7748 section 5.2's test
   vectors byte-for-byte, including the base-point case (u = 9, little-endian, used for
   public-key generation). Callers always pass the same unclamped, stored private-key
   bytes; there is no separate "clamped private key" representation to keep in sync. */
void feb_x25519(uint8_t out[FEB_X25519_KEY_LEN], const uint8_t scalar[FEB_X25519_KEY_LEN], const uint8_t point[FEB_X25519_KEY_LEN]);

/* out = feb_x25519(scalar, base_point=9). Convenience for public-key generation. */
void feb_x25519_base(uint8_t out[FEB_X25519_KEY_LEN], const uint8_t scalar[FEB_X25519_KEY_LEN]);

/* Copies `random_32` (fresh CSPRNG output -- esp_fill_random() on ESP32,
   furi_hal_random_fill_buf() on Flipper; the platform RNG itself is outside this
   contract) into `out_private` verbatim. Named as a function rather than left as a bare
   copy at call sites so "generate a keypair" reads as one step; RFC 7748 clamping is
   applied fresh inside feb_x25519()/feb_x25519_base() on every use, not stored here. */
void feb_x25519_keypair(uint8_t out_private[FEB_X25519_KEY_LEN], uint8_t out_public[FEB_X25519_KEY_LEN], const uint8_t random_32[FEB_X25519_KEY_LEN]);

/* Constant-time all-zero check over `len` bytes at `data`. Callers must reject an
   all-zero X25519 shared secret (docs/PAIRING.md's "Security boundary") before using it
   in any derivation -- RFC 7748's X25519 function is defined for every 32-byte input, so
   this all-zero check is the only public-key validation this protocol performs. */
int feb_is_all_zero(const uint8_t *data, size_t len);

/* ---- SHA-256 (FIPS 180-4), one-shot ---- */
void feb_sha256(const uint8_t *data, size_t len, uint8_t out[FEB_SHA256_LEN]);

/* ---- HMAC-SHA-256 (RFC 2104), one-shot ---- */
void feb_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[FEB_HMAC_SHA256_LEN]);

/* ---- HKDF-SHA-256 (RFC 5869), one-shot Extract-and-Expand ----
   Returns 0 on success, nonzero if out_len == 0 or out_len > FEB_HKDF_MAX_LEN. A NULL
   salt with salt_len == 0 is valid per RFC 5869 (treated as a zero-filled hash-length
   salt); this protocol always supplies an explicit salt, but the primitive itself
   follows the RFC exactly. */
int feb_hkdf_sha256(
    const uint8_t *salt, size_t salt_len,
    const uint8_t *ikm, size_t ikm_len,
    const uint8_t *info, size_t info_len,
    uint8_t *out, size_t out_len);

/* Constant-time comparison over the full `len` bytes of both buffers regardless of
   where they first differ. Returns 1 if equal, 0 if not equal or if len == 0. Used for
   every proof/confirmation/tag comparison per docs/PROTOCOL.md's "Implementation
   security requirements." */
int feb_consttime_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* Zeroizes `len` bytes at `buf` in a way that cannot be legally optimized away by a
   compiler that can prove `buf` is otherwise dead (CWE-14) -- e.g. via a volatile
   pointer/byte-wise write on the Flipper, or mbedtls_platform_zeroize() on the ESP32
   backend. Every ephemeral-secret cleanup path (pairing success, failure, expiry,
   disconnect; session keys from step 6 onward) must go through this one function rather
   than a bare memset(), so the guarantee is centralized and auditable in one place per
   firmware (docs/PLAN.md step 5 "Zeroization approach"). */
void feb_secure_zero(void *buf, size_t len);

#endif /* FEB_PAIRING_CRYPTO_H */
