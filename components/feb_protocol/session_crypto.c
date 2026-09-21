#include "session_crypto.h"

#include <string.h>

#include "mbedtls/cipher.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include "pairing_crypto.h"

void feb_gcm_encrypt(
    const uint8_t key[FEB_SESSION_KEY_LEN],
    const uint8_t nonce[FEB_SESSION_NONCE_LEN],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *ciphertext_out,
    uint8_t tag_out[FEB_SESSION_GCM_TAG_LEN])
{
    mbedtls_gcm_context ctx;
    int rc;

    mbedtls_gcm_init(&ctx);
    rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, FEB_SESSION_KEY_LEN * 8u);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plaintext_len,
                                       nonce, FEB_SESSION_NONCE_LEN,
                                       aad, aad_len,
                                       plaintext, ciphertext_out,
                                       FEB_SESSION_GCM_TAG_LEN, tag_out);
    }
    if (rc != 0) {
        if (plaintext_len > 0) {
            feb_secure_zero(ciphertext_out, plaintext_len);
        }
        feb_secure_zero(tag_out, FEB_SESSION_GCM_TAG_LEN);
    }
    mbedtls_gcm_free(&ctx);
}

int feb_gcm_decrypt(
    const uint8_t key[FEB_SESSION_KEY_LEN],
    const uint8_t nonce[FEB_SESSION_NONCE_LEN],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *ciphertext, size_t ciphertext_len,
    const uint8_t tag[FEB_SESSION_GCM_TAG_LEN],
    uint8_t *plaintext_out)
{
    mbedtls_gcm_context ctx;
    int rc;

    mbedtls_gcm_init(&ctx);
    rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, FEB_SESSION_KEY_LEN * 8u);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx, ciphertext_len,
                                      nonce, FEB_SESSION_NONCE_LEN,
                                      aad, aad_len,
                                      tag, FEB_SESSION_GCM_TAG_LEN,
                                      ciphertext, plaintext_out);
    }
    mbedtls_gcm_free(&ctx);
    if (rc != 0) {
        if (ciphertext_len > 0) {
            mbedtls_platform_zeroize(plaintext_out, ciphertext_len);
        }
        return 0;
    }
    return 1;
}
