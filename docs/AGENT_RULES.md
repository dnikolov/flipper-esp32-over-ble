# Shared agent rules

Rules that apply identically across the firmware/board developer agents
(`esp32-developer`, `esp32c5-developer`, `heltec-developer`, `flipper-developer`), pulled out
of the four agent files so a fact isn't maintained in four places (TP-08). Each bullet is the
*rule*; `docs/LESSONS.md`'s anchor is the *why* — read the anchor when you need the incident,
skip it when you already know the rule. Each agent file keeps only what's board-specific
(pins, strapping, flash/RAM size, ports, reset quirks, ABI limits, stack budgets) and says to
read this file before touching shared/protocol/codec code.

## Reporting discipline

- A clean build (`idf.py build`, `fbt.cmd`) plus passing host-native tests is close to zero
  evidence about BLE/peer/radio/stack runtime behavior. Report build results as build results
  only, and state plainly what was validated statically versus what remains hardware-pending.
- Any fact about the *other* firmware's implementation must be confirmed by reading its source
  (`flipper/*.c` or the relevant `esp32*/heltec/main/main.c`), never assumed.
  `docs/LESSONS.md#flipper-facts-must-be-read-not-assumed`

## Handback contract

When reporting back to the caller (main session or another agent):

- ≤300 words.
- Use `file:line` references, not restated code.
- List changed files plus a diff summary (what changed, not the full diff).
- Never paste file contents that are already on disk — point at the path/lines instead.
- Flag blockers or decisions explicitly rather than silently picking one.

## Task-context contract

The caller supplies task context — don't reflexively read all of `docs/SESSION_MEMORY.md` or
`docs/PLAN.md` at the start of every invocation. Read only the section relevant to your board/
task, and only if the brief doesn't already give you what you need.

## Protocol / codec convergence

- **ATT MTU vs attribute length.** Size outgoing fragments against the peer's declared
  characteristic max length (`FEB_FLIPPER_WRITE_EFFECTIVE_MTU` / `FEB_FLIPPER_WRITE_CHAR_MAX_LEN`),
  never the negotiated ATT MTU — exceeding it is ATT error 0x0D / NimBLE status 269.
  `docs/LESSONS.md#att-mtu-vs-attribute-length`
- **`sdkconfig.defaults` isn't retroactive.** After editing it, grep the generated `sdkconfig`
  to confirm the value took; regenerate (delete + rebuild) if it didn't — defaults only seed a
  *fresh* sdkconfig. `docs/LESSONS.md#sdkconfig-defaults-not-retroactive`
- **Shared codec changes need the checker.** When adding shared codec functions/macros/structs
  in parallel with the peer firmware's agent, run `python tools/check_shared_headers.py` before
  reporting done — it catches macro/prototype drift but not struct-body shape divergence (a
  tagged union vs. named fields), so for any new composite/optional-field shape also read the
  struct definition on both sides. `docs/LESSONS.md#wardriving-struct-shape-divergence`
- **Header/implementation drift.** Keep a header's declaration comment in sync with its
  implementation whenever you touch either. `docs/LESSONS.md#header-contract-vs-implementation-drift`
- **Unwired API is a bug.** A declared shared-contract API left uncalled on either side is a
  bug on both firmwares, not just a TODO. `docs/LESSONS.md#unwired-declared-api`
- **Don't validate against the peer, validate against the spec.** Never check a bound (array
  size, nesting depth, buffer cap) against the other firmware's implementation or the shared
  test vectors — validate against `docs/PROTOCOL.md` directly.
  `docs/LESSONS.md#two-implementations-agreeing-is-not-two-implementations-being-right`
- **Wardriving BLE discovery doubles as the reconnect scan.** Keep `start_scan()`'s centralized
  `wardriving_ble_active` guard. Never skip the BLE interval callback's discovery for "no GPS
  fix"; discard no-fix results at window close instead. Clamp any no-fix Wi-Fi re-arm with
  `FEB_WARDRIVING_NO_FIX_RETRY_FLOOR_MS` — a 0 ms interval is legal and trips the task watchdog.
  All boards have regressed on one of these. `docs/LESSONS.md#wardriving-ble-discovery-is-the-reconnect-scan`
  and `#no-fix-retry-needs-a-floor`
- **`esptool` reads aren't reset-free by default** (most default to `--after hard_reset`, which
  boots the app and, on this project, opens a fresh pairing window). Pin
  `--before default_reset --after no_reset` for a true read-only snapshot.
  `docs/LESSONS.md#esptool-read-commands-are-not-reset-free`
- **Tight system-thread stack budgets.** Any callback path reachable from a small system/host
  thread (NimBLE host task, 4096 bytes on the ESP-IDF side; Flipper's `BleEventWorker`, 1280
  bytes; other Furi/FreeRTOS timer or service threads of similar size) must keep buffers
  ≥100 bytes file-scope `static`, add up the whole nested call chain rather than spot-checking
  one frame, and get a real `-fstack-usage` measurement before being trusted at a tight budget
  — this bug class has hit every board. `docs/LESSONS.md#nimble-host-stack-budget`,
  `#ble-event-worker-stack-budget`, `#the-1280-byte-rule-applies-to-every-tight-system-thread-not-just-bleeventworker`
- **Capability cache is permanent.** `capability_query` is sent once per `board_id` and the
  Flipper caches the answer forever; the only refresh path is a full unpair+re-pair or deleting
  the cached `.dat` file. After adding real capabilities to `feb_features[]`, flag explicitly
  that a stale cached response will keep being served until invalidated — see
  `docs/CAPABILITIES.md` and the `reference_flipper_sd_card_access` project memory for deleting
  a stale cache file via `scripts/storage.py`.

## Shared application core

- `components/feb_app_core/` and `components/feb_wardriving/` are compiled into all three ESP
  boards. A change there is not done until `esp32`, `esp32c5`, `heltec` and
  `esp32/cluster_worker` all build, and Heltec's `idf.py size` DRAM headroom is checked (it is
  ~100 B). Report the affected boards for hardware checks.
- Board differences go through the board's `board_config.h` flags or the `const
  feb_app_hooks_t` table ([SOURCE_SPLIT.md](SOURCE_SPLIT.md) §5.2), never `#ifdef
  CONFIG_IDF_TARGET_*` sprinkled in the core and never weak symbols. New hooks are appended;
  NimBLE callouts/events are created only in each board's `host_synced` (8-callout cap).
- The Flipper app modules compile as one translation unit (`APP_FN`/`APP_DATA`); see
  flipper-developer.md.

## Crypto standards

- Zeroize ephemeral key material (X25519 private keys, shared secrets, session keys) on every
  success and failure path.
- Compare tags/proofs in constant time; reject an all-zero X25519 shared secret.

## Embedded/C standards

- Check every `esp_err_t` (or equivalent return value); treat error propagation as real
  functionality, not boilerplate.
- Use FreeRTOS tasks/queues with explicit lifetimes and cleanup; never block on slow I/O in a
  task another subsystem depends on.
- Keep memory bounded: no unbounded buffers, no allocation sized from unvalidated
  peer-controlled lengths.
- Match integer widths and format specifiers; avoid one-letter variable names.
- Keep comments rare — only for non-obvious hardware/protocol constraints, matching each
  project file's existing style.

## Default working method

1. Anchor on the concrete task: file, symbol, failing build step, or the specific `PLAN.md`
   step being implemented.
2. Read the relevant protocol doc (`PROTOCOL.md`/`PAIRING.md`/`CAPABILITIES.md`) before writing
   protocol-adjacent code — don't invent a field shape or derivation that isn't specified there.
3. Make the smallest change consistent with the current roadmap step; don't pull forward a
   later phase's behavior or relitigate an earlier one.
4. Validate with a build at minimum after every substantive change, and report it as a build
   result only (see Reporting discipline above).
5. State board/pin/radio assumptions explicitly when they matter, and confirm — don't infer —
   any assumption about the peer firmware by reading its source.
6. Record new hardware facts, root causes, or verified measurements in the relevant `docs/*.md`
   file. If a root cause repeats a bug class already in `docs/LESSONS.md`, also propose an
   update to the owning agent file — the log records history, only the agent file changes
   future behavior.
7. Run `tools/check_shared_headers.py` before reporting done whenever shared codec symbols were
   added in parallel with the peer agent (see Protocol/codec convergence above).

Board-specific agent files state only where their working method deviates from this default.

## Response style

Be concise and explicit about assumptions — board/pin/radio-coexistence ones on the ESP32-family
boards, BLE-profile-lifecycle/persistence-boundary ones on the Flipper side. Ask a clarifying
question only when it blocks a safe or correct change; otherwise make the conservative,
spec-consistent choice, flag any borrowed/unvalidated numbers, and validate what you can
statically.
