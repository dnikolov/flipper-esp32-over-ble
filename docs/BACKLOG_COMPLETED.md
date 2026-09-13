# Completed backlog items

Archive of [BACKLOG.md](BACKLOG.md) rows that are fully resolved (fixed and verified, or
investigated and closed as not-a-bug/product-choice). Moved here to keep BACKLOG.md focused on
open, actionable items — this file is the scannable one-line-per-item index; the full narrative
for each lives in [PROJECT_HISTORY.md](PROJECT_HISTORY.md).

Do not resurrect an item from here back into BACKLOG.md without a genuinely new finding — if one
turns up (like G07's H03 evidence), add it as a fresh cross-reference in the *open* item, not by
reopening a closed one here.

## P0 — correctness / security

| ID | Title | Resolution |
| --- | --- | --- |
| G04 | Pairing ceremony (`pair_init`→`pair_complete`) had no application-level timeout | DONE 2026-09-12 |
| G05 | Absolute `uint32_t` millisecond deadlines wrap at ~49.7 days uptime | DONE 2026-09-12 |
| G11 | Flipper never closed the connection on auth/GCM/sequence failure | DONE 2026-09-12 |
| G26 | AES-GCM 24-bit sequence cap not enforced | DONE 2026-09-11 (`3111fa2`) |

## P1 — real bugs in normal use

| ID | Title | Resolution |
| --- | --- | --- |
| G14 | `any_saved_pairing_exists()` matched any directory entry, including a crashed-save `.dat.tmp` leftover | DONE 2026-09-12 |
| G17 | `client_auth` proof failure left the Flipper UI stuck on "Authenticating…" | DONE 2026-09-12 (commit `3ffffc2`); USER_GUIDE.md already documented the fix, confirmed 2026-09-13 |
| G27 | `pending_command_kind` was never cleared; a stray `internal_error` always looked like a wardriving self-stop | DONE 2026-09-12 |
| G28 | Wardriving CSV wrote a WiGLE header on every `FSOM_OPEN_APPEND`, not only on a genuinely new file | DONE 2026-09-12 |
| G30 | Wardriving log/dedup state had no lock between the Wi-Fi `sys_evt` writer and the NimBLE-host drain reader | DONE 2026-09-13 — moved the Wi-Fi-source dedup/append loop to the NimBLE host task, matching the already-safe BLE-source sibling. Build-verified; live concurrent-load test tracked separately as [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H02. |
| G31 | `backlog_remaining` used an unlocked `size_t` subtract — could underflow under G30's race | DONE 2026-09-12 |
| G35 | ESP32 `wardriving_dedup_reset()` wiped the flash-log-gating dedup table on every `start`/`stop` | DONE |
| BL02 | Flipper didn't query wardriving status on (re)connect | DONE 2026-09-12 (commit `1f0cb8e`); hardware-verified 2026-09-13 |
| BL03 | Wardriving CSV filename was timestamped to the second, minting a new file on every reconnect | DONE 2026-09-12; file lifetime changed to per-calendar-day; hardware-verified 2026-09-13 |
| BL05 | Staying on Wardriving screen while wardriving is active broke the connection | DONE 2026-09-12 (commit `424aecd`); multi-capability re-entrancy guard added to `queue_and_send_protected()`; hardware-verified 2026-09-13 |
| BL06 | LED constantly solid green during wardriving | INVESTIGATED 2026-09-12, hardware-verified correct per-protocol 2026-09-13 — behavior is correct (backlog continuously replenished), no code change needed |
| BL07 | ESP32 disconnected after a while and would not reconnect until Flipper FAP restart | DONE 2026-09-13 — removed a backwards reconnect-time WiFi-scan throttle. Hardware-confirmed via live serial log for a plain reconnect. **Note:** reconnect while wardriving is actively running hits a separate, unrelated stall — see [HARDENING_BACKLOG.md](HARDENING_BACKLOG.md) H01 (not a BL07 regression). |
| G29 | Wardriving CSV dedup reset on restart | DONE 2026-09-11 — chosen scope: file lifetime, documented in `docs/CAPABILITIES.md`. Hardware re-verification (a real stop/restart mid-capture) still pending. |

## P2 — robustness / cost / defense-in-depth

| ID | Title | Resolution |
| --- | --- | --- |
| G12 | Flipper fragmented every record at ATT MTU 23 (16-byte payload) even after MTU negotiation | DONE 2026-09-13 — added `negotiated_att_mtu` tracking, fragments against `min(negotiated_att_mtu, FEB_NOTIFY_CHAR_EFFECTIVE_MTU)`. Hardware-confirmed via live serial log: MTU negotiated to 256, `hello_ack` in 2×64-byte fragments. |
| G15 | ESP32 HMAC `full[32]` scratch not zeroized after truncating to the 16-byte wire value | DONE 2026-09-12 |
| G16 | Factory reset didn't zeroize in-RAM `stored_pairing_secret` before `esp_restart()` | DONE 2026-09-12 |
| G19 | Reconnect spawned an `xTaskCreate(..., 3072)` just to sleep once, every ~30s during a prolonged outage | DONE 2026-09-13 — replaced with a reused `ble_npl_callout`. Build-verified. |
| G20 | `notify_data_callback`'s NULL-context path setting `*data_len = PAYLOAD_MAX` instead of `0` | **Not a bug — the suggested fix was wrong and broke runtime auth.** Reverted 2026-09-12. |
| G21 | Pairing/capability/CSV path buffers sized at 96 bytes, one constant short of the real max | DONE 2026-09-13 — bumped to 160, buffers now reference the shared constants, added error logging on path-build failure. Build-verified, 527/527 host tests pass. |
| G22 | Wardriving dedup table shared 128 slots across Wi-Fi/BLE with silent collision eviction | DONE |
| G24 | ESP32 built with `-Og`, not `-Os` | **Product choice, not a bug** — record in BASELINES.md if changed |
| G32 | Factory-reset LED RMT channel leaked on partial init failure | DONE 2026-09-12 |
| G33 | `board_id_len` took `snprintf()`'s return value verbatim; `<stdio.h>` not directly included | DONE 2026-09-12 |
| G34 | ESP32 `feb_gcm_encrypt` failure path used `memset`, not `feb_secure_zero` | DONE 2026-09-12 |

## Codebase & agent cost-efficiency

- Canonical, agent-usable build/flash scripts for both platforms — DONE 2026-09-12. `tools/build_esp32.ps1` (build, `-Port`/`-SkipBuild`/`-CaptureBootLog`/`-CaptureSeconds`), `tools/build_flipper.ps1` (syncs into the pinned Unleashed checkout and builds, optional `-Port` to transfer), `tools/flash_flipper.ps1` (transfers a built FAP via `runfap.py`, never auto-launches) are the canonical entry points now.
- BLE active scanning for `ble_scan` and wardriving's own capture engine — DONE (enabled 2026-09-11, `e92aad9`; confirmed 2026-09-12 already hardcoded on for wardriving too). The runtime active/passive *toggle* remains open — still tracked in BACKLOG.md.
