"""Generates tests/vectors/vectors.h from first principles (no CBOR library), directly
implementing the canonical encoding rules in docs/PROTOCOL.md (fixed field order,
shortest-form CBOR, definite-length only), the fragment header layout in
docs/PROTOCOL.md#fragmentation, and the pairing ceremony in docs/PAIRING.md /
docs/PROTOCOL.md#initial-pairing-records. Re-run after editing this file; do not
hand-edit vectors.h.
"""
import hashlib
import hmac
import textwrap

FRAG_HEADER_SIZE = 4
ATT_WRITE_OVERHEAD = 3


def cbor_uint(n: int) -> bytes:
    if n < 24:
        return bytes([0x00 | n])
    if n < 256:
        return bytes([0x18, n])
    if n < 65536:
        return bytes([0x19]) + n.to_bytes(2, "big")
    if n < 2**32:
        return bytes([0x1A]) + n.to_bytes(4, "big")
    return bytes([0x1B]) + n.to_bytes(8, "big")


def _length_prefix(major: int, n: int) -> bytes:
    if n < 24:
        return bytes([(major << 5) | n])
    if n < 256:
        return bytes([(major << 5) | 24, n])
    if n < 65536:
        return bytes([(major << 5) | 25]) + n.to_bytes(2, "big")
    return bytes([(major << 5) | 26]) + n.to_bytes(4, "big")


def cbor_bytes(b: bytes) -> bytes:
    return _length_prefix(2, len(b)) + b


def cbor_text(s: str) -> bytes:
    b = s.encode("ascii")
    return _length_prefix(3, len(b)) + b


def cbor_map_header(n: int) -> bytes:
    return _length_prefix(5, n)


def fragment_capacity(mtu: int) -> int:
    return mtu - ATT_WRITE_OVERHEAD - FRAG_HEADER_SIZE


def fragment_record(record: bytes, mtu: int, message_id: int):
    cap = fragment_capacity(mtu)
    count = (len(record) + cap - 1) // cap
    assert count <= 255
    frags = []
    for i in range(count):
        chunk = record[i * cap:(i + 1) * cap]
        header = bytes([0x00, message_id, i, count])
        frags.append(header + chunk)
    return frags


# ---- error payload: {"code": text, "message": text, "request_id": uint} ----
def error_payload(code: str, message: str, request_id: int) -> bytes:
    out = cbor_map_header(3)
    out += cbor_text("code") + cbor_text(code)
    out += cbor_text("message") + cbor_text(message)
    out += cbor_text("request_id") + cbor_uint(request_id)
    return out


# ---- unencrypted shape: {version, type, session_id, board_id, payload} ----
def unencrypted_record(version: int, type_: str, session_id: bytes, board_id: str, payload: bytes) -> bytes:
    out = cbor_map_header(5)
    out += cbor_text("version") + cbor_uint(version)
    out += cbor_text("type") + cbor_text(type_)
    out += cbor_text("session_id") + cbor_bytes(session_id)
    out += cbor_text("board_id") + cbor_text(board_id)
    out += cbor_text("payload") + payload
    return out


SESSION_ID = bytes.fromhex("0011223344556677")
BOARD_ID = "esp32-c6-test01"
PAYLOAD = error_payload("internal_error", "test vector", 42)
RECORD = unencrypted_record(2, "error", SESSION_ID, BOARD_ID, PAYLOAD)

FRAGS_MTU23 = fragment_record(RECORD, 23, message_id=7)
FRAGS_MTU247 = fragment_record(RECORD, 247, message_id=7)

# ---- malformed fragmentation cases (docs/PROTOCOL.md#fragmentation rejection rules) ----
dup_frags = list(FRAGS_MTU23[:2]) + [FRAGS_MTU23[1]]  # fragment 1 repeated

inconsistent = list(FRAGS_MTU23)
bad_second = bytearray(inconsistent[1])
bad_second[3] = (bad_second[3] + 1) % 256  # fragment_count changes mid-message
inconsistent[1] = bytes(bad_second)

# oversized: header claims fragment_count large enough that count * capacity > 768
capacity23 = fragment_capacity(23)
oversized_count = (768 // capacity23) + 2
oversized_frag0 = bytes([0x00, 9, 0, oversized_count]) + FRAGS_MTU23[0][FRAG_HEADER_SIZE:]

out_of_order = [FRAGS_MTU23[0], FRAGS_MTU23[2] if len(FRAGS_MTU23) > 2 else FRAGS_MTU23[-1]]

# ---- malformed CBOR records (operate on a full reassembled buffer, not fragments) ----
def raw(*parts: bytes) -> bytes:
    out = b""
    for p in parts:
        out += p
    return out


duplicate_key = raw(
    cbor_map_header(5),
    cbor_text("version"), cbor_uint(2),
    cbor_text("version"), cbor_uint(3),  # duplicate key
    cbor_text("session_id"), cbor_bytes(SESSION_ID),
    cbor_text("board_id"), cbor_text(BOARD_ID),
    cbor_text("payload"), PAYLOAD,
)

out_of_order_fields = raw(
    cbor_map_header(5),
    cbor_text("type"), cbor_text("error"),   # type before version: wrong order
    cbor_text("version"), cbor_uint(2),
    cbor_text("session_id"), cbor_bytes(SESSION_ID),
    cbor_text("board_id"), cbor_text(BOARD_ID),
    cbor_text("payload"), PAYLOAD,
)

indefinite_length_map = raw(
    bytes([0xBF]),  # indefinite-length map, major type 5, additional info 31
    cbor_text("version"), cbor_uint(2),
    bytes([0xFF]),  # break
)

missing_field = raw(
    cbor_map_header(4),  # only 4 entries: board_id omitted
    cbor_text("version"), cbor_uint(2),
    cbor_text("type"), cbor_text("error"),
    cbor_text("session_id"), cbor_bytes(SESSION_ID),
    cbor_text("payload"), PAYLOAD,
)

unexpected_type = raw(
    cbor_map_header(5),
    cbor_text("version"), cbor_text("2"),  # version as text, not uint
    cbor_text("type"), cbor_text("error"),
    cbor_text("session_id"), cbor_bytes(SESSION_ID),
    cbor_text("board_id"), cbor_text(BOARD_ID),
    cbor_text("payload"), PAYLOAD,
)

oversized_payload_message = "x" * 500  # pushes the payload map's own encoding past 512 bytes
oversized_payload = error_payload("internal_error", oversized_payload_message, 42)
oversized_payload_record = unencrypted_record(2, "error", SESSION_ID, BOARD_ID, oversized_payload)
assert len(oversized_payload) > 512, "vector must actually exceed FEB_CBOR_MAX_PAYLOAD"


# =====================================================================================
# Step 5: pairing primitive (RFC known-answer) and golden end-to-end pairing vectors.
# =====================================================================================

# ---- X25519 (RFC 7748 section 5), pure Python, self-validated below against the RFC's
# own published test vectors before being trusted to generate the golden pairing vector.
# This is the same Montgomery-ladder algorithm curve25519-donna and other compact
# reference implementations use; it exists here only to generate test data, not as
# production code. ----
_P = 2**255 - 19
_A24 = 121665


def _decode_scalar(k: bytes) -> int:
    k = bytearray(k)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    return int.from_bytes(bytes(k), "little")


def _decode_u(u: bytes) -> int:
    val = int.from_bytes(u, "little")
    return val & ((1 << 255) - 1)


def _encode_u(u: int) -> bytes:
    return (u % _P).to_bytes(32, "little")


def x25519(k_bytes: bytes, u_bytes: bytes) -> bytes:
    k = _decode_scalar(k_bytes)
    x1 = _decode_u(u_bytes)
    x2, z2 = 1, 0
    x3, z3 = x1, 1
    swap = 0
    for t in reversed(range(255)):
        k_t = (k >> t) & 1
        swap ^= k_t
        if swap:
            x2, x3 = x3, x2
            z2, z3 = z3, z2
        swap = k_t

        A = (x2 + z2) % _P
        AA = (A * A) % _P
        B = (x2 - z2) % _P
        BB = (B * B) % _P
        E = (AA - BB) % _P
        C = (x3 + z3) % _P
        D = (x3 - z3) % _P
        DA = (D * A) % _P
        CB = (C * B) % _P
        x3 = (DA + CB) % _P
        x3 = (x3 * x3) % _P
        z3 = (DA - CB) % _P
        z3 = (z3 * z3) % _P
        z3 = (x1 * z3) % _P
        x2 = (AA * BB) % _P
        z2 = (E * (AA + _A24 * E)) % _P
    if swap:
        x2, x3 = x3, x2
        z2, z3 = z3, z2
    return _encode_u((x2 * pow(z2, _P - 2, _P)) % _P)


def x25519_base(k_bytes: bytes) -> bytes:
    return x25519(k_bytes, (9).to_bytes(32, "little"))


# RFC 7748 section 5.2 test vectors (fetched from the published RFC text; not
# transcribed from memory).
_RFC7748_TC1_SCALAR = bytes.fromhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4")
_RFC7748_TC1_U = bytes.fromhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")
_RFC7748_TC1_OUT = bytes.fromhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
_RFC7748_TC2_SCALAR = bytes.fromhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d")
_RFC7748_TC2_U = bytes.fromhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")
_RFC7748_TC2_OUT = bytes.fromhex("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957")
_RFC7748_ALICE_PRIV = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
_RFC7748_ALICE_PUB = bytes.fromhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
_RFC7748_BOB_PRIV = bytes.fromhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
_RFC7748_BOB_PUB = bytes.fromhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
_RFC7748_SHARED = bytes.fromhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")

assert x25519(_RFC7748_TC1_SCALAR, _RFC7748_TC1_U) == _RFC7748_TC1_OUT, "x25519 impl fails RFC7748 test case 1"
assert x25519(_RFC7748_TC2_SCALAR, _RFC7748_TC2_U) == _RFC7748_TC2_OUT, "x25519 impl fails RFC7748 test case 2"
assert x25519_base(_RFC7748_ALICE_PRIV) == _RFC7748_ALICE_PUB, "x25519_base fails RFC7748 Alice"
assert x25519_base(_RFC7748_BOB_PRIV) == _RFC7748_BOB_PUB, "x25519_base fails RFC7748 Bob"
assert x25519(_RFC7748_ALICE_PRIV, _RFC7748_BOB_PUB) == _RFC7748_SHARED, "x25519 ECDH fails RFC7748 (Alice side)"
assert x25519(_RFC7748_BOB_PRIV, _RFC7748_ALICE_PUB) == _RFC7748_SHARED, "x25519 ECDH fails RFC7748 (Bob side)"

# u = 0 is the identity/all-zero input; X25519 must yield an all-zero output for any
# scalar, which is exactly the case feb_is_all_zero() exists to reject
# (docs/PAIRING.md's "reject an all-zero X25519 shared secret").
_ZERO32 = bytes(32)
_x25519_zero_output = x25519(_RFC7748_TC1_SCALAR, _ZERO32)
assert _x25519_zero_output == _ZERO32, "x25519(k, 0) must be all-zero"


# ---- SHA-256, HMAC-SHA-256, HKDF-SHA-256: computed with Python's own hashlib/hmac
# (stdlib, not hand-rolled), so only RFC-published *inputs* are transcribed here, never
# RFC-published *outputs*. ----
def hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt if salt else bytes(32), ikm, hashlib.sha256).digest()
    t = b""
    okm = b""
    counter = 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([counter]), hashlib.sha256).digest()
        okm += t
        counter += 1
    return okm[:length]


_SHA256_ABC_INPUT = b"abc"
_SHA256_ABC_DIGEST = hashlib.sha256(_SHA256_ABC_INPUT).digest()

_HMAC_KEY = bytes.fromhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")  # RFC 4231 TC1
_HMAC_DATA = bytes.fromhex("4869205468657265")  # "Hi There"
_HMAC_MAC = hmac.new(_HMAC_KEY, _HMAC_DATA, hashlib.sha256).digest()

_HKDF_IKM = bytes.fromhex("0b" * 22)  # RFC 5869 test case 1
_HKDF_SALT = bytes.fromhex("000102030405060708090a0b0c")
_HKDF_INFO = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9")
_HKDF_OKM_L32 = hkdf_sha256(_HKDF_SALT, _HKDF_IKM, _HKDF_INFO, 32)


# ---- pairing record payload / envelope encoders, matching pairing.h field order ----
def pair_init_payload(pairing_epoch: bytes, device_nonce: bytes, esp32_pub: bytes) -> bytes:
    out = cbor_map_header(3)
    out += cbor_text("pairing_epoch") + cbor_bytes(pairing_epoch)
    out += cbor_text("device_nonce") + cbor_bytes(device_nonce)
    out += cbor_text("esp32_public_key") + cbor_bytes(esp32_pub)
    return out


def pair_reply_payload(client_nonce: bytes, flipper_pub: bytes, confirmation: bytes) -> bytes:
    out = cbor_map_header(3)
    out += cbor_text("client_nonce") + cbor_bytes(client_nonce)
    out += cbor_text("flipper_public_key") + cbor_bytes(flipper_pub)
    out += cbor_text("confirmation") + cbor_bytes(confirmation)
    return out


def pair_confirm_payload(confirmation: bytes) -> bytes:
    out = cbor_map_header(1)
    out += cbor_text("confirmation") + cbor_bytes(confirmation)
    return out


def pair_complete_payload(confirmation: bytes) -> bytes:
    out = cbor_map_header(1)
    out += cbor_text("confirmation") + cbor_bytes(confirmation)
    return out


def pairing_envelope(version: int, type_: str, board_id: str, payload: bytes) -> bytes:
    out = cbor_map_header(4)
    out += cbor_text("version") + cbor_uint(version)
    out += cbor_text("type") + cbor_text(type_)
    out += cbor_text("board_id") + cbor_text(board_id)
    out += cbor_text("payload") + payload
    return out


def pairing_transcript(version: int, service_uuid: bytes, board_id: str, pairing_epoch: bytes,
                        client_nonce: bytes, device_nonce: bytes, esp32_pub: bytes, flipper_pub: bytes) -> bytes:
    out = cbor_map_header(8)
    out += cbor_text("version") + cbor_uint(version)
    out += cbor_text("service_uuid") + cbor_bytes(service_uuid)
    out += cbor_text("board_id") + cbor_text(board_id)
    out += cbor_text("pairing_epoch") + cbor_bytes(pairing_epoch)
    out += cbor_text("client_nonce") + cbor_bytes(client_nonce)
    out += cbor_text("device_nonce") + cbor_bytes(device_nonce)
    out += cbor_text("esp32_public_key") + cbor_bytes(esp32_pub)
    out += cbor_text("flipper_public_key") + cbor_bytes(flipper_pub)
    return out


# ---- golden end-to-end pairing vector: fixed, deterministic (not random) inputs run
# through the whole T -> K_shared -> K_confirm -> pairing_secret -> confirmation-tag
# pipeline from docs/PROTOCOL.md#initial-pairing-records, to catch a wiring bug (wrong
# field order, wrong concatenation order, wrong domain-separation string) that
# per-primitive vectors alone cannot catch -- exactly the class of bug step 3 found
# twice in the assembled code rather than the primitives (docs/PLAN.md step 5
# "Test-vector strategy"). Inputs are derived from fixed labels via SHA-256 rather than
# hand-picked, so they are reproducible from this script's own source with no external
# randomness and no hand-transcribed hex. ----
PAIR_SERVICE_UUID = bytes.fromhex("9c3f7e6af4034c319ea258a7a20fb811")
PAIR_VERSION = 2
PAIR_BOARD_ID = "esp32-c6-golden01"
PAIR_ESP32_PRIVATE = hashlib.sha256(b"feb-golden-esp32-private").digest()
PAIR_FLIPPER_PRIVATE = hashlib.sha256(b"feb-golden-flipper-private").digest()
PAIR_EPOCH = hashlib.sha256(b"feb-golden-pairing-epoch").digest()[:16]
PAIR_CLIENT_NONCE = hashlib.sha256(b"feb-golden-client-nonce").digest()[:16]
PAIR_DEVICE_NONCE = hashlib.sha256(b"feb-golden-device-nonce").digest()[:16]

PAIR_ESP32_PUBLIC = x25519_base(PAIR_ESP32_PRIVATE)
PAIR_FLIPPER_PUBLIC = x25519_base(PAIR_FLIPPER_PRIVATE)
PAIR_K_SHARED = x25519(PAIR_ESP32_PRIVATE, PAIR_FLIPPER_PUBLIC)
assert PAIR_K_SHARED == x25519(PAIR_FLIPPER_PRIVATE, PAIR_ESP32_PUBLIC), "ECDH must agree from both sides"
assert not all(b == 0 for b in PAIR_K_SHARED), "golden vector's shared secret must not be degenerate"

PAIR_TRANSCRIPT = pairing_transcript(
    PAIR_VERSION, PAIR_SERVICE_UUID, PAIR_BOARD_ID, PAIR_EPOCH,
    PAIR_CLIENT_NONCE, PAIR_DEVICE_NONCE, PAIR_ESP32_PUBLIC, PAIR_FLIPPER_PUBLIC,
)

PAIR_K_CONFIRM = hkdf_sha256(
    salt=PAIR_EPOCH, ikm=PAIR_K_SHARED,
    info=b"flipper-esp32-over-ble/v2/pair-confirm", length=32,
)
PAIR_SECRET = hkdf_sha256(
    salt=PAIR_EPOCH + PAIR_CLIENT_NONCE + PAIR_DEVICE_NONCE, ikm=PAIR_K_SHARED,
    info=b"flipper-esp32-over-ble/v2/x25519-pairing-secret" + PAIR_BOARD_ID.encode("ascii"), length=32,
)
PAIR_FLIPPER_CONFIRM = hmac.new(PAIR_K_CONFIRM, b"flipper-confirm" + PAIR_TRANSCRIPT, hashlib.sha256).digest()
PAIR_ESP32_CONFIRM = hmac.new(PAIR_K_CONFIRM, b"esp32-confirm" + PAIR_TRANSCRIPT, hashlib.sha256).digest()
PAIR_COMPLETE_TAG = hmac.new(PAIR_K_CONFIRM, b"complete" + PAIR_TRANSCRIPT, hashlib.sha256).digest()[:16]

PAIR_INIT_PAYLOAD = pair_init_payload(PAIR_EPOCH, PAIR_DEVICE_NONCE, PAIR_ESP32_PUBLIC)
PAIR_INIT_RECORD = pairing_envelope(PAIR_VERSION, "pair_init", PAIR_BOARD_ID, PAIR_INIT_PAYLOAD)
PAIR_REPLY_PAYLOAD = pair_reply_payload(PAIR_CLIENT_NONCE, PAIR_FLIPPER_PUBLIC, PAIR_FLIPPER_CONFIRM)
PAIR_REPLY_RECORD = pairing_envelope(PAIR_VERSION, "pair_reply", PAIR_BOARD_ID, PAIR_REPLY_PAYLOAD)
PAIR_CONFIRM_PAYLOAD = pair_confirm_payload(PAIR_ESP32_CONFIRM)
PAIR_CONFIRM_RECORD = pairing_envelope(PAIR_VERSION, "pair_confirm", PAIR_BOARD_ID, PAIR_CONFIRM_PAYLOAD)
PAIR_COMPLETE_PAYLOAD = pair_complete_payload(PAIR_COMPLETE_TAG)
PAIR_COMPLETE_RECORD = pairing_envelope(PAIR_VERSION, "pair_complete", PAIR_BOARD_ID, PAIR_COMPLETE_PAYLOAD)

PAIR_ERROR_PAYLOAD = error_payload("pairing_failed", "bad confirmation", 0)
PAIR_ERROR_RECORD = pairing_envelope(PAIR_VERSION, "error", PAIR_BOARD_ID, PAIR_ERROR_PAYLOAD)


# =====================================================================================
# Step 6: runtime session establishment (hello/hello_ack/client_auth) and AES-256-GCM
# protected records. AES-256-GCM (not AES-128-GCM as docs/PROTOCOL.md originally
# specified) because the Flipper's only exported raw-key AES-GCM primitive
# (furi_hal_crypto_gcm_encrypt_and_tag/_decrypt_and_verify) is hardcoded to a 256-bit key
# at the hardware level -- see docs/PLAN.md step 6.
# =====================================================================================

# ---- AES-256 + GCM, pure Python, self-validated below against a directly-fetched GCM
# spec test vector (McGrew & Viega "Test Case 16", via hostap's test-aes.c, itself
# citing the GCM spec) before being trusted to generate the golden session vector. The
# S-box is derived from the GF(2^8) multiplicative inverse + affine transform (FIPS-197
# section 5.1.1) rather than hard-coded as a 256-entry table, to avoid a transcription
# error -- same rationale as this file's from-scratch X25519 above. This exists only to
# generate test data, not as production code. ----
def _gf_mul8(a: int, b: int) -> int:
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p


def _gf_inv8(a: int) -> int:
    if a == 0:
        return 0
    for b in range(1, 256):
        if _gf_mul8(a, b) == 1:
            return b
    raise ValueError("no inverse")


def _build_sbox():
    sbox = [0] * 256
    for a in range(256):
        inv = _gf_inv8(a)
        b = inv
        for shift in (1, 2, 3, 4):
            x = ((inv << shift) | (inv >> (8 - shift))) & 0xFF
            b ^= x
        b ^= 0x63
        sbox[a] = b
    return sbox


_SBOX = _build_sbox()
_RCON = [0x01]
for _i in range(1, 14):
    _RCON.append(_gf_mul8(_RCON[-1], 2))


def _key_expansion_256(key: bytes):
    Nk, Nr = 8, 14
    w = [list(key[4 * i:4 * i + 4]) for i in range(Nk)]
    for i in range(Nk, 4 * (Nr + 1)):
        temp = list(w[i - 1])
        if i % Nk == 0:
            temp = temp[1:] + temp[:1]
            temp = [_SBOX[b] for b in temp]
            temp[0] ^= _RCON[i // Nk - 1]
        elif i % Nk == 4:
            temp = [_SBOX[b] for b in temp]
        w.append([w[i - Nk][j] ^ temp[j] for j in range(4)])
    round_keys = []
    for r in range(Nr + 1):
        rk = []
        for c in range(4):
            rk += w[r * 4 + c]
        round_keys.append(bytes(rk))
    return round_keys


def _bytes_to_state(block: bytes):
    return [[block[c * 4 + r] for c in range(4)] for r in range(4)]


def _state_to_bytes(state) -> bytes:
    out = bytearray(16)
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = state[r][c]
    return bytes(out)


def _add_round_key(state, rk):
    return [[state[r][c] ^ rk[c * 4 + r] for c in range(4)] for r in range(4)]


def _sub_bytes(state):
    return [[_SBOX[state[r][c]] for c in range(4)] for r in range(4)]


def _shift_rows(state):
    return [state[r][r:] + state[r][:r] for r in range(4)]


def _mix_columns(state):
    new_state = [[0] * 4 for _ in range(4)]
    for c in range(4):
        a = [state[r][c] for r in range(4)]
        new_state[0][c] = _gf_mul8(a[0], 2) ^ _gf_mul8(a[1], 3) ^ a[2] ^ a[3]
        new_state[1][c] = a[0] ^ _gf_mul8(a[1], 2) ^ _gf_mul8(a[2], 3) ^ a[3]
        new_state[2][c] = a[0] ^ a[1] ^ _gf_mul8(a[2], 2) ^ _gf_mul8(a[3], 3)
        new_state[3][c] = _gf_mul8(a[0], 3) ^ a[1] ^ a[2] ^ _gf_mul8(a[3], 2)
    return new_state


def aes256_encrypt_block(key: bytes, block: bytes) -> bytes:
    assert len(key) == 32 and len(block) == 16
    round_keys = _key_expansion_256(key)
    Nr = 14
    state = _bytes_to_state(block)
    state = _add_round_key(state, round_keys[0])
    for rnd in range(1, Nr):
        state = _sub_bytes(state)
        state = _shift_rows(state)
        state = _mix_columns(state)
        state = _add_round_key(state, round_keys[rnd])
    state = _sub_bytes(state)
    state = _shift_rows(state)
    state = _add_round_key(state, round_keys[Nr])
    return _state_to_bytes(state)


_GCM_R = 0xE1000000000000000000000000000000  # reduction constant (NIST SP 800-38D 6.3)


def _gf128_mul(x_int: int, y_int: int) -> int:
    z = 0
    v = y_int
    for i in range(128):
        if (x_int >> (127 - i)) & 1:
            z ^= v
        lsb = v & 1
        v >>= 1
        if lsb:
            v ^= _GCM_R
    return z


def _ghash(h_int: int, data: bytes) -> int:
    y = 0
    for i in range(0, len(data), 16):
        block = data[i:i + 16]
        if len(block) < 16:
            block = block + b"\x00" * (16 - len(block))
        y ^= int.from_bytes(block, "big")
        y = _gf128_mul(y, h_int)
    return y


def _inc32(block: bytes) -> bytes:
    prefix = block[:12]
    ctr = (int.from_bytes(block[12:16], "big") + 1) & 0xFFFFFFFF
    return prefix + ctr.to_bytes(4, "big")


def _gctr(key: bytes, icb: bytes, data: bytes) -> bytes:
    if not data:
        return b""
    out = bytearray()
    cb = icb
    for i in range(0, len(data), 16):
        ks = aes256_encrypt_block(key, cb)
        block = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(block, ks[:len(block)]))
        cb = _inc32(cb)
    return bytes(out)


def gcm_encrypt(key: bytes, iv: bytes, aad: bytes, plaintext: bytes):
    assert len(iv) == 12, "this protocol only ever uses 96-bit nonces"
    h_int = int.from_bytes(aes256_encrypt_block(key, bytes(16)), "big")
    j0 = iv + b"\x00\x00\x00\x01"
    ciphertext = _gctr(key, _inc32(j0), plaintext)
    u = (-len(ciphertext)) % 16
    v = (-len(aad)) % 16
    s_input = (aad + b"\x00" * v + ciphertext + b"\x00" * u +
               (len(aad) * 8).to_bytes(8, "big") + (len(ciphertext) * 8).to_bytes(8, "big"))
    s = _ghash(h_int, s_input)
    t = _gctr(key, j0, s.to_bytes(16, "big"))
    return ciphertext, t[:16]


# GCM spec Test Case 16 (AES-256, AAD + plaintext, 96-bit IV) -- fetched directly from
# hostap's tests/test-aes.c (raw source, not summarized/transcribed from memory), which
# itself cites the GCM spec (McGrew & Viega).
_GCM_TC16_KEY = bytes.fromhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308")
_GCM_TC16_IV = bytes.fromhex("cafebabefacedbaddecaf888")
_GCM_TC16_AAD = bytes.fromhex("feedfacedeadbeeffeedfacedeadbeefabaddad2")
_GCM_TC16_PT = bytes.fromhex(
    "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a7"
    "21c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39")
_GCM_TC16_CT = bytes.fromhex(
    "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1a"
    "a8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662")
_GCM_TC16_TAG = bytes.fromhex("76fc6ece0f4e1768cddf8853bb2d551b")

_gcm_tc16_ct, _gcm_tc16_tag = gcm_encrypt(_GCM_TC16_KEY, _GCM_TC16_IV, _GCM_TC16_AAD, _GCM_TC16_PT)
assert _gcm_tc16_ct == _GCM_TC16_CT, "AES-256-GCM impl fails GCM spec Test Case 16 (ciphertext)"
assert _gcm_tc16_tag == _GCM_TC16_TAG, "AES-256-GCM impl fails GCM spec Test Case 16 (tag)"


# ---- hello/hello_ack/client_auth payloads, matching session.h field order ----
def hello_payload(client_nonce: bytes) -> bytes:
    return cbor_map_header(1) + cbor_text("client_nonce") + cbor_bytes(client_nonce)


def hello_ack_payload(device_nonce: bytes, proof: bytes) -> bytes:
    out = cbor_map_header(2)
    out += cbor_text("device_nonce") + cbor_bytes(device_nonce)
    out += cbor_text("proof") + cbor_bytes(proof)
    return out


def client_auth_payload(proof: bytes) -> bytes:
    return cbor_map_header(1) + cbor_text("proof") + cbor_bytes(proof)


def session_transcript(version: int, board_id: str, session_id: bytes,
                        client_nonce: bytes, device_nonce: bytes) -> bytes:
    out = cbor_map_header(5)
    out += cbor_text("version") + cbor_uint(version)
    out += cbor_text("board_id") + cbor_text(board_id)
    out += cbor_text("session_id") + cbor_bytes(session_id)
    out += cbor_text("client_nonce") + cbor_bytes(client_nonce)
    out += cbor_text("device_nonce") + cbor_bytes(device_nonce)
    return out


def session_aad(version: int, type_: str, session_id: bytes, sequence: int, board_id: str) -> bytes:
    out = cbor_map_header(5)
    out += cbor_text("version") + cbor_uint(version)
    out += cbor_text("type") + cbor_text(type_)
    out += cbor_text("session_id") + cbor_bytes(session_id)
    out += cbor_text("sequence") + cbor_uint(sequence)
    out += cbor_text("board_id") + cbor_text(board_id)
    return out


def session_nonce(session_id: bytes, direction: int, sequence: int) -> bytes:
    assert len(session_id) == 8
    return session_id + bytes([direction]) + (sequence & 0xFFFFFF).to_bytes(3, "big")


def protected_record(version: int, type_: str, session_id: bytes, board_id: str,
                      sequence: int, ciphertext: bytes, tag: bytes) -> bytes:
    out = cbor_map_header(7)
    out += cbor_text("version") + cbor_uint(version)
    out += cbor_text("type") + cbor_text(type_)
    out += cbor_text("session_id") + cbor_bytes(session_id)
    out += cbor_text("board_id") + cbor_text(board_id)
    out += cbor_text("sequence") + cbor_uint(sequence)
    out += cbor_text("ciphertext") + cbor_bytes(ciphertext)
    out += cbor_text("tag") + cbor_bytes(tag)
    return out


DIR_FLIPPER_TO_ESP32 = 0x00
DIR_ESP32_TO_FLIPPER = 0x01

# ---- golden end-to-end session vector: fixed, deterministic inputs run through the
# full hello -> hello_ack -> client_auth -> session_key -> one protected record
# pipeline, for the same reason step 5's golden pairing vector exists -- per-primitive
# vectors alone don't catch a wiring bug (field order, concatenation order,
# domain-separation string) in the assembled derivation. Reuses this file's existing
# golden PAIR_SECRET as the stored pairing_secret both sides already agree on. ----
SESS_VERSION = 2
SESS_BOARD_ID = "esp32-c6-golden01"  # same board_id as the golden pairing vector
SESS_PAIRING_SECRET = PAIR_SECRET     # the pairing_secret both sides already share
SESS_SESSION_ID = hashlib.sha256(b"feb-golden-session-id").digest()[:8]
SESS_CLIENT_NONCE = hashlib.sha256(b"feb-golden-session-client-nonce").digest()[:16]
SESS_DEVICE_NONCE = hashlib.sha256(b"feb-golden-session-device-nonce").digest()[:16]

SESS_TRANSCRIPT = session_transcript(SESS_VERSION, SESS_BOARD_ID, SESS_SESSION_ID,
                                      SESS_CLIENT_NONCE, SESS_DEVICE_NONCE)
SESS_FLIPPER_PROOF = hmac.new(SESS_PAIRING_SECRET, b"runtime-flipper" + SESS_TRANSCRIPT, hashlib.sha256).digest()[:16]
SESS_ESP32_PROOF = hmac.new(SESS_PAIRING_SECRET, b"runtime-esp32" + SESS_TRANSCRIPT, hashlib.sha256).digest()[:16]

SESS_KEY = hkdf_sha256(
    salt=SESS_CLIENT_NONCE + SESS_DEVICE_NONCE, ikm=SESS_PAIRING_SECRET,
    info=b"flipper-esp32-over-ble/v2/aes-256-gcm" + SESS_BOARD_ID.encode("ascii") + SESS_SESSION_ID,
    length=32,
)
assert len(SESS_KEY) == 32

SESS_HELLO_PAYLOAD = hello_payload(SESS_CLIENT_NONCE)
SESS_HELLO_RECORD = unencrypted_record(SESS_VERSION, "hello", SESS_SESSION_ID, SESS_BOARD_ID, SESS_HELLO_PAYLOAD)
SESS_HELLO_ACK_PAYLOAD = hello_ack_payload(SESS_DEVICE_NONCE, SESS_FLIPPER_PROOF)
SESS_HELLO_ACK_RECORD = unencrypted_record(SESS_VERSION, "hello_ack", SESS_SESSION_ID, SESS_BOARD_ID, SESS_HELLO_ACK_PAYLOAD)
SESS_CLIENT_AUTH_PAYLOAD = client_auth_payload(SESS_ESP32_PROOF)
SESS_CLIENT_AUTH_RECORD = unencrypted_record(SESS_VERSION, "client_auth", SESS_SESSION_ID, SESS_BOARD_ID, SESS_CLIENT_AUTH_PAYLOAD)

# One protected `error` record, ESP32 -> Flipper, sequence 1 (the first protected record
# of a fresh session per docs/PROTOCOL.md's "begin protected records at sequence 1").
SESS_PROT1_PAYLOAD = error_payload("internal_error", "session golden vector", 7)
SESS_PROT1_SEQ = 1
SESS_PROT1_AAD = session_aad(SESS_VERSION, "error", SESS_SESSION_ID, SESS_PROT1_SEQ, SESS_BOARD_ID)
SESS_PROT1_NONCE = session_nonce(SESS_SESSION_ID, DIR_ESP32_TO_FLIPPER, SESS_PROT1_SEQ)
SESS_PROT1_CT, SESS_PROT1_TAG = gcm_encrypt(SESS_KEY, SESS_PROT1_NONCE, SESS_PROT1_AAD, SESS_PROT1_PAYLOAD)
SESS_PROT1_RECORD = protected_record(SESS_VERSION, "error", SESS_SESSION_ID, SESS_BOARD_ID,
                                      SESS_PROT1_SEQ, SESS_PROT1_CT, SESS_PROT1_TAG)

# A second valid protected record at sequence 2, same direction -- lets an implementation's
# own host test exercise sequence continuity/replay-rejection policy against two genuinely
# valid, independently-derived ciphertexts rather than hand-mutating one record.
SESS_PROT2_PAYLOAD = error_payload("internal_error", "session golden vector seq2", 8)
SESS_PROT2_SEQ = 2
SESS_PROT2_AAD = session_aad(SESS_VERSION, "error", SESS_SESSION_ID, SESS_PROT2_SEQ, SESS_BOARD_ID)
SESS_PROT2_NONCE = session_nonce(SESS_SESSION_ID, DIR_ESP32_TO_FLIPPER, SESS_PROT2_SEQ)
SESS_PROT2_CT, SESS_PROT2_TAG = gcm_encrypt(SESS_KEY, SESS_PROT2_NONCE, SESS_PROT2_AAD, SESS_PROT2_PAYLOAD)
SESS_PROT2_RECORD = protected_record(SESS_VERSION, "error", SESS_SESSION_ID, SESS_BOARD_ID,
                                      SESS_PROT2_SEQ, SESS_PROT2_CT, SESS_PROT2_TAG)

# Tampered variants of the sequence-1 record: same envelope shape, but must fail
# AES-256-GCM authentication -- exercising docs/PROTOCOL.md's "reject modified ciphertext
# [and] modified AAD" requirement.
_tampered_ct = bytearray(SESS_PROT1_CT)
_tampered_ct[0] ^= 0x01
SESS_PROT1_RECORD_BAD_CIPHERTEXT = protected_record(
    SESS_VERSION, "error", SESS_SESSION_ID, SESS_BOARD_ID, SESS_PROT1_SEQ, bytes(_tampered_ct), SESS_PROT1_TAG)

# Same ciphertext/tag, but the outer record's own `sequence` field (part of the AAD) is
# changed -- a decoder must recompute the AAD from the record it actually received, so
# this must also fail authentication, not just "decode to garbage".
SESS_PROT1_RECORD_BAD_AAD = protected_record(
    SESS_VERSION, "error", SESS_SESSION_ID, SESS_BOARD_ID, SESS_PROT1_SEQ + 1, SESS_PROT1_CT, SESS_PROT1_TAG)


def c_bytes(name: str, data: bytes) -> str:
    hex_bytes = ", ".join(f"0x{b:02x}" for b in data)
    wrapped = textwrap.fill(hex_bytes, width=96, initial_indent="    ", subsequent_indent="    ")
    # {name}_LEN must be a genuine compile-time integer constant expression (not just a
    # `static const` object, which ISO C does not treat as one) because generated array
    # aggregates below (e.g. FEB_VEC_..._LENS[]) use it as a static initializer element.
    return f"static const uint8_t {name}[] = {{\n{wrapped}\n}};\n#define {name}_LEN sizeof({name})\n"


def c_frag_array(name: str, frags) -> str:
    parts = [c_bytes(f"{name}_{i}", f) for i, f in enumerate(frags)]
    list_line = f"static const uint8_t *const {name}[] = {{" + ", ".join(f"{name}_{i}" for i in range(len(frags))) + "};\n"
    len_line = f"static const size_t {name}_LENS[] = {{" + ", ".join(f"{name}_{i}_LEN" for i in range(len(frags))) + "};\n"
    count_line = f"static const size_t {name}_COUNT = {len(frags)};\n"
    return "".join(parts) + list_line + len_line + count_line


with open("vectors.h", "w") as f:
    f.write("/* Generated by generate_vectors.py. Do not hand-edit. */\n")
    f.write("#ifndef FEB_TEST_VECTORS_H\n#define FEB_TEST_VECTORS_H\n\n")
    f.write("#include <stdint.h>\n#include <stddef.h>\n\n")

    f.write("/* Single valid unencrypted `error` record, reassembled form. */\n")
    f.write(c_bytes("FEB_VEC_RECORD", RECORD))
    f.write("\n")

    f.write("/* Same record, fragmented at ATT MTU 23 and 247 (message_id = 7). */\n")
    f.write(c_frag_array("FEB_VEC_FRAGS_MTU23", FRAGS_MTU23))
    f.write("\n")
    f.write(c_frag_array("FEB_VEC_FRAGS_MTU247", FRAGS_MTU247))
    f.write("\n")

    f.write("/* Malformed fragmentation cases (docs/PROTOCOL.md#fragmentation). */\n")
    f.write(c_frag_array("FEB_VEC_DUP_FRAGS", dup_frags))
    f.write(c_frag_array("FEB_VEC_INCONSISTENT_COUNT_FRAGS", inconsistent))
    f.write(c_bytes("FEB_VEC_OVERSIZED_FRAG0", oversized_frag0))
    f.write(c_frag_array("FEB_VEC_OUT_OF_ORDER_FRAGS", out_of_order))
    f.write("\n")

    f.write("/* Malformed CBOR records (full reassembled buffers). */\n")
    f.write(c_bytes("FEB_VEC_DUPLICATE_KEY", duplicate_key))
    f.write(c_bytes("FEB_VEC_OUT_OF_ORDER_FIELDS", out_of_order_fields))
    f.write(c_bytes("FEB_VEC_INDEFINITE_LENGTH_MAP", indefinite_length_map))
    f.write(c_bytes("FEB_VEC_MISSING_FIELD", missing_field))
    f.write(c_bytes("FEB_VEC_UNEXPECTED_TYPE", unexpected_type))
    f.write(c_bytes("FEB_VEC_OVERSIZED_PAYLOAD_RECORD", oversized_payload_record))
    f.write("\n")

    f.write("/* ---- Step 5: pairing crypto primitives (docs/PLAN.md step 5) ---- */\n\n")

    f.write("/* X25519 (RFC 7748 section 5.2), two independent scalar/u-coordinate cases. */\n")
    f.write(c_bytes("FEB_VEC_X25519_TC1_SCALAR", _RFC7748_TC1_SCALAR))
    f.write(c_bytes("FEB_VEC_X25519_TC1_U", _RFC7748_TC1_U))
    f.write(c_bytes("FEB_VEC_X25519_TC1_OUTPUT", _RFC7748_TC1_OUT))
    f.write(c_bytes("FEB_VEC_X25519_TC2_SCALAR", _RFC7748_TC2_SCALAR))
    f.write(c_bytes("FEB_VEC_X25519_TC2_U", _RFC7748_TC2_U))
    f.write(c_bytes("FEB_VEC_X25519_TC2_OUTPUT", _RFC7748_TC2_OUT))
    f.write("\n")

    f.write("/* X25519 (RFC 7748 section 6.1) Diffie-Hellman example: exercises base-point\n")
    f.write("   public-key generation and that ECDH agrees computed from either side. */\n")
    f.write(c_bytes("FEB_VEC_X25519_ALICE_PRIVATE", _RFC7748_ALICE_PRIV))
    f.write(c_bytes("FEB_VEC_X25519_ALICE_PUBLIC", _RFC7748_ALICE_PUB))
    f.write(c_bytes("FEB_VEC_X25519_BOB_PRIVATE", _RFC7748_BOB_PRIV))
    f.write(c_bytes("FEB_VEC_X25519_BOB_PUBLIC", _RFC7748_BOB_PUB))
    f.write(c_bytes("FEB_VEC_X25519_SHARED", _RFC7748_SHARED))
    f.write("\n")

    f.write("/* X25519 with an all-zero u-coordinate always yields an all-zero output for any\n")
    f.write("   scalar -- the case feb_is_all_zero() exists to reject (docs/PAIRING.md). */\n")
    f.write(c_bytes("FEB_VEC_X25519_ZERO_U", _ZERO32))
    f.write(c_bytes("FEB_VEC_X25519_ZERO_OUTPUT", _x25519_zero_output))
    f.write("\n")

    f.write("/* SHA-256 (FIPS 180-4 short message example). */\n")
    f.write(c_bytes("FEB_VEC_SHA256_INPUT", _SHA256_ABC_INPUT))
    f.write(c_bytes("FEB_VEC_SHA256_DIGEST", _SHA256_ABC_DIGEST))
    f.write("\n")

    f.write("/* HMAC-SHA-256 (RFC 4231 test case 1). */\n")
    f.write(c_bytes("FEB_VEC_HMAC_KEY", _HMAC_KEY))
    f.write(c_bytes("FEB_VEC_HMAC_DATA", _HMAC_DATA))
    f.write(c_bytes("FEB_VEC_HMAC_MAC", _HMAC_MAC))
    f.write("\n")

    f.write("/* HKDF-SHA-256 (RFC 5869 test case 1 inputs; output requested at L=32, this\n")
    f.write("   protocol's FEB_HKDF_MAX_LEN, computed directly rather than truncated from the\n")
    f.write("   RFC's published L=42 OKM -- HKDF-Expand output is prefix-stable in L, so this\n")
    f.write("   is the same value, just not obtained by hand-splicing a longer hex string). */\n")
    f.write(c_bytes("FEB_VEC_HKDF_IKM", _HKDF_IKM))
    f.write(c_bytes("FEB_VEC_HKDF_SALT", _HKDF_SALT))
    f.write(c_bytes("FEB_VEC_HKDF_INFO", _HKDF_INFO))
    f.write(c_bytes("FEB_VEC_HKDF_OKM_L32", _HKDF_OKM_L32))
    f.write("\n")

    f.write("/* ---- Golden end-to-end pairing vector (docs/PLAN.md step 5 \"Test-vector\n")
    f.write("   strategy\"): fixed, non-random inputs run through the full pairing derivation\n")
    f.write("   pipeline, catching a wiring bug (field order, concatenation order,\n")
    f.write("   domain-separation string) that per-primitive vectors alone cannot. This vector\n")
    f.write("   is test-only -- real pairing attempts always use genuinely random keys/nonces. */\n")
    f.write(c_bytes("FEB_VEC_PAIR_SERVICE_UUID", PAIR_SERVICE_UUID))
    f.write(f'#define FEB_VEC_PAIR_BOARD_ID "{PAIR_BOARD_ID}"\n')
    f.write(f"#define FEB_VEC_PAIR_BOARD_ID_LEN {len(PAIR_BOARD_ID)}u\n")
    f.write(c_bytes("FEB_VEC_PAIR_ESP32_PRIVATE", PAIR_ESP32_PRIVATE))
    f.write(c_bytes("FEB_VEC_PAIR_ESP32_PUBLIC", PAIR_ESP32_PUBLIC))
    f.write(c_bytes("FEB_VEC_PAIR_FLIPPER_PRIVATE", PAIR_FLIPPER_PRIVATE))
    f.write(c_bytes("FEB_VEC_PAIR_FLIPPER_PUBLIC", PAIR_FLIPPER_PUBLIC))
    f.write(c_bytes("FEB_VEC_PAIR_EPOCH", PAIR_EPOCH))
    f.write(c_bytes("FEB_VEC_PAIR_CLIENT_NONCE", PAIR_CLIENT_NONCE))
    f.write(c_bytes("FEB_VEC_PAIR_DEVICE_NONCE", PAIR_DEVICE_NONCE))
    f.write(c_bytes("FEB_VEC_PAIR_TRANSCRIPT", PAIR_TRANSCRIPT))
    f.write(c_bytes("FEB_VEC_PAIR_K_SHARED", PAIR_K_SHARED))
    f.write(c_bytes("FEB_VEC_PAIR_K_CONFIRM", PAIR_K_CONFIRM))
    f.write(c_bytes("FEB_VEC_PAIR_SECRET", PAIR_SECRET))
    f.write(c_bytes("FEB_VEC_PAIR_FLIPPER_CONFIRM", PAIR_FLIPPER_CONFIRM))
    f.write(c_bytes("FEB_VEC_PAIR_ESP32_CONFIRM", PAIR_ESP32_CONFIRM))
    f.write(c_bytes("FEB_VEC_PAIR_COMPLETE_TAG", PAIR_COMPLETE_TAG))
    f.write("\n")

    f.write("/* Same golden vector's payload/envelope CBOR encodings, for testing pairing.h's\n")
    f.write("   codec functions independent of the crypto pipeline above. */\n")
    f.write(c_bytes("FEB_VEC_PAIR_INIT_PAYLOAD", PAIR_INIT_PAYLOAD))
    f.write(c_bytes("FEB_VEC_PAIR_INIT_RECORD", PAIR_INIT_RECORD))
    f.write(c_bytes("FEB_VEC_PAIR_REPLY_PAYLOAD", PAIR_REPLY_PAYLOAD))
    f.write(c_bytes("FEB_VEC_PAIR_REPLY_RECORD", PAIR_REPLY_RECORD))
    f.write(c_bytes("FEB_VEC_PAIR_CONFIRM_PAYLOAD", PAIR_CONFIRM_PAYLOAD))
    f.write(c_bytes("FEB_VEC_PAIR_CONFIRM_RECORD", PAIR_CONFIRM_RECORD))
    f.write(c_bytes("FEB_VEC_PAIR_COMPLETE_PAYLOAD", PAIR_COMPLETE_PAYLOAD))
    f.write(c_bytes("FEB_VEC_PAIR_COMPLETE_RECORD", PAIR_COMPLETE_RECORD))
    f.write("\n")

    f.write("/* Pairing-phase `error` record: same envelope, no session_id (docs/PROTOCOL.md\n")
    f.write("   \"Pairing record wire envelope\"), reusing feb_error_payload_t from cbor_codec.h. */\n")
    f.write(c_bytes("FEB_VEC_PAIR_ERROR_PAYLOAD", PAIR_ERROR_PAYLOAD))
    f.write(c_bytes("FEB_VEC_PAIR_ERROR_RECORD", PAIR_ERROR_RECORD))
    f.write("\n")

    f.write("/* ---- Step 6: AES-256-GCM primitive KAT + runtime session vectors\n")
    f.write("   (docs/PLAN.md step 6). AES-256-GCM, not AES-128-GCM as originally specified\n")
    f.write("   -- see docs/PLAN.md step 6's \"AES-128-GCM -> AES-256-GCM protocol revision\". ---- */\n\n")

    f.write("/* AES-256-GCM known-answer vector: GCM spec \"Test Case 16\" (McGrew & Viega),\n")
    f.write("   fetched directly from hostap's tests/test-aes.c, not transcribed from memory. */\n")
    f.write(c_bytes("FEB_VEC_GCM_KEY", _GCM_TC16_KEY))
    f.write(c_bytes("FEB_VEC_GCM_IV", _GCM_TC16_IV))
    f.write(c_bytes("FEB_VEC_GCM_AAD", _GCM_TC16_AAD))
    f.write(c_bytes("FEB_VEC_GCM_PLAINTEXT", _GCM_TC16_PT))
    f.write(c_bytes("FEB_VEC_GCM_CIPHERTEXT", _GCM_TC16_CT))
    f.write(c_bytes("FEB_VEC_GCM_TAG", _GCM_TC16_TAG))
    f.write("\n")

    f.write("/* Golden end-to-end session vector: fixed, non-random inputs run through the\n")
    f.write("   full hello -> hello_ack -> client_auth -> session_key -> AAD/nonce ->\n")
    f.write("   AES-256-GCM pipeline from docs/PROTOCOL.md#session-establishment, reusing this\n")
    f.write("   file's golden FEB_VEC_PAIR_SECRET as the pairing_secret both sides already\n")
    f.write("   share. Test-only -- real sessions always use genuinely random nonces. */\n")
    f.write(f'#define FEB_VEC_SESS_BOARD_ID "{SESS_BOARD_ID}"\n')
    f.write(f"#define FEB_VEC_SESS_BOARD_ID_LEN {len(SESS_BOARD_ID)}u\n")
    f.write(c_bytes("FEB_VEC_SESS_SESSION_ID", SESS_SESSION_ID))
    f.write(c_bytes("FEB_VEC_SESS_CLIENT_NONCE", SESS_CLIENT_NONCE))
    f.write(c_bytes("FEB_VEC_SESS_DEVICE_NONCE", SESS_DEVICE_NONCE))
    f.write(c_bytes("FEB_VEC_SESS_TRANSCRIPT", SESS_TRANSCRIPT))
    f.write(c_bytes("FEB_VEC_SESS_FLIPPER_PROOF", SESS_FLIPPER_PROOF))
    f.write(c_bytes("FEB_VEC_SESS_ESP32_PROOF", SESS_ESP32_PROOF))
    f.write(c_bytes("FEB_VEC_SESS_KEY", SESS_KEY))
    f.write("\n")

    f.write("/* hello/hello_ack/client_auth payload + full-record CBOR encodings. */\n")
    f.write(c_bytes("FEB_VEC_SESS_HELLO_PAYLOAD", SESS_HELLO_PAYLOAD))
    f.write(c_bytes("FEB_VEC_SESS_HELLO_RECORD", SESS_HELLO_RECORD))
    f.write(c_bytes("FEB_VEC_SESS_HELLO_ACK_PAYLOAD", SESS_HELLO_ACK_PAYLOAD))
    f.write(c_bytes("FEB_VEC_SESS_HELLO_ACK_RECORD", SESS_HELLO_ACK_RECORD))
    f.write(c_bytes("FEB_VEC_SESS_CLIENT_AUTH_PAYLOAD", SESS_CLIENT_AUTH_PAYLOAD))
    f.write(c_bytes("FEB_VEC_SESS_CLIENT_AUTH_RECORD", SESS_CLIENT_AUTH_RECORD))
    f.write("\n")

    f.write("/* One valid protected `error` record at sequence 1 (ESP32 -> Flipper, the first\n")
    f.write("   protected record of a fresh session) plus its AAD/nonce/plaintext components,\n")
    f.write("   and a second valid record at sequence 2 for sequence-continuity testing. */\n")
    f.write(c_bytes("FEB_VEC_SESS_PROT1_PAYLOAD", SESS_PROT1_PAYLOAD))
    f.write(c_bytes("FEB_VEC_SESS_PROT1_AAD", SESS_PROT1_AAD))
    f.write(c_bytes("FEB_VEC_SESS_PROT1_NONCE", SESS_PROT1_NONCE))
    f.write(c_bytes("FEB_VEC_SESS_PROT1_CIPHERTEXT", SESS_PROT1_CT))
    f.write(c_bytes("FEB_VEC_SESS_PROT1_TAG", SESS_PROT1_TAG))
    f.write(c_bytes("FEB_VEC_SESS_PROT1_RECORD", SESS_PROT1_RECORD))
    f.write(c_bytes("FEB_VEC_SESS_PROT2_PAYLOAD", SESS_PROT2_PAYLOAD))
    f.write(c_bytes("FEB_VEC_SESS_PROT2_RECORD", SESS_PROT2_RECORD))
    f.write("\n")

    f.write("/* Tampered variants of the sequence-1 record: same shape, must fail AES-256-GCM\n")
    f.write("   authentication (docs/PROTOCOL.md \"reject modified ciphertext [and] modified\n")
    f.write("   AAD\"). BAD_AAD keeps the original ciphertext/tag but changes the outer\n")
    f.write("   `sequence` field the AAD is built from, so a correct decoder must recompute\n")
    f.write("   the AAD from the record it actually received and reject it. */\n")
    f.write(c_bytes("FEB_VEC_SESS_PROT1_RECORD_BAD_CIPHERTEXT", SESS_PROT1_RECORD_BAD_CIPHERTEXT))
    f.write(c_bytes("FEB_VEC_SESS_PROT1_RECORD_BAD_AAD", SESS_PROT1_RECORD_BAD_AAD))

    f.write("\n#endif /* FEB_TEST_VECTORS_H */\n")

print("wrote vectors.h")
print("record length:", len(RECORD))
print("mtu23 fragments:", len(FRAGS_MTU23))
print("mtu247 fragments:", len(FRAGS_MTU247))
print("oversized payload encoded length:", len(oversized_payload))
print("pairing transcript T length:", len(PAIR_TRANSCRIPT))
print("pair_init record length:", len(PAIR_INIT_RECORD))
print("pair_reply record length:", len(PAIR_REPLY_RECORD))
print("pair_confirm record length:", len(PAIR_CONFIRM_RECORD))
print("pair_complete record length:", len(PAIR_COMPLETE_RECORD))
print("session transcript S length:", len(SESS_TRANSCRIPT))
print("session AAD length (seq1):", len(SESS_PROT1_AAD))
print("hello record length:", len(SESS_HELLO_RECORD))
print("hello_ack record length:", len(SESS_HELLO_ACK_RECORD))
print("client_auth record length:", len(SESS_CLIENT_AUTH_RECORD))
print("protected record length (seq1):", len(SESS_PROT1_RECORD))
