# Step 3 codec tests

Host-native unit tests for the framing (`framing.c`/`.h`) and canonical-CBOR
(`cbor_codec.h` and its per-capability split — see docs/PROJECT_HISTORY.md's 2026-09-08
"Codebase and agent cost-efficiency pass" entry:
`cbor_primitives.c`/`.h`, `cbor_records.c`/`.h`, `cbor_wifi_scan.c`/`.h`,
`cbor_ble_scan.c`/`.h`, `cbor_wardriving.c`/`.h`) modules — see
[docs/PLAN.md](../docs/PLAN.md) step 3 and [docs/PROTOCOL.md](../docs/PROTOCOL.md). No
board required; built and run with MSVC (`cl.exe`) on the host.

- `vectors/generate_vectors.py` — generates `vectors/vectors.h` directly from the
  canonical-encoding rules in PROTOCOL.md (not from either firmware's codec). Re-run
  after editing it; do not hand-edit `vectors.h`.
- `esp32/` — compiles `esp32/main/framing.c` and the five split `esp32/main/cbor_*.c`
  codec files against `vectors/vectors.h` into a standalone host executable.
- `flipper/` — compiles `flipper/framing.c` and the five split `flipper/cbor_*.c` codec
  files against the same vectors into a separate standalone host executable.

Both test executables are independent implementations exercised against the same
fixed vectors — the point is to catch the two firmwares' codecs disagreeing with each
other, not just disagreeing with themselves. Passing here satisfies step 3's "done
when" bar; it does not exercise real BLE transport (see PLAN.md's on-device smoke-test
follow-up) or adversarial fuzzing (deferred to step 9).

## Step 5 additions (pairing)

`vectors/generate_vectors.py` also generates the pairing (docs/PLAN.md step 5) vectors,
built against `esp32/main/pairing.h`/`pairing_crypto.h` (and their byte-identical
`flipper/` copies):

- Per-primitive known-answer vectors: X25519 (RFC 7748 §5.2 and the §6.1 Alice/Bob
  Diffie-Hellman example, plus an all-zero-shared-secret case), SHA-256 (FIPS 180-4
  "abc"), HMAC-SHA-256 (RFC 4231 test case 1), HKDF-SHA-256 (RFC 5869 test case 1
  inputs, output requested at this protocol's actual L=32). The X25519 vectors validate
  a pure-Python Montgomery-ladder implementation inside the generator itself against the
  RFC's published outputs before it is trusted to compute anything else — see the
  generator's assertions.
- One golden end-to-end pairing vector (`FEB_VEC_PAIR_*`): fixed, non-random inputs run
  through the full T -> K_shared -> K_confirm -> pairing_secret -> confirmation-tag
  pipeline, plus the corresponding pair_init/pair_reply/pair_confirm/pair_complete
  envelope and payload CBOR encodings and one pairing-phase `error` record. Per-primitive
  vectors alone can't catch a wiring bug (wrong field order, wrong concatenation order,
  wrong domain-separation string) in the assembled derivation — exactly the class of bug
  the step 3 codec found twice, in framing.c rather than in a primitive.

`tests/flipper/test_pairing.c` (built/run via `tests/flipper/build_pairing.ps1`, a
sibling to `build.ps1` rather than folded into it) compiles `flipper/pairing_crypto.c`
and `flipper/pairing.c` against these vectors — per-primitive checks, the golden
end-to-end pipeline, and all four payload/envelope CBOR round trips.

`tests/esp32/test_pairing.c` (built/run via `tests/esp32/build_pairing.ps1`, the same
sibling-script pattern) compiles `esp32/main/pairing_crypto.c` and `esp32/main/pairing.c`
against the same vectors, backed by ESP-IDF's vendored mbedtls (`sha256.c`, `md.c`,
`hkdf.c`, `bignum.c`/`bignum_core.c`, `constant_time.c`, `platform_util.c`, compiled from
`$IDF_PATH/components/mbedtls/mbedtls/library` under a minimal
`tests/esp32/mbedtls_test_config.h` rather than ESP-IDF's own Kconfig-generated config or
the vendored default `mbedtls_config.h`, to avoid pulling in `MBEDTLS_PSA_CRYPTO_C`'s much
larger dependency surface for a handful of primitives). `feb_x25519()`/`feb_x25519_base()`
are a hand-rolled RFC 7748 Montgomery ladder built directly on `mbedtls_mpi` bignum
primitives rather than `mbedtls_ecp_mul()`/`mbedtls_ecdh_*` — mbedtls's own Curve25519
public-key validation (`ecp_check_pubkey_mx()`) rejects `u=0` and a few other low-order
points with an error, which `feb_x25519()` (declared `void`, no error path, and required
by `pairing_crypto.h` to be a total function matching RFC 7748 for every 32-byte input)
has no way to signal or safely recover from; see `esp32/main/pairing_crypto.c`'s top
comment and `docs/PROJECT_HISTORY.md`'s step 5 entry for the full
investigation. Both `esp32/` and `flipper/` pairing host tests are green; step 5's
`pairing.c`/`pairing_crypto.c` implementation work is done on both sides. BLE
window/storage integration into `main.c`/`flipper_esp32_over_ble.c` remains a separate
follow-up pass.

## Step 6 additions (runtime session establishment)

`vectors/generate_vectors.py` also generates the runtime-session (docs/PLAN.md step 6)
vectors, built against `esp32/main/session.h`/`session_crypto.h` (and their
byte-identical `flipper/` copies). Runtime records use **AES-256-GCM**, not AES-128-GCM
as `docs/PROTOCOL.md` originally specified — the Flipper's only exported raw-key AES-GCM
primitive is hardcoded to a 256-bit key at the hardware level; see `docs/PLAN.md` step 6's
"AES-128-GCM -> AES-256-GCM protocol revision" entry for the full finding.

- **AES-256-GCM known-answer vector** (`FEB_VEC_GCM_*`): the GCM spec's own "Test Case 16"
  (McGrew & Viega, AES-256 with AAD and a 96-bit IV), fetched directly from hostap's
  `tests/test-aes.c` (raw source, not summarized or transcribed from memory). The
  generator's own from-scratch AES-256 block cipher (S-box derived from the GF(2^8)
  multiplicative inverse + affine transform, not a hard-coded table) and GCM mode are
  self-validated against this vector before being trusted to compute the golden session
  vector below — see the generator's assertions.
- **Golden end-to-end session vector** (`FEB_VEC_SESS_*`): fixed, non-random inputs run
  through the full `hello` -> `hello_ack` -> `client_auth` -> session-key derivation ->
  AAD/nonce construction -> AES-256-GCM pipeline from
  `docs/PROTOCOL.md#session-establishment`, reusing the step-5 golden vector's
  `pairing_secret` as the value both sides already share. Includes the
  `hello`/`hello_ack`/`client_auth` payload and full-record CBOR encodings, one valid
  protected `error` record at sequence 1 and a second at sequence 2 (for an
  implementation's own sequence-continuity/replay tests), and two tampered variants of
  the sequence-1 record (`_BAD_CIPHERTEXT`, `_BAD_AAD`) that must fail AES-256-GCM
  authentication — exercising `docs/PROTOCOL.md`'s "reject modified ciphertext [and]
  modified AAD" requirement. `_BAD_AAD` keeps the original ciphertext/tag but changes the
  outer record's own `sequence` field, so a correct decoder must recompute the AAD from
  the record it actually received rather than trust a cached value.

`tests/esp32/test_session.c` (built/run via `tests/esp32/build_session.ps1`) compiles
`esp32/main/session_crypto.c` (backed by `mbedtls_gcm_*`, already Kconfig-enabled — no new
sdkconfig option was needed) and `esp32/main/session.c` against these vectors: **27/27
checks pass**.

`tests/flipper/test_session.c` (built/run via `tests/flipper/build_session.ps1`) compiles
`flipper/session_crypto.c` and `flipper/session.c` against the same vectors: **42/42
checks pass**. Since the real `furi_hal_crypto_gcm_encrypt_and_tag`/`_decrypt_and_verify`
only exist inside the pinned Unleashed SDK (STM32WB AES1 hardware, no host equivalent),
this host test links `session_crypto.c` against a host-only from-scratch AES-256-GCM
substitute for that one API surface (`tests/flipper/host_shims/furi_hal_crypto_stub.c` —
S-box derived via the GF(2^8) inverse+affine transform, not a hand-copied table, mirroring
`generate_vectors.py`'s own from-scratch AES-256-GCM rationale) so the real, unmodified
production `session_crypto.c`/`session.c` can actually be compiled and exercised on a
desktop; the stub is never linked into the real FAP (`fbt` only sees the real SDK header).
This means the ESP32 host test validates its real production crypto backend end to end,
while the Flipper host test validates the real `session.c` logic against a simulated
backend for that one hardware-only primitive — a known asymmetry, not an oversight.

Both firmwares' `main.c`/`flipper_esp32_over_ble.c` now wire the full
`hello`/`hello_ack`/`client_auth` flow, including the reset-vs-runtime-auth boot decision
and the Flipper's `unknown_board` fallback and auto-connect-when-a-saved-record-exists UX
change — see `docs/PROJECT_HISTORY.md`'s step 6 entry for the full
detail, judgment calls made, and gaps found along the way. `idf.py build` and
`fbt.cmd fap_flipper_esp32_over_ble` both pass clean. Neither board has been flashed with
this code yet — the hardware verification pass is a separate follow-up requiring explicit
user go-ahead, per this project's hardware-safety rule.

## Phase 3 additions (real GPS driver / `gps` capability / wardriving `utc_timestamp_s`)

`vectors/generate_vectors.py` also generates the `gps` capability's vectors (docs/PLAN.md
"Real GPS driver, wardriving fix-dependency, and real wardriving-record timestamps"), built
against `esp32/main/cbor_gps.h`/`.c` (and their byte-identical `flipper/` copies): a
`command` payload (empty and non-empty arguments), and `status` payloads for all three
`state` values (`no_signal`/`acquiring` with no `result`, `fix` with the full
`lat_e7_offset`/`lon_e7_offset`/`fix_quality`/`satellites`/`hdop_e1`/`utc_timestamp_s`/
`altitude_dm_offset` result map -- the seventh field, appended after `utc_timestamp_s`, added
2026-09-12 same session). The existing `<wardriving-record>` vectors also gained the new
`utc_timestamp_s` field (present right after `timestamp_ms`, per the updated field order).

`tests/esp32/test_framing_cbor.c` (via `tests/esp32/build.ps1`, now also compiling
`esp32/main/cbor_gps.c`) and `tests/flipper/test_flipper_codec.c` (via
`tests/flipper/build.ps1`) both exercise these vectors.

`esp32/main/nmea_parser.c`/`.h` -- the pure, zero-ESP-IDF-dependency GGA/RMC sentence
parser underneath `location.c`'s real UART-driven GPS driver -- has its own host-native
test (`tests/esp32/test_location.c`, via `tests/esp32/build_location.ps1`), retained under
its original filename even though it no longer compiles `location.c` directly:
`location.c` now depends on `driver/uart.h` and FreeRTOS (real hardware I/O, a dedicated
background parse task) and is not host-buildable, the same split already established
between `wardriving_record_format.c` (pure, host-tested) and `wardriving_log.c`
(ESP-IDF-only, not host-tested). Test sentences are taken verbatim from
`tools/gps_antenna_last_run.log`, a real hardware-captured NMEA sample, plus three synthetic
sentences shaped to match the NMEA 0183 spec: "no fix yet" GGA, a "void" RMC, and a
negative-altitude GGA (below mean sea level -- not present in the captured log).

`idf.py build` passes clean with the real GPS driver, the new `gps` capability's
command/status dispatch, and wardriving's per-record GPS-fix discard now reading the real
3-state driver instead of the fixed-coordinate stub's binary `has_fix`. Hardware
verification (a real module driving all three `gps` states through a cold-start-to-fix
cycle, and wardriving's discard/resume behavior around a lost/regained fix) is a separate
follow-up requiring explicit user go-ahead, per this project's hardware-safety rule.
