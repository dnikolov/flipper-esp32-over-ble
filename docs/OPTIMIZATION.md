# Codebase & agent cost-efficiency backlog

Recommendations from a 2026-09-08 review of file sizes / read-cost across the codebase and
the two developer subagents. Items already done are marked so; the rest are left for a later
session. Line numbers/sizes below are as of that date and will drift — re-check before
acting on any of them.

## Done (2026-09-08)

- **Wardriving codec shape reconciled** between `esp32/main/cbor_codec.h`/`.c` and
  `flipper/cbor_codec.h`/`.c` — was a real structural divergence (tagged union vs. two
  always-present named fields; two presence flags vs. one), not just cosmetic. See
  `docs/LESSONS.md#wardriving-struct-shape-divergence`.
- **`tools/check_shared_headers.py`** added — diffs macro values and function prototypes
  between each esp32/flipper header pair. Run it whenever either developer agent touches a
  shared header. It does **not** catch struct-body shape divergence (see above) — still read
  the actual struct on both sides for any new composite/optional-field/union shape.
- **Both agent files restructured**: narrative/incident writeups moved to `docs/LESSONS.md`,
  agent files cut to imperative rules + links. `esp32-developer.md` ~4.7k→2.7k tokens,
  `flipper-developer.md` ~6.3k→4.0k tokens (kept fuller — three of its sections are live,
  ongoing traps, not resolved history). Each file's own near-duplicate sections (shared with
  the other agent file) were merged into one `docs/LESSONS.md` entry instead of two copies.
- **Read-discipline rule** added to both agent files: grep for the symbol first, then
  `Read` with `offset`/`limit`, for any file over ~800 lines.
- **Split `cbor_codec.c`/`.h` per capability** (2026-09-08, ESP32 side; Flipper side done in
  parallel by the flipper-developer agent using identical target filenames). Both
  `esp32/main/cbor_codec.c`/`flipper/cbor_codec.c` are gone, replaced by
  `cbor_primitives.c`/`.h`, `cbor_records.c`/`.h`, `cbor_wifi_scan.c`/`.h`,
  `cbor_ble_scan.c`/`.h`, `cbor_wardriving.c`/`.h` on each side, plus a small
  `cbor_internal.h` per side for implementation-only shared macros (`FEB_CBOR_I_KLEN` on
  the ESP32 side — not part of either firmware's shared-header API contract). Each
  `cbor_codec.h` is now a thin umbrella that defines only the genuinely cross-section
  macros/enum (`feb_cbor_status_t`, `FEB_CBOR_MAX_*`, `FEB_SESSION_ID_LEN`,
  `FEB_GCM_TAG_LEN`) and `#include`s the five split headers, so nothing that already
  included `cbor_codec.h` needed to change. Verified: the actual `encode_head`/`decode_head`
  static helpers the original plan assumed were cross-section turned out to be used only
  within the primitives section itself (confirmed by grep) — they stayed `static` inside
  `cbor_primitives.c` rather than being promoted; `FEB_CBOR_I_KLEN` (the string-literal-length
  macro, originally unprefixed `KLEN`) was the helper that actually needed promoting, since
  records/wifi_scan/ble_scan/wardriving all use it. `cbor_wardriving.h` has a real
  cross-file macro dependency on `cbor_wifi_scan.h`/`cbor_ble_scan.h` (reuses
  `FEB_WIFI_SCAN_BSSID_LEN`/`FEB_BLE_SCAN_ADDRESS_LEN`/`FEB_BLE_SCAN_NAME_MAX_LEN` directly),
  so it `#include`s both; every split header is written to be self-sufficient (each
  `#include`s `cbor_codec.h`, safe via the standard guarded-recursive-include pattern) rather
  than relying solely on the umbrella's own include order. `esp32/main/CMakeLists.txt` and
  three `tests/esp32/build*.ps1` scripts (`build.ps1`, `build_pairing.ps1`,
  `build_session.ps1` — all three link the codec, not just the one the original plan
  mentioned) now list the five new files instead of `cbor_codec.c`. All esp32 host-native
  suites (`test_framing_cbor.c`, `test_pairing.c`, `test_session.c`) and a full clean
  `idf.py build` pass with zero new warnings; `tools/check_shared_headers.py` reports `OK`
  for `cbor_codec.h` against the Flipper's equally-split umbrella.

  **Orchestrator follow-up after both sub-agents finished:** added the five new split-header
  pairs to `tools/check_shared_headers.py`'s `HEADER_PAIRS` (the whole point of the tool is to
  catch drift in exactly these files). First run reported five false "macro differs" mismatches
  — root-caused to a **pre-existing bug** in the script's own macro-value regex (`\s*` between
  a macro name and its value matches newlines, so a bare include-guard `#define X` immediately
  followed by a blank line swallows the *next* line's content as the guard's fake "value").
  Fixed (`\s*` → `[ \t]*` so the value capture can't cross a line boundary); re-run reports
  `OK` on all 11 header pairs, confirming the split headers really are byte-identical in API
  surface. Also found and fixed: `tests/flipper/build_pairing.ps1` and
  `tests/flipper/build_session.ps1` still hardcoded the deleted `cbor_codec.c` as a compile
  source (the flipper-developer agent's task only named `build.ps1`; these two were missed,
  mirroring the same gap the esp32-developer agent caught and fixed on its own side
  unprompted) — both now list the five new files, both re-verified passing (67/67, 57/57).

## Not yet done

### 1. Extract capability-dispatch layer from both main files

Pays off on every future capability (GPS, GPIO, sensors, wardriving's own dispatch once
wired), since that's exactly where the growth lands.

- **ESP32** (`esp32/main/main.c:862-1484`, 623 lines / ~7k tok): `handle_capability_query`,
  the `wifi_scan_*`/`ble_scan_*` handlers, `handle_command`, `start_wifi_subsystem`, the
  `*_str()` mappers. Needs a narrow interface back into `main.c`: `send_protected()`,
  `send_protected_error()`, the conn handle, relevant session fields.
- **Flipper** — three independent extractions, in order of increasing risk:
  1. `app_storage.c` (`flipper/flipper_esp32_over_ble.c:278-555`) — pure functions over a
     `Storage*` and cached paths. Near-zero risk.
  2. `ui.c` (`flipper/flipper_esp32_over_ble.c:1950-2155`) — draw/input callbacks. Low risk.
  3. `capabilities.c` (`flipper/flipper_esp32_over_ble.c:1124-1613`) — scan client + display
     state. Higher risk: touches shared profile/session statics.

⚠️ Before splitting the Flipper side, decide the static-buffer arena question first (see
`docs/LESSONS.md#static-buffer-pattern-trades-ram-for-stack-safety` — mutually-exclusive
per-capability buffers may eventually want to share one arena as they multiply). Spreading
those statics across translation units doesn't hurt stack safety, but makes that later
consolidation harder to see. Decide before splitting, not after.

Hold this item until a natural roadmap boundary — it touches shared static state on firmware
that's currently hardware-verified.

### 3. Don't split (confirmed deliberately out of scope)

- **`flipper/pairing_crypto.c`** — a `curve25519-donna` port deliberately kept diffable
  against upstream for auditability. Splitting destroys that property.
- **`tests/vectors/vectors.h`** (98KB, generated) — nothing should ever `Read` it whole;
  this is really a "never read in full" rule, not a split candidate.

### 4. Smaller items

- **Normalize the ESP32 build invocation — done 2026-09-08.** Added `tools/build_esp32.ps1`
  (clears `MSYSTEM`, sources `export.ps1`, runs `idf.py build`, tails output; `-TailLines`
  param, default 150). `.claude/settings.json`'s three near-duplicate `idf.py build` allow
  entries collapsed into one entry for this script.
- **`docs/CODE_REVIEW_FINDINGS.md`/`docs/CODE_REVIEW_FIX_PLAN.md` — checked, keeping both
  (2026-09-08).** Not folding into `docs/PROJECT_HISTORY.md`: `docs/PLAN.md`'s Backlog section
  actively cross-references specific finding numbers by anchor (`finding #8`, `finding #11`,
  `finding #17`), so FINDINGS.md is still the live detail record behind those pointers, not
  dead history. Most items are marked FIXED inline; the still-open ones (#5-#8, #11-#24, the
  style/documentation section) are the same items already tracked in `docs/SESSION_MEMORY.md`'s
  "Known open items" and `docs/PLAN.md`'s Backlog — expected duplication between a point-in-time
  review record and the living backlog, not accidental drift.
