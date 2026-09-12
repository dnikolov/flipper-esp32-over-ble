# Firmware & Protocol Security and Correctness Review — 2026-09-10

## 1. Executive Summary & Project Context

### Project Overview
The `flipper-esp32-over-ble` project establishes a secure, authenticated bridge between a Flipper Zero (acting as a BLE peripheral / GATT server) and an ESP32-C6-DevKitC-1-N4 (acting as a BLE central / GATT client). The architecture operates in distinct security and capability layers:
1. **Reset-Gated X25519 Pairing:** One-shot Diffie-Hellman key exchange over unauthenticated BLE within an ephemeral physical reset window, deriving a long-term 32-byte `pairing_secret` stored securely on both devices.
2. **Runtime Authentication & Session Derivation:** On connection, peers exchange nonces and mutual HMAC-SHA-256 proofs bound to `pairing_secret`, deriving an ephemeral 32-byte session key via HKDF-SHA-256.
3. **AES-256-GCM Transport & Strict Canonical CBOR Framing:** Protected records are encrypted with AES-256-GCM using deterministic 12-byte nonces (`session_id || direction || sequence`) and strict fixed-order canonical CBOR encoding.
4. **Autonomous Capabilities (Wi-Fi Scan, BLE Scan, Wardriving):** Exposing hardware peripherals through command/status RPCs. Wardriving operates autonomously across BLE disconnects, buffering captures with location data into a custom checksummed circular raw-flash partition on the ESP32, and automatically streaming backlogs to the Flipper for WiGLE CSV export to SD storage.

### Documentation Analysis & Baseline Verification
Review of all documentation ([README.md](README.md), [CLAUDE.md](CLAUDE.md), [docs/PROTOCOL.md](docs/PROTOCOL.md), [docs/PAIRING.md](docs/PAIRING.md), [docs/PLAN.md](docs/PLAN.md), [docs/SESSION_MEMORY.md](docs/SESSION_MEMORY.md), [docs/LESSONS.md](docs/LESSONS.md), [docs/BASELINES.md](docs/BASELINES.md), and [docs/CAPABILITIES.md](docs/CAPABILITIES.md)) reveals high architectural discipline and extensive hardware-tested lessons. However, there are significant latent bugs and performance bottlenecks in recently implemented modules (`wardriving`, `wardriving_dedup`, `wardriving_csv`, and GATT transmission queues).

---

## 2. Critical & High-Severity Bugs

### BUG-01: Wardriving Backlog Timestamp Backdating Logic Error Inverting Capture History
- **Severity:** Critical (Data Loss / Data Corruption)
- **Status:** Active in firmware
- **Locations:** [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L1698-L1707) and [flipper/wardriving_csv.c](flipper/wardriving_csv.c#L9-L19)
- **Description:**
  When buffered backlog records accumulated while disconnected are streamed to the Flipper upon reconnection, they are transmitted in chronological order (oldest record first, ascending `record->timestamp_ms`).
  In [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L1698-L1705):
  ```c
  if(record->timestamp_ms >= wardriving_csv_anchor_timestamp_ms) {
      wardriving_csv_anchor_timestamp_ms = record->timestamp_ms;
      DateTime now;
      furi_hal_rtc_get_datetime(&now);
      wardriving_csv_anchor_unix_time = datetime_datetime_to_timestamp(&now);
  }
  uint32_t first_seen_unix = feb_wardriving_backdate_first_seen(
      record->timestamp_ms, wardriving_csv_anchor_timestamp_ms, wardriving_csv_anchor_unix_time);
  ```
  Because the records are received in monotonically ascending timestamp order, the condition `record->timestamp_ms >= wardriving_csv_anchor_timestamp_ms` evaluates to `true` on **every single backlog record**.
  Consequently, `wardriving_csv_anchor_timestamp_ms` is immediately updated to equal the current record's `record->timestamp_ms`, and `wardriving_csv_anchor_unix_time` is set to the current real-time clock.
  When [flipper/wardriving_csv.c](flipper/wardriving_csv.c#L14) computes `delta_ms = anchor_timestamp_ms - record_timestamp_ms`, `delta_ms` is **always zero**.
- **Impact:**
  Every historical record captured minutes or hours in the past is logged to the WiGLE CSV with a timestamp of the exact second it was received over BLE during the drain, destroying real capture chronology and corrupting wardriving surveys.
- **Recommended Fix:**
  The anchor must be established from the ESP32's *current uptime* at session connect time (or from the first live/current record), not updated per historical record during backlog replay. Alternatively, calculate the anchor once upon session establishment using a handshake message containing the ESP32's current uptime.

---

### BUG-02: ESP32 Wardriving Dedup Distance Math Factor of 1,000x Off (33 Kilometers Instead of 30 Meters)
- **Severity:** High (Algorithmic / Functional Defect)
- **Status:** Active in firmware
- **Location:** [esp32/main/wardriving_dedup.c](esp32/main/wardriving_dedup.c#L60-L72)
- **Description:**
  In [esp32/main/wardriving_dedup.c](esp32/main/wardriving_dedup.c#L56-L72):
  ```c
  /* Location moved: check if either lat or lon changed by ≥30m.
     At the equator, 1 degree ≈ 111 km, so 1e7 units ≈ 1.11 cm.
     30 m ≈ 2.7e6 units. Use 3e6 as a conservative round number. */
  ...
  if (lat_delta >= 3000000ULL || lon_delta >= 3000000ULL) {
      return true;
  }
  ```
  The author calculated:
  - $1 \text{ degree} = 10^7 \text{ units} \approx 111,000 \text{ m} = 11,100,000 \text{ cm}$.
  - $1 \text{ unit} \approx 1.11 \text{ cm}$.
  - Therefore, $30 \text{ meters} = 3,000 \text{ cm}$.
  - Number of units in $30 \text{ meters}$: $3,000 \text{ cm} / 1.11 \text{ cm/unit} \approx 2,700 \text{ units}$ ($2.7 \times 10^3$).
  Instead, the author erroneously computed $30 \text{ m} \approx 2.7 \times 10^6 \text{ units}$ and rounded to `3000000ULL` ($3 \times 10^6$).
  $3 \times 10^6 \text{ units} = 3,000,000 \times 1.11 \text{ cm} = 33,300 \text{ meters} = 33.3 \text{ kilometers}$!
- **Impact:**
  The ESP32 flash-log deduplication filter *never* logs a moved device location unless the vehicle drives more than 33 kilometers away from the previous observation. A vehicle driving through a town or neighborhood will never update location coordinates for known APs/BLE devices.
- **Recommended Fix:**
  Correct threshold from `3000000ULL` to `2700ULL` (or implement the flat-earth cosine-adjusted distance formula as correctly done on the Flipper in [flipper/wardriving_csv.c](flipper/wardriving_csv.c#L240-L253)).

---

### BUG-03: Direct-Mapped XOR Hash in ESP32 Dedup Table Flushes and Evicts Without Discrimination
- **Severity:** High (Flash Degradation / Filter Failure)
- **Status:** Active in firmware
- **Location:** [esp32/main/wardriving_dedup.c](esp32/main/wardriving_dedup.c#L23-L27) and [esp32/main/wardriving_dedup.c](esp32/main/wardriving_dedup.c#L45-L53)
- **Description:**
  [esp32/main/wardriving_dedup.c](esp32/main/wardriving_dedup.c#L23-L27) hashes a 6-byte MAC address into a 128-slot table using:
  ```c
  static size_t hash_address(const uint8_t address[6])
  {
      return ((address[0] ^ address[1] ^ address[2] ^ address[3] ^ address[4] ^ address[5])
              % FEB_WARDRIVING_DEDUP_TABLE_SIZE);
  }
  ```
  1. The XOR sum produces an 8-bit value (0..255), modulo 128. Any two addresses sharing the same XOR parity collide in the exact same slot.
  2. In `should_log_record`:
     ```c
     if (memcmp(entry->address, address, 6) != 0) {
         return true;
     }
     ```
     When two devices collide, every observation of device A evicts device B and vice versa. If two devices in range share a hash slot, deduplication fails completely, and both devices are appended to flash on every single scan cycle.
  3. `dedup_entry_t` does not store `payload_kind` (Wi-Fi vs BLE). A BLE peripheral address and a Wi-Fi BSSID that collide in hash slot evict each other.
- **Impact:**
  High flash wear and excessive BLE transmission during dense scans due to high collision frequency of a 7-bit XOR hash.
- **Recommended Fix:**
  Use a 2-way or 4-way set-associative table or an open-addressing table with a better hash (such as Murmur3 or FNV-1a), and include `payload_kind` in the entry structure.

---

### BUG-04: Lack of In-Flight Transmit Guard Clobbers Fragmentation State and `tx_done_action`
- **Severity:** High (Protocol Crash / GATT Write Corruption)
- **Status:** Active in firmware
- **Locations:** [esp32/main/main.c](esp32/main/main.c#L957-L970), [esp32/main/main.c](esp32/main/main.c#L1435-L1445), [esp32/main/main.c](esp32/main/main.c#L1844-L1858), and [esp32/main/main.c](esp32/main/main.c#L2240-L2255)
- **Description:**
  Transmission of multi-fragment records relies on shared static globals:
  - `tx_fragment_buf`, `tx_fragment_offsets`, `tx_fragment_lens`, `tx_fragment_total`, `tx_fragment_next`, and `tx_done_action`.
  When `queue_and_send_protected()` is called:
  ```c
  static bool queue_and_send_protected(uint16_t conn_handle, const char *type, size_t type_len,
                                       const uint8_t *payload, size_t payload_len,
                                       tx_done_action_t next_action)
  {
      if (!encode_and_queue_protected_record(type, type_len, payload, payload_len, rt_tx_sequence)) {
          return false;
      }
      rt_tx_sequence++;
      tx_done_action = next_action;
      send_next_tx_fragment(conn_handle);
      return true;
  }
  ```
  There is **no check** verifying whether `tx_fragment_next < tx_fragment_total` (i.e. whether a previous multi-fragment transmission is currently in progress).
  If a record is being transmitted (e.g. wardriving backlog batch or Wi-Fi scan results) and an event arrives:
  - A `command` from Flipper arrives (`handle_command` -> `send_protected(...)` or `send_protected_error(...)`)
  - Or `wardriving_maybe_kick_send()` fires from a live capture completion
  `queue_encoded_record_for_tx()` instantly resets `tx_fragment_write_pos = 0` and overwrites `tx_fragment_buf` and `tx_done_action = next_action`.
- **Impact:**
  In-flight multi-fragment messages have their tail fragments clobbered by the new message. The `write_complete()` callback for the prior write then sends fragments of the *new* record, causing fragment corruption, sequence desync, or dropped state machine transitions (such as clobbering `TX_DONE_CONTINUE_WARDRIVING`).
- **Recommended Fix:**
  Add a busy check in `queue_and_send_protected()`: if `tx_fragment_next < tx_fragment_total`, queue the outbound message or reject/defer the sender.

---

### BUG-05: Premature Clearing of `wifi_scan_in_progress` and `ble_scan_in_progress` Before GATT Transmission Completes
- **Severity:** High (Race Condition / GATT Collision)
- **Status:** Active in firmware
- **Locations:** [esp32/main/main.c](esp32/main/main.c#L1317-L1320) and [esp32/main/main.c](esp32/main/main.c#L1570-L1574)
- **Description:**
  In `wifi_scan_send_next_batch()`:
  ```c
  if (payload_len == 0 ||
      !queue_and_send_protected(conn_handle, "status", strlen("status"),
                                pairing_payload_encode_buf, payload_len,
                                is_complete ? TX_DONE_NONE : TX_DONE_CONTINUE_WIFI_SCAN)) {
      ...
  }
  if (is_complete) {
      wifi_scan_in_progress = false;
  }
  ```
  `wifi_scan_in_progress = false;` is set synchronously when the last batch is *queued* (only fragment 0 handed to NimBLE), NOT when `write_complete()` confirms that all fragments of that final batch were transmitted.
  The same defect exists in [esp32/main/main.c](esp32/main/main.c#L1570-L1574) for `ble_scan_send_next_batch()`.
- **Impact:**
  While the final batch's fragments (1..N) are still being written to the BLE link, `wifi_scan_in_progress` is already `false`. A user request or automated wardriving kick immediately passes the busy check and starts a new radio operation or queues a new record, colliding with the tail fragments.
- **Recommended Fix:**
  Clear `wifi_scan_in_progress` and `ble_scan_in_progress` only inside `write_complete()` under a completion action (e.g. `TX_DONE_WIFI_SCAN_COMPLETED`).

---

### BUG-06: Unsynchronized Cross-Thread Buffer and State Access in Flipper App
- **Severity:** High (Concurrency / Memory Corruption)
- **Status:** Active in firmware
- **Locations:** [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L295-L315), [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L1888-L1940), and [flipper/framing.c](flipper/framing.c#L35-L45)
- **Description:**
  In the Flipper application:
  - User commands (`send_wifi_scan_command`, `send_ble_scan_command`, `send_wardriving_command`) execute on the **GUI / Main thread** ([flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L3130-L3170)).
  - Pairing responses, hello ack, capability queries, and GATT event handling execute on the **BleEventWorker thread** ([flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L2300-L2450)).
  Both threads call `send_pairing_record()`, which directly accesses and mutates:
  1. `static uint8_t pairing_record_buf[FEB_MAX_RECORD_SIZE];`
  2. `outgoing_message_id++;`
  3. `feb_fragment_record()`'s internal `static uint8_t frag_buf[...]` in [flipper/framing.c](flipper/framing.c#L38).
- **Impact:**
  If the user presses a button while the BLE worker is processing or responding to a BLE event, the shared static buffers are corrupted concurrently without mutual exclusion.
- **Recommended Fix:**
  Route all outbound transmissions from the GUI thread to the BLE thread via message queue events, or protect `send_pairing_record()` and `pairing_record_buf` with a dedicated Furi mutex.

---

## 3. Medium-Severity Bugs & Protocol Non-Compliances

### BUG-07: Missing 24-Bit Sequence Number Wrapping Check (AES-GCM Nonce Reuse Risk)
- **Severity:** Medium (Cryptographic Risk)
- **Locations:** [esp32/main/session.c](esp32/main/session.c#L524-L535), [flipper/session.c](flipper/session.c#L460-L475), and [esp32/main/main.c](esp32/main/main.c#L963)
- **Description:**
  [docs/PROTOCOL.md](docs/PROTOCOL.md#cryptographic-requirements) states:
  > "A sequence counter begins at 1 for each authenticated session, increases by exactly one for every protected record in a direction, and must never wrap. A new BLE session is required before $2^{24} - 1$ protected records are sent."
  In `feb_session_build_nonce()`:
  ```c
  out[FEB_SESSION_ID_LEN + 1] = (uint8_t)((sequence >> 16) & 0xFFu);
  out[FEB_SESSION_ID_LEN + 2] = (uint8_t)((sequence >> 8) & 0xFFu);
  out[FEB_SESSION_ID_LEN + 3] = (uint8_t)(sequence & 0xFFu);
  ```
  Neither `feb_session_encrypt_record()` nor `queue_and_send_protected()` verifies `sequence <= 0xFFFFFF`. If `sequence` exceeds $16,777,215$, the lower 24 bits wrap, causing catastrophic AES-GCM nonce reuse under the same session key.
- **Recommended Fix:**
  In `feb_session_encrypt_record()` and sequence advance points, reject with error and terminate connection if `sequence >= 0xFFFFFF`.

---

### BUG-08: Unhandled Pairing-Phase Errors and Missing `unsupported_version` Enforcement
- **Severity:** Medium (Protocol Defect)
- **Locations:** [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L2335-L2355) and [esp32/main/main.c](esp32/main/main.c#L2980-L3000)
- **Description:**
  1. If ESP32 aborts pairing and sends an unencrypted `error` record (4-field pairing envelope), Flipper's `profile_event_handler` peeks `field_count == 4`, checks for `pair_init`, `pair_confirm`, `pair_complete`, and ignores anything else (`"Ignoring pairing record with unexpected type 'error'"`). Flipper never transitions to `PairingPhaseFailed` and hangs until connection timeout.
  2. [docs/PROTOCOL.md](docs/PROTOCOL.md#protocol-versions-and-identifiers) requires:
     > "A peer that receives an unsupported version must respond with an unencrypted error record with code `unsupported_version`, then close the BLE connection."
     Both sides currently drop records silently without sending `unsupported_version`.
- **Recommended Fix:**
  Add `error` handling inside Flipper's 4-field pairing envelope branch, and add `unsupported_version` reply generation before terminating the link.

---

### BUG-09: Undrained Sector Record Skipping and Count Desync on Flash Partial Write Failure
- **Severity:** Medium (Log Reliability)
- **Location:** [esp32/main/wardriving_log.c](esp32/main/wardriving_log.c#L490-L515)
- **Description:**
  In `wardriving_log_mark_drained()`:
  ```c
  for (i = 0; i < count; i++) {
      if (!wd_write(sector, offset + 6u, &flag_byte, 1)) {
          ESP_LOGW(...);
          continue; /* leave it undrained */
      }
      ...
  }
  if (count > 0) {
      wd_oldest_sector = wd_peek_sector[count];
      wd_oldest_offset = wd_peek_offset[count];
  }
  ```
  If `wd_write` fails for record 0, `continue` skips decrementing `wd_pending_count`. However, at the bottom of the function, `wd_oldest_sector` and `wd_oldest_offset` are still advanced to `wd_peek_offset[count]`.
- **Impact:**
  The failed record is never re-peeked during the current boot session, while `wd_pending_count` remains permanently desynchronized (+1 higher than actual pending records).
- **Recommended Fix:**
  Stop advancing `count` at the first write failure, or set `wd_oldest_sector`/`wd_oldest_offset` to the first failed record index.

---

### BUG-10: Flipper Hardcoded 16-Byte Fragment Capacity Under-Utilizes BLE Link and Floods Queue
- **Severity:** Medium (Performance / Reliability)
- **Location:** [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L304-L312)
- **Description:**
  Flipper always calls `feb_fragment_capacity(FEB_DEFAULT_ATT_MTU)` (23 MTU = 16-byte capacity) for all outgoing records.
  Even after MTU exchange (where ESP32 supports 256 bytes and Flipper attribute supports 64 bytes `PAYLOAD_MAX`), Flipper chops every command/response into 16-byte chunks.
  Each 16-byte fragment calls `ble_gatt_characteristic_update()` in a synchronous loop. The return value is unchecked, causing fragment drop under high queue congestion.
- **Recommended Fix:**
  Size fragments against `PAYLOAD_MAX` (or negotiated MTU) for post-negotiation traffic, and check the return status of `ble_gatt_characteristic_update()`.

---

### BUG-11: Raw RSSI Integer Underflow on Sub-Negative 128 dBm Signal
- **Severity:** Low/Medium (Data Encoding)
- **Locations:** [esp32/main/main.c](esp32/main/main.c#L1184) and [esp32/main/main.c](esp32/main/main.c#L1600)
- **Description:**
  `out->rssi_offset = (uint64_t)((int)rec->rssi + 128);`
  If `rec->rssi < -128` (reported on noisy RF or uncalibrated radio), `(int)rec->rssi + 128` becomes negative. Casting directly to `uint64_t` underflows to $\approx 1.84 \times 10^{19}$.
  `feb_cbor_encode_uint` encodes this as a 9-byte unsigned integer, inflating the CBOR payload.
- **Recommended Fix:**
  Clamp `rec->rssi` between -128 and 127 before calculating `rssi_offset`.

---

## 4. Concrete Optimization Opportunities

### OPT-01: Eliminate $O(N^2)$ Re-Encoding Loops in Status Batching
- **Locations:** [esp32/main/main.c](esp32/main/main.c#L1260-L1285), [esp32/main/main.c](esp32/main/main.c#L1530-L1555), and [esp32/main/main.c](esp32/main/main.c#L1915-L1940)
- **Current Behavior:**
  To ensure a batch of records fits within the 512-byte payload limit, the batch sender executes a loop:
  On each iteration $i$, it copies `trial = result;` (a 2.7 KB struct copy), adds record $i$, and re-encodes the entire CBOR payload from record 0 to $i$.
  For 32 records, this performs:
  - 528 individual record encodings per batch.
  - Over 170 KB of struct memory copies.
  - A 33rd re-encode of the whole batch after the loop.
- **Optimization:**
  Calculate or measure each record's CBOR size individually (or pre-calculate worst-case per record). Append records to the CBOR stream linearly in a single pass without copying structs or re-encoding earlier entries.

---

### OPT-02: Reclaim >15 KB of ESP32 Static BSS Memory
- **Locations:** [esp32/main/main.c](esp32/main/main.c#L1870-L1875) and [esp32/main/wardriving_log.c](esp32/main/wardriving_log.c#L23-L27)
- **Current Behavior:**
  Multiple large file-scope and function-scope static buffers are maintained permanently in BSS:
  - `peek_scratch[FEB_WARDRIVING_PEEK_SCRATCH_LEN]`: 7,680 bytes
  - `peeked[FEB_WARDRIVING_MAX_RECORDS_PER_BATCH]`: 2,688 bytes
  - `result` & `trial`: $2 \times 2,688 = 5,376$ bytes
  - Sector bookkeeping arrays: ~5,000 bytes
- **Optimization:**
  Because `wifi_scan`, `ble_scan`, and `wardriving` sends are strictly sequential on the NimBLE host task, share a unified `tx_staging_buf` union or scratch area. This saves over 15 KB of permanent SRAM.

---

### OPT-03: Flipper Display String Formatting Safeguards
- **Location:** [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c#L2795-L2815)
- **Current Behavior:**
  ```c
  int n = snprintf(footer + pos, sizeof(footer) - pos, "L:WiFi ");
  if(n > 0) pos += (size_t)n;
  ```
  If `pos >= sizeof(footer)`, `sizeof(footer) - pos` wraps around to `SIZE_MAX`, exposing an unbounded write.
- **Optimization:**
  Guard with `if(pos < sizeof(footer))` before each `snprintf` call, or clamp `n` to `sizeof(footer) - pos - 1`.

---

## 5. Prioritized Action Plan for Subsequent Agents

| Priority | Bug ID | Target File | Action Required | Verification |
|---|---|---|---|---|
| **P0** | **BUG-01** | [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c) | Fix anchor calculation so `record->timestamp_ms` does not reset the anchor on ascending streams | Unit test with synthetic backlog stream in [tests/flipper/test_session.c](tests/flipper/test_session.c) |
| **P0** | **BUG-02** | [esp32/main/wardriving_dedup.c](esp32/main/wardriving_dedup.c) | Change delta threshold from `3000000ULL` to `2700ULL` | Host unit test in [tests/esp32/test_wardriving_log.c](tests/esp32/test_wardriving_log.c) |
| **P1** | **BUG-04** | [esp32/main/main.c](esp32/main/main.c) | Guard `queue_and_send_protected()` when `tx_fragment_next < tx_fragment_total` | Verify no clobbering under simultaneous command + backlog drain |
| **P1** | **BUG-05** | [esp32/main/main.c](esp32/main/main.c) | Move clearing of `wifi_scan_in_progress` & `ble_scan_in_progress` to `write_complete()` | Test scan completion with immediate follow-up scan |
| **P1** | **BUG-06** | [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c) | Synchronize GUI-thread sends and BLE-worker sends with mutex or message queue | Concurrency stress test on Flipper |
| **P2** | **BUG-03** | [esp32/main/wardriving_dedup.c](esp32/main/wardriving_dedup.c) | Replace 7-bit XOR hash with set-associative / improved hash including `payload_kind` | Dedup collision test |
| **P2** | **BUG-07** | [esp32/main/session.c](esp32/main/session.c), [flipper/session.c](flipper/session.c) | Reject sequence numbers $\ge 2^{24}-1$ to avoid AES-GCM nonce reuse | Test vector with sequence $0xFFFFFF$ |
| **P2** | **BUG-08** | [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c) | Add pairing envelope `error` type handler | Test pairing error response decoding |
| **P2** | **BUG-09** | [esp32/main/wardriving_log.c](esp32/main/wardriving_log.c) | Handle flash write failure in `mark_drained` without corrupting pointer/count | Unit test simulating partial flash write error |
| **P3** | **BUG-10** | [flipper/flipper_esp32_over_ble.c](flipper/flipper_esp32_over_ble.c) | Use effective MTU instead of hardcoded 16-byte capacity for post-pairing sends | Measure throughput and fragment count |
| **P3** | **OPT-01** | [esp32/main/main.c](esp32/main/main.c) | Replace $O(N^2)$ trial re-encoding with linear single-pass encoding | Measure CPU execution time during status send |
| **P3** | **OPT-02** | [esp32/main/main.c](esp32/main/main.c) | Unify static buffers for scan and wardriving staging | Check `.bss` section size with `size` tool |
