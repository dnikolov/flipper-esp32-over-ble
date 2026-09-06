/* Minimal MBEDTLS_CONFIG_FILE for the host-native pairing (tests/esp32/
   build_pairing.ps1) and session (tests/esp32/build_session.ps1) test builds.
   Deliberately narrow -- only what pairing_crypto.c/session_crypto.c actually use
   (mbedtls_sha256, mbedtls_md_hmac, mbedtls_hkdf, mbedtls_mpi_* for X25519,
   mbedtls_gcm_* and mbedtls_aes_* for AES-256-GCM, step 6) -- instead of the full
   vendored mbedtls/mbedtls_config.h default (which pulls in MBEDTLS_PSA_CRYPTO_C
   and a much larger dependency surface not needed here, and not what ESP-IDF's
   own Kconfig-generated config enables either). Leaving MBEDTLS_CIPHER_C
   undefined here is deliberate: with MBEDTLS_PSA_CRYPTO_C also undefined,
   mbedtls's own config_adjust_legacy_crypto.h auto-derives MBEDTLS_BLOCK_CIPHER_C
   from MBEDTLS_GCM_C, which lets gcm.c route through the lighter
   mbedtls_block_cipher_*() path (block_cipher.c + aes.c only) instead of the
   generic mbedtls_cipher_*() wrapper (cipher.c/cipher_wrap.c, unused here). The
   real esp32c6 target build does not use this file; ESP-IDF supplies its own
   MBEDTLS_CONFIG_FILE from sdkconfig (see esp32/sdkconfig.defaults). */
#ifndef FEB_MBEDTLS_TEST_CONFIG_H
#define FEB_MBEDTLS_TEST_CONFIG_H

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_GCM_C
#define MBEDTLS_AES_C

#endif /* FEB_MBEDTLS_TEST_CONFIG_H */
