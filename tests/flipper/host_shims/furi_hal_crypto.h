/* Host-test-only substitute for the real Flipper firmware's targets/furi_hal_include/
   furi_hal_crypto.h. NOT part of the frozen protocol contracts and NOT linked into the
   real FAP build (fbt.cmd builds against the pinned Unleashed checkout's real header +
   the real STM32WB AES1-hardware-backed furi_hal_crypto.c instead -- see
   flipper/session_crypto.c's top comment).

   Exists solely so tests/flipper/test_session.c can compile+link the real, unmodified
   flipper/session_crypto.c (which calls furi_hal_crypto_gcm_encrypt_and_tag()/
   furi_hal_crypto_gcm_decrypt_and_verify()) on a desktop host with no STM32WB hardware
   available. furi_hal_crypto_stub.c behind this header provides a from-scratch,
   correctness-focused (not hardware-accurate-timing) software AES-256-GCM implementation,
   self-validated by test_session.c's GCM-spec-Test-Case-16 known-answer check before any
   golden-vector assertion is trusted.

   Only the subset of the real header that flipper/session_crypto.c actually calls is
   declared here -- this is deliberately not a full furi_hal_crypto.h port. */
#ifndef FEB_TEST_FURI_HAL_CRYPTO_STUB_H
#define FEB_TEST_FURI_HAL_CRYPTO_STUB_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    FuriHalCryptoGCMStateOk,
    FuriHalCryptoGCMStateError,
    FuriHalCryptoGCMStateAuthFailure,
} FuriHalCryptoGCMState;

FuriHalCryptoGCMState furi_hal_crypto_gcm_encrypt_and_tag(
    const uint8_t* key,
    const uint8_t* iv,
    const uint8_t* aad,
    size_t aad_length,
    const uint8_t* input,
    uint8_t* output,
    size_t length,
    uint8_t* tag);

FuriHalCryptoGCMState furi_hal_crypto_gcm_decrypt_and_verify(
    const uint8_t* key,
    const uint8_t* iv,
    const uint8_t* aad,
    size_t aad_length,
    const uint8_t* input,
    uint8_t* output,
    size_t length,
    const uint8_t* tag);

#endif /* FEB_TEST_FURI_HAL_CRYPTO_STUB_H */
