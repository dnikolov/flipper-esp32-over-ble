# Implementation Plan

This plan implements the trusted-environment BLE pairing decision in [DECISIONS.md](DECISIONS.md) and protocol v2 in [PROTOCOL.md](PROTOCOL.md). The target board is the ESP32-C6 DevKitC-1-N4.

## Roadmap phases

- **Phase 1 (done):** board/SDK/firmware/build baselines — see `docs/BASELINES.md` and the "Phase 1 status" entry in `docs/SESSION_MEMORY.md`.
- **Phase 2 (in progress):** core BLE transport, record framing, trusted-environment pairing, and authenticated runtime sessions on the ESP32-C6 — steps 1-9 below are this phase's implementation detail.
- **Phase 3 (near-term aim, decided 2026-09-07):** production-ready wardriving on the ESP32-C6. Covers finishing step 7 (capability registry), the follow-on `wifi_scan`-command step, the GPS/`ble_scan`/`wardriving` capability, step 8 (hardened persistence for both the pairing record and the wardriving log), and step 9 (full-system validation). "Production-ready" means field-usable unattended for hours, survives power loss without corrupting the wardriving log, and passes step 9's negative-security-test suite — not just "the happy path works once on a bench."
- **Phase 4 (later, decided 2026-09-07):** Heltec WiFi LoRa 32 V2 board support — a second, structurally different target (classic ESP32/Xtensa, not C6) adding display and LoRa capabilities. Does not start until Phase 3 is complete.
- **Phase 5 (later, much larger, decided 2026-09-07):** Zigbee/Thread and `gpio_control`. Zigbee/Thread recon (passive scanning, Phase 5a) first, then participation (active stack join / possible border-router role, Phase 5b) as a separately-scoped, order-of-magnitude-larger effort with no committed timeline. `gpio_control` (moved here 2026-09-07, previously a tentative Phase 3 item) rides along in this phase rather than blocking Phase 3's wardriving focus.

## Confirmed setup choices

- Target board: ESP32-C6-DevKitC-1-N4, connected by USB for flash-size verification.
- Flipper build: standalone external FAP against Unleashed stable `unlshd-092` (API 88.4), pinned to commit `3c9be0fdd9d301a9436765099a2d1780b36a1795`.
- FAP compatibility: target the pinned Unleashed stable API only. A future firmware-version adapter is deferred to phase 2.
- ESP-IDF: use ESP-IDF `v5.5.2` for the initial C6 baseline; record the installed toolchain revision when setup completes.
- Initial phase 1 scope: ESP-IDF firmware skeleton and standalone FAP skeleton, both built before adding project code.

## 1. Establish the build baselines

- Select and record one ESP-IDF release that supports `esp32c6`, NimBLE central mode, X25519, HKDF-SHA-256, HMAC-SHA-256, AES-256-GCM, and encrypted NVS.
- Create the ESP-IDF project under `esp32/` with an `esp32c6` target and a partition table suitable for the board's verified flash size.
- Create the standalone FAP project, delivered as a standalone external FAP, and pin it to the locally cached Flipper firmware API revision. It owns a custom BLE GATT profile while active; custom/in-tree firmware is optional hardening for stronger key storage or Bluetooth coexistence.
- Build an unmodified baseline for both targets before adding project code.

**Done when:** both baseline images build reproducibly and the C6 flash size is measured on the target board.

## 2. Prove the BLE transport

### Confirmed transport configuration

- Real hardware validation will use both the ESP32-C6 and a Flipper Zero.
- If no ESP32 pairing record exists, the Flipper app presents an explicit pair/connect action and starts its temporary BLE profile for that workflow.
- If a pairing record exists, the Flipper app attempts to connect automatically to the saved ESP32.
- The ESP32-C6 scans and attempts connection automatically at boot.
- The first transport smoke test uses a fixed payload. The implementation will then advance to the protocol CBOR envelope.
- Enforce one active connection and use bounded exponential reconnect backoff with a maximum of five automatic retries.

### Step 2 implementation status

- [x] ESP32-C6 NimBLE central transport scaffold implemented and builds successfully.
- [x] Flipper standalone FAP GATT peripheral transport scaffold implemented and builds successfully against Unleashed API 88.4.
- [x] Real-device discovery, fixed-payload write/notification exchange, and disconnect recovery test — verified on physical hardware, see `docs/SESSION_MEMORY.md`.

**Done when:** the C6 discovers the service, connects, writes a test record, receives a notification, and recovers from a disconnect on real hardware. ✅ Met on 2026-09-02.

**Future enhancement:** add an adapter layer for later Unleashed API revisions after the pinned stable baseline is working.

### Revised long-run reconnect policy (supersedes "five retries" above for production behavior)

The original five-retry ceiling was designed for recovering from a transient disconnect during active use, not for a board that may go unattended for hours or days (see the wardriving use case in step 7). Production reconnect behavior is:

- Bounded exponential backoff for the first several attempts, same as above, for fast recovery from a transient disconnect.
- After reaching a backoff ceiling, do not give up — continue retrying indefinitely at a slow, fixed cadence (on the order of minutes) so a board left running for a long unattended stretch is still connectable whenever a Flipper eventually comes into range, without requiring a reboot.
- When BLE-source wardriving scanning (step 7) is active, do not run a separate dedicated reconnect scan — reuse the same passive scan pass, filtering for the Flipper's fixed v2 service UUID, and trigger a connection attempt on a match. Fall back to a dedicated reconnect scan using the policy above only when BLE-source wardriving is not running.

**Action item (not yet done):** stress-test this reconnect/backoff behavior on real hardware — repeated disconnects, confirm backoff timing and the indefinite slow-retry behavior past the ceiling, confirm a disconnect mid-write does not wedge state — before relying on it under steps 3+.

## 3. Define and implement record framing

- Fragment header format, size limits, and rejection rules are defined in [PROTOCOL.md](PROTOCOL.md#fragmentation) — implement exactly as specified there rather than re-deriving the format here.
- Derive fragment payload capacity from negotiated ATT MTU minus the GATT and fragment-header overhead.
- Reject duplicate, inconsistent, oversized, incomplete, and out-of-order fragments without allocating from peer-controlled lengths.
- Implement canonical CBOR envelope encoding and bounded decoding with limits on nesting, map entries, arrays, text, and byte strings, using the fixed-field-order definition of "canonical" in [PROTOCOL.md](PROTOCOL.md#canonical-cbor-encoding-definition).

**Done when:** two independent codec tests exchange records at ATT MTU 23 and 247, including fragmented records and malformed-input rejection. ✅ Met on 2026-09-02 — see "Step 3 status" below.

### Step 3 status (2026-09-02)

- `esp32/main/framing.c`/`cbor_codec.c` and `flipper/framing.c`/`cbor_codec.c` implemented against the shared `framing.h`/`cbor_codec.h` contracts and `tests/vectors/vectors.h`.
- Host-native MSVC tests: ESP32 side 16/16 checks pass (`tests/esp32/build.ps1`); Flipper side 39/39 checks pass (`tests/flipper/build.ps1`, includes an extra regression test for the uneven-fragment-capacity bug below). Both were actually executed, not assumed.
- Real target builds both pass: `idf.py build` (ESP-IDF v5.5.2, esp32c6) and `fbt.cmd fap_flipper_esp32_over_ble` (pinned Unleashed unlshd-092). Neither firmware's `main.c`/`flipper_esp32_over_ble.c` was modified — the codec isn't wired into the live BLE transport yet.
- Two real bugs were found independently in both firmwares' first-draft `framing.c` and fixed identically in both: (1) the oversized-fragment check bounded on the *maximum* possible total instead of the true minimum, which would spuriously reject legitimate near-768-byte records whenever fragment capacity doesn't evenly divide record length (e.g. any 768-byte record at ATT MTU 247); (2) reassembly state wasn't reset immediately after a message completed, so `message_id` wraparound at 256 (a normal long-session event) could misread a new message as a continuation of the already-completed one.
- **Backlog item found for step 6:** `feb_cbor_decode_protected()` bounds `ciphertext` to `FEB_CBOR_MAX_BYTES_LEN` (256 bytes) on both firmwares, but AES-128-GCM ciphertext is plaintext-length, whose real max is `FEB_CBOR_MAX_PAYLOAD` (512 bytes) — a legitimate near-512-byte protected payload would currently fail to decode. Consistent (not diverging) between both firmwares today, and no step-3 vector exercises it yet, so it was left alone rather than changed unilaterally outside this step's scope. Fix when protected records are wired in.
- **On-device smoke test: ✅ passed 2026-09-03.** Both `main.c` and `flipper_esp32_over_ble.c` were wired to send/receive a fragmented CBOR `error` record over the existing step 2 transport; a real hardware run exchanged it successfully in both directions (ESP32->Flipper and Flipper->ESP32), each side correctly reassembling and decoding the other's record. Along the way, a real stack-overflow bug was found and fixed in the shared `framing.c` (`feb_fragment_record()`'s internal 772-byte fragment buffer was stack-local, overflowing the Flipper's 1280-byte `BleEventWorker` thread stack — see `docs/SESSION_MEMORY.md`'s 2026-09-03 entries for the full diagnosis). Step 3 is now fully closed; step 4 (radio coexistence validation) is next.

### Step 3 implementation decisions (2026-09-02 grill-me session)

- **No CBOR library.** Hand-roll a schema-specific canonical CBOR codec on both sides — the message shapes are fixed and small, and a general-purpose library would still need custom validation layered on top for this protocol's stricter rejection rules. Avoids adding an unpinned third-party build dependency to either firmware.
- **Validation gate is host-native, not on-device.** Step 3's "done when" bar is satisfied by host-native unit tests with no board required — the codec is pure byte-buffer logic with no BLE/Furi/ESP-IDF dependency once written portably. Step 9 already owns live/adversarial fuzzing of the fragment and CBOR parsers on real hardware, so step 3 doesn't need to duplicate that. After the host tests pass, run one on-device smoke test over the already-working step 2 transport as an extra confidence pass before starting step 4 — this smoke test is not itself part of the formal "done when," it's a bonus check.
- **Malformed fragment/message handling:** PROTOCOL.md specifies fragment-layer rejection rules but is silent on the connection-level consequence (unlike the crypto/session layer, which explicitly closes the connection on failure). Decision: drop and continue — discard the reassembly buffer for the affected `message_id`, keep the BLE connection open, send no response. A persistent flood of malformed fragments is expected to be caught by the existing 30-second idle-connection timeout and session/auth layer, not by the fragment layer itself.
- **File layout:** `framing.c/.h` and `cbor_codec.c/.h` per firmware (`esp32/main/`, `flipper/`), called from the existing `main.c` / `flipper_esp32_over_ble.c` entry points instead of writing the fixed ASCII test payload directly. A new top-level `tests/esp32/` and `tests/flipper/` compile those same files (not copies) against shared fixtures, so "two independent implementations" always means the code that actually ships.
- **Host test compiler: MSVC (`cl.exe`)**, via the installed Visual Studio Community VC.Tools component. No gcc/MinGW is on PATH; WSL is present but unconfirmed for gcc. Testing the portable-C codec files under a third, non-gcc compiler family is a useful extra portability signal (both real targets build with gcc-family cross-compilers), though a clean MSVC build doesn't replace the real `idf.py build` / `fbt.cmd` builds as the final word on target-compiler acceptance.
- **Test vectors: one canonical, hand-authored set** in `tests/vectors/`, derived directly from PROTOCOL.md's byte-level spec (not generated by either codec), shared by both host test binaries. Chosen over independently-authored vectors per side to avoid double-authoring effort and drift; the tradeoff is that a spec misreading baked into the shared vectors would fool both implementations equally, but the spec's byte layout is fully pinned and unambiguous.
- **Execution order:** shared contracts and vectors (`framing.h`/`cbor_codec.h`, `tests/vectors/`, the PROTOCOL.md canonical-CBOR addendum) are written first, directly, before any firmware-specific implementation starts — so both sides converge on the same field order and constants instead of each inventing its own. The `esp32-developer` and `flipper-developer` subagents then implement their respective `.c` files and host test binaries in parallel, since neither depends on the other once the contracts exist.

## 4. Validate BLE / Wi-Fi radio coexistence

The ESP32-C6 has a single 2.4GHz radio shared between Wi-Fi, BLE, and (later) 802.15.4. Step 7's `wifi_scan`/`ble_scan`/`wardriving` capabilities depend on this radio being usable concurrently for scanning and for maintaining the BLE connection to the Flipper, so this needs validating before those capabilities are built on top of unverified assumptions — pulled ahead of pairing/session work rather than left to the final validation pass (step 9).

**802.15.4 is out of scope for this step.** No 802.15.4 code or radio activity exists on either firmware yet (Zigbee/Thread recon is step 7's capability-roadmap item 4, "later") and this step's tests only exercise Wi-Fi scanning + BLE, so a step named after all three radios would claim coverage it doesn't provide. 802.15.4 coexistence gets its own validation pass once the Zigbee/Thread recon phase actually adds 802.15.4 radio activity to test against.

- Using the step 2/3 transport (no pairing/session/capability layers needed yet), run Wi-Fi scanning, an active BLE connection to the Flipper, and a passive BLE observer scan together for an extended period (30+ minutes).
- Test both configurations: BLE-source scanning **paused** during an active connection, and BLE-source scanning **concurrent** with an active connection (ESP-IDF's Wi-Fi/BT coexistence scheduler and NimBLE's concurrent central+observer roles make this possible in principle — this step determines whether it's stable in practice). Only build an automatic pause-on-connection-degradation fallback if the concurrent configuration proves unstable; do not build it preemptively.
- Validate the merged reconnect-scan behavior from step 2: while BLE-source scanning is active, confirm it correctly detects and connects to the Flipper's advertised v2 service UUID within the same scan pass, without a separate dedicated reconnect scan running at the same time.
- Record safe minimum/maximum bounds and sensible default values for Wi-Fi and BLE scan intervals from these results — step 7's `wardriving` capability uses these as its interval defaults/bounds rather than guessed values.

**Done when:** Wi-Fi scanning, an active BLE connection, and a BLE observer scan run together without the connection dropping outside the reconnect policy's expected behavior, for both the paused and concurrent configurations, with results and chosen interval bounds recorded in this plan or `docs/SESSION_MEMORY.md`. ✅ Met on 2026-09-03 — see "Step 4 results" below.

### Step 4 implementation decisions (2026-09-03 grill-me session)

- **Scope narrowed to BLE/Wi-Fi**, per the note above — 802.15.4 deferred to the Zigbee/Thread recon phase (step 7 item 4).
- **Harness is throwaway instrumentation.** A temporary ESP32-side test build (e.g. `esp32/main/coex_test.c` or a separate build target), not wired into `main.c` and not shared with step 7's real `wifi_scan` capability code — this step answers a hardware/scheduler stability question, not a step-7 implementation preview. No Flipper firmware changes are needed: the coexistence question is specific to the ESP32's shared radio, and the Flipper just needs to stay connectable as an ordinary peripheral throughout.
- **Test structure: a bounded parameter sweep, not one validation run.** One single confirmation run for the BLE-source-scanning-**paused** configuration (no coexistence contention to sweep there). For the **concurrent** configuration, an ascending sweep of 3-4 points from conservative to the theoretical radio maximum (continuous back-to-back Wi-Fi scans, BLE observer scan window ≈ interval — ~100% duty cycle), stopping early once a point exceeds its retry budget (see below) and reporting untested points above it as "not run — presumed unstable by extrapolation." Concrete conservative/moderate interval values are picked from ESP-IDF's Wi-Fi/BT coexistence guidance during implementation, not specified here.
- **Run order:** paused-configuration baseline first (validates the fallback path and the automation harness itself on the lowest-risk case before committing hours to the sweep), then the ascending concurrent sweep.
- **Two-tier pass/fail per point**, not a single binary verdict: a **hard fail** is a disconnect that does not recover within the reconnect policy's expected backoff/retry behavior (the literal "done when" violation) or a wedged/stuck state. A **degradation signal** is any unexpected disconnect that does cleanly recover — this doesn't hard-fail the point, but excludes it from being reported as a "safe default," since a config that technically survives via backoff at every interval could still be masking real degradation. Record disconnect count and timing per point, not just pass/fail, so the min/max/default bounds are backed by numbers.
- **Merged reconnect-scan validation runs at every sweep point**, not just once: each point's structured summary also records whether the BLE observer scan correctly detected and connected to the Flipper's advertised v2 service UUID within its own scan pass at that point's duty cycle (step 2's merged-scan behavior), independent of whether a disconnect occurred.
- **Logging: automated structured summary lines**, not manual log review — the throwaway test code emits periodic/end-of-run summary lines (disconnect count, longest gap, merged-scan-reconnect outcome) on top of the raw UART log, since manually eyeballing 4-5 runs of 30+ minutes each is tedious and error-prone exactly where precision matters (e.g. a disconnect that recovered slower than the backoff schedule expects).
- **Fully unattended overnight execution, with best-effort auto-recovery on a hang** (e.g. re-`idf.py flash` to reset the board and resume), rather than halt-and-report-only — accepted with the explicit tradeoff that an auto-recovered hang could mask a real instability the sweep exists to catch, mitigated by the next two rules.
- **A hang always invalidates and restarts that sweep point's full 30-minute window** — no splicing data from before/after a recovery into one "point," so a point that only looks stable because it was silently restarted mid-run can't be miscounted as evidence for a wider safe interval than it earned.
- **Retry cap: 2 restarts (3 total tries) per sweep point.** If a point hasn't produced one clean, uninterrupted 30-minute run within 3 attempts, stop retrying it, record it as **"unstable — exceeded retry budget"** (itself a useful data point), and move to the next point. Overall wall-clock ceiling for the whole sweep: ~5-6 hours, so a string of bad luck across points can't run past a reasonable overnight window.
- **Physical setup required before an unattended run starts:** laptop plugged in and not sleeping (keeps the USB/COM ports alive), Flipper charged and left powered on in BLE range for the whole run, and real Wi-Fi access points present in the environment for the scan to find. Confirmed for the 2026-09-03 session; reconfirm before any future overnight run.
- **Flipper app launch/start is fully scriptable over its CLI (COM8) — no manual button press needed.** Confirmed live on real hardware 2026-09-03: `loader open "/ext/apps/Connectivity/flipper_esp32_over_ble.fap"` launches the FAP (external FAPs must be opened **by full SD-card path**, not by name — `loader open <name>` only searches the compiled-in app table, which doesn't include this app), then `input send ok press`, `input send ok short`, `input send ok release` (in that exact order) over the same CLI reliably triggers the app's `bt_profile_start()` and moves it to "Waiting for ESP32...". The GUI input dispatcher (`gui_input()`) requires a `press` event to mark a key "ongoing" before it will forward `short`/`release` for that key — a lone `short` is silently discarded as "non-complementary input" (debug-log only, easy to miss). This makes the overnight run genuinely hands-off from the start, not just after one manual kickoff.
- **Flashing authorization:** repeated automatic reflashing of the ESP32-C6 (not the Flipper) during this test is explicitly authorized by the user for this step, per this project's hardware-safety rule.
- **Correction to the COM8-scriptability claim above (2026-09-03, actual run session):** the scripted Flipper CLI launch sequence (`loader open` + `input send ok press/short/release`) that tested clean in the prior grill-me session proved unreliable when actually driving the real overnight run — repeated `WriteLine` calls on COM8 threw "semaphore timeout" errors (writes hanging while reads kept working), reproducing even after physical USB replugs and even with a single clean open-then-write attempt (no rapid probing). Root cause not resolved. The actual run's Flipper app launch was done manually (physical button press) instead, and the orchestrator's periodic Flipper health-check over COM8 was disabled for the same reason (see `docs/SESSION_MEMORY.md`'s step 4 results entry). Treat COM8 scriptability as unreliable, not proven, until someone root-causes the write-hang; don't block a future run on fixing it — manual launch is a fine fallback since the sweep itself needs no further Flipper-side interaction.

### Step 4 results (2026-09-03)

Full unattended sweep completed on real hardware: ESP32-C6 (`COM9`) running the throwaway `esp32/coex_test/` harness, Flipper (`COM8`) running the existing `flipper_esp32_over_ble` FAP as an ordinary peripheral (launched manually — see correction above). All 5 points (1 paused baseline + 4 ascending concurrent points) completed cleanly on the first attempt each — no retries, no "unstable" points, no hard fails at any duty cycle up to the theoretical maximum:

| Point | Label | Config | BLE observer duty | Wi-Fi scan cadence | Result | Disconnects | Hard fails | Degradations |
|---|---|---|---|---|---|---|---|---|
| 0 | baseline-paused | paused | off while connected | continuous | PASS | 0 | 0 | 0 |
| 1 | concurrent-conservative-10pct | concurrent | ~10% (window=100ms/interval=1000ms) | every 30s | PASS | 0 | 0 | 0 |
| 2 | concurrent-moderate-50pct | concurrent | ~50% (window=100ms/interval=200ms) | every 15s | PASS | 0 | 0 | 0 |
| 3 | concurrent-aggressive-90pct | concurrent | ~90% (window=135ms/interval=150ms) | continuous | PASS | 0 | 0 | 0 |
| 4 | concurrent-max-100pct | concurrent | ~100% (window=30ms/interval=30ms, NimBLE's own default fast-scan params) | continuous | PASS | 0 | 0 | 0 |

Total clean run time: 5 × 30 minutes = 2.5 hours, well inside the ~5-6 hour ceiling.

**Recommended interval bounds for step 7's `wardriving`/`ble_scan` capability**, derived from these results:

- **Minimum (most conservative) BLE observer duty:** point 1's values (window=100ms, interval=1000ms, ~10%) — proven stable, lowest radio-time cost.
- **Maximum (most aggressive) BLE observer duty:** point 4's values (window=30ms, interval=30ms, ~100%) — proven stable even under continuous Wi-Fi scanning; these are also NimBLE's own default fast-scan parameters, already relied on by the existing proven reconnect scan in `esp32/main/main.c`, so this isn't a novel untested config.
- **Default:** point 2 or 3's values (50-90% duty, Wi-Fi every 15s or continuous) are a reasonable default balance of scan responsiveness vs. radio contention — pick based on step 7's actual power/latency priorities when that capability is implemented; all four concurrent points tested equally clean, so this is a product choice, not a stability constraint.
- **Wi-Fi scan cadence:** continuous back-to-back scanning is proven safe to combine with BLE central+observer at any tested duty cycle up to 100%; a slower cadence (15-30s) is also fine and reduces Wi-Fi radio time if wardriving doesn't need continuous coverage.

**Accepted gap — merged reconnect-scan behavior was not actually exercised.** The step's design called for validating that a disconnect during concurrent BLE-observer scanning is recovered via the same observer scan pass (no separate dedicated reconnect scan) rather than just validating raw stability. Because zero disconnects occurred at any point in this sweep, that reconnect path never actually fired — `merged_reconnect_ok` reported `na` (not applicable) for every point. This sweep therefore proves coexistence *stability* thoroughly (its literal "done when" bar) but does not independently prove the merged-reconnect-scan *mechanism* under this harness; that mechanism was implemented per the step 2 revised policy and code-reviewed as part of the harness build, but a live forced-disconnect test would be needed for full confidence. Not re-opening step 4 for this — flagging it as a backlog item, since step 9's full-system validation pass (which already includes reconnect/replay testing) is a more natural place to close this gap under real authenticated traffic.

**Two orchestration incidents during the run, both non-hardware, both fixed — recorded for future automation:**

1. **Windows file-locking conflict between the PowerShell orchestrator and Git-Bash coreutils reading the same log file.** The orchestrator (`tools/coex/run_coex_sweep.ps1`) writes structured events to a `.jsonl` file via `Add-Content` on every parsed line. Two separate attempts to tail that same file live from Git-Bash (`tail -f`, then a discrete polling loop using `tail`/`wc`/`grep`) each caused `Add-Content` to intermittently fail with "the process cannot access the file because it is being used by another process" — and because the script had `$ErrorAction = "Stop"` set globally, this crashed the entire orchestrator (hang-detection, reflash-recovery, and result-collection all went down with it) each time, even though the *ESP32 hardware itself was completely unaffected* both times (its sweep state lives in NVS and doesn't depend on the orchestrator watching). Fixed by wrapping every `Add-Content` call in try/catch so a log-write failure is now a non-fatal warning instead of a terminating error. **Lesson for future automation on Windows: don't read a file with Git-Bash coreutils while a PowerShell process is actively appending to it, even with short-lived discrete reads — the sharing-violation risk is real and recurring, not a one-off.** If live-tailing an actively-written log is needed again, prefer a mechanism that doesn't open the same file handle-for-handle (e.g., have the writer duplicate lines to a second file nobody else touches, or use `Get-Content -Wait` from PowerShell instead of Git-Bash coreutils).
2. Both incidents were caught and recovered within ~15 minutes each by restarting the orchestrator with `-SkipInitialFlash -SkipFlipperLaunch` (so recovery never touched the already-running hardware or burned a sweep-point retry attempt); direct one-shot UART reads were used both times to independently confirm the board's actual state before and after resuming orchestrator monitoring.

## 5. Implement trusted-environment pairing

- On ESP reset, generate a fresh 16-byte `pairing_epoch` and open exactly one 120-second pairing window.
- Generate a fresh X25519 ephemeral keypair and 16-byte `device_nonce` for each pairing attempt.
- Implement `pair_init`, `pair_reply`, `pair_confirm`, and `pair_complete` exactly as defined in [PROTOCOL.md](PROTOCOL.md).
- Generate the Flipper X25519 keypair and 16-byte `client_nonce` only after the pairing UI is active.
- Derive `K_shared`, `K_confirm`, and the 32-byte `pairing_secret` with the documented HKDF inputs and transcript.
- Reject an all-zero X25519 shared secret. Compare confirmation tags in constant time. Zeroize all ephemeral secrets on success, failure, expiry, and disconnect.
- Persist `pairing_secret` transactionally on the ESP32 before `pair_complete`; persist it on Flipper only after completion verification.
- Close pairing immediately after success. Return `pairing_disabled` after the window closes or succeeds; a later ESP reset permits replacement pairing.
- The Flipper stores multiple pairing records, keyed by `board_id`, rather than a single record — supporting more than one paired board at a time (see step 7).

**Done when:** a first pairing survives reboot, a reset-and-repair replaces the old relationship for that board only (other stored pairings are unaffected), and passive capture does not expose the persisted pairing secret. ✅ Met on 2026-09-05 — see `docs/SESSION_MEMORY.md`'s "Step 5 hardware verification executed" entry.

### Step 5 implementation decisions (2026-09-03 grill-me session)

A design-review session on 2026-09-03 walked step 5 before any implementation started (no code written, no board flashed this session). Full detail and rationale is in `docs/SESSION_MEMORY.md`'s dated entry for this session; summary for the next session to pick up from:

- **Real feasibility blocker found and resolved.** A standalone Flipper FAP cannot link against the firmware's own X25519, HKDF, or HMAC-SHA-256 — checking `targets/f7/api_symbols.csv` (API 88.4) in the actual pinned Unleashed checkout shows every `mbedtls_*` symbol (including all ECDH/Curve25519 and all HMAC/SHA-256 functions) marked `-` (unexported); no HKDF symbol exists at any API version; no lower-level bignum/ECC/hash fallback is exported either. Only `furi_hal_crypto_gcm_*` (AES-GCM, raw key) is usable. **Resolution:** keep the existing X25519/HKDF/HMAC-SHA-256 protocol design as specified (no protocol simplification — every alternative that avoids new asymmetric-crypto code in the FAP also gives up the "protects against passive BLE capture during pairing" property DECISIONS.md relies on, which was judged not worth trading away). The ESP32 side has no gap at all: `mbedtls_ecdh_*` (Curve25519), `mbedtls_md_hmac` (SHA-256), and `mbedtls_gcm_*` are all Kconfig-default-enabled already; only `mbedtls_hkdf` needs one new `CONFIG_MBEDTLS_HKDF_C=y` added to `esp32/sdkconfig.defaults` (Kconfig-default `n`). The Flipper side needs SHA-256/HMAC/HKDF hand-rolled from spec (RFC 6234/2104/5869 — mechanical, low-risk once SHA-256 exists, matching this project's existing hand-rolled-over-third-party-library pattern from step 3's CBOR codec) and needs X25519 itself **ported from a known, small, audited reference implementation** (e.g. curve25519-donna, BSD-licensed, ~200-300 lines, no dependencies) rather than derived in-house — field arithmetic over 2^255-19 has well-known correctness pitfalls that are easy to get subtly wrong and hard to catch by testing alone, unlike the mechanical hash/MAC/KDF constructions. The exact reference source is an implementation-time choice for `flipper-developer`, documented wherever it lands (with its license) rather than pinned here.
- **`docs/references/flipper-firmware/upstream` mismatch found, unrelated to the above but discovered during this research.** Its `REVISION.txt` shows a vanilla `flipperdevices/flipperzero-firmware.git` checkout at commit `2d8711939ac8442a572219ed0fb4beaa02a89858`, not the pinned Unleashed `unlshd-092` at `3c9be0fdd9d301a9436765099a2d1780b36a1795`. All API-surface findings in this session used the correct full checkout at `C:\Users\Deyan\unleashed-firmware-unlshd-092` instead (confirmed via `git log -1` against the pinned commit). The mislabeled cached mirror was not fixed this session — flagged as a follow-up, since anyone trusting `docs/references/` directly (per its own description in `CLAUDE.md`) would get wrong ABI answers.
- **`service_uuid` byte order in transcript `T`** is now pinned to RFC 4122 big-endian string order — see `docs/PROTOCOL.md`'s updated "Initial pairing records" section. This was an undocumented ambiguity in the same class as step 2's real UUID-byte-order bug; closed proactively before implementation this time.
- **Pairing record wire envelope defined.** PROTOCOL.md never actually specified the outer CBOR shape for `pair_init`/`pair_reply`/`pair_confirm`/`pair_complete` (or pairing-phase `error`), only the payload fields per type — a genuine gap, since these records can't use the general `session_id`-bearing envelope (no session exists yet at pairing time). Resolved as `{version, type, board_id, payload: {...}}`, documented in `docs/PROTOCOL.md`'s new "Pairing record wire envelope" section. Pairing-phase `error` (`pairing_disabled`/`pairing_failed`/`pairing_expired`) uses this same envelope; runtime-phase `error` keeps the original session-bearing shape — distinguished by connection phase, not record content.
- **Pairing window retry semantics clarified.** "One attempt per reset window" means literally one attempt total, consumed by any outcome (success, disconnect, expiry, malformed input, bad confirmation) — not "the window stays open for a retry until the 120s clock runs out." See the updated `docs/PAIRING.md` wording. Chosen over a stay-open-for-retry alternative to minimize an active attacker's window for multiple tries and to match the existing "physical reset = authorization to re-pair" framing, at the cost of requiring a physical reset to recover from a transient failure (e.g. an incidental mid-attempt BLE disconnect).
- **`board_id` generation pulled into this step's scope**, ahead of its textual placement under step 7 — step 7's own wording already says it must be generated "before pairing," so this isn't out-of-order implementation, just a step-numbering note for a future reader: `board_id` (MAC-derived, immutable, NVS-persisted) needs to exist before Step 5's pairing ceremony can run at all (it's in transcript `T`, in `pair_init`'s envelope, and in the `pairing_secret` HKDF `info` string).
- **Zeroization approach**: a volatile-pointer-based secure-clear helper on both sides (plain `memset` is not sufficient — an optimizing compiler can legally delete a `memset` on a variable it can prove is dead before going out of scope, CWE-14). ESP32 uses mbedtls's existing `mbedtls_platform_zeroize()`; Flipper gets its own small equivalent, exposed as a named function (e.g. `feb_secure_zero()`) in the new shared `pairing_crypto.h` contract so every ephemeral-secret cleanup path (success, failure, expiry, disconnect) goes through one auditable call instead of ad-hoc `memset`s.
- **NVS/Flipper-storage atomicity confirmed sufficient as-is, no workaround needed.** ESP-IDF's NVS blob format already double-buffers by internal version/generation on every write (`nvs_storage.cpp`'s `VER_0_OFFSET`/`VER_1_OFFSET` chunk versioning, entry-state-flip-last write ordering) — a plain `nvs_set_blob(pairing_secret) + nvs_commit()` on one key is power-loss-safe per ESP-IDF's own documented guarantee, no hand-rolled shadow-key scheme required. On the Flipper, `storage_file_open`/`_write`/`_sync`/`_close`, `storage_common_rename`, and `storage_common_remove` are all FAP-exported; a real in-tree precedent (`applications/main/archive/helpers/archive_favorites.c`) already does write-temp -> close -> remove-old -> rename for its own persisted state — the pairing-record path should copy that sequence but add `storage_file_sync()` before close (missing in the precedent, but available and documented to flush to the underlying FS).
- **File layout and execution order mirror step 3 exactly**: new shared frozen contracts (`pairing.h` for the record/state-machine shapes, `pairing_crypto.h` for the X25519/SHA-256/HMAC/HKDF/zeroize function signatures) plus `tests/vectors/` additions, written first — before `esp32-developer` (wiring ESP32's native mbedtls behind the contract) and `flipper-developer` (implementing the ported X25519 + hand-rolled SHA-256/HMAC/HKDF behind the same contract) implement independently in parallel, each with a new host-native test binary under `tests/esp32/`/`tests/flipper/`.
- **Test-vector strategy**: per-primitive known-answer vectors (RFC 7748 for X25519, RFC 4231 for HMAC-SHA-256, RFC 5869 for HKDF, NIST vectors for SHA-256) plus one additional **golden end-to-end pairing vector** — fixed, non-random X25519 keypairs/nonces/`board_id` with pre-computed expected `T`, `K_shared`, `K_confirm`, `pairing_secret`, and all four confirmation tags. Per-primitive vectors alone can't catch a wiring bug (wrong field order in `T`, wrong concatenation order in an HKDF salt, wrong domain-separation string) — exactly the class of bug step 3 found twice, in the assembled code rather than the primitives. This vector is test-only; it never appears in either firmware's real runtime path, which always uses genuinely random keys/nonces.
- **Nothing has been built or flashed yet** — this session was design-only. Next session should: write the shared contracts and `tests/vectors/` additions first, then delegate `esp32-developer`/`flipper-developer` implementation in parallel per the file-layout decision above.

### Step 5 status (2026-09-03)

Shared contracts written and frozen: `esp32/main/pairing_crypto.h` + `pairing.h`,
byte-identical at `flipper/pairing_crypto.h` + `pairing.h` (diff-verified). `tests/vectors/generate_vectors.py`
extended with per-primitive RFC/NIST known-answer vectors (X25519, SHA-256, HMAC-SHA-256,
HKDF-SHA-256) and one golden end-to-end pairing vector, per the test-vector strategy
above — see `docs/SESSION_MEMORY.md`'s dated entry for full detail, including how the
vectors were verified without a Python crypto library available. `esp32/main/main.c` and
`flipper/flipper_esp32_over_ble.c` were not touched. Implementation of `pairing.c`/
`pairing_crypto.c` and host-native test binaries against these contracts is delegated to
`esp32-developer`/`flipper-developer` in parallel next; BLE window/storage integration
into `main.c`/`flipper_esp32_over_ble.c` remains a separate follow-up pass.

**ESP32 side done (2026-09-03):** `esp32/main/pairing_crypto.c` (mbedtls-backed
SHA-256/HMAC/HKDF, plus a hand-rolled RFC 7748 Montgomery ladder for X25519 built
directly on mbedtls's bignum primitives — `mbedtls_ecp_mul()`'s own Curve25519 low-order
point rejection is incompatible with `feb_x25519()`'s void, total-function contract, see
`docs/SESSION_MEMORY.md`'s dated entry) and `esp32/main/pairing.c` (CBOR envelope/payload
codecs and the transcript/KDF pipeline) pass 42/42 host-native checks
(`tests/esp32/build_pairing.ps1`) against every per-primitive and golden-vector case, and
`idf.py build` passes clean with `CONFIG_MBEDTLS_HKDF_C=y` now added to
`esp32/sdkconfig.defaults`. `main.c` still untouched — window timer, BLE integration, and
NVS persistence remain a separate follow-up pass, same as the Flipper side.

**Flipper side implemented and build-verified (2026-09-03).** `flipper/pairing_crypto.c`
(hand-rolled SHA-256/HMAC-SHA-256/HKDF-SHA-256 plus a faithful port of
`curve25519-donna.c` for X25519) and `flipper/pairing.c` (envelope/payload codecs,
transcript builder, KDF pipeline) are implemented and pass a new host-native test
(`tests/flipper/test_pairing.c`, 67/67 checks including the full golden pairing pipeline)
and the pinned Unleashed `fbt.cmd fap_flipper_esp32_over_ble` build. A compile-breaking
typo in the frozen `pairing_crypto.h` comment (an embedded `*/` that prematurely closed
the header's top block comment) was found and fixed identically in both the `flipper/` and
`esp32/main/` copies — see `docs/SESSION_MEMORY.md`'s dated entry for detail. ESP32-side
`pairing_crypto.c`/`pairing.c` and all `main.c`/`flipper_esp32_over_ble.c` BLE/storage/
window-timer wiring remain outstanding.

### Step 5 BLE/storage/window-timer wiring decisions (2026-09-05 grill-me session)

A design-review session on 2026-09-05 walked the last remaining piece of step 5 — wiring
the already-frozen, already-tested `pairing.c`/`pairing_crypto.c` into `esp32/main/main.c`
and `flipper/flipper_esp32_over_ble.c` — before any implementation started (no code written,
no board flashed this session). Decided as one coherent pass (board_id, window/state
machine, BLE wiring, UI, and persistence together, not split across sessions, since
persistence sequencing is already part of the state machine's terminal transitions):

- **`board_id` generation:** recomputed deterministically every ESP32 boot from the chip's
  base factory MAC (`esp_efuse_mac_get_default()` — not the BLE-stack-derived MAC, which is
  a per-interface offset from the base MAC) — no NVS write needed for `board_id` itself,
  since it's a pure function of an already-permanent hardware value. Format:
  `"esp32c6-"` + 12 lowercase hex chars of the 6-byte MAC (e.g. `esp32c6-acebe6fffeda`,
  20 bytes, under `pairing.h`'s `FEB_PAIRING_BOARD_ID_MAX_LEN` = 32).
- **Window consumption:** the ESP32 accepts exactly one pairing attempt per reset window
  (already specified in `docs/PAIRING.md`), but only a **fully-established BLE connection's**
  outcome (success, disconnect, malformed input, bad confirmation) consumes that attempt.
  A bare `ble_gap_connect()` failure (link never comes up, no pairing bytes exchanged) does
  not — the board keeps scanning/retrying connection establishment via the existing
  `schedule_reconnect()` path (reused unchanged, just gated on the window still being open)
  until either a real link comes up or the 120s window expires. Chosen both because it reads
  "first connection" in `docs/PAIRING.md` most literally as "first established link," and
  because it's less code: reusing the existing retry path unchanged is simpler than adding
  logic to suppress it specifically during pairing.
- **Window-expiry mechanism:** synchronous deadline checks only (checked before starting a
  new scan/connect attempt and before sending `pair_init` on a fresh connection) — not an
  async timer that can interrupt an in-progress handshake. An in-flight handshake (a handful
  of small BLE round trips, realistically sub-second once a connection exists) is allowed to
  run to completion even if the 120s deadline passes mid-handshake, rather than adding
  cross-task cancellation logic to race against BLE event callbacks. Accepted tradeoff: a
  theoretical few-hundred-ms overrun past the nominal deadline in a pathological
  slow-handshake case, judged low-risk given how fast the real handshake is.
- **Post-window ESP32 behavior:** once the window closes (success, failure, or expiry), the
  ESP32 disconnects and goes **fully idle** — no more scanning or connecting until the next
  physical reset. This is deliberately temporary/minimal: step 6 (runtime authenticated
  sessions) doesn't exist yet, so there is genuinely nothing productive a live connection can
  do post-window today; step 6 will replace "go idle" with "attempt runtime auth using the
  stored `pairing_secret`" rather than this being a design step 6 has to unwind.
- **NVS write failure during persistence** is treated like any other ceremony failure (no
  `pair_complete` is sent, the connection ends, the window closes) — consistent with the
  general policy above rather than a special retry path.
- **The existing step 3 on-device smoke-test code is removed**, not kept behind a flag —
  `build_and_send_smoke_record`/`send_smoketest_reply` and their call sites on both firmwares
  are replaced by the real pairing flow. It served its purpose validating the framing/codec
  on real hardware and has no ongoing role once real pairing records flow over the link.
- **Flipper UI trigger:** kept as the existing single explicit OK-press action (matches the
  already-hardware-proven code exactly) for both the no-saved-pairing and
  already-paired-for-this-board cases. The `docs/SESSION_MEMORY.md` "Confirmed Phase 2
  transport configuration" language about auto-connecting when a saved pairing exists was
  written for *runtime* reconnection, which needs step 6 to mean anything — building that
  UX split now would mean designing for a step 6 flow that isn't specified yet.
- **Replacing an existing board's pairing:** no extra confirmation dialog — a silent
  overwrite when `pair_init` arrives for a `board_id` the Flipper already has a stored record
  for, consistent with `docs/DECISIONS.md`'s "physical reset authorizes a new pairing
  relationship, including replacement" framing. The detailed on-screen status (below) shows
  which `board_id` is involved, so the user isn't blind to what's happening even without a
  dialog.
- **On-screen status detail:** full per-phase status text through the ceremony (e.g.
  waiting-for-ESP32 -> connected/exchanging-keys -> confirming -> saving -> paired, or
  failed at any point), not just a collapsed success/fail indicator — explicit user
  preference, overriding this session's initial minimal-UI recommendation.
- **LED status stub (new, Flipper-side only):** continuous `sequence_blink_start_blue` from
  "waiting for ESP32" through the entire handshake; switches to solid
  `sequence_set_only_blue_255` only once `pair_complete` is verified *and* the secret is
  persisted (i.e. solid blue means "pairing succeeded," not merely "BLE link connected");
  resets to off (`sequence_reset_blue`) on any failure or return to idle. All three
  sequences and `notification_message()` are confirmed exported in the pinned Unleashed API
  (`targets/f7/api_symbols.csv`), usable directly from a standalone FAP via
  `furi_record_open(RECORD_NOTIFICATION)`. Explicitly scoped as a minimal stub hardcoded to
  this one flow — a general status/notification abstraction for other app states
  (capability streaming, wardriving status, etc.) is a new backlog item (see Backlog below),
  not built now. No ESP32-side LED feedback is in scope (ESP32 has no requested LED
  behavior this pass).
- **Flipper storage layout: one file per `board_id`** (e.g.
  `pairings/<board_id>.dat`, each holding just that board's `pairing_secret`), not one shared
  file with a record list. Chosen because step 5's own "done when" bar ("other stored
  pairings are unaffected") holds by construction with per-file storage — replacing one
  board's file via the existing atomic temp-file + `storage_file_sync()` + close + rename
  pattern (the `archive_favorites.c` precedent) can't touch another board's file — rather
  than resting on the correctness of "rewrite the whole list, keep N-1 records unchanged"
  logic. Also less code: no list serialization or find/replace-in-list logic needed.
  Discovering all paired boards (step 7's board list) means listing the directory.
- **`board_id` filename validation:** validated against a strict safe-filename charset
  (`[A-Za-z0-9_-]` only, matching the ESP32's own `esp32c6-<12 hex>` format) **before any
  filesystem operation** touches it, rejecting anything else immediately as malformed input
  — no file-existence check, no ceremony continues. Flagged and fixed proactively this
  session: `board_id` is attacker-influenceable input arriving before any cryptographic
  confirmation (pairing is intentionally unauthenticated until the confirmation tags
  validate), so using it unvalidated as a filename (e.g. for an early
  does-a-record-already-exist check, which happens right after `pair_init`, well before
  confirmation) would be a path-traversal-shaped input-validation gap — independent of, and
  not covered by, this project's already-accepted physical-possession threat model.
- **ESP32 doesn't read back its own stored `pairing_secret`** in this pass — write-only for
  step 5's scope; step 6 will be the first code that reads it back for runtime
  authentication. Persistence itself stays a plain NVS blob write + commit, no
  version/validity-marker sophistication yet (that's step 8's stated scope).
- **Delivery scope:** implementation is delegated to `esp32-developer` and
  `flipper-developer` in parallel, **build-verified only** this round (`idf.py build` and
  `fbt.cmd fap_flipper_esp32_over_ble`, no board touched) — matching the established
  steps 3/5 pattern. The actual hardware pairing test (reset-and-repair, reboot-survives
  persistence, a live ceremony between both physical devices, and confirming passive capture
  doesn't expose the persisted secret) is a separate follow-up requiring explicit user
  go-ahead first, per this project's hardware-safety rule.
- **Out of scope for this pass**, already covered elsewhere in this plan: unpair/factory-reset
  UI and NVS encryption/version-marker sophistication (step 8), and a multi-board selection
  menu UI (step 7 — though the per-file storage layout above already supports it
  structurally without rework).

### Step 5 wiring status (2026-09-05)

Both halves of the wiring design above are now implemented and build-verified, in parallel,
by `esp32-developer` and `flipper-developer`.

**Flipper side**: `flipper/flipper_esp32_over_ble.c` now runs the real pairing responder flow
(pair_init -> pair_reply -> pair_confirm -> pair_complete) against the frozen `pairing.c`/
`pairing_crypto.c`, persists per-board secrets to `/data/pairings/<board_id>.dat` via the
atomic temp-file/sync/rename pattern, validates `board_id` before any filesystem
operation, and drives the detailed on-screen status text and blink/solid-blue LED feedback
exactly as decided above. Build-verified only (`fbt.cmd fap_flipper_esp32_over_ble`), not
flashed — see `docs/SESSION_MEMORY.md`'s matching 2026-09-05 dated entry for full
implementation detail, one flagged deliberate deviation (deriving `pairing_secret`
immediately after `K_shared` rather than after both confirmations validate — same output
bytes, earlier in-memory timing only), and a real latent UI-state bug found and fixed along
the way (a mid-session BLE disconnect previously left the app stuck showing a connected
state forever).

**ESP32 side**: `esp32/main/main.c` now computes `board_id` at boot from the factory base
MAC, opens one 120-second pairing window per reset (synchronous deadline checks only, no
interrupting timer), drives the real pairing initiator flow (pair_init -> pair_reply ->
pair_confirm -> pair_complete) against the same frozen `pairing.c`/`pairing_crypto.c`,
persists `pairing_secret` to a dedicated NVS namespace before sending `pair_complete`
(an NVS failure is treated like any other ceremony failure), and goes fully idle after the
window closes for any reason until the next physical reset. `idf.py build` passes clean
(exit code 0). Two gaps flagged for later, not fixed this round: no periodic
`feb_reassembly_check_timeout()` call (pre-existing, not introduced this round), and a
received pairing-phase `error` from the Flipper gets no echoed reply (a deliberate choice,
not spec-mandated). See `docs/SESSION_MEMORY.md`'s matching 2026-09-05 ESP32-side entry.

Step 5's formal "done when" bar (survives reboot, reset-and-repair replaces only that
board's record, passive capture doesn't expose the secret) requires an actual hardware run
and is **not yet fully met**. As of 2026-09-05, the "first pairing" clause is now met: the
real ceremony ran successfully end to end on both physical devices after fixing two real bugs
found along the way (an ESP32-side write-fragment/GATT-characteristic-size mismatch, and a
Flipper-side stack overflow in the ported X25519 code) plus one cosmetic LED bug — see
`docs/SESSION_MEMORY.md`'s "step 5 hardware pairing test — first successful ceremony, two
real bugs found and fixed" entry for full detail. Reboot-survival, reset-and-repair, and
passive-capture confirmation are the remaining checks for this "done when" bar.

### Step 5 hardware verification plan (2026-09-05 grill-me session, not yet executed)

A design-review session on 2026-09-05 walked the methodology for the three remaining
"done when" checks before any of them were run. No hardware was touched this session — the
plan below is saved for a future clean session to execute. Key decisions:

- **Reboot-survival is verified out-of-band, not by adding read-back code to either
  firmware.** Dump the ESP32's NVS pairing region directly (`esptool.py read_flash 0x9000
  0x6000`, per `esp32/partitions.csv`'s NVS offset/size) before and after a reboot and diff
  the raw bytes; pull the Flipper's `/data/pairings/<board_id>.dat` off before and after a
  reboot (CLI or qFlipper) and diff those bytes directly. Chosen over adding temporary
  diagnostic read-back code to avoid touching the frozen ceremony logic again for a
  test-only path. Accepted side effect: this leaves a raw dump containing the real
  `pairing_secret` in plaintext on the dev machine's disk during the test — consistent with
  the project's already-accepted physical-possession threat model, but the dump files must
  be deleted once the diffs are confirmed (see step 11 below).
- **Reboot-survival and reset-and-repair are entangled by the current design and must be
  kept deliberately separate.** Every ESP32 reset (not just "the first") unconditionally
  opens a fresh 120-second pairing window and, if a connection completes within it, sends
  `pair_init` unconditionally — there is no check for an existing stored secret (that check
  doesn't exist until step 6). So a reboot done to test persistence must happen with the
  Flipper's app **not** launched/connectable, letting the window expire unused, or it
  silently becomes a reset-and-repair instead. A separate, later reset is used to
  deliberately exercise reset-and-repair.
  - **Flagged, not resolved, while working through this:** the plan doesn't currently say
    what step 6 will do about this same window during its own reconnect flow — whether a
    reset should first attempt runtime auth with a stored secret and only open a pairing
    window if none exists, or whether every reset still unconditionally opens a
    full re-pairing window first and runtime auth only ever kicks in after that window
    closes unused. `docs/PLAN.md` step 5's existing "step 6 will replace 'go idle'..." note
    only covers **post-window** behavior, not this. Needs a decision when step 6 is
    actually designed — noted here so it isn't silently assumed either way.
- **"Other pairings unaffected" is tested by planting a dummy second file**
  (`/data/pairings/fake-board-id-test.dat`, throwaway bytes, matching the
  `[A-Za-z0-9_-]` filename charset) rather than waiting for a second real board (the Heltec
  board doesn't exist yet) or resting solely on the per-file-storage design argument. Confirm
  it's byte-for-byte untouched after a real reset-and-repair on the one real board.
- **Passive-capture confirmation is a code audit, not a live RF capture.** No BLE sniffer
  hardware exists in this project (confirmed by search — nothing in `docs/BASELINES.md` or
  `docs/hardware/`). This pass re-walks the actual shipped `pairing.c`/ceremony code on both
  firmwares to confirm only X25519 public keys, nonces, and confirmation/completion tags
  ever cross the wire (never `K_shared` or `pairing_secret`), refreshing the 2026-09-03
  design-session reasoning against the real shipped code. A literal over-the-air capture is
  explicitly deferred to a far-future backlog item pending acquisition of sniffer hardware
  (e.g. an nRF52840 dongle running Nordic's nRF Sniffer for Bluetooth LE, or a
  Sniffle-compatible dongle) — a separate project/purchasing decision, not bundled into this
  pass.
- **The exact 2026-09-05 hardware-tested build no longer exists and can't be recovered.**
  The current `esp32/build/flipper_esp32_over_ble.bin` (648,112 bytes) and Flipper `.fap`
  (41,212 bytes) don't match the sizes recorded from the successful pairing-ceremony test
  (`0x9e260` = 647,776 bytes; 38,284 bytes) — the later same-day "live code-health defect
  fixes" session rebuilt both in place, overwriting the tested artifacts, and this project
  has no git repo to check out the pre-fix source from. Decision: don't hand-revert the four
  known code-health changes from their prose description to reconstruct an approximation
  (real risk of a transcription bug right before a security-relevant test) — just flash
  current code for this whole test. This also means the code-health fixes (particularly the
  newly-wired `feb_reassembly_check_timeout()` timer/mutex machinery on both sides, never
  before exercised on real hardware) get their first hardware exposure bundled into this same
  run rather than needing an isolated pass.

**Full sequence for the next session to execute** (mechanical pre-flight — checking the
current count of active peer sessions for `esp32/`/`flipper/`/`COM8`/`COM9` conflicts, and
reconfirming `COM9`/`COM8` are still the right ports — is not repeated here since both are
standing project conventions, not part of this session's decisions):

1. Flash both boards with current code.
2. Run a fresh pairing ceremony.
3. Capture the "before" snapshot: ESP32 NVS pairing region (`esptool.py read_flash 0x9000
   0x6000`) and the Flipper's real `board_id`'s `.dat` file, both saved to disk.
4. Reboot the Flipper alone (app closed); re-pull its `.dat` file; diff against baseline
   (expect identical).
5. Physical RST on the ESP32 alone, Flipper app not launched, let the window expire unused;
   dump NVS again; diff against baseline (expect identical).
6. Plant `/data/pairings/fake-board-id-test.dat` with throwaway bytes.
7. Physical RST on the ESP32; launch the Flipper app promptly so a full new ceremony
   completes within the window.
8. Dump/pull again: the real `board_id`'s NVS blob and `.dat` file should have **changed**;
   the dummy file should be **byte-identical** to step 6 (untouched).
9. Re-walk the shipped `pairing.c`/ceremony code on both firmwares for the passive-capture
   code-audit argument above.
10. Record results in `docs/PLAN.md`/`docs/SESSION_MEMORY.md`: step 5 closure status, the
    step-6 window-vs-runtime-auth ambiguity flagged above, and the deferred RF-capture
    backlog item.
11. Delete the raw NVS/`.dat` dumps (they contain the real `pairing_secret` in plaintext)
    once the diffs are confirmed.

Requires explicit user go-ahead before step 1 touches any hardware, per this project's
hardware-safety rule — this session ended with the plan agreed but not executed.

**Executed 2026-09-05 — all three checks passed; full detail in `docs/SESSION_MEMORY.md`.**
Two new findings surfaced (neither fixed this session, both added to Backlog below):
pairing files are saved under the `bt` service's data directory instead of this app's own
(a thread-context bug in `APP_DATA_PATH` resolution from inside the BLE callback), and the
custom BLE profile appears to stay live/connectable after "paired," letting a subsequent
ESP32 reset silently complete a new ceremony without a fresh OK-press.

## 6. Add authenticated runtime sessions

- Implement `hello`, `hello_ack`, and `client_auth` using the stored `pairing_secret`.
- The Flipper looks up which stored pairing record to use by the `board_id` carried in the incoming `hello` record.
- Derive a fresh 32-byte AES-256-GCM key for every connection using HKDF-SHA-256, `client_nonce`, `device_nonce`, `board_id`, and `session_id`. (Originally specified as a 16-byte AES-128-GCM key; revised to AES-256-GCM during this step's design review — see "Step 6 implementation decisions" below.)
- Generate a new ESP32 `session_id` for every authenticated session.
- Enforce per-direction sequence counters, the specified 12-byte nonce construction, canonical-CBOR AAD, and a 16-byte GCM tag.
- Disconnect on authentication failure, replay, unexpected sequence, malformed CBOR, or counter exhaustion. Do not resume counters after reconnect.

**Done when:** both sides pass shared known-answer vectors and reject modified ciphertext, modified AAD, replayed records, and sequence gaps.

### Step 6 implementation decisions (2026-09-06 grill-me session)

A design-review session on 2026-09-06 walked step 6 before any implementation started (no code written, no board flashed this session). Full protocol wording lives in `docs/PROTOCOL.md`'s new "Runtime auth failure handling" section and `docs/PAIRING.md`'s revised "Reset-gated BLE onboarding"/"Explicit re-pairing" sections; summary for the next session to pick up from:

- **Confirmed the crypto/wire spec already had no gap of the step-3 kind.** Unlike step 3's canonical-CBOR ambiguity, `docs/PROTOCOL.md` already fully specifies the HKDF `info` string, the 12-byte nonce construction, and the AAD content byte-for-byte for `hello`/`hello_ack`/`client_auth` and protected records. Nothing needed closing there before implementation.
- **Reset no longer unconditionally opens a pairing window.** On boot, if a `pairing_secret` is already stored, the ESP32 attempts runtime auth (`hello`/`hello_ack`/`client_auth`) first; a window opens only if no secret exists yet, or if that attempt fails with the new `unknown_board` error (below). This replaces the step-5-era "every reset opens a window" behavior, which is what let the 2026-09-05 hardware test's incidental reset silently re-pair a still-"paired" Flipper (see that test's second finding in `docs/SESSION_MEMORY.md`) — that finding is now resolved by this design, not by a separate fix. It also makes an `esp_reset_reason()`-based distinction between an intentional reset and a crash/watchdog/brownout reboot unnecessary for this purpose: either way, a reboot with a valid stored secret just resumes runtime auth silently; a window only opens on an actual auth rejection, regardless of why the board rebooted. **`esp_reset_reason()` differentiation is explicitly not planned** — noted here so it isn't rediscovered as an open question later.
- **New protocol gap closed: `unknown_board`.** Nothing in the prior error-code list covered "the Flipper has no stored record for this `board_id`" — a real gap, since that's exactly the signal the ESP32 needs to fall back to opening a window. Added as a new `error.code` value. Deliberately **not** used for a proof-verification failure (either peer's stored secret doesn't match) — that's treated as a real anomaly (desync/corruption/impersonation attempt), logged and rate-limited per the existing requirement, but does not auto-open a window; recovering from it requires a deliberate action (Flipper-side unpair, or clearing the ESP32's stored secret), not automatic renegotiation. Conflating the two would let a radio attacker force repeated pairing-window openings just by corrupting proofs in transit.
- **Explicit re-pair trigger while a valid mutual secret still exists on both sides:** the Flipper's local "unpair this board" action (step 8 scope) is the sole mechanism — it deletes only the Flipper's own stored record. No ESP32-side action is needed: the ESP32's next connection attempt gets `unknown_board` from the Flipper and falls back to opening a window on its own. This only works because the Flipper is the one side that can make runtime auth fail on demand (it controls whether `hello_ack` succeeds at all). Symmetric case — the ESP32 loses/never had its own secret (fresh board, or NVS erased) — needs no Flipper-side action either, since it goes straight to opening a window; the Flipper's stale record for that `board_id` gets silently overwritten, per the existing "replacing an existing board's pairing" behavior.
  - **This revises the step-5-era `docs/DECISIONS.md`/`docs/PAIRING.md` framing** that physical reset alone is unconditional authorization to re-pair — both docs are updated to reflect that reset only re-opens pairing when no working secret exists or runtime auth is rejected as `unknown_board`.
- **Factory-reset scope (ESP32, "erase all state, keep firmware"):**
  - Available now, no new firmware code: a host-tool NVS-partition erase over USB — `esptool.py --port COM9 erase_region 0x9000 0x6000` (NVS offset/size per `esp32/partitions.csv`), or the equivalent `parttool.py erase_partition --partition-name=nvs`. Leaves the app partition untouched; `board_id` (MAC-derived, step 7) is recomputed identically afterward, so no identity loss.
  - **Explicitly rejected: a Flipper-triggered remote factory-reset command.** Factory reset must always be a physical/local-access action on the ESP32 itself, never reachable over an authenticated BLE session — a deliberate security boundary, not an oversight.
  - **Backlogged, scheduled soon (right after step 6, before step 7):** an in-firmware, no-PC/no-session physical gesture (e.g. reading BOOT/GPIO9 as a plain input post-boot, or an NVS-persisted reboot-counter pattern) for the case where neither a PC nor a working Flipper session is available. Not yet designed; needs the strapping-pin datasheet check this project's hardware-safety rule already requires before relying on GPIO9 post-boot.
- **Multi-board pairing confirmed compatible with auto-connect, with one real gap closed.** The Flipper advertises one fixed v2 service UUID regardless of how many boards it has stored records for; whichever paired board connects first and sends `hello` with a recognized `board_id` becomes the active session (first-connect-wins, per the existing multi-board policy below). An unrecognized `board_id` is exactly the `unknown_board` case above — previously undefined, now specified.
- **Flipper launch behavior confirmed:** opening the FAP with any saved pairing record present immediately starts advertising and waits for a reconnect/`hello` automatically — no OK-press required. The explicit OK-press action is reserved for the no-saved-record (brand-new pairing) case only. This gives the already-recorded `docs/SESSION_MEMORY.md` "auto-connect" design note (written before step 6 existed) its actual meaning.
- **Pairing-file path-resolution bug (Backlog, below) will be fixed now, ahead of/alongside step 6**, rather than deferred to step 8 again — step 6 adds more state written from the same `BleEventWorker`-thread callback path that has the bug, so fixing it once now is cheaper than re-verifying against a growing pile of misplaced state.
- **Flipper-side crypto scratch buffers: file-scope `static`, matching existing precedent exactly.** The AAD-encode buffer, the plaintext `payload` buffer (up to 512 bytes per `docs/PROTOCOL.md`), and the `ciphertext`/`tag` buffers are all reachable from `profile_event_handler()` on the 1280-byte `BleEventWorker` stack that already caused one real crash (see step 3's 2026-09-03 stack-overflow entries in `docs/SESSION_MEMORY.md`). This continues the pattern already used with 100% consistency in `flipper/pairing_crypto.c` (every X25519/SHA-256/HMAC/HKDF buffer there is `static`, each with an inline rationale comment) rather than opening a new discussion per buffer.
  - **`furi_hal_crypto_gcm_*` itself audited directly** (`docs/references/flipper-firmware/upstream/targets/f7/furi_hal/furi_hal_crypto.c`, confirmed matching the pinned checkout) and confirmed **not** a stack-budget risk: it's a thin wrapper around the STM32WB's memory-mapped `AES1` hardware peripheral, processing 16 bytes at a time via register writes — no software AES math, no buffer that scales with input size. Every local in that call chain (`iv_and_counter[16]`, `dtag[16]`, `block[4]`/`last_block[4]`) is a small fixed-size array, total depth on the order of 16-32 bytes. No hardware-verification caveat is needed for this specific primitive — only this app's own AAD/payload/ciphertext buffers need the `static` treatment above.
- **Session teardown/reconnect: existing `docs/PROTOCOL.md` spec (30s idle timeout, no counter resumption across reconnects, disconnect-on-any-auth-failure) confirmed sufficient as-is** — nothing new to add. Practical behavior confirmed: on any disconnect (radio range loss, idle timeout, or auth failure), both sides discard in-memory session state (session key, `session_id`, sequence counters — `pairing_secret` is untouched) and the Flipper's custom profile keeps advertising as long as the FAP is still open (only an explicit app exit tears it down); the ESP32 reconnects per its existing step-4 bounded-then-indefinite backoff and re-runs `hello`/`hello_ack`/`client_auth` automatically. This fully covers the wardriving "step away from the car, come back a minute later" scenario with no user action required, as long as the app was never explicitly exited.
- **File layout and execution order mirror steps 3 and 5**: new/extended shared contracts
  (the `hello`/`hello_ack`/`client_auth` record shapes and the protected-record AES-256-GCM
  encode/decode — most likely extending `cbor_codec.h`/`framing.h` rather than a new header,
  since step 3 already defined the protected-record CBOR shape itself; the exact split is an
  implementation-time call) plus `tests/vectors/` additions, written first — before
  `esp32-developer` and `flipper-developer` implement independently in parallel, each against
  a new host-native test binary, same as steps 3 and 5.
- **Nothing has been built or flashed yet** — this session was design-only.

### Step 6 contract-writing session: AES-128-GCM -> AES-256-GCM protocol revision (found before implementation)

Before writing the shared `session_crypto.h`/`session.h` contracts, a real feasibility
blocker was found (not previously caught by the 2026-09-06 grill-me session above): the
Flipper's only exported raw-key AES-GCM primitive, `furi_hal_crypto_gcm_encrypt_and_tag()`/
`furi_hal_crypto_gcm_decrypt_and_verify()`, is hardcoded to a 256-bit key at the hardware
level. Traced into `furi_hal_crypto.c`'s `crypto_key_init_bswap()` (both GCM and CTR go
through it): it unconditionally sets `CRYPTO_KEYSIZE_256B` in the AES1 peripheral's control
register and reads exactly 8 `uint32_t` words (32 bytes) from the key pointer, regardless of
caller intent — there is no 128-bit path through this API. Software mbedtls GCM is not a
fallback either (step 5's ABI audit already found every `mbedtls_*` symbol unexported in
this firmware). This mismatch would have surfaced as a hardware-only build/runtime failure
if implementation had started from the AES-128-GCM spec as written.

Presented two options to the user: (A) revise the protocol to AES-256-GCM (HKDF-SHA-256
session-key derivation already supports 32-byte output via the existing
`FEB_HKDF_MAX_LEN`=32 cap; ESP32's mbedtls already supports 256-bit GCM keys with no new
code), or (B) keep AES-128-GCM and port a software AES-128-GCM implementation into the
Flipper firmware, repeating the ported-crypto stack-budget risk class that has already
caused three real hardware crashes this project (step 3 twice, step 5 once). **User chose
(A).** `docs/PROTOCOL.md` ("Cryptographic requirements"), `docs/DECISIONS.md` ("Runtime
protection"), `docs/PAIRING.md` ("BLE session"), `docs/BASELINES.md`, `docs/STANDALONE_FAP.md`,
and this step's spec bullet above were all updated: AES-128-GCM -> AES-256-GCM, session key
16 -> 32 bytes, HKDF `info` string suffix `aes-128-gcm` -> `aes-256-gcm`. Nonce construction
(12 bytes), AAD content, and the 16-byte GCM tag are unaffected by key size and are unchanged.
Dated narrative entries in `docs/SESSION_MEMORY.md`/`docs/PROJECT_HISTORY.md` describing
pre-step-6 work are left as accurate historical record, not retroactively edited.

`.claude/agents/esp32-developer.md` and `.claude/agents/flipper-developer.md` were also
updated (AES-128-GCM -> AES-256-GCM) as part of this same pass, since both agents will
implement against this corrected spec.

Shared contracts written this session (mirrored byte-for-byte between `esp32/main/` and
`flipper/`, following the framing.h/pairing.h precedent of crypto-primitive header + protocol
-glue header):

- **`session_crypto.h`** (new): declares `feb_gcm_encrypt()`/`feb_gcm_decrypt()`, one-shot
  AES-256-GCM AEAD primitives (`FEB_SESSION_KEY_LEN`=32, `FEB_SESSION_NONCE_LEN`=12). Backed
  by `mbedtls_gcm_*` on the ESP32 and `furi_hal_crypto_gcm_encrypt_and_tag`/
  `_decrypt_and_verify` on the Flipper — implementation (`session_crypto.c`) is delegated,
  not written this session.
- **`session.h`** (new): the `hello`/`hello_ack`/`client_auth` payload structs and
  encode/decode (mirroring how `pair_init`/etc. payloads live in `pairing.h`, not
  `cbor_codec.h`, per that file's own documented scope note); the transcript `S` builder
  (`{version, board_id, session_id, client_nonce, device_nonce}`); the runtime proof helpers
  (`feb_session_flipper_proof`/`feb_session_esp32_proof`, wrapping `pairing_crypto.h`'s
  existing `feb_hmac_sha256()` — no new HMAC primitive needed); session-key derivation
  wrapping `pairing_crypto.h`'s existing `feb_hkdf_sha256()`; the AAD builder
  (`{version, type, session_id, sequence, board_id}` canonical CBOR, reusing `cbor_codec.h`'s
  primitives); the 12-byte nonce builder (`session_id[0..7] || direction || sequence`); and
  high-level `feb_session_encrypt_record()`/`feb_session_decrypt_record()` wrapping
  `cbor_codec.h`'s existing `feb_protected_record_t` around `session_crypto.h`'s GCM
  primitives. Does **not** own sequence-counter state, the 30-second idle timeout, or the
  reset-vs-runtime-auth boot decision — those stay implementation-specific per firmware in
  `main.c`/`flipper_esp32_over_ble.c`, matching how `pairing.h` never owned the window timer
  either.
- **`tests/vectors/generate_vectors.py`** extended with: an AES-256-GCM known-answer vector
  (NIST SP 800-38D test case, encrypt/decrypt/tamper-reject), the session-key HKDF derivation
  vector, the transcript `S` vector, both runtime proof vectors (`runtime-flipper`/
  `runtime-esp32` labels), and one golden end-to-end vector covering `hello` ->
  `hello_ack` -> `client_auth` -> one protected `error` record round trip, all derived from
  the script's own from-scratch AES-GCM/HMAC/HKDF (reusing the step-5 generator's existing
  from-scratch X25519 and stdlib `hashlib`/`hmac` ground truth) rather than transcribed by
  hand.
- **Also fixed alongside this pass** (Backlog item, scheduled for step 6): the Flipper
  pairing-file path-resolution bug (`APP_DATA_PATH` resolved from the wrong thread inside
  `handle_pair_complete()`) — delegated to `flipper-developer` together with the
  `session.c`/`session_crypto.c` implementation and `main.c`/`flipper_esp32_over_ble.c`
  wiring, since both touch the same file and this was already the agreed plan.
- **Nothing has been built or flashed yet.** Next: `esp32-developer`/`flipper-developer`
  implement `session_crypto.c`/`session.c` against the frozen contracts in parallel, each
  with a new host-native test binary exercising the vectors above, then wire
  `hello`/`hello_ack`/`client_auth` and the reset-vs-runtime-auth boot logic into
  `main.c`/`flipper_esp32_over_ble.c` per this step's design decisions — build-verify only,
  no hardware touched, per this project's hardware-safety rule.

### Step 6 status (2026-09-06)

`esp32-developer` and `flipper-developer` implemented `session_crypto.c`/`session.c` and the
`main.c`/`flipper_esp32_over_ble.c` wiring in parallel, against the frozen contracts above.
Both sides host-test-verified and build-verified; **neither board has been flashed with this
code yet** — see `docs/SESSION_MEMORY.md`'s matching 2026-09-06 entries for full narrative
detail (judgment calls, the path-resolution-bug fix, real gaps found). Summary:

- **ESP32**: `session_crypto.c` backed by `mbedtls_gcm_*` (already Kconfig-enabled, no new
  sdkconfig option needed). `main.c` now checks NVS for a stored `pairing_secret` at boot and
  picks a runtime-auth or pairing boot mode accordingly — a reset with a valid secret no
  longer unconditionally opens a pairing window, closing the gap the 2026-09-05 hardware test
  found. Host test (`tests/esp32/test_session.c`/`build_session.ps1`): **27/27 checks pass**.
  `idf.py build`: clean, exit 0.
- **Flipper**: `session_crypto.c` backed by `furi_hal_crypto_gcm_encrypt_and_tag`/
  `_decrypt_and_verify` (32-byte key, matching the AES-256-GCM revision above exactly).
  `flipper_esp32_over_ble.c` now responds to `hello` (looking up the stored record for the
  incoming `board_id`, replying `unknown_board` via the pairing envelope if none exists) and
  verifies `client_auth` in constant time; opening the FAP with any saved pairing record now
  starts advertising immediately with no OK-press, per this step's design decision. Host test
  (`tests/flipper/test_session.c`/`build_session.ps1`): **42/42 checks pass** (against a
  host-only simulated `furi_hal_crypto_gcm_*` substitute for that one hardware-only API
  surface — see `tests/README.md`'s "Step 6 additions" for why). `fbt.cmd
  fap_flipper_esp32_over_ble`: clean, artifact 51,780 bytes.
- **Pairing-file path-resolution bugfix (Backlog item above) is done**, and turned out to
  need a different fix than planned: `APP_DATA_PATH(path)` is a compile-time string macro
  (`"/data/" + path`), not a runtime call, so caching its *expansion* from the app's own
  thread (as originally planned) would have produced the same wrong string regardless of
  caller. The real thread-identity resolution happens inside the storage service, at the
  moment a storage call is issued (`storage_processing.c`'s `storage_process_alias()`, keyed
  off `furi_thread_get_appid()` of whichever thread issues that call). Fixed instead by
  calling the exported `storage_common_resolve_path_and_ensure_app_directory()` once from the
  app's own thread at init, caching the fully-resolved absolute path (no longer starting with
  `/data`, so it bypasses the thread-dependent alias check entirely regardless of which
  thread — including `BleEventWorker` — uses it afterward).
- Both sides' stack-budget audits are clean: every new scratch buffer reachable from
  `profile_event_handler`/the BLE event path is file-scope `static`, matching the
  `pairing.c`/`pairing_crypto.c` precedent. No bugs were found in the frozen contracts by
  either agent.

**Judgment calls made, flagged for review, none blocking:**
- ESP32: repeated-proof-failure rate limiting (1/2/4/8/16/32s backoff then a 5-minute fixed
  cadence, reset on any success) and a 5-second `hello_ack` timeout — neither is numerically
  specified anywhere in the docs.
- ESP32: the `unknown_board` pairing-window fallback opens only after the connection that
  received it has already closed, not on the same connection — matching "next connection
  attempt" from this step's design decisions.
- Flipper: routes an incoming record to the pairing-envelope or session-envelope decoder by
  peeking the CBOR map header's field count (4 vs. 5 vs. 7 — the three envelope shapes are
  already distinct sizes) before fully decoding, since a receiver can't otherwise know which
  shape arrived first.
- Flipper: a malformed `hello`/`client_auth` payload is silently dropped with no reply, the
  same as a proof-verification failure — `docs/PROTOCOL.md` only specifies a reply for
  `unknown_board`, so this keeps the two failure modes indistinguishable to an attacker
  (arguably a security positive, not just an omission).

**Real gaps found, not fixed this round (flagged by the implementing agents, out of this
round's stated scope) — added to Backlog below:**
- ESP32: `schedule_reconnect()`'s `MAX_RECONNECT_RETRIES = 5` still hard-stops forever after
  5 raw GAP-level connect failures, contradicting the step-4-documented "bounded exponential
  backoff then indefinite slow-cadence retry" production policy — this now matters more under
  step 6's always-scanning runtime-auth mode than it did before. Pre-existing (step 2/4 era),
  not introduced this round.
- Neither side enforces the 30-second idle-connection timeout on an authenticated-but-idle
  session yet (`docs/PROTOCOL.md` "Reliability and reconnect behavior") — there is no
  capability traffic to be idle *between* until step 7, so this was explicitly out of scope
  this round, but it needs to land before step 7 ships real traffic.
- Neither side sends the spec-mandated `unsupported_version` error + connection close for a
  bad `version` field on the new session-envelope path — this gap already existed on the
  pairing-envelope path before step 6 and was mirrored (not fixed) rather than unilaterally
  diverging from the existing pairing-path behavior.

**On-hardware verification executed 2026-09-06 — all checks passed.** Both boards reflashed
with the already build-verified step-6 code. Real finding: boards paired before this flash need
a fresh pairing ceremony, because the step-6 path-resolution bugfix means the app now looks in
the correct (previously-empty) pairing directory instead of the old buggy one where prior
records were stranded — not a defect, but re-pairing is required once per previously-paired
board. That fallout doubled as the test: ESP32 booted with its old NVS secret and correctly
attempted runtime auth first (no unconditional window); Flipper replied `unknown_board` for the
now-unrecognized record; ESP32 opened a pairing window on the *next* connection attempt (not
the same one) and completed a fresh ceremony; the new pairing file was confirmed saved to the
correct directory; a subsequent ESP32 reset then completed `hello`/`hello_ack`/`client_auth`
silently with no window and no OK-press. Full detail and log excerpts in
`docs/SESSION_MEMORY.md`'s matching 2026-09-06 entry.

**Real bug found and fixed same session:** the runtime-session-auth success path
(`handle_client_auth()` in `flipper/flipper_esp32_over_ble.c`) updated the on-screen text to
`ESP32 session active` but never stopped the blue LED's blink or set it solid — unlike the
pairing-ceremony's own success path, which already did both. Fixed by adding the same two
`notification_message()` calls (`sequence_blink_stop` then `sequence_set_only_blue_255`),
matching the existing convention exactly. Build-verified, then hardware-verified by redeploying
to the Flipper (no ESP32 touch needed — see `docs/SESSION_MEMORY.md`'s matching entry for why):
screen and LED both confirmed correct on the physical device.

## 7. Implement board identity and capability registry

- Generate and persist an immutable printable ASCII `board_id` during controlled provisioning or first boot before pairing. Derive it from the chip's factory-programmed MAC address to avoid collisions across multiple boards without a separate provisioning step.
- Implement `capability_query` and `capability_response` after runtime authentication only.
- Add commands incrementally, with input validation, request IDs, bounded output, and explicit `unsupported_capability` errors.

### Step 7 implementation decisions (2026-09-07 grill-me session)

- **Step 7 scope split.** Step 7 itself covers only the board-identity/capability-registry plumbing: `capability_query`/`capability_response` wired up post-runtime-auth, reporting `board`/`firmware`/`features`. It does NOT include implementing the `command` message type or any real `wifi_scan` command handler — that is split into its own separate future roadmap step. Result pagination, scan trigger vs. streamed results, argument validation, and all payload-size-constraint reasoning are deferred there. Step 7's "done when" bar (Flipper renders the authenticated capability list correctly) is satisfiable without any working `command` handling.
- **`capability_query` lifecycle.** The Flipper sends `capability_query` exactly once per `board_id` — specifically, the first time runtime auth succeeds for that `board_id` AND no locally persisted capability file exists for it yet. The result is persisted and never automatically re-queried on subsequent reconnects, even though this differs from the project's usual "distrust and re-verify every session" pattern used for authentication. This is a deliberate, explicit exception because a capability list is non-sensitive cached metadata, not a security credential.
- **Staleness handling.** Nothing automatically invalidates or refreshes a persisted capability record. The only way to force a fresh `capability_query` is a full unpair + re-pair (a manual "refresh capabilities" UI action without full re-pair was considered and explicitly rejected/backlogged for now).
- **Storage location (Flipper side).** The persisted capability record is stored in its own separate file per `board_id`, distinct from the pairing-secret file. Rationale: the pairing-secret file is security-sensitive and will get atomic-write/versioning hardening in step 8; the capability-cache file is not sensitive and shouldn't force that hardening work to reason about a second, unrelated write path.
- **Unpair/factory-reset behavior.** Deletes both the pairing file and the capability file for that `board_id` together, as one operation — no orphaned capability file is left behind.
- **The `requested` field on `capability_query`** (already defined in PROTOCOL.md's Message payloads table as an optional array-of-text-strings filter): the ESP32 ignores it entirely for now and always returns the full registry regardless of what's sent (or not sent); the Flipper always omits it. This is flagged as an explicit backlog item: revisit later whether this field is even worth keeping once there's a real multi-capability use case that would benefit from partial queries.
- **The `firmware` field value.** Sourced from a hand-maintained string constant in ESP32 source (e.g. a `FEB_FIRMWARE_VERSION` define, something like `"0.1.0"`), bumped manually by hand. Explicitly NOT build-injected (e.g. not `git describe` at build time) — that was considered and rejected as unnecessary complexity for a value nothing currently makes decisions based on.
- **The `board` field value.** Also a hand-maintained opaque string constant per firmware target (e.g. `"esp32-c6-devkit"` for this board; a future Heltec firmware target would define its own distinct string). The Flipper treats it as a pure opaque display string — no enum, no allowlist, no validation against known values. Capability gating is driven entirely by the `features` array, never by the `board` string.
- **`features` list generation (ESP32 side).** A hardcoded compile-time array/constant (today: just containing `"wifi_scan"`). Explicitly no runtime hardware-detection abstraction/framework built now — that gets designed later, informed by real hardware specifics, when the GPS phase is designed in its own future grill-me session.
- **Command-handling scaffolding.** Step 7 does NOT add any generic `command`-message rejection/dispatch scaffold (e.g. a generic "reject a command naming an unsupported capability" path), even though `unsupported_capability` is already a defined error code in PROTOCOL.md. That entire mechanism, including the `unsupported_capability` rejection path, is owned by the future `wifi_scan` follow-on step, designed together with the real command handler from a clean slate — building rejection-only scaffolding with no real handler behind it yet was explicitly rejected as premature.

### Step 7 implementation-level decisions (2026-09-07 follow-up grill-me session)

A second, narrower grill-me session filled in implementation gaps the first session (above) left open, before any code was written:

- **Capability-cache file (Flipper side).** Same pattern as the pairing-secret file: its own subdirectory (`capabilities/`, alongside the existing `pairings/`), one file per `board_id` named `<board_id>.dat`, written atomically (temp file, exact write verification, `storage_file_sync()`, close, rename) — mirroring `build_pairing_path`/`pairing_storage_save` in `flipper/flipper_esp32_over_ble.c`. The file stores the raw canonical-CBOR `capability_response` *payload* bytes verbatim (not a separate hand-rolled on-disk format), decoded on load with the existing `cbor_codec` payload decoder. Rationale: reuses existing code, and a future wire-format change to `capability_response` is already a coordinated both-firmwares event under this project's conventions, so a non-sensitive cache file trivially invalidated by unpair+repair doesn't need its own independent migration story.
- **`capability_query` trigger timing.** Fully automatic, no UI gesture: fires immediately once runtime auth succeeds, whenever no `capabilities/<board_id>.dat` exists yet. Consistent with runtime auth itself already being automatic on every reconnect.
- **Missing/malformed `capability_response` handling.** No special-cased timeout or retry for this one exchange. Relies entirely on the existing 30-second idle-connection close and the protocol's general malformed-record handling (PROTOCOL.md's already-specified rejection/drop behavior). Since nothing is cached until a valid response lands, the failure mode is simply "retried automatically on the next reconnect" — building a bespoke retry timer for one specific message type this early was rejected as premature, consistent with how the rest of the protocol avoids per-message special-casing.
- **On-screen rendering.** Extend the existing single fixed-layout status screen (`draw_callback` in `flipper/flipper_esp32_over_ble.c`) with a compact line for `board` + `features` (today just `wifi_scan`), rather than building a new scrollable list view/screen. Backlogged: a real scrollable capability-list screen, once `features` actually grows large enough (likely around the GPS/wardriving phase) to need one — see Backlog below.
- **Build order.** No new shared contract needs freezing first (unlike step 3's `framing.c`/`cbor_codec.c`, which were genuinely new and needed independent parallel implementation to cross-check agreement) — `capability_query`/`capability_response`'s wire shape and field order are already fully specified, unchanged, in PROTOCOL.md/CAPABILITIES.md. `esp32-developer` and `flipper-developer` implement in parallel directly against the existing spec plus the four decisions above, with no prerequisite contract-authoring step.

### Capability roadmap

Capabilities ship incrementally, gated on hardware actually present on a given board — see [CAPABILITIES.md](CAPABILITIES.md) for the registry format and the full, current capability list. See "Roadmap phases" above for how these map onto Phase 3/4/5.

1. **`wifi_scan`** (Phase 3) — first capability, needs no extra hardware.
2. **GPS + wardriving** (Phase 3, after a GY-NEO6MV2/NEO-6M GPS module is wired to the C6): add `ble_scan` and the composite `wardriving` capability. Wardriving runs autonomously from boot once started — it does not require an active Flipper connection to keep capturing — buffering into a bounded, power-loss-safe on-device log (see step 8), and streams the backlog plus live results to whichever Flipper connects and authenticates. Scan results captured before GPS achieves a fix are discarded (a future Flipper-settable backfill-to-first-fix option is backlogged, not yet implemented). Making this production-ready is Phase 3's stated aim.
3. **Heltec board support** (Phase 4, separate baseline, starts after Phase 3 completes): display and LoRa capabilities on a second, structurally different board — Heltec WiFi LoRa 32 V2, classic ESP32/Xtensa, not the C6 — see `docs/BASELINES.md`. This is a second target platform, not a peripheral addition to the C6.
4. **Zigbee/Thread recon** (Phase 5a): passive `zigbee`/`thread` scanning/sniffing capabilities, matching the `wifi_scan`/`ble_scan` pattern — no network joining or commissioning.
5. **Zigbee/Thread participation** (Phase 5b, much later, separately scoped): active stack participation (joining as an end-device / Thread node, possibly a border-router role) — an order of magnitude larger effort than recon; not committed to a timeline.
6. **`gpio_control`** (Phase 5): generic GPIO control, reserving strapping/JTAG pins (GPIO0, 4, 5, 8, 9, 15) from generic control actions.

### Multi-board pairing

- Multiple boards (e.g. a C6 and a Heltec) may be paired to one Flipper at a time, each with its own stored pairing record (step 5).
- The Flipper is the BLE peripheral, so only one BLE connection is active at a time: whichever paired board connects first occupies the slot. Switching boards today means powering down the currently-connected one so another can connect.
- **Backlog item:** a manual "disconnect current board" Flipper UI action to free the connection slot without physically powering off a board. Automatic arbitration (the Flipper detecting and prompting about a second board trying to connect while one is active) is gated on an unresolved BLE-HAL feasibility question — whether the Flipper's peripheral role can advertise while already serving a connection — and is not currently planned.

**Done when:** Flipper renders only the authenticated capability list reported by the paired board, correctly reflecting that board's hardware.

### Step 7 status (2026-09-07)

`esp32-developer` and `flipper-developer` implemented both sides in parallel, directly against
the two grill-me sessions' decisions above — no shared-contract-freeze step, as planned, since
`capability_query`/`capability_response`'s wire shape was already fully pinned in
`docs/PROTOCOL.md`/`docs/CAPABILITIES.md`. Both build-verified only (`idf.py build` /
`fbt.cmd fap_flipper_esp32_over_ble`), no hardware touched — see `docs/SESSION_MEMORY.md`'s
matching dated entry for full narrative detail. Summary:

- **ESP32**: new `feb_capability_query_payload_t`/`feb_capability_response_payload_t` codecs in
  `cbor_codec.h`/`.c`; `main.c` gained `handle_capability_query()` (hardcoded
  `board="esp32-c6-devkit"`, `firmware="0.1.0"`, `features=["wifi_scan"]`) and its dispatch from
  a new `RUNTIME_AUTH_STATE_AUTHENTICATED` branch. This is also the first code path that decodes
  any protected (post-auth) record at all — before this step, an authenticated-session record
  had nowhere to go and would have incorrectly failed auth. `idf.py build`: clean, `0xa7570`
  bytes, 56% of the app partition free.
- **Flipper**: matching codecs (diff-verified against the ESP32 copy — byte-identical except one
  pre-existing comment-wording difference on `FEB_CBOR_MAX_BYTES_LEN`); new `capabilities/`
  storage subdirectory (mirroring `pairings/`'s atomic temp-file/sync/rename pattern) with
  `build_capability_path()`/`capability_storage_exists()`/`_save()`/`_load()`; automatic
  query-or-load in a new `capability_bootstrap()` called from `handle_client_auth()`; response
  handling via a new 7-field-envelope branch in `profile_event_handler()`; the status screen's
  `draw_callback()` gained a 5th line for `board`+`features`. `fbt.cmd
  fap_flipper_esp32_over_ble`: clean, artifact 62,036 bytes.
- **Sequence-counter cross-check (done by the orchestrating session, not either agent):** both
  sides independently converged on identical semantics — `session_seq_out`/`session_seq_in`
  (Flipper) and `rt_tx_sequence`/`rt_rx_sequence` (ESP32) each reset at connect and set to `1`
  exactly once client_auth completes, confirming the two independent implementations agree
  without needing a pre-freeze step, as the build-order decision predicted.
- **Real regression found and fixed during the Flipper-side implementation:** the new
  `AppEvent` capability fields pushed a couple of `post_*` functions' `AppEvent event` locals
  over this project's own documented static-storage threshold for anything reachable from
  `profile_event_handler` (the same 1280-byte-`BleEventWorker`-stack class of bug that has hit
  this codebase three times before — see step 3's `docs/SESSION_MEMORY.md` entries). Fixed by
  converting `post_pairing_phase()`/`post_capability_info()` to `static AppEvent event;` with an
  explicit per-call `memset`+field-reset (a static with a designated initializer would only run
  once).
- **Hardware-verified 2026-09-07.** Both boards flashed (`idf.py -p COM9 flash`; `fbt.cmd
  launch APPSRC=flipper_esp32_over_ble` on COM8), user-authorized. The ESP32's repeated,
  clean `hello`/`hello_ack`/`client_auth` cycles confirmed no regression to the existing
  runtime-auth path. The capability exchange itself is confirmed by a different, stronger
  piece of evidence than a log line: a brand-new `capabilities/esp32c6-acebe6fffeda.dat`
  (58 bytes) now exists on the Flipper, in a directory that did not exist before this
  session's implementation, matching the paired board's exact `board_id` — this file could
  only have been produced by a real `capability_query` → `capability_response` round trip
  (send, receive, decrypt, persist) using today's new code. The ESP32-side monitor capture
  never caught the literal `sending capability_response` log line, but this is a benign
  capture-window artifact, not a gap: attaching the monitor forces a board reset (a known
  quirk), so that capture window began *after* the real exchange had already happened and
  been cached — and `capability_query` fires only once per board by design, so it correctly
  did not repeat on the later reconnects the capture did observe. **Physical Flipper screen
  visually confirmed by the user 2026-09-07**: the new `board: features` line renders
  correctly — step 7's "done when" bar is now fully met on all three fronts (persistence,
  decode, and display).

## 8. Harden persistent state and release configuration

- Store the C6 pairing record in a dedicated encrypted NVS namespace with a version, validity marker, and atomic replacement procedure. Cross-referenced from `docs/CODE_REVIEW_FINDINGS.md` finding #17: today's ESP32 storage (`persist_pairing_secret`/`load_pairing_secret`) is a bare `nvs_set_blob()` with none of version/validity-marker/atomicity — a real, currently-uncosted gap against the written contract that this step owns closing.
- Implement explicit local unpair/factory-reset behavior and define which pairing record is removed on each side (unpairing one board must not disturb other stored pairing records — see step 7).
- Store the Flipper pairing record through an atomic app-owned storage update (temporary file, exact write verification, `storage_file_sync()`, close, rename) and do not log it. Treat local SD-card, debug, and modified-firmware access as outside the standalone FAP protection boundary (see [PROTOCOL.md](PROTOCOL.md) "Implementation security requirements" — this is an accepted limitation, not a gap to close in this phase).
- The wardriving buffer (step 7) uses the same atomic-persistence philosophy: a hand-rolled, checksummed, append-only log on raw flash (not a FAT-based wear-levelling filesystem), so an unclean power loss (e.g. car ignition cut) loses at most the single record being written at that instant, never the rest of the log. Circular — drop the oldest record when the buffer is full.

**Done when:** interrupted writes, reboot during pairing, unpair, and factory reset leave no ambiguous paired state, for both the pairing record and the wardriving log.

**See also:** "Deferred: hardware hardening" below — Secure Boot, flash encryption, and related eFuse-dependent work are explicitly out of scope for this phase and are not part of this step's "done when" bar.

## 9. Validate the complete system

- Run codec, key-derivation, X25519, HMAC, AES-GCM, and CBOR test vectors on both targets.
- Fuzz the fragment and CBOR parsers with truncated, oversized, duplicated, and invalid inputs.
- Exercise first pair, expired window, reset-and-repair, reconnect, ciphertext tampering, replay, bad confirmation, power loss, and unpair flows.
- Confirm the idle-connection timeout (30 seconds without a record, per [PROTOCOL.md](PROTOCOL.md)) does not fire spuriously during a live wardriving view session through a stretch with no new results — add a keepalive/heartbeat if needed (backlog item, not yet implemented).
- Re-confirm (not re-derive) step 4's radio-coexistence interval bounds under real authenticated, streaming wardriving traffic load — step 4's fixed-payload/throwaway-timer test validates the coexistence *mechanism* (Wi-Fi/BT scheduler, NimBLE concurrent central+observer roles), which doesn't depend on payload content, but step 6's per-record AES-128-GCM/HMAC compute and step 7's continuous streaming traffic pattern are heavier loads step 4 never exercised.
- Document tested ESP-IDF and Flipper firmware revisions, flashing steps, reset behavior, and residual security boundary.

**Done when:** the full pairing-to-command flow succeeds repeatedly on the physical C6 and Flipper, and every negative security test has the specified rejection behavior.

## Deferred: hardware hardening (explicitly out of scope for this phase)

Physical possession of either paired device (Flipper or ESP32) is accepted as fully compromising to that device's stored secrets and data for the current phase — see [PROTOCOL.md](PROTOCOL.md) "Implementation security requirements." Treat a lost or stolen paired device as game over for that pairing relationship: reset (ESP32) or unpair (Flipper) immediately. Other stored pairings are unaffected.

Because of that accepted threat model, none of the following are required for any "done when" bar in this plan, and none of them are scheduled:

- Secure Boot, flash encryption, and NVS encryption on the ESP32.
- Signed firmware updates and production debug/download restrictions.
- Any irreversible eFuse configuration.

**No irreversible hardware operations are to be performed on any board in this phase.** If this work is ever picked up, it requires a dedicated sacrificial board for eFuse validation before any irreversible setting is applied to a board actually in use — do not attempt it on the primary development board.

## Wi-Fi scan capability (Phase 3 follow-on step)

### wifi_scan implementation decisions (2026-09-07 grill-me session)

A grill-me design-review session on 2026-09-07 walked the first real capability command end to
end, before any implementation started. This is the "follow-on `wifi_scan`-command step" named
in Phase 3's roadmap description above — not step 8 (hardened persistence) and not part of step
7 (which explicitly deferred all `command`/`status` handling to this step). Decisions:

- **Trigger and output.** Manual only: the Flipper user presses a "Scan now" action; the ESP32
  runs one scan and reports results. Nothing is persisted or exported to SD — that remains
  `wardriving`'s job once GPS lands (see [CAPABILITIES.md](CAPABILITIES.md)). Results are shown
  on-screen only, cleared when the user leaves the results view.
- **Display.** A new dedicated scrollable list view (Furi list/submenu widget) on the Flipper,
  showing every reported AP — not squeezed into the existing single fixed-layout status screen,
  and not truncated to a "top N" summary.
- **Result cap.** The ESP32 reports at most 32 APs per scan. If more are found, it keeps the 32
  with the strongest RSSI and silently drops the rest.
- **Wire format is new shared contract, not a step-7-style reuse.** Step 7's `capability_query`/
  `capability_response` reused an already-fully-specified wire shape, so `esp32-developer`/
  `flipper-developer` implemented directly in parallel with no freeze step. This step introduces
  genuinely new payload shapes (the `command`/`status` types were previously undefined in
  practice), so it follows steps 3/5's pattern instead: freeze the exact CBOR shapes in
  `docs/PROTOCOL.md`/`docs/CAPABILITIES.md` first (done this session — see PROTOCOL.md's new
  "`wifi_scan` command and status payloads" section), add test vectors, then delegate parallel
  implementation.
- **`command.arguments`.** Always an empty map for `wifi_scan` — no scan configuration exposed
  yet (channel selection, active/passive, duration, etc. are all backlog items for later if a
  real need shows up).
- **`status.state` values.** Only `"partial"` and `"complete"` — no separate "scan started"
  acknowledgment state, and no total/progress count field. Reasoning: the scan itself completes
  server-side in one shot (`WIFI_EVENT_SCAN_DONE` fires once, after ~1.5-2s of default-timing
  full-channel active scanning — well under the 30-second idle-connection timeout, so no
  keepalive/heartbeat design is needed here), so pagination exists purely to fit the 512-byte
  payload cap, not to reflect live scan progress. A progress indicator would only reflect
  page-delivery progress, not real scan progress, and was rejected as not worth the added field.
- **Per-AP fields and two real wire-format gaps closed this session:**
  - SSID is encoded as a CBOR byte string, not text — real 802.11 SSIDs are not guaranteed valid
    UTF-8, and this project's hand-rolled codec has no UTF-8 validation on `text` fields at all
    (a pre-existing gap, first exposed by this field). The Flipper sanitizes for display.
  - RSSI is encoded as an unsigned offset (`rssi_dbm + 128`), not a native signed integer,
    because PROTOCOL.md's "Canonical CBOR encoding (definition)" section explicitly rejects
    negative integers in payload maps and calls widening that rule "a deliberate protocol
    revision... not a decoder-local choice." The offset encoding avoids touching that rule.
  - PHY generation is one text string naming the highest generation the AP advertises support
    for (`"11b"`/`"11g"`/`"11n"`/`"11ax"`), not a bitmask/array — a real AP typically advertises
    several generations at once for backward compatibility, and CAPABILITIES.md already frames
    PHY generation as one per-AP property.
  - Auth mode is a full-fidelity string enum matching every `wifi_auth_mode_t` value in the
    pinned ESP-IDF v5.5.2 (`esp_wifi_types_generic.h`), plus an `"unknown"` fallback for any
    value not in that set — added to the protocol at the user's request, even though
    CAPABILITIES.md did not previously mention auth mode at all.
- **Busy handling, no dedup cache.** A `wifi_scan` command received while one is already running
  is rejected with `error` code `busy`. No `request_id` dedup cache is kept for this capability:
  PROTOCOL.md's general dedup-cache guidance (under "Reliability and reconnect behavior") exists
  to avoid repeating non-idempotent work, and `wifi_scan` is a pure read with no side effects, so
  it doesn't apply — a retried `request_id` just triggers a normal fresh scan.
- **Wi-Fi driver lifecycle (ESP32).** `esp_netif`/default event loop/`esp_wifi` initialize once
  at boot, in STA mode without connecting to anything, and stay resident for the device's whole
  lifetime — matching the future `wardriving` capability's need for an always-on radio, and
  avoiding any re-init race with an already-active BLE connection. Not lazy-initialized on first
  scan.
- **Scan execution model (ESP32).** The scan runs asynchronously:
  `esp_wifi_scan_start(..., block=false)` plus a `WIFI_EVENT_SCAN_DONE` handler on the default
  event-loop's own task. It must not run synchronously inside the existing protected-record
  handler (`esp32/main/main.c`'s authenticated-record dispatch, around the
  `RUNTIME_AUTH_STATE_AUTHENTICATED` branch), because that code runs on the NimBLE host task —
  the same task responsible for BLE connection supervision — and a multi-second blocking scan
  there risks connection-timeout/missed-event regressions of the kind this project has already
  hit twice (the step-3 stack-overflow bugs from unsafe assumptions about BLE-callback-path
  code).

**Nothing has been built yet** — this session was design-only. Next: write the frozen contract
into `docs/PROTOCOL.md`/`docs/CAPABILITIES.md` (see PROTOCOL.md's new "`wifi_scan` command and
status payloads" section, added this session) plus new test vectors covering the RSSI-offset and
SSID-as-bytes encodings in particular, then delegate `esp32-developer`/`flipper-developer`
implementation in parallel.

### wifi_scan test vectors (2026-09-07)

Added to `tests/vectors/generate_vectors.py`/`vectors.h` (orchestrating session, not delegated —
same rationale as the pre-step-7 shared-contract fixes: these are cross-firmware convergence
artifacts both implementations must agree on byte-for-byte). Covers:

- `<ap-result>` encodings: a normal entry, plus the `rssi_offset` encoding's two extremes
  (`rssi_dbm` -128 -> offset 0, +127 -> offset 255), a hidden network (zero-length SSID), a
  non-UTF-8 SSID (raw bytes), and the `auth = "unknown"` fallback.
- `result` maps (`{"aps": [...]}`) with one AP, three APs, and an empty array (the "prior
  `partial` already delivered everything" case).
- `command` payload: valid (empty `arguments`) and malformed (non-empty `arguments`, must be
  rejected `invalid_command`).
- `status` payload: mid-scan `partial`, final `complete` with results, final `complete` with an
  empty `aps` array, and a malformed `state` string.
- End-to-end: the valid `command` and both real `status` payloads wrapped as protected records
  under the existing step 6 golden session, continuing its per-direction sequence counters
  (`command` is that session's first Flipper->ESP32 protected record; the two `status` records
  continue the ESP32->Flipper counter after the existing `FEB_VEC_SESS_PROT1`/`PROT2` at
  sequence 3/4).

Not generated: a 32-AP-capacity vector or a >32-AP overflow case — the cap/keep-strongest
behavior is ESP32-side scan-result logic to unit-test with its own mocked scan data, not a codec
concern the shared vector file needs to carry.

`generate_vectors.py` runs clean; `vectors.h` regenerated. Next: delegate
`esp32-developer`/`flipper-developer` implementation in parallel against these vectors plus the
frozen `docs/PROTOCOL.md` contract, then build-verify both (host-native tests + `idf.py build` /
`fbt.cmd fap_flipper_esp32_over_ble`). Hardware-verify only after explicit user go-ahead.

### wifi_scan implementation: ESP32 side complete (2026-09-07)

`esp32-developer` implemented the ESP32 side in parallel with `flipper-developer` (Flipper side
in progress as of this entry). Summary:

- **Codec** (`cbor_codec.h`/`.c`): generic `feb_command_payload_t`/`feb_status_payload_t`
  (capability/request_id/arguments; request_id/state/result — not wifi_scan-specific, per
  PROTOCOL.md's general `command`/`status` types) plus the wifi_scan-specific
  `feb_wifi_scan_ap_t`/`feb_wifi_scan_result_payload_t`.
- **Wi-Fi driver**: `esp_netif`/default event loop/`esp_wifi` initialized once at boot in STA
  mode, never connecting, resident for the device's lifetime — `esp32/main/CMakeLists.txt` now
  also requires `esp_wifi`/`esp_netif`/`esp_event`.
- **Command dispatch**: new `handle_command()` in the authenticated protected-record branch —
  rejects wrong capability (`unsupported_capability`), non-empty `arguments`
  (`invalid_command`), and a scan already running (`busy`).
- **Scan execution**: `esp_wifi_scan_start(block=false)` plus a `WIFI_EVENT_SCAN_DONE` handler on
  the default event-loop task (never the NimBLE host task) — fetches up to 64 raw results (see
  judgment call below), keeps the 32 strongest by RSSI, collapses PHY bits to the highest
  generation string, maps `wifi_auth_mode_t` to PROTOCOL.md's full enum with an `unknown`
  fallback, then hands off to the NimBLE host task via a `ble_npl_callout` (the same
  cross-task pattern `reassembly_timeout_co` already used).
- **Status delivery**: batches APs per `status` record to fit `FEB_CBOR_MAX_PAYLOAD`, chaining
  `partial` → ... → `complete` through a new tx-completion action, reusing the existing
  fragmentation/AES-256-GCM machinery.

**Real spec gap found and fixed (promoted into `docs/PROTOCOL.md`'s "Nesting depth" section):**
validating `status.result` (`result` map → `aps` array → `<ap-result>` map → its own fields —
3 real container levels) by continuing to add depth on top of the existing "payload starts at
depth 2" convention pushes the innermost fields to depth 5, one past `FEB_CBOR_MAX_NESTING` (4)
— confirmed as an actually-failing host-native test against the frozen
`FEB_VEC_WIFI_SCAN_STATUS_PARTIAL_PAYLOAD`/`STATUS_COMPLETE_PAYLOAD` vectors before the fix.
Resolved: `command.arguments`/`status.result` each get a **fresh nesting-depth budget starting
at 0** when a schema-aware decoder recurses into them for a still-generically-validated
sub-piece, rather than inheriting depth from the field's position inside `payload`. Purely a
decoder-internal bookkeeping convention with no wire representation — but both firmwares must
apply it identically. Flagged to the in-flight `flipper-developer` agent directly (session
message, not a rerun) so it applies the same convention rather than risking an independent,
possibly-incompatible fix or shipping without ever exercising the case.

**Other judgment calls** (not spec gaps, ESP32-local): `wifi_ap_record_t.ssid` is a null-padded
33-byte ESP-IDF buffer with no separate length field, so an SSID containing an embedded null
byte (legal, rare) reports truncated — an accepted ESP-IDF API limitation, documented inline,
not fixable in this app. Raw-scan fetch is capped at 64 (`FEB_WIFI_SCAN_RAW_MAX`, distinct from
the 32-AP wire cap) because ESP-IDF doesn't guarantee RSSI-ordered results, so real
top-32-by-RSSI selection needs headroom; a real scan returning >64 APs would only consider the
first 64 in driver order — accepted as extremely-unlikely-in-practice rather than building
dynamic allocation for it. On disconnect, `esp_wifi_scan_stop()` is called (never clearing
`wifi_scan_in_progress` directly) so the guaranteed resulting `SCAN_DONE` event does cleanup
uniformly through the normal no-connection discard path, avoiding a race between an old
in-flight scan's completion and a new connection's fresh scan.

Build-verified only, no hardware touched: host-native tests pass (46/46 `tests/esp32/build.ps1`
incl. 17 new wifi_scan checks; 31/31 `build_session.ps1` incl. 6 new end-to-end wifi_scan
protected-record checks matching the frozen vectors byte-for-byte); `idf.py build` clean after a
full rebuild, `0x12e420` bytes, 21% of the app partition free.

### wifi_scan implementation: Flipper side complete (2026-09-07)

`flipper-developer` implemented the Flipper side against the same frozen contract, including the
nesting-depth fix above (independently rediscovered the same gap before the coordinator's message
about it arrived — converged on the identical depth-0-budget fix; confirms the two firmwares
agree, the same cross-check value step 3's parallel-implementation pattern has produced before).
This run stalled once (no progress for 600s) mid-edit on the depth fix and was resumed with an
explicit instruction to re-verify actual file state before continuing, rather than trust its own
prior stated intent — finished clean on resume. Summary:

- **Codec** (`cbor_codec.h`/`.c`): matching generic `command`/`status` codecs and the
  `wifi_scan`-specific `<ap-result>`/`result` codecs; `arguments`/`result` captured as opaque
  spans at the generic-codec layer (capability-specific validation, e.g. "arguments must be
  empty for wifi_scan," lives in the dispatch layer, not the codec).
- **UI**: a new scrollable results view (`AppScreenWifiScanResults`) added to this file's
  existing single-`ViewPort`/`AppEvent`-queue architecture (see judgment call below) rather than
  a `Submenu`/`ViewDispatcher` — Up/Down scroll, header shows found count, one `SSID chN dBm`
  row per AP, footer position indicator. "OK: scan" appears on the main screen only once a
  session is active and the board advertises `wifi_scan`. Results clear on Back-from-results and
  on any disconnect/reconnect.
- **Command send / status receive**: `send_wifi_scan_command()` sends from the app's own main
  thread (the first sender in this file not running inside a BLE-thread callback);
  `handle_wifi_scan_status()`/`handle_runtime_error()` decode on the BLE thread and post events
  through the existing queue.
- **Stack safety**: all BLE-thread-reachable decode structs made `static`, matching the existing
  `post_pairing_phase`/`post_capability_info` pattern; the 32-AP display array is file-scope
  `static` (not just for `BleEventWorker` — this app's own main-thread stack size isn't pinned
  anywhere in this project's docs, so it was kept off any thread's stack).

**Judgment calls / flagged concerns (not fixed, recorded here and in Backlog below):**
- **UI widget deviated from the task brief's literal wording** ("Furi list/submenu widget") in
  favor of extending the existing single-`ViewPort` architecture, since this app has no
  `ViewDispatcher`/scene-manager at all today and introducing one would be a materially larger
  structural change than this step's scope. Still satisfies CAPABILITIES.md/PLAN.md's actual
  requirement (dedicated scrollable view, every AP shown, cleared on exit).
- **Stack-usage concern: measured (2026-09-07, see `docs/SESSION_MEMORY.md`'s matching entry) and
  fixed, superseding the estimate below.** Real `-fstack-usage` measurement using the actual
  `fbt.cmd`-invoked compiler flags found the worst case was 6 stacked `feb_cbor_skip_value` frames
  (depths 0-5, not 5 — the depth check fires one call *after* the guard value, so the
  instantly-rejected depth-5 call still costs a full frame), totaling 1016 of 1280 bytes (20.6%
  headroom, below this project's 30% bar) before the fix, purely from `flipper/cbor_codec.c`'s
  per-recursion-level duplicate-key-check arrays. Fixed by making those arrays `static`, indexed
  by recursion depth (Flipper-side only, `cbor_codec.h`'s contract/ESP32 side untouched) — new
  worst case 632 of 1280 bytes (49.4%, 50.6% headroom). One frame (`hci_user_evt_proc()`, ST
  vendor code) remains unmeasured — flagged as a caveat, not assumed negligible.
  *(Original estimate, superseded but kept for history: the depth-0 `feb_cbor_skip_value()`
  budget for `arguments`/`result` lets a misbehaving-but-authenticated ESP32 force up to 5 nested
  stack frames through one call site — 2 more than any other `skip_value` call site in this file
  reaches today. Rough estimate (~150-200 bytes/frame, compiler-dependent): ~900-1100 bytes of the
  1280-byte `BleEventWorker` budget in the theoretical worst case — tighter than anything else in
  this codebase, though not a confirmed overflow, and not something either agent could
  unilaterally tighten once it's a fixed cross-firmware contract decision.)* Given this project has
  hit this exact bug class three times already (step 3's two stack-overflow entries, this
  session's step-7 `AppEvent`-locals fix), **do not treat clean builds as proof of stack safety
  here** — see Backlog below.
- **No `request_id` correlation on `busy` errors**: any protected `busy` error surfaces in the
  UI regardless of whether its `request_id` matches the outstanding scan — acceptable since the
  UI itself never allows two scans in flight.
- **Unsynchronized cross-thread session-state access**: `send_wifi_scan_command()` (main thread)
  reads `session_key`/`session_seq_out`/`outgoing_message_id` that BLE-thread senders also touch,
  with no mutex — argued safe because "Scan now" is only reachable after
  `capability_bootstrap()`'s own BLE-thread send has already returned, not enforced by any lock.
- **Pre-existing gap, not introduced this session**: `capability_query`/`capability_response`
  (step 7) still have zero host-test coverage in `tests/flipper/`.

Build-verified only, no hardware touched: host-native tests pass (117/117 `tests/flipper/
build.ps1`, 57/57 `build_session.ps1` incl. 3 new end-to-end wifi_scan protected-record cases,
67/67 `build_pairing.ps1` unaffected); `fbt.cmd fap_flipper_esp32_over_ble` clean, artifact
70,204 bytes (up from 62,036 at step 7).

**Both sides now build- and host-test-verified against the same frozen contract and vectors.**

### wifi_scan `cbor_codec.h` cross-firmware convergence fix (2026-09-07)

The combined-repo sanity pass found real drift beyond the intentionally-per-firmware
`arguments`/`result` opaque-span handling: unlike every prior step, the two agents' independently
written `cbor_codec.h` headers were not byte-identical for the new wifi_scan section — the
Flipper's `feb_cbor_encode_wifi_scan_result_payload()` took `(out, out_cap, const
feb_wifi_scan_ap_t *aps, size_t ap_count)` where the ESP32's took `(out, out_cap, const
feb_wifi_scan_result_payload_t *payload)` (inconsistent with every other `encode_*_payload`
function in this codebase, which all take a payload struct pointer), plus a differently-named
macro (`FEB_WIFI_SCAN_MAX_APS` vs `FEB_WIFI_SCAN_MAX_APS_PER_RECORD`) and reworded comments. This
broke this project's established convention (since step 3) that `cbor_codec.h`'s actual API
surface — not just its `.c` implementation style — is held byte-identical between firmwares, with
only comment wording allowed to differ.

Fixed directly by the orchestrating session (same rationale as the pre-step-7 convergence fixes:
a cross-firmware .h-contract reconciliation, not board-specific implementation work). The
function was unused outside `tests/flipper/`'s own test file (the Flipper only ever *decodes* a
`result` payload — the ESP32 is the one that encodes it, since it's the scanner), so the fix was
low-risk: changed the Flipper's signature/macro name to match the ESP32's exactly, updated the
one `.c` implementation and its three call sites in `test_flipper_codec.c`, and renamed
`flipper_esp32_over_ble.c`'s one remaining reference to the old macro name. All three Flipper
host-test suites re-verified green after the fix (117/117 `build.ps1`, 57/57 `build_session.ps1`,
67/67 `build_pairing.ps1`), and `fbt.cmd fap_flipper_esp32_over_ble` rebuilt clean at the same
70,204-byte artifact size (confirming the signature change was purely mechanical, no behavior
change). `esp32/main/cbor_codec.h`/`flipper/cbor_codec.h` are now byte-identical except the one
pre-existing, already-known `FEB_CBOR_MAX_BYTES_LEN` comment-wording difference from step 6.

**Stack-usage measurement done (2026-09-07)** — see the "Stack-usage concern: measured..." bullet
above and `docs/SESSION_MEMORY.md`'s matching entry; real (not estimated), fixed on the Flipper
side.

**Hardware verification started 2026-09-07, was blocked on a new ESP32-side bug — bug now fixed
and hardware-reverified, full pass still pending.**
See `docs/SESSION_MEMORY.md`'s "2026-09-07: wifi_scan first hardware test" and "`nimble_host`
wifi_scan stack-overflow fix" entries for full detail. Summary: a real, deterministic
Flipper-side bug (`FEB_WIFI_SCAN_CMD_PAYLOAD_MAX_LEN` undersized, 32→64 fixed) was found and
fixed — the OK-press send path is hardware-confirmed working end-to-end at the transport/crypto
layer. Immediately after, a **new, deterministic ESP32-side bug** was found: the ESP32 crashed
with a `Guru Meditation Error: Core 0 panic'ed (Stack protection fault)` in FreeRTOS task
`nimble_host` (~4084-byte stack) roughly 4-5 seconds after `wifi_scan started` logs, every time
(2/2 repro rate). Root cause: `wifi_scan_send_next_batch()` (runs entirely on `nimble_host`) held
its working set — two full 32-entry `feb_wifi_scan_result_payload_t`, a `feb_status_payload_t`,
and a `FEB_CBOR_MAX_PAYLOAD`-sized buffer — as stack-locals. **Fixed** by converting all four to
file-scope `static` (same bug class, same fix pattern as this project's Flipper-side
`BleEventWorker`/ported-X25519 stack issues). `idf.py build` passes clean.
**Hardware-reverified 2026-09-07: 3/3 clean `wifi_scan` attempts, zero crashes** (previously
2/2 crashed before the fix), with the idle-reconnect/runtime-reauth cycle also confirmed clean
across ~30 cycles in the same capture window (no regression).

**wifi_scan hardware verification fully closed out (2026-09-07).** See
`docs/SESSION_MEMORY.md`'s "ESP32-side `feb_cbor_skip_value` stack check, busy-path finding,
and results-screen confirmation" entry for full detail:
- ESP32-side `feb_cbor_skip_value` stack usage measured (66.3% headroom, clears this
  project's 30% bar) — no fix needed.
- Busy/error path confirmed unreachable from the real UI by design (client-side gate in
  `flipper/flipper_esp32_over_ble.c` prevents a second command while one is in flight) — not
  a bug, no hardware repro needed to confirm this.
- Results-screen rendering user-confirmed correct on real hardware, including correct
  screen-state recovery across an idle-timeout reconnect.
- **Accepted gap:** no non-ASCII/non-printable SSID was available nearby to test that specific
  render path on real hardware (host-native codec tests already cover the encoding). Per this
  step's own design note ("if one is nearby"), this doesn't block closing out verification.

**Done when** (the step's original wire-format/build bar) was already met 2026-09-07 at
implementation; the follow-on hardware-verification pass described above is now also
complete, with the one accepted non-ASCII-SSID gap noted above.

## Backlog

Smaller items surfaced during design review, not yet scheduled to a specific step above.

**Next-session order (2026-09-06 grill-me session, decided before any of this was implemented):**
the in-firmware factory-reset gesture below was next and **has since been implemented and
build-verified** (2026-09-06, same day) — before step 7 starts, per the existing "scheduled
soon, before step 7" note on that item. Next up: the `MAX_RECONNECT_RETRIES` hard-stop fix and
the 30-second idle-connection-timeout enforcement (both below), in either order, since step 7
depends on neither directly but both matter more once step 7 makes "board runs unattended for
hours" a real use case. The missing `unsupported_version` handling (also below) can wait past
step 7 — lowest urgency of the four, no concrete failure scenario pending. This ordering was a
deliberate choice to keep the factory-reset work scoped to one clean unit rather than stacking
unrelated bugfixes into the same build-verify pass.

**Reprioritized same day, later 2026-09-06 diagnosis session, then implemented same day:** the
idle-connection-timeout item was the immediate next implementation task (ahead of
`MAX_RECONNECT_RETRIES`, which was still open at the time) — see its entry below for the
confirmed root-cause chain and the implementation that landed. It stopped being a
step-7-readiness nicety and became a fix for a real bug the user hit: closing and reopening
the Flipper FAP mid-session leaves the ESP32 stuck until a physical reset. The
`MAX_RECONNECT_RETRIES` hard-stop fix noted as next-up here **has since been implemented,
build-verified, and hardware-verified (2026-09-07)** — see its own backlog entry above. Both
items previously blocking step 7 (idle-timeout hardware test, `MAX_RECONNECT_RETRIES` hardware
test) are now done. **Step 7 (board identity/capability registry) is next**, and per this
project's convention of a dedicated grill-me design session before implementing a new roadmap
step (steps 3, 5, and 6 each got one), that design session should happen before writing any
step 7 code.

- **Unexplained "blocked by classifier" security-warning annotation on a Haiku subagent's
  completed task** (2026-09-07, deploying the FAP to the Flipper via `fbt.cmd launch` as part
  of the pre-step-7 hardware re-verification — see `docs/SESSION_MEMORY.md`'s "pre-step-7 fix
  plan hardware-verified" entry). The agent's own tool-by-tool trace showed nothing unusual
  (only port-discovery queries and the documented deploy flow), a direct follow-up question to
  the same agent found nothing in its transcript matching the flag, and the flagged task's
  output file on disk was empty (0 bytes) when inspected, so the actual cause couldn't be
  determined from either side. Treated as a probable harness-level false positive on a
  hardware-write action rather than a real problem, since the deploy's own output was
  unambiguous and the subsequent ESP32-side hardware behavior independently confirmed the
  Flipper had correctly validated the runtime-auth handshake. Not investigated further this
  session — worth a closer look if it recurs (same flag on a similar hardware-write task) or if
  a way is found to inspect the classifier's own reasoning rather than just the subagent's
  transcript.
- Manual "disconnect current board" Flipper UI action (step 7).
- Automatic BLE connection arbitration between multiple paired boards — gated on an unresolved BLE-HAL feasibility question (step 7).
- GPS backfill-to-first-fix as a Flipper-settable wardriving option, instead of discarding pre-fix results (step 7).
- Idle-connection keepalive/heartbeat during a live wardriving view session (step 9).
- Real scrollable capability-list screen on the Flipper, once `features` grows large enough to need one (step 7's initial implementation extends the existing single status screen instead — see "Step 7 implementation-level decisions" above).
- ~~Measure real BleEventWorker stack usage for the wifi_scan `command`/`status` decode path~~
  **Measured and fixed 2026-09-07** (Flipper side only) — real `-fstack-usage` against the actual
  `fbt.cmd` compile flags found the true worst case was 1016/1280 bytes (20.6% headroom, below
  this project's 30% bar), driven by `feb_cbor_skip_value()`'s recursion actually reaching 6
  stacked frames (depths 0-5, not 5 — the depth check fires one call after the guard value).
  Fixed by making the map case's per-recursion-level duplicate-key arrays `static`, indexed by
  depth (72 bytes/frame instead of 136); new worst case 632/1280 bytes (49.4%, 50.6% headroom).
  See `docs/SESSION_MEMORY.md`'s matching 2026-09-07 entry for the full per-function table,
  the fix rationale, and the one remaining measurement caveat (`hci_user_evt_proc()`'s own
  frame, ST vendor code, not measured). ESP32-side `feb_cbor_skip_value` was not examined for
  the same issue — flagged as an open question for `esp32-developer`, not fixed here (this task
  was explicitly scoped to the Flipper side only). Hardware verification of this fix is
  **done** (2026-09-07) — the Flipper-side send path is confirmed reaching the ESP32 every
  time; see the "Wi-Fi scan capability" section's hardware-verification entry above for the
  separate ESP32-side crash bug this uncovered, still open.
- **New, not yet fixed (2026-09-07): ESP32 `nimble_host` task stack overflow crash during
  wifi_scan.** See the "Wi-Fi scan capability" section's hardware-verification entry above and
  `docs/SESSION_MEMORY.md`'s matching dated entry for full detail — a `Guru Meditation Error`
  stack-protection fault in the ~4KB `nimble_host` task, deterministic, ~4-5s after every
  `wifi_scan started` log line. Needs `esp32-developer` to trace the actual call chain and fix
  it before wifi_scan hardware verification can continue.
- Check ESP32-side `feb_cbor_skip_value()` (in `esp32/main/cbor_codec.c`) for the same
  recursion-depth stack-usage issue found and fixed on the Flipper side above — flagged but not
  examined, since that task was explicitly scoped to the Flipper only.
- Adopt a real `ViewDispatcher`/scene-manager architecture on the Flipper FAP, instead of the single-`ViewPort`/`AppEvent`-queue pattern every screen so far (including wifi_scan's results view) has been bolted onto — flagged by `flipper-developer` as a materially larger structural change than any single step's scope, but each new screen makes the single-`ViewPort` approach a bit more strained.
- Add host-test coverage for `capability_query`/`capability_response` (step 7) on the Flipper side — currently zero, a pre-existing gap noticed while adding wifi_scan's own test coverage.
- Mutex (or documented-safe alternative) for the Flipper's cross-thread `session_key`/`session_seq_out`/`outgoing_message_id` access — `send_wifi_scan_command()` (app main thread) and the existing BLE-thread senders touch the same session state with no lock today; currently argued safe only by a UI-gating invariant ("Scan now" unreachable before `capability_bootstrap()`'s send returns), not enforced by any lock.
- Automatic pause-on-degradation fallback for concurrent BLE-source wardriving scanning while connected — only if step 4 shows concurrent operation is unstable.
- Generalize the Flipper's pairing-flow LED status stub (step 5, 2026-09-05 session: continuous blue blink while waiting/handshaking, solid blue on pairing success, off on failure) into a reusable status/notification abstraction usable by other app states (capability streaming, wardriving status, etc.), instead of the hardcoded single-flow stub built for step 5.
- ~~Flipper pairing files are saved under the wrong app's data directory~~ **Fixed
  2026-09-06** alongside step 6's implementation — see that step's "Step 6 status" entry
  for the real fix (`storage_common_resolve_path_and_ensure_app_directory()` called once
  from the app's own thread, not a cached `APP_DATA_PATH` string expansion as originally
  planned, since that macro is a compile-time string substitution with no thread-identity
  content to cache).
- ~~Custom BLE profile may stay connectable after "paired," allowing silent re-pairing
  without a fresh OK-press~~ **Resolved by design, not by a targeted fix** — step 6's
  reset-vs-runtime-auth decision (see step 6's 2026-09-06 implementation decisions) means a
  reset no longer unconditionally opens a pairing window when a valid stored secret exists,
  which is what let this happen. No profile-teardown-on-success change was needed.
- **In-firmware, no-PC/no-session physical factory-reset gesture** — for the case where neither
  a PC (for the `esptool`/`parttool` NVS-erase path) nor a working Flipper session is available.
  **Fully designed in a 2026-09-06 grill-me session, then implemented and build-verified the
  same day** (`esp32/main/factory_reset.c`/`.h`, wired into `main.c`/`CMakeLists.txt` — see
  `docs/SESSION_MEMORY.md`'s "in-firmware factory-reset gesture: implementation" entry for the
  real hardware fact found along the way (onboard LED is WS2812-addressable, not plain GPIO) and
  the build result). **Not yet flashed or hand-tested** — that remains a separate follow-up
  requiring explicit user go-ahead. ESP32-only, no FAP/BLE involvement at all, exactly as
  designed:
  - **Mechanism:** read GPIO9 (the DevKitC-1's onboard BOOT button) as a plain digital input.
    Confirmed safe against this project's strapping-pin rule: `docs/hardware/esp32-c6-devkitc-1/README.md`
    and the vendor guide state GPIO8/9/etc. are strapping pins only "during chip power-up or
    system reset" — reading GPIO9 well after boot, and driving GPIO8 (the onboard RGB LED, also
    a strapping pin) for feedback, both fall outside that sampling window.
  - **When monitored:** continuously, via a low-priority background task/timer polling GPIO9 for
    as long as the board is powered on — not just a narrow boot window — so it works regardless
    of pairing/session state (matches the gesture's own motivation: a board that's been running
    unattended for a while, no laptop handy).
  - **Trigger:** hold BOOT for 5 continuous seconds. Releasing early aborts silently with no
    special "aborted" signal — normal operation just resumes.
  - **Feedback:** the onboard RGB LED (GPIO8) blinks for the duration of the hold. No distinct
    "confirmed" blink at the 5s mark — the erase-and-restart cycle itself is the confirmation,
    avoiding an extra unrequested timing constant.
  - **On commit (5s reached):** a full NVS-partition erase (`nvs_flash_erase()` +
    `nvs_flash_init()`) — deliberately the same scope as the existing documented PC-based method
    (`esptool.py erase_region`/`parttool.py erase_partition --partition-name=nvs`), not a
    narrower pairing-namespace-only erase, so the two recovery paths behave identically as more
    NVS namespaces are added later — followed by `esp_restart()`. No new post-erase code path:
    the reboot falls straight into the existing step-6 boot logic (no stored secret ->
    open a pairing window), reused verbatim.
  - **Delivery scope for the next session:** build-verify only (`idf.py build`), delegated to
    `esp32-developer`. Actual flashing/live-button-hold testing is a separate follow-up requiring
    explicit user go-ahead first, per this project's hardware-safety rule.
- **ESP32 `schedule_reconnect()`'s `MAX_RECONNECT_RETRIES = 5` hard-stops forever** after 5
  raw GAP-level `ble_gap_connect()` failures, instead of the step-4-documented "bounded
  exponential backoff then indefinite slow-cadence retry" production policy (found during
  step 6 implementation, 2026-09-06 — pre-existing since step 2/4, not introduced by step 6,
  but now more consequential under step 6's always-scanning runtime-auth mode: a board whose
  *physical link* fails 5 times in a row, as opposed to connecting fine but failing proof
  verification, goes idle forever until a physical reset).
  **FIXED 2026-09-06** — `esp32/main/main.c` now gives GAP-level connect failures the same
  two-phase exponential-then-flatten shape already used for runtime-auth proof failures
  (`runtime_auth_backoff_delay_ms()`/`schedule_runtime_auth_backoff()`): a new
  `reconnect_backoff_delay_ms()` mirrors `runtime_auth_backoff_delay_ms()`'s structure —
  exponential ramp (1, 2, 4, ... s) for the first `MAX_RECONNECT_RETRIES` (still 5, now
  meaning "ramp length" rather than a hard cap) consecutive failures, then a fixed cadence
  indefinitely. `MAX_RECONNECT_RETRIES` is never compared as a stop condition anymore.
  Deliberately does **not** reuse the auth path's 5-minute
  `FEB_RUNTIME_AUTH_SLOW_CADENCE_MS` — that cadence is doing double duty as an
  anti-hammering throttle against repeated bad credentials, which doesn't apply to a plain
  link-layer connect failure (the peer could be back and connectable within seconds), and
  reusing it here would reintroduce a "board goes quiet for a long stretch for no reason"
  symptom on this path like the one the scan-stall fix just eliminated on a different one.
  Instead uses its own new constant, `FEB_RECONNECT_SLOW_CADENCE_MS` (30 s). `reconnect_retries`
  increments are now clamped at `0xFFu` the same way `fail_runtime_auth()` clamps
  `runtime_auth_failure_count`, preventing `uint8_t` wraparound. Build-verified via
  `idf.py build` (exit success, `flipper_esp32_over_ble.bin` 0xa5740 bytes, 57% of the app
  partition free — unchanged from the pre-fix build).
  **HARDWARE-VERIFIED 2026-09-07** on the real ESP32-C6 (`COM9`)/Flipper (`COM8`) pair, via a
  real `idf_monitor.py` session. Repro method: with the FAP running, physically separated the
  two devices to a marginal-range distance. This produced two distinct failure shapes, both
  worth recording since only one exercises this fix:
  - Short NimBLE-internal link-establishment retries (`"Reattempt connection; reason = 0x3e"`)
    that give up within a few hundred ms surface as a plain `BLE_GAP_EVENT_DISCONNECT`
    (`reason=574` = `BLE_HS_HCI_ERR(0x3E)`) and correctly bypass this fix entirely — they fall
    into the disconnect-reason switch's `default:` case (`main.c:1132-1137`), which just calls
    `start_scan()` immediately, same as any plain idle/link-loss disconnect. This is expected,
    not a gap: cheap immediate rescan is the right behavior for a transient blip.
  - When the link genuinely couldn't establish within the full 30 s `ble_gap_connect()` timeout
    (`main.c:1042`), it surfaced as `BLE_GAP_EVENT_CONNECT` with `status=13` (`BLE_HS_ETIMEOUT`),
    logged `connection failed: 13`, and **this is what actually drives `schedule_reconnect()`**.
    Confirmed the exact intended ramp end to end: `reconnect retry 1/5 in 1000 ms` -> `2/5 in
    2000 ms` -> `3/5 in 4000 ms` -> `4/5 in 8000 ms` -> `5/5 in 16000 ms` -> then, past the old
    hard-stop point, `reconnect retry in 30000 ms (consecutive failures=6)` — confirming the
    ramp flattens to the indefinite 30 s slow cadence instead of stopping forever, exactly as
    designed. Bringing the Flipper back into range let the very next 30 s-cadence attempt
    succeed (`connected; exchanging MTU` -> `client_auth sent; runtime session authenticated`),
    confirming full recovery.
  - A live-diagnosis false start along the way, worth noting for future sessions: an early,
    narrower read of the log (before the 30 s `connection failed: 13` timeout had occurred)
    looked like `schedule_reconnect()` was unreachable for this failure class at all. That read
    was wrong — it was based on a monitor filter that happened to exclude the `connection
    failed:`/`connect start failed:` log lines, not a real gap in the code. Corrected once the
    fuller log was inspected; no code change was needed.
  **`MAX_RECONNECT_RETRIES` fix and its ramp-then-flatten behavior are now HARDWARE-VERIFIED and
  READY FOR PRODUCTION USE.**
- **New, undiagnosed bug: BLE scan pipeline silently stalls for minutes, then self-recovers**
  — found live during a literal FAP-close/reopen repro, 2026-09-06 (see
  `docs/SESSION_MEMORY.md`'s "literal FAP-close/reopen repro surfaces a new, undiagnosed
  scan-stall bug" entry for the full log evidence). After a clean app-exit disconnect and a
  few seconds of normal rescanning, the scan stopped producing *any* advertisement-report
  callbacks — not just missing the Flipper, but missing every nearby device too — for ~169 s
  (confirmed via the device's own uptime clock), then spontaneously found the peer and
  reconnected successfully with no error, panic, or watchdog reset logged. Distinct from both
  items above: the disconnect here was clean (not the radio-reset race), and no failed
  `ble_gap_connect()` ever occurred (not the `MAX_RECONNECT_RETRIES` path). Root cause
  unknown. **Reproduced a second time same-session under a real `idf_monitor.py` session**
  (env var `ESP_IDF_MONITOR_TEST=1` bypasses its TTY requirement) — ruling out the first
  capture's passive-serial workaround as the cause. Second stall: ~107 s of total scan silence
  (vs. ~169 s the first time), same shape, clean reconnect afterward.
  **ROOT-CAUSED and FIXED 2026-09-06** — see `docs/SESSION_MEMORY.md`'s "scan-stall
  ROOT-CAUSED" entry for the full evidence and the fix entry immediately after it for exact
  line numbers. It was not a stall at all: the BLE controller's duplicate-report filter is keyed
  on **device address only** (`CONFIG_BT_LE_SCAN_DUPL_TYPE_DEVICE=y`) and is never periodically
  flushed (`CONFIG_BT_LE_SCAN_DUPL_CACHE_REFRESH_PERIOD=0`), while `start_scan()` scanned
  forever with `filter_duplicates = 1`. When the FAP exits, the Flipper reverts to its default
  serial profile and advertises a non-matching payload (`len=28`) from the same MAC; the ESP32
  reported it once, didn't match, and the address was then cached — so when the FAP was
  relaunched and advertised the v2 payload (`len=24`) from that same MAC, every report was
  filtered out until cache eviction (minutes, depending on ambient advertiser density) let one
  through. Nothing hangs; the radio, host and scan are healthy throughout. **Fix:**
  `params.filter_duplicates = 0` in `start_scan()` (`esp32/main/main.c:370`) — peer rediscovery
  no longer depends on controller cache timing at all. **HARDWARE-VERIFIED 2026-09-06** — see
  `docs/SESSION_MEMORY.md`'s "scan-stall fix hardware-verified" entry: flashed to COM9, a real
  FAP close/reopen cycle reconnected in ~2.8 s (vs. the previous 100+ s stalls).
- **ESP32 `scan_log_count` never reset — scan logging died permanently after 40 events.**
  **FIXED alongside the item above, same session.** Was: a lifetime counter
  (`main.c:101` declared it, `main.c:996-998` gated logging on `< 40` and incremented it, never
  reset anywhere) across every scan session since boot, so in a dense RF environment
  per-advertisement logging went silent within seconds of boot and stayed silent until reboot,
  while the match/connect logic kept running unconditionally underneath. Found while
  root-causing the item above; it did **not** cause that bug but it made healthy 10 ms
  reconnects log-identical to genuinely delayed ones, which sent the first diagnosis down the
  wrong path. **Fix:** replaced with an aggregate that can't go silent — `main.c:104-106`,
  `1005-1007`, `1298-1308` — a summary line every `FEB_SCAN_SUMMARY_INTERVAL_MS` (10 s,
  `main.c:43`) piggybacked on the existing `reassembly_timeout_cb()` callout, reporting both the
  window count and an uncapped lifetime total, suppressed when the window count is zero. Same
  build-verified/not-yet-flashed status as the item above.
- ~~Neither firmware enforces the 30-second idle-connection timeout on an
  authenticated-but-idle session~~ **ESP32 side implemented and build-verified 2026-09-06**
  (`docs/PROTOCOL.md` "Reliability and reconnect behavior," found during step 6
  implementation, 2026-09-06). Out of scope for step 6 itself — there's no capability
  traffic to be idle *between* until step 7 — but needed to land before step 7 ships real
  traffic.
  **Promoted to immediate next step (2026-09-06 diagnosis session)** — this same gap turned
  out to explain a real, user-observed bug, not just a future step-7 need: closing and
  reopening the Flipper FAP while a runtime session is active leaves the ESP32 stuck
  "connected" until a physical ESP32 reset. Root cause traced by reading the pinned Unleashed
  source (`applications/services/bt/bt_service/bt_api.c`/`bt.c`): `flipper_esp32_over_ble.c`'s
  `stop_service()` (app-exit path) calls `bt_disconnect(app->bt)`, which maps to
  `bt_close_connection()` — for a custom (non-RPC) GATT profile this only calls
  `bt_close_rpc_connection()` (a no-op here) and `furi_hal_bt_stop_advertising()`; it does
  **not** terminate an already-established connection. The only actual teardown comes from the
  next call, `bt_profile_restore_default()`, which `bt.h` documents as causing a full BLE
  coprocessor "2nd core restart" — an abrupt radio reset, not a clean disconnect PDU. The
  ESP32 central never sees an explicit termination; it just stops hearing from the peer and
  has to wait out NimBLE's default connection supervision timeout (`ble_gap_connect()` is
  called with `NULL` connection params, i.e. library defaults — tens of seconds) before
  `BLE_GAP_EVENT_DISCONNECT` fires and reconnect scanning restarts. This matches this app's
  own documented design constraint (a comment at `flipper_esp32_over_ble.c:811-812` already
  notes the FAP avoids calling `bt_disconnect()` from inside `profile_event_handler` and
  instead "relies on the peer disconnecting" for BLE-level teardown) — the app-exit path is
  the one place that assumption doesn't hold, since there's no peer initiating anything.
  **Fix implemented 2026-09-06** (`esp32/main/main.c` only, source-only, build-verified via
  `idf.py build`, not yet flashed): piggybacked on the existing `reassembly_timeout_co`
  periodic callout (already firing every 1000 ms) rather than adding a new timer. A new
  file-scope `last_record_activity_ms` resets on `BLE_GAP_EVENT_CONNECT` and on every
  successfully reassembled inbound record (`FEB_FRAME_MESSAGE_COMPLETE`); the callout
  terminates the connection via `ble_gap_terminate()` once `now - last_record_activity_ms >=
  FEB_IDLE_TIMEOUT_MS` (30000 ms, matching the spec text exactly) while
  `runtime_auth_state == RUNTIME_AUTH_STATE_AUTHENTICATED`, then lets the existing
  `BLE_GAP_EVENT_DISCONNECT` handler and `start_scan()`/reconnect logic take over unchanged —
  no duplicated cleanup. Only *received* records reset the timer (a judgment call: PROTOCOL.md's
  "without a record" doesn't specify direction; outbound sends don't reset it). Build result:
  exit code 0, app binary `0xa5700` bytes, 57% of the smallest app partition free. **Not yet
  hardware-tested** — confirms the code compiles and is wired into the existing
  callout/disconnect paths, not that it actually resolves the FAP-close/reopen bug on real
  hardware; that requires flashing and requires explicit user go-ahead per this project's
  hardware-safety rule. A Flipper-side companion fix (finding a real way to proactively
  terminate an active connection on app exit, rather than relying on the profile-restore side
  effect) remains a secondary, not required, improvement — flag it as a follow-up if the
  ESP32-side timeout alone doesn't feel fast enough once tested on hardware.
  **Hardware-verified 2026-09-06** (real ESP32-C6-DevKitC-1 on COM9, real Flipper on COM8):
  flashed, paired fresh (prior stored secret didn't match the Flipper's storage state, so the
  BOOT-hold factory-reset gesture was used first to force a clean pairing window), then reset
  the ESP32 again to exercise the runtime-auth path. Serial log confirms the full mechanism:
  `client_auth sent; runtime session authenticated` at t=3.59s, then
  `idle authenticated connection (30792 ms without a record); terminating` at t=34.29s — firing
  right on schedule (30000 ms threshold plus up to ~1000 ms until the next periodic-callout
  tick) — followed by an unattended reconnect (scan → found peer → connect → hello/hello_ack/
  client_auth) completing with `runtime session authenticated` again at t=35.3s, no physical
  reset or user action required. Flipper showed `ESP32 session active` / solid blue LED both
  before and after, confirming the round trip resolved cleanly on that side too (the exact
  mid-cycle screen transition wasn't watched closely, but the before/after state is
  unambiguous). **Caveat:** this test reproduces generic 30-second silence, not the literal
  FAP-close/reopen user action the bug report described — but since the fix terminates on
  elapsed idle time regardless of *why* traffic stopped, this is a valid proxy for that bug
  path too; the app-exit-specific repro (closing the FAP mid-session, reopening it, timing the
  ESP32's recovery) is still a reasonable follow-up if ever in doubt.
- **Replace the 30-second idle-connection timeout with a heartbeat/keep-alive, instead of a
  blunt reconnect cycle** (raised 2026-09-06, after the user noticed the Flipper's LED/screen
  flicker back to "Waiting for ESP32..." every idle-timeout cycle during an otherwise-healthy,
  just-quiet session — see `docs/SESSION_MEMORY.md`'s scan-stall-fix-adjacent entries the same
  day). Root cause of the flicker: `docs/PROTOCOL.md`'s current spec ("a peer closes an idle
  authenticated connection after 30 seconds without a record") can't distinguish "the link is
  actually dead" from "the user just hasn't sent anything in 30s" — both cases hit the same
  disconnect-and-reconnect path, and each cycle briefly re-triggers the Flipper's
  waiting-for-first-connection LED/screen state (`PairingPhaseSessionActive` isn't excluded from
  the reset guard at `flipper_esp32_over_ble.c:1204`, alongside `PairingPhaseDone`). A heartbeat
  doesn't remove the need for a timeout — a truly dead link still needs to be detected and torn
  down — but it changes what resets the timer: instead of "30s without a *user* record," it
  becomes "30s without a heartbeat ack," so a genuinely idle-but-healthy session never
  disconnects/reconnects at all (no LED flicker, no unnecessary MTU/service-discovery/GATT
  re-negotiation overhead), while a dead link is still caught the same way. **Scope: a wire-
  protocol change**, not a quick patch — needs a new message type (or reuse of an existing one)
  in the shared contract, both firmwares implementing it identically, and a `docs/PROTOCOL.md`
  update to the "Reliability and reconnect behavior" section, per this project's
  keep-both-firmwares-in-lockstep convention. Deliberately **not** implemented this session (the
  user asked to backlog the idea, not fix the cosmetic flicker or the timeout mechanism now) —
  needs its own design/grill-me session before implementation, same pattern as the factory-reset
  gesture. The cosmetic Flipper-side LED/screen flicker itself (the guard-condition fix at
  `flipper_esp32_over_ble.c:1204`) is a separate, much smaller fix that remains available
  independently of whether this heartbeat redesign ever happens.
- **Neither firmware sends the spec-mandated `unsupported_version` error + connection close**
  for a bad `version` field on the session-envelope path (found during step 6 implementation,
  2026-09-06). This gap already existed on the pairing-envelope path before step 6 and was
  mirrored rather than unilaterally fixed this round.

### Remediations proposed after the 2026-09-05 hardware pairing test

All three bugs found during that test (see `docs/SESSION_MEMORY.md`) built cleanly and passed every host-native test. These items attack the *classes* rather than the individual bugs; the agent definitions in `.claude/agents/` were updated the same day with the behavioral half of the lessons.

- **Catch stack-budget violations at build time.** The `BleEventWorker` 1280-byte stack overflow has now recurred three times (step 3 twice, step 5 once), each time found only by crashing real hardware, because host-native tests run with a desktop-sized stack and are structurally incapable of catching it. Investigate adding `-fstack-usage` (per-function `.su` files) or `-Wstack-usage=N` to the FAP build and checking the functions reachable from `profile_event_handler` against a budget, so this class becomes a build failure instead of an MPU fault. Highest-value item here: it converts a recurring hardware-debug cycle into a compile-time check.
- **Promote implicit cross-firmware constants into the shared contract.** The Flipper's `PAYLOAD_MAX` (64, the Write characteristic's declared max attribute value length) was a Flipper-side implementation detail that was silently also a hard constraint on every ESP32 write — an invariant living in neither `framing.h` nor `docs/PROTOCOL.md`, therefore checked by neither side's tests. It is currently duplicated as `FEB_FLIPPER_WRITE_CHAR_MAX_LEN` on the ESP32 side. Consider moving it (and any similar peer-visible value) into `framing.h` so the two copies cannot drift silently.
- **Step 9 must exercise the real negotiated MTU, not just forced-small fragments.** Step 3's on-device smoke test pinned fragment capacity to `feb_fragment_capacity(23)` to exercise multi-fragment reassembly, which also guaranteed the oversized-write path was never tested — that is precisely how the ATT `0x0D` bug stayed latent. Add an explicit at-real-MTU case, and more generally treat "this test pins a parameter" as requiring a note about which failure modes the pinning excludes.

### Live code-health defects (consolidated 2026-09-05)

Small, known defects, gathered here because they were previously scattered across dated narrative entries in `docs/SESSION_MEMORY.md` and consequently sat unaddressed — some since step 3. The first four were fixed and build-verified on 2026-09-05 (see "Live code-health defect fixes" below); only the last remains open.

- ~~`esp32/main/main.c`: `FEB_TX_MAX_FRAGMENTS` (48) still documents its derivation as `ceil(FEB_MAX_RECORD_SIZE / feb_fragment_capacity(23))`...~~ **Fixed.**
- ~~`framing.h` (both copies): still documents `feb_fragment_record()` as writing "into a caller-owned buffer sized >= `FEB_FRAG_HEADER_SIZE` + capacity — no dynamic allocation,"...~~ **Fixed.**
- ~~`esp32/main/main.c`: `feb_reassembly_check_timeout()` is never called...~~ **Fixed** (and the same gap, not previously documented here, turned out to exist on the Flipper side too — also fixed, see below).
- ~~`cbor_codec.h` (both copies): `FEB_CBOR_MAX_BYTES_LEN` (256) bounds `ciphertext`, but `FEB_CBOR_MAX_PAYLOAD` is 512...~~ **Fixed ahead of step 6** — this was a pure constant-widening bugfix with no new decode path wired in, so it didn't need to wait for step 6's actual protected-record implementation.
- `esp32/main/pairing_crypto.c`: the X25519 ladder uses `mbedtls_mpi_mod_mpi()`, which is not documented as constant-time (data-dependent normalization loop). Accepted for now given the threat model and one-shot pairing use, but a genuine timing-side-channel gap versus a production constant-time field implementation. **Still open.**

### Live code-health defect fixes (2026-09-05)

Delegated to `esp32-developer` and `flipper-developer` in parallel, each build-verified (`idf.py build` exit 0; `fbt.cmd fap_flipper_esp32_over_ble` clean) after their changes. No board was flashed.

- **`FEB_TX_MAX_FRAGMENTS`** (`esp32/main/main.c`, ESP32-only — the Flipper side has no equivalent TX fragment array): changed from `48u` to the true `ceil(768/60) = 13u`, with a corrected derivation comment noting the accepted, currently-unhandled fallback gap if ATT MTU negotiation never completes (capacity would drop to `feb_fragment_capacity(23) = 16`, needing up to 48 fragments).
- **`feb_fragment_record()` doc comment** (`framing.h`, both copies): rewritten to describe the real implementation — a file-scope static buffer sized `FEB_FRAG_HEADER_SIZE + FEB_MAX_RECORD_SIZE`, safe because fragmentation is synchronous and single-in-flight on both firmwares, not a caller-owned buffer parameter.
- **`feb_reassembly_check_timeout()` wiring**: ESP32 side (`esp32/main/main.c`) now calls it periodically via an NimBLE `ble_npl_callout` on a `FEB_REASSEMBLY_CHECK_INTERVAL_MS` (half the 2s timeout) cadence. The Flipper side (`flipper/flipper_esp32_over_ble.c`) had the identical undocumented gap — found during this fix, not previously listed above — and now calls it from a FuriTimer callback guarded by a new `reassembly_mutex` shared with the feed path. Both follow the existing drop-and-continue policy on timeout: discard the reassembly buffer, keep the connection open, no response sent.
- **`FEB_CBOR_MAX_BYTES_LEN`** (`cbor_codec.h`, both copies): raised from `256u` to `512u` to match `FEB_CBOR_MAX_PAYLOAD`, so a maximum-size AES-128-GCM ciphertext can round-trip. Confirmed safe: both call sites just bound-check a pointer+length into the existing input buffer, no fixed-size copy target sized off the old constant.

### Pre-step-7 shared-contract convergence fixes (2026-09-07)

Six work items from `docs/CODE_REVIEW_FINDINGS.md` (#1, #2, #3, #4, #9, #10), executed per
`docs/CODE_REVIEW_FIX_PLAN.md` in the orchestrating session (not delegated to
`esp32-developer`/`flipper-developer`, per that plan's decision D6 — these are cross-firmware
convergence edits whose entire purpose is that both copies end up behaving identically). Done
ahead of step 7 because all six live in the shared CBOR/framing/session-crypto primitives step
7's capability payload schemas and protected-record path build directly on top of.

- **`feb_cbor_skip_value()` bounds/type/depth convergence** (findings #1–#3): added the missing
  `pos >= in_len` bounds guard to the Flipper's map-entry validator (was a real OOB read,
  remotely reachable pre-authentication); converged both sides to accept only CBOR majors 0
  (uint), 2 (bytes), 3 (text), 4 (array), 5 (map) — removed the ESP32's negative-integer
  acceptance and the Flipper's `true`/`false`/`null` acceptance; converged the Flipper's payload
  nesting depth from `1` to `2` at both call sites (`cbor_codec.c` and the previously-unscoped
  `pairing.c`), matching the ESP32 and `cbor_codec.h`'s own documented `FEB_CBOR_MAX_NESTING`
  derivation. Recorded in `docs/PROTOCOL.md`'s "Canonical CBOR encoding" section (permitted
  payload value types, nesting depth).
- **Over-length `board_id` now zeroes, not clamps** (finding #4): `flipper/session.c`'s
  `feb_session_derive_key()` and `flipper/pairing.c`'s `feb_pairing_derive_secret()` now zero
  their output and return early on `board_id_len > FEB_PAIRING_BOARD_ID_MAX_LEN`, matching the
  ESP32 — an all-zero key/secret fails authentication immediately and visibly, where the
  previous silent truncation derived a real but wrong key/secret. Not currently reachable (both
  application layers already bound `board_id` before reaching these functions), but step 7 adds
  new callers of this path.
- **Flipper GCM wrapper now fails deterministically** (finding #9): `feb_gcm_encrypt()`/
  `feb_gcm_decrypt()` in `flipper/session_crypto.c` now zeroize their output buffers on a
  hardware failure / failed tag verification, via `feb_secure_zero()`, matching the ESP32 and
  this project's "zeroize on every success and failure path" security property.
- **Trailing bytes after a decoded record now rejected** (finding #10): all three record-level
  decoders (`feb_cbor_decode_unencrypted`, `feb_cbor_decode_protected`,
  `feb_cbor_decode_pairing_envelope`) on both firmwares now require the decode to consume exactly
  `in_len` bytes. Payload-specific decoders (`hello`, `pair_*`, `error`, etc.) deliberately exempt
  — see `docs/PROTOCOL.md`'s "Trailing bytes" note for why.

Test-vector-driven: new shared vectors added to `tests/vectors/vectors.h` (via
`generate_vectors.py`) covering negative-int/`true`/`null` payload values, a truncated map header
(W1's repro), payload nesting at-limit and one-level-too-deep, and a record with one trailing
byte — plus direct `feb_cbor_skip_value()` unit cases (previously zero direct coverage on either
side). All six host-native test binaries (`tests/esp32/` and `tests/flipper/`, framing/cbor,
pairing, session) pass on both firmwares after the fixes; confirmed red beforehand in the
expected asymmetric pattern (predominantly Flipper-side failures, since four of the six items
were the Flipper's independently-written copy having drifted looser than the ESP32's). Build-
verified only (`idf.py build`, `fbt.cmd fap_flipper_esp32_over_ble`) — **not yet hardware
re-verified**; see `docs/SESSION_MEMORY.md`'s matching entry for the recommended pairing +
runtime-auth hardware check before these are trusted in the field.
