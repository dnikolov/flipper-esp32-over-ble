/* Implements flipper/session_crypto.h (frozen contract, do not change the header).
   docs/PLAN.md step 6: the Flipper's only exported raw-key AES-GCM primitive is
   furi_hal_crypto_gcm_encrypt_and_tag()/furi_hal_crypto_gcm_decrypt_and_verify(), a thin
   wrapper around the STM32WB's memory-mapped AES1 hardware peripheral (confirmed by
   reading docs/references/flipper-firmware/upstream/targets/f7/furi_hal/furi_hal_crypto.c,
   matching the pinned checkout) -- no software AES math, every internal local is a small
   fixed-size array (iv_and_counter[16], dtag[16], block[4]), so unlike the ported X25519 in
   pairing_crypto.c this primitive is not itself a BleEventWorker stack-budget risk (see
   docs/PLAN.md step 6's 2026-09-06 session for the full audit). Both functions init and
   deinit the AES1 engine internally; no separate furi_hal_crypto_init() call is needed by
   this file. */
#include "session_crypto.h"

#include <furi_hal_crypto.h>

void feb_gcm_encrypt(
    const uint8_t key[FEB_SESSION_KEY_LEN],
    const uint8_t nonce[FEB_SESSION_NONCE_LEN],
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* plaintext,
    size_t plaintext_len,
    uint8_t* ciphertext_out,
    uint8_t tag_out[FEB_SESSION_GCM_TAG_LEN]) {
    /* Per this header's contract, this cannot fail for any well-formed input of the sizes
       this protocol ever uses; furi_hal_crypto_gcm_encrypt_and_tag()'s only failure path
       (FuriHalCryptoGCMStateError) is a hardware key-load failure, not a data-dependent
       condition, so the return value is intentionally not surfaced here (matches this
       header's declared void return, mirroring feb_x25519()'s total-function style in
       pairing_crypto.h). */
    (void)furi_hal_crypto_gcm_encrypt_and_tag(
        key, nonce, aad, aad_len, plaintext, ciphertext_out, plaintext_len, tag_out);
}

int feb_gcm_decrypt(
    const uint8_t key[FEB_SESSION_KEY_LEN],
    const uint8_t nonce[FEB_SESSION_NONCE_LEN],
    const uint8_t* aad,
    size_t aad_len,
    const uint8_t* ciphertext,
    size_t ciphertext_len,
    const uint8_t tag[FEB_SESSION_GCM_TAG_LEN],
    uint8_t* plaintext_out) {
    FuriHalCryptoGCMState state = furi_hal_crypto_gcm_decrypt_and_verify(
        key, nonce, aad, aad_len, ciphertext, plaintext_out, ciphertext_len, tag);
    return state == FuriHalCryptoGCMStateOk ? 1 : 0;
}
