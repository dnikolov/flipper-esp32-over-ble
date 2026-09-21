/* Shared contract, mirrored byte-for-byte in flipper/session_crypto.h. AES-256-GCM
   primitives used by session.c (docs/PLAN.md step 6) to encrypt/decrypt protected records.

   Runtime records use AES-256-GCM, not AES-128-GCM as docs/PROTOCOL.md originally
   specified: the Flipper's only exported raw-key AES-GCM primitive
   (furi_hal_crypto_gcm_encrypt_and_tag/_decrypt_and_verify) is hardcoded to a 256-bit key
   at the hardware level (crypto_key_init_bswap() in furi_hal_crypto.c unconditionally
   selects CRYPTO_KEYSIZE_256B and reads 32 key bytes) -- there is no 128-bit path through
   that API. The ESP32 backs this header with mbedtls_gcm_* (Kconfig-default-enabled,
   supports 256-bit keys natively, no new sdkconfig option needed).

   One-shot over a fully-buffered input; no streaming context is exposed, matching
   pairing_crypto.h's rationale -- every record this protocol encrypts is already bounded by
   FEB_CBOR_MAX_PAYLOAD (512 bytes), so a one-shot interface stays portable across mbedtls's
   context-object backend and the Flipper's fire-and-forget hardware-peripheral backend. */
#ifndef FEB_SESSION_CRYPTO_H
#define FEB_SESSION_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define FEB_SESSION_KEY_LEN 32u   /* AES-256 key */
#define FEB_SESSION_NONCE_LEN 12u
#define FEB_SESSION_GCM_TAG_LEN 16u

/* Encrypts `plaintext` (`plaintext_len` bytes, may be 0) into `ciphertext_out` (same
   length as plaintext_len -- GCM is a stream cipher over the underlying block cipher) and
   writes the 16-byte authentication tag to `tag_out`. `aad` (`aad_len` bytes, may be 0 with
   aad == NULL) is authenticated but not encrypted. Cannot fail for any well-formed input of
   the sizes this protocol ever uses; returns void to match feb_x25519()'s "total function"
   style in pairing_crypto.h. */
void feb_gcm_encrypt(
    const uint8_t key[FEB_SESSION_KEY_LEN],
    const uint8_t nonce[FEB_SESSION_NONCE_LEN],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *ciphertext_out,
    uint8_t tag_out[FEB_SESSION_GCM_TAG_LEN]);

/* Decrypts `ciphertext` (`ciphertext_len` bytes, may be 0) into `plaintext_out` (same
   length) and verifies `tag` in constant time. Returns 1 if the tag verifies and
   `plaintext_out` is valid, 0 on authentication failure -- callers must treat 0 as fatal per
   docs/PROTOCOL.md ("discard the record and close the BLE connection without replying"),
   never retry or partially trust `plaintext_out` when this returns 0. */
int feb_gcm_decrypt(
    const uint8_t key[FEB_SESSION_KEY_LEN],
    const uint8_t nonce[FEB_SESSION_NONCE_LEN],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *ciphertext, size_t ciphertext_len,
    const uint8_t tag[FEB_SESSION_GCM_TAG_LEN],
    uint8_t *plaintext_out);

#endif /* FEB_SESSION_CRYPTO_H */
