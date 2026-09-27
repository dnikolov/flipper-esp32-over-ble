/* Minimal MBEDTLS_CONFIG_FILE for the host-native pairing (tests/esp32/
   build_pairing.ps1), session (tests/esp32/build_session.ps1), and meshtastic_proto
   (tests/esp32/build_meshtastic_proto.ps1) test builds. Deliberately narrow -- only what
   pairing_crypto.c/session_crypto.c/meshtastic_proto.c actually use (mbedtls_sha256,
   mbedtls_md_hmac, mbedtls_hkdf, mbedtls_mpi_* for X25519, mbedtls_gcm_* and mbedtls_aes_*
   for AES-256-GCM (step 6), and mbedtls_aes_crypt_ctr for meshtastic_scan's AES-128-CTR
   default-channel decrypt, 2026-09-27) -- instead of the full vendored mbedtls/
   mbedtls_config.h default (which pulls in MBEDTLS_PSA_CRYPTO_C and a much larger dependency
   surface not needed here, and not what ESP-IDF's own Kconfig-generated config enables
   either). Leaving MBEDTLS_CIPHER_C undefined here is deliberate: with MBEDTLS_PSA_CRYPTO_C
   also undefined, mbedtls's own config_adjust_legacy_crypto.h auto-derives
   MBEDTLS_BLOCK_CIPHER_C from MBEDTLS_GCM_C, which lets gcm.c route through the lighter
   mbedtls_block_cipher_*() path (block_cipher.c + aes.c only) instead of the generic
   mbedtls_cipher_*() wrapper (cipher.c/cipher_wrap.c, unused here). MBEDTLS_CIPHER_MODE_CTR
   is needed in addition to MBEDTLS_AES_C specifically for mbedtls_aes_crypt_ctr() (aes.h
   guards that prototype behind it) -- harmless to define for the pairing/session test
   binaries too, since they never call it. The real esp32c6/esp32 target builds do not use
   this file; ESP-IDF supplies its own MBEDTLS_CONFIG_FILE from sdkconfig (see
   esp32/sdkconfig.defaults / heltec/sdkconfig.defaults). */
#ifndef FEB_MBEDTLS_TEST_CONFIG_H
#define FEB_MBEDTLS_TEST_CONFIG_H

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_GCM_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_MODE_CTR

#endif /* FEB_MBEDTLS_TEST_CONFIG_H */
