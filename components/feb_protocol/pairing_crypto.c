#include "pairing_crypto.h"

#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

/* ---- X25519 (RFC 7748), hand-rolled Montgomery ladder over mbedtls_mpi ----
   mbedtls's own high-level Curve25519 path (mbedtls_ecp_mul()/mbedtls_ecdh_*)
   internally calls mbedtls_ecp_check_pubkey(), which for Montgomery groups
   (ecp.c's ecp_check_pubkey_mx()/ecp_check_bad_points_mx()) rejects u=0 and a
   handful of other known-low-order points with MBEDTLS_ERR_ECP_INVALID_KEY.
   feb_x25519() is declared void -- it has no way to report or safely recover
   from that rejection -- and this header's own contract requires it to be a
   total function over every 32-byte input (RFC 7748 defines X25519 for all
   inputs; this protocol pushes the *only* rejection, all-zero output, to the
   caller via feb_is_all_zero()). So the RFC 7748 section 5 Montgomery ladder
   is implemented directly against mbedtls's bignum (mbedtls_mpi) primitives
   here, bypassing mbedtls_ecp_mul()'s extra validation -- still "backed by
   mbedtls", just at the modular-arithmetic layer. See docs/SESSION_MEMORY.md's
   step 5 esp32-developer entry for the full investigation. Residual caveat:
   this uses mbedtls_mpi_mod_mpi() for reduction, which is not documented as
   constant-time (unlike mbedtls's own internal Curve25519 ladder) -- flagged
   as a follow-up hardening item, not fixed in this round. */

#define FEB_X25519_A24 121665
#define FEB_X25519_LADDER_BITS 255u

static const char *const FEB_X25519_PRIME_HEX =
    "7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFED";

static int feb_mod_add(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b, const mbedtls_mpi *p)
{
    int rc = mbedtls_mpi_add_mpi(r, a, b);
    if (rc != 0) {
        return rc;
    }
    return mbedtls_mpi_mod_mpi(r, r, p);
}

static int feb_mod_sub(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b, const mbedtls_mpi *p)
{
    int rc = mbedtls_mpi_sub_mpi(r, a, b);
    if (rc != 0) {
        return rc;
    }
    return mbedtls_mpi_mod_mpi(r, r, p);
}

static int feb_mod_mul(mbedtls_mpi *r, const mbedtls_mpi *a, const mbedtls_mpi *b, const mbedtls_mpi *p)
{
    int rc = mbedtls_mpi_mul_mpi(r, a, b);
    if (rc != 0) {
        return rc;
    }
    return mbedtls_mpi_mod_mpi(r, r, p);
}

void feb_x25519(uint8_t out[FEB_X25519_KEY_LEN], const uint8_t scalar[FEB_X25519_KEY_LEN], const uint8_t point[FEB_X25519_KEY_LEN])
{
    uint8_t clamped[FEB_X25519_KEY_LEN];
    uint8_t u_bytes[FEB_X25519_KEY_LEN];
    mbedtls_mpi p, a24, k, x1, x2, z2, x3, z3;
    mbedtls_mpi t0, t1, a, aa, b, bb, e, c, d, da, cb;
    mbedtls_mpi pm2, zinv, result;
    int rc = 0;
    int t;
    unsigned char swap = 0;

    memcpy(clamped, scalar, FEB_X25519_KEY_LEN);
    clamped[0] = (uint8_t)(clamped[0] & 0xF8u);
    clamped[31] = (uint8_t)(clamped[31] & 0x7Fu);
    clamped[31] = (uint8_t)(clamped[31] | 0x40u);

    memcpy(u_bytes, point, FEB_X25519_KEY_LEN);
    u_bytes[31] = (uint8_t)(u_bytes[31] & 0x7Fu);

    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&a24);
    mbedtls_mpi_init(&k);
    mbedtls_mpi_init(&x1);
    mbedtls_mpi_init(&x2);
    mbedtls_mpi_init(&z2);
    mbedtls_mpi_init(&x3);
    mbedtls_mpi_init(&z3);
    mbedtls_mpi_init(&t0);
    mbedtls_mpi_init(&t1);
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&aa);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&bb);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&c);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&da);
    mbedtls_mpi_init(&cb);
    mbedtls_mpi_init(&pm2);
    mbedtls_mpi_init(&zinv);
    mbedtls_mpi_init(&result);

    rc |= mbedtls_mpi_read_string(&p, 16, FEB_X25519_PRIME_HEX);
    rc |= mbedtls_mpi_lset(&a24, FEB_X25519_A24);
    rc |= mbedtls_mpi_read_binary_le(&k, clamped, sizeof(clamped));
    rc |= mbedtls_mpi_read_binary_le(&x1, u_bytes, sizeof(u_bytes));
    rc |= mbedtls_mpi_lset(&x2, 1);
    rc |= mbedtls_mpi_lset(&z2, 0);
    rc |= mbedtls_mpi_copy(&x3, &x1);
    rc |= mbedtls_mpi_lset(&z3, 1);

    for (t = (int)FEB_X25519_LADDER_BITS - 1; t >= 0 && rc == 0; t--) {
        unsigned char k_t = (unsigned char)mbedtls_mpi_get_bit(&k, (size_t)t);

        swap = (unsigned char)(swap ^ k_t);
        rc |= mbedtls_mpi_safe_cond_swap(&x2, &x3, swap);
        rc |= mbedtls_mpi_safe_cond_swap(&z2, &z3, swap);
        swap = k_t;

        rc |= feb_mod_add(&a, &x2, &z2, &p);
        rc |= feb_mod_mul(&aa, &a, &a, &p);
        rc |= feb_mod_sub(&b, &x2, &z2, &p);
        rc |= feb_mod_mul(&bb, &b, &b, &p);
        rc |= feb_mod_sub(&e, &aa, &bb, &p);
        rc |= feb_mod_add(&c, &x3, &z3, &p);
        rc |= feb_mod_sub(&d, &x3, &z3, &p);
        rc |= feb_mod_mul(&da, &d, &a, &p);
        rc |= feb_mod_mul(&cb, &c, &b, &p);

        rc |= feb_mod_add(&t0, &da, &cb, &p);
        rc |= feb_mod_mul(&x3, &t0, &t0, &p);
        rc |= feb_mod_sub(&t1, &da, &cb, &p);
        rc |= feb_mod_mul(&t1, &t1, &t1, &p);
        rc |= feb_mod_mul(&z3, &x1, &t1, &p);

        rc |= feb_mod_mul(&x2, &aa, &bb, &p);
        rc |= feb_mod_mul(&t0, &a24, &e, &p);
        rc |= feb_mod_add(&t0, &aa, &t0, &p);
        rc |= feb_mod_mul(&z2, &e, &t0, &p);
    }

    if (rc == 0) {
        rc |= mbedtls_mpi_safe_cond_swap(&x2, &x3, swap);
        rc |= mbedtls_mpi_safe_cond_swap(&z2, &z3, swap);
    }

    if (rc == 0) {
        rc |= mbedtls_mpi_copy(&pm2, &p);
        rc |= mbedtls_mpi_sub_int(&pm2, &pm2, 2);
        rc |= mbedtls_mpi_exp_mod(&zinv, &z2, &pm2, &p, NULL);
        rc |= feb_mod_mul(&result, &x2, &zinv, &p);
    }

    if (rc == 0) {
        rc |= mbedtls_mpi_write_binary_le(&result, out, FEB_X25519_KEY_LEN);
    }
    if (rc != 0) {
        memset(out, 0, FEB_X25519_KEY_LEN);
    }

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&a24);
    mbedtls_mpi_free(&k);
    mbedtls_mpi_free(&x1);
    mbedtls_mpi_free(&x2);
    mbedtls_mpi_free(&z2);
    mbedtls_mpi_free(&x3);
    mbedtls_mpi_free(&z3);
    mbedtls_mpi_free(&t0);
    mbedtls_mpi_free(&t1);
    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&aa);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&bb);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&c);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&da);
    mbedtls_mpi_free(&cb);
    mbedtls_mpi_free(&pm2);
    mbedtls_mpi_free(&zinv);
    mbedtls_mpi_free(&result);

    mbedtls_platform_zeroize(clamped, sizeof(clamped));
}

void feb_x25519_base(uint8_t out[FEB_X25519_KEY_LEN], const uint8_t scalar[FEB_X25519_KEY_LEN])
{
    static const uint8_t base_point[FEB_X25519_KEY_LEN] = {9};

    feb_x25519(out, scalar, base_point);
}

void feb_x25519_keypair(uint8_t out_private[FEB_X25519_KEY_LEN], uint8_t out_public[FEB_X25519_KEY_LEN], const uint8_t random_32[FEB_X25519_KEY_LEN])
{
    memcpy(out_private, random_32, FEB_X25519_KEY_LEN);
    feb_x25519_base(out_public, out_private);
}

int feb_is_all_zero(const uint8_t *data, size_t len)
{
    uint8_t acc = 0;
    size_t i;

    if (data == NULL) {
        return len == 0 ? 1 : 0;
    }
    for (i = 0; i < len; i++) {
        acc = (uint8_t)(acc | data[i]);
    }
    return acc == 0 ? 1 : 0;
}

void feb_sha256(const uint8_t *data, size_t len, uint8_t out[FEB_SHA256_LEN])
{
    if (mbedtls_sha256(data, len, out, 0) != 0) {
        memset(out, 0, FEB_SHA256_LEN);
    }
}

void feb_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[FEB_HMAC_SHA256_LEN])
{
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (md_info == NULL || mbedtls_md_hmac(md_info, key, key_len, data, data_len, out) != 0) {
        memset(out, 0, FEB_HMAC_SHA256_LEN);
    }
}

int feb_hkdf_sha256(
    const uint8_t *salt, size_t salt_len,
    const uint8_t *ikm, size_t ikm_len,
    const uint8_t *info, size_t info_len,
    uint8_t *out, size_t out_len)
{
    const mbedtls_md_info_t *md_info;

    if (out == NULL || out_len == 0 || out_len > FEB_HKDF_MAX_LEN) {
        return 1;
    }
    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        memset(out, 0, out_len);
        return 1;
    }
    if (mbedtls_hkdf(md_info, salt, salt_len, ikm, ikm_len, info, info_len, out, out_len) != 0) {
        memset(out, 0, out_len);
        return 1;
    }
    return 0;
}

int feb_consttime_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    size_t i;

    if (a == NULL || b == NULL || len == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    }
    return diff == 0 ? 1 : 0;
}

void feb_secure_zero(void *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    mbedtls_platform_zeroize(buf, len);
}
