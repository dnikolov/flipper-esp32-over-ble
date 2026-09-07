# Code review findings (2026-09-07)

Full-repository scan of both firmwares' application and shared-contract code (`esp32/main/`,
`flipper/`, `tests/`), covering correctness, embedded-target memory/performance, contract gaps
against `docs/PROTOCOL.md`, and style/documentation. This is a point-in-time review record, not
a living doc — check `git log`/`git blame` for what has since changed, and cross off items here
as they're fixed rather than editing the descriptions.

Scope note: the AES-256-GCM protected-record path (`feb_session_encrypt_record`/
`feb_session_decrypt_record`, `feb_cbor_encode/decode_protected`, `feb_gcm_encrypt/decrypt`) has
**zero call sites in either application** today (step 7 hasn't wired it in yet — confirmed by
grep). Findings below that touch that path are real bugs in already-written code, but they are
currently unreachable from a live connection; they become live the moment step 7 lands.

## Critical / correctness

### 1. Out-of-bounds read in the Flipper's CBOR map validator
`flipper/cbor_codec.c:373`, inside `feb_cbor_skip_value()`'s major-type-5 (map) case:

```c
for(size_t i = 0; i < count; i++) {
    if((uint8_t)(in[pos] >> 5) != 3) {   // no pos < in_len check
```

`pos` advances past each key/value pair and can reach or exceed `in_len` before this read. A
peer sending a map header that declares more entries than the buffer actually holds causes a
read past the end of `in`. The ESP32's equivalent function has the missing guard
(`esp32/main/cbor_codec.c:485`: `if (pos >= in_len || ...)`). Reachable from any malformed
`payload` map arriving on the write characteristic, pre-authentication — the highest-priority
fix in this list: small, isolated, remotely triggerable.

### 2. The two `feb_cbor_skip_value()` implementations accept different CBOR value sets
`payload_span` is exactly what gets encrypted and covered by AAD once step 7 lands, so both
peers must agree on what counts as a structurally valid payload. They currently don't:

| Major type | ESP32 (`esp32/main/cbor_codec.c`) | Flipper (`flipper/cbor_codec.c`) |
| --- | --- | --- |
| 1 (negative int) | **accepted** (line 393) | rejected |
| 7 (`true`/`false`/`null`) | **rejected** (line 516, falls to `default:`) | accepted (line 405) |

The Flipper's own comment at the top of its `cbor_codec.h` says future capability/command
payloads will use `true`/`false`/`null` simple values — the ESP32 rejects those today. This is a
live landmine for step 7 and the future `wifi_scan` step. `feb_cbor_skip_value()` has **zero
direct unit-test coverage** on either side (checked `tests/esp32/` and `tests/flipper/`), which
is how this drifted unnoticed.

### 3. Nesting depth differs by one level between the two decoders
The payload span is validated starting at `depth = 2` on the ESP32
(`esp32/main/cbor_codec.c:758`, inside `feb_cbor_decode_unencrypted()`) but at `depth = 1` on the
Flipper (`flipper/cbor_codec.c:655`), against the same `FEB_CBOR_MAX_NESTING` limit. The Flipper
will accept one level of payload nesting the ESP32 rejects as `FEB_CBOR_ERR_TOO_DEEP`.

### 4. Over-length `board_id` silently derives an all-zero AES-256 session key on the ESP32
`feb_session_derive_key()` bounds `board_id_len` at `FEB_PAIRING_BOARD_ID_MAX_LEN` (32), but the
runtime envelope decoders (`feb_cbor_decode_unencrypted`/`_protected`) only bound `board_id` at
the general `FEB_CBOR_MAX_TEXT_LEN` (64) — only `pairing.c`'s pairing-envelope decoder enforces
32. On a 33–64-byte `board_id` the two sides diverge:

- ESP32 (`esp32/main/session.c:451-454`): `memset(out, 0, FEB_SESSION_KEY_LEN); return;` — the
  caller gets a **known all-zero session key** with no error signal at all (function is `void`).
- Flipper (`flipper/session.c:391-393`): silently **truncates** `board_id_len` to 32 and derives
  a real (but wrong) key.

Both are incorrect; the ESP32's all-zero key is the dangerous one, since `feb_session_derive_key`
returns `void` and no caller can detect the failure. The same clamp-vs-zero split exists between
the proof helpers (`session_proof_tag()` on the Flipper vs. `feb_session_hmac_label()` on the
ESP32).

### 5. Fragment header validation diverges between the two `framing.c` implementations
The protocol says `flags` is reserved and must be all zero. The ESP32 enforces this
(`esp32/main/framing.c:104`); the Flipper (`flipper/framing.c`) parses `header.flags` into a
struct field and never checks it against zero. Conversely, the Flipper rejects a mid-message
fragment whose payload length exceeds the capacity established by fragment 0
(`flipper/framing.c:152`); the ESP32 has no equivalent check. The two implementations also
return different `feb_frame_status_t` codes for the same malformed input in at least one path
(ordering of the inconsistent-count vs. duplicate-fragment checks differs).

### 6. `client_auth` rejection desyncs the two connection-state machines
The ESP32 marks itself `RUNTIME_AUTH_STATE_AUTHENTICATED` on its own *GATT write completion*
callback (`esp32/main/main.c:1015-1017`) — i.e. as soon as it finishes sending `client_auth`,
with no confirmation from the peer. Per `docs/PROTOCOL.md`'s "Runtime auth failure handling," if
the Flipper's proof check on `client_auth` fails, it deliberately sends **no reply at all**
(`flipper/flipper_esp32_over_ble.c:842-846`, matching the spec's "peer is being impersonated"
guidance). Net effect: the ESP32 logs "runtime session authenticated" and considers the session
live, while the Flipper sits at "Authenticating..." indefinitely. Only the 30-second idle
timeout eventually recovers this. The no-reply rule itself is correct per spec; the gap is that
the ESP32 has no timeout or confirmation waiting for a possible silent rejection after sending
`client_auth`.

### 7. No timeout on the pairing ceremony (only runtime auth has one)
`FEB_HELLO_ACK_TIMEOUT_MS` bounds how long the ESP32 waits for `hello_ack`
(`esp32/main/main.c:901`, checked in `reassembly_timeout_cb()`), but there is no equivalent
deadline anywhere in the `pair_init` → `pair_reply` → `pair_confirm` → `pair_complete` flow. If
the Flipper connects during an open pairing window and never replies to `pair_init`, the ESP32
holds the connection open with no recovery path short of the BLE stack's own long connection-
supervision timeout.

### 8. `uint32_t` millisecond-clock wraparound handled inconsistently
`esp_timer_get_time() / 1000` wraps at ~49.7 days of uptime — well within the "board left running
unattended for hours/days" wardriving scenario this project explicitly designed for (see
`docs/SESSION_MEMORY.md`'s reconnect-policy revision). Two of three deadline checks use an
absolute comparison that breaks on wrap:

- `esp32/main/main.c:292` — `now_ms >= pairing_window_deadline_ms` (in `pairing_window_is_open()`)
- `esp32/main/main.c:1308` — `now_ms >= hello_ack_deadline_ms`

The third, `esp32/main/main.c:1315` (idle-connection timeout), correctly uses the wrap-safe form
`(uint32_t)(now_ms - last_record_activity_ms) >= FEB_IDLE_TIMEOUT_MS`. Notably,
`pairing_window_deadline_ms` is recomputed at an arbitrary uptime on the `unknown_board` fallback
path (`esp32/main/main.c:1122`), so a wrap there makes a freshly-opened pairing window appear
already expired.

### 9. Flipper's GCM wrapper silently discards hardware failure status
`flipper/session_crypto.c:31`, `feb_gcm_encrypt()`: discards the return value of
`furi_hal_crypto_gcm_encrypt_and_tag()`. On a hardware key-load failure, `ciphertext_out` is left
**completely untouched**, and whatever stale bytes were already in that buffer go out over BLE.
The ESP32's equivalent zeroizes `ciphertext_out`/`tag_out` on failure
(`esp32/main/session_crypto.c:29-34`). Likewise `feb_gcm_decrypt()`
(`flipper/session_crypto.c:44-46`) doesn't zeroize `plaintext_out` when verification fails, where
the ESP32 does (`esp32/main/session_crypto.c:59-64`) — a direct violation of this project's
stated "zeroize on every success and failure path" security property
(`CLAUDE.md`/`docs/PROTOCOL.md`).

### 10. Trailing bytes after a decoded record are never rejected
Neither `feb_cbor_decode_unencrypted()` nor `feb_cbor_decode_protected()`, on either firmware,
checks that the decode consumed exactly `in_len` bytes. Two different byte strings can decode to
the same logical record if one has trailing garbage appended. Not an authentication bypass today
(the AAD is always rebuilt from the freshly-parsed fields, never trusted from the wire), but it
breaks the "canonical CBOR, byte-identical encoding" guarantee `docs/PROTOCOL.md` is built on,
and it's cheap to close: one length check per decoder.

## Memory & performance (embedded-target relevant)

### 11. ~3 KB of X25519 intermediate ladder state is left in Flipper `.bss`, unzeroized, for the app's entire lifetime
The stack-overflow fix (see `docs/SESSION_MEMORY.md`'s 2026-09-03 entries) converted the whole
curve25519-donna port's locals to function-local `static` arrays to get them off the 1280-byte
`BleEventWorker` stack. But only the outermost function,
`x25519_donna_scalarmult()` (`flipper/pairing_crypto.c:762-766`), zeroizes its own five arrays
before returning. Never zeroized anywhere:

- `cmult()`'s eight arrays `a`–`h`, ~1216 bytes (`flipper/pairing_crypto.c:592`)
- `fmonty()`'s nine arrays, ~1224 bytes (`flipper/pairing_crypto.c:513`)
- `crecip()`'s ten arrays, ~800 bytes (`flipper/pairing_crypto.c:653`)
- `fmul()`/`fsquare()`'s `t[19]`, ~152 bytes each

These hold intermediate values derived from the ephemeral X25519 private scalar and persist in
static memory for as long as the FAP process runs — arguably worse than a stack frame, which at
least gets overwritten by the next call on that stack. Two independent fixes, either sufficient:
add `feb_secure_zero()` calls at each function's exit paths, or (better, given the Flipper's heap
is far larger than the 1280-byte thread stack that forced this into statics in the first place)
move this scratch state into one `malloc()`'d arena that's freed (which zeroizes on free, or is
explicitly zeroized first) once the pairing ceremony completes.

Total permanent `.bss` overhead from crypto scratch alone is roughly **4.7 KB**, plus
`framing.c`'s 772-byte `frag_buf` and the 768-byte reassembly buffer and session-layer statics —
all resident for the app's whole lifetime, used only during a handshake that happens once (or a
few times) per boot.

### 12. The Flipper fragments every outgoing record at 16 bytes/fragment, including post-MTU-negotiation session records
`flipper/flipper_esp32_over_ble.c:186`, `send_pairing_record()`, hardcodes
`feb_fragment_capacity(FEB_DEFAULT_ATT_MTU)` — always the pre-negotiation 23-byte MTU, giving a
16-byte payload per fragment — for **every** record this function sends, including `hello_ack`
and `client_auth` sent long after the connection's actual negotiated MTU is known to be larger.
Since the Write/Notify characteristics' `PAYLOAD_MAX` is 64 bytes, the real safe ceiling is 60
bytes/fragment (`feb_fragment_capacity(64 + FEB_ATT_WRITE_OVERHEAD)`). A `hello_ack` (~90 bytes)
costs 6 BLE notifications today instead of 2; `pair_reply` (~120 bytes) costs 8 instead of 2. The
16-byte conservatism is justified in the surrounding comment only for pairing records sent before
MTU is settled — it is being applied unconditionally instead. Fixing this is roughly a 3–4×
reduction in BLE packet count and connection-event usage for every record exchange.

### 13. Every reconnect attempt spawns a fresh 3 KB FreeRTOS task just to sleep once
`esp32/main/main.c:433` and `:455` (`schedule_reconnect()` / `schedule_runtime_auth_backoff()`)
both call `xTaskCreate(reconnect_task, "ble_reconnect", 3072, ...)`, where the task body is just
`vTaskDelay(delay); start_scan(); vTaskDelete(NULL)`. The same file already uses
`ble_npl_callout` (`reassembly_timeout_co`) to get equivalent one-shot delayed-callback behavior
with no extra stack allocation at all. Under the now-indefinite slow-cadence retry policy (see
`docs/SESSION_MEMORY.md`'s `MAX_RECONNECT_RETRIES` fix), this means a 3 KB heap allocation and
free every 30 seconds during a prolonged outage — exactly the unattended-for-hours scenario the
retry policy was redesigned around. Worth converting to a second `ble_npl_callout` to remove the
allocation churn entirely.

### 14. ESP32 image built with `-Og`, not `-Os`
`esp32/sdkconfig`: `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` (debug-oriented `-Og`), with
`CONFIG_COMPILER_OPTIMIZATION_SIZE` commented out. Current app image is ~677 KB against a
0x180000 (1.5 MB) partition, so there's no immediate pressure, but on a 4 MB-flash board this is
a baseline choice worth making deliberately (and recording in `docs/BASELINES.md`) rather than
inheriting the ESP-IDF project-template default.

### 15. `strlen()` recomputed in every fixed-order field decoder's inner comparison loop
Every canonical-order decoder on the ESP32 side (`feb_cbor_decode_unencrypted`, `_protected`,
`feb_cbor_decode_error_payload`, the pairing/session payload decoders) recomputes
`strlen(names[j])` for each candidate key against each field — e.g. up to 25 `strlen()` calls to
decode one 5-field envelope. Precomputing a `{const char *name; size_t len;}` table once (or
using `sizeof(literal) - 1` the way the Flipper's decoders already do via `KLEN`/`sizeof`) removes
this at no cost.

### 16. 256-byte stack buffer in the ESP32's NimBLE notification-receive path
`esp32/main/main.c:1144`, inside `BLE_GAP_EVENT_NOTIFY_RX` handling in `gap_event()`:
`uint8_t buffer[FEB_RX_FRAGMENT_BUFFER_SIZE]` (256 bytes) as a stack-local. This runs on the
4096-byte NimBLE host task stack, so it isn't at risk today, but it's the same "buffer-sized-off-
a-protocol-constant, stack-local, on a size-constrained thread" pattern that caused three
separate stack-overflow bugs on the Flipper side this project already had to chase down and fix
(see `docs/SESSION_MEMORY.md`'s 2026-09-03 entries). Worth a static, on principle, before it ever
becomes a problem on a smaller stack.

## Gaps against the documented protocol contract

### 17. ESP32's NVS pairing storage has no version, validity marker, or atomic replacement
`docs/PROTOCOL.md`'s "Implementation security requirements" explicitly requires: "The ESP32
stores `pairing_secret` in a dedicated NVS namespace with a version, validity marker, and atomic
replacement procedure." `esp32/main/main.c:245-282` (`persist_pairing_secret`/
`load_pairing_secret`) is a bare `nvs_set_blob()` of 32 raw bytes with no version field, no
validity marker, and no explicit atomicity handling beyond whatever NVS itself provides. The
Flipper side, by contrast, *does* implement the atomic temp-write/verify/sync/rename procedure
its own corresponding spec line requires (`pairing_storage_save()`,
`flipper/flipper_esp32_over_ble.c:288-318`). This is a real, currently-uncosted gap against the
written contract, not a style nit.

### 18. Pairing-file storage I/O runs synchronously on the BLE event-dispatch thread
`pairing_storage_load()` (`flipper/flipper_esp32_over_ble.c:737`, called from `handle_hello()`)
and `pairing_storage_save()` (`:605`, called from `handle_pair_complete()`) both perform SD-card
file I/O directly inside `profile_event_handler()`, which runs on the high-priority
`BleEventWorker` thread that also pumps the BLE stack's own event loop. This can block that
thread for tens to low-hundreds of milliseconds per call. Given this project's history of
unexplained post-MTU-negotiation hangs (see `docs/SESSION_MEMORY.md`'s 2026-09-03 investigation,
ultimately root-caused elsewhere but still open on "what else runs synchronously in this path"),
this is worth deliberately ruling in or out rather than leaving as an unexamined assumption.

### 19. Flipper's outgoing-fragment path has no delivery-error detection
`emit_fragment()` (`flipper/flipper_esp32_over_ble.c:178`) discards the return value of
`ble_gatt_characteristic_update()` entirely, and `send_pairing_record()`'s only success signal is
`fragment_count != 0` from `feb_fragment_record()` — which reports whether fragmentation
succeeded, not whether any fragment was actually delivered. Every ESP32 GATT write, by contrast,
is checked via the `write_complete()` callback and its `error->status`. The Flipper has no
equivalent signal anywhere in its TX path.

### 20. `notify_data_callback`'s NULL-context path returns an unsafe length
`flipper/flipper_esp32_over_ble.c:110-113`:
```c
if(context == NULL) {
    if(data) *data = NULL;
    if(data_len) *data_len = PAYLOAD_MAX;   // should be 0
    return false;
}
```
Setting `*data_len` to 64 while `*data` is `NULL` means any caller that honors the pair as given
(rather than checking the `false` return first) reads 64 bytes starting at a null pointer.

### 21. Pairing-file path buffers are sized off the wrong constant
`pairing_storage_save()`/`pairing_storage_load()` declare `static char final_path[96]` /
`tmp_path[96]` / `path[96]` (`flipper/flipper_esp32_over_ble.c:290`, `:327`), but 96 is
`FEB_PAIRINGS_PATH_MAX_LEN`, the cap for the *directory* path alone. A full file path is
`dir + "/" + board_id (up to 32) + ".dat.tmp"`, up to ~137 bytes. `build_pairing_path()` does
correctly detect and reject the resulting truncation (returns `false`), so there's no overflow —
but the practical effect is that pairing a board with a long `board_id` silently fails to persist
its secret, with only a log line to explain why.

### 22. `any_saved_pairing_exists()` matches any directory entry, including leftovers
`flipper/flipper_esp32_over_ble.c:345`: opens the pairings directory and returns true if
`storage_dir_read()` finds *any* entry at all — including a `.dat.tmp` file left behind by a
previous save that crashed mid-write before the final rename. The app would then auto-advertise
on launch claiming "Have saved pairing" with no actual usable secret behind it.

### 23. AES-256-GCM protected-record layer is entirely dead code today
`feb_session_encrypt_record`, `feb_session_decrypt_record`, `feb_cbor_encode_protected`,
`feb_cbor_decode_protected`, `feb_gcm_encrypt`, `feb_gcm_decrypt`, `feb_sha256`, and both
array-header codec functions have zero call sites in `esp32/main/main.c` or
`flipper/flipper_esp32_over_ble.c` (confirmed by grep). Expected, since step 7
(capability/command handling) hasn't been implemented yet — but it means the AAD construction,
nonce derivation, and GCM encrypt/decrypt path have never been exercised against a live
connection on real hardware. When step 7 wires this in, findings #2, #3, #4, and #10 above all
become simultaneously live, since they all sit on this exact path. Recommend closing those first.

### 24. Truncated proof/confirmation HMAC halves left unzeroized on the ESP32
`feb_session_flipper_proof()`/`feb_session_esp32_proof()` (`esp32/main/session.c:423-435`) and
the pairing-confirmation equivalents (`esp32/main/pairing.c:838-859`) each compute a full 32-byte
HMAC into a local `full[32]`, copy out only the 16 bytes the wire format needs, and return without
zeroizing `full`. The Flipper's equivalents (`session_proof_tag()`,
`flipper/session.c:309-342`) do zeroize. Separately, `perform_factory_reset()`
(`esp32/main/factory_reset.c:131`) erases NVS and calls `esp_restart()` but never zeroizes the
in-RAM `stored_pairing_secret` global first — a soft reset (as opposed to a power cycle) doesn't
clear RAM, so the secret transiently outlives the "erase everything" gesture in memory, however
briefly.

## Style & documentation

- **`snprintf()` used without including `<stdio.h>`** — `esp32/main/main.c:240`
  (`compute_board_id()`). Currently compiles only because `esp_log.h` happens to pull in
  `<stdio.h>` transitively; the Flipper's equivalent file includes it directly and explicitly.
- **`board_id_len` takes `snprintf()`'s return value verbatim** — `esp32/main/main.c:242`. That
  return value is the length that *would have been written* given unlimited space, not the
  actual written length; not reachable today (format produces ~20 chars into a 33-byte buffer)
  but would set a length past the buffer's actual contents if the format ever grew.
- **`notify_cccd_handle` is overloaded as a "just subscribed" flag** —
  `esp32/main/main.c:983-984`: zeroing the handle to signal "subscription just completed"
  discards the actual handle value and makes the one-shot-vs-persistent-state distinction hard to
  follow at the call site.
- **Both "byte-for-byte mirrored" shared headers actually differ** — `esp32/main/framing.h` vs.
  `flipper/framing.h`, and `esp32/main/cbor_codec.h` vs. `flipper/cbor_codec.h`, despite each
  file's own top comment claiming byte-for-byte mirroring. The differences are comment text only
  (confirmed by diff; no semantic/declaration drift) — but the Flipper's copy of `framing.h`
  dropped one sentence present in the ESP32's: *"`emit`'s callback must copy out anything it
  needs to keep past the call [since the buffer is reused for the next fragment]"* — which is
  exactly the contract `emit_fragment()` in `flipper_esp32_over_ble.c` depends on to be correct.
  Worth restoring verbatim mirroring (or a shared source-of-truth mechanism) given the project's
  own stated goal for these files.
- **`out_record` pointer used outside the reassembly mutex's critical section** —
  `flipper/flipper_esp32_over_ble.c:897-983`: `profile_event_handler()` releases
  `reassembly_mutex` and then decodes and runs the full pairing/session ceremony against a
  pointer into `reassembly.buffer`. This is safe today only because `feb_reassembly_reset()`
  doesn't touch the buffer contents, which is an implementation detail of `framing.c`, not a
  documented contract. Separately, `profile_start()` resets `reassembly` without holding the
  mutex at all (`:994`), inconsistent with the file's own stated cross-thread-safety discipline
  for this variable.
- **`furi_check(bt_profile_restore_default(app->bt))`** in `stop_service()`
  (`flipper/flipper_esp32_over_ble.c:1111`) crashes the whole app on a recoverable teardown
  failure rather than logging and continuing.
- **`feb_hkdf_sha256()` returns `1` on the ESP32 and `-1` on the Flipper** for the same failure
  condition; the shared header only documents "nonzero on failure," so this is technically
  conformant, but pinning the exact value in the header would remove the ambiguity for free.
- **No CI, and no single top-level test runner** — six separate `build_*.ps1` scripts across
  `tests/esp32/` and `tests/flipper/`, run by hand, with nothing that checks in one pass that
  both implementations still agree on the shared `tests/vectors/` vector set. Given that this
  project's entire design rests on two independent implementations producing byte-identical
  output, a single cross-check runner is arguably the highest-value missing piece of tooling —
  findings #2, #3, and #5 above are exactly the class of bug such a runner would have caught
  automatically.
- **`led_init()` leaks its RMT channel handle on partial failure** —
  `esp32/main/factory_reset.c:96-113`: if `rmt_new_simple_encoder()` or `rmt_enable()` fails after
  `rmt_new_tx_channel()` already succeeded, the function returns without releasing the channel.
  Low-impact (factory-reset LED feedback is best-effort and this path is failure-only), but worth
  a `rmt_del_channel()` on the error paths for cleanliness.

## Suggested fix order

1. **#1** (OOB read) — small, isolated, remotely reachable from any malformed payload.
2. **#2, #3, #5, #10** — codec/framing convergence between the two implementations, plus adding
   direct unit tests for `feb_cbor_skip_value()` and a shared cross-implementation vector runner.
   Do this *before* step 7 wires up the protected-record path, since all four compound there.
3. **#4** (`board_id` length handling) and **#9** (Flipper GCM failure handling) — both make the
   crypto layer fail unsafely rather than loudly.
4. **#6, #7, #8** — state-machine and timer robustness (client_auth desync, missing pairing
   timeout, clock wraparound).
5. **#11, #12, #13** — the memory/performance items; all self-contained, no cross-firmware
   coordination needed, each independently valuable.

Items #17–#24 and the style/documentation section are lower urgency but should be tracked
against `docs/PLAN.md`'s backlog so they don't get lost before the relevant roadmap steps
(storage hardening, step 7) begin.
