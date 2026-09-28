# Build Baselines

This file records the pinned inputs for phase 1. The baseline images must be built before project behavior is added.

## ESP32-C6

- Board: ESP32-C6-DevKitC-1-N4
- ESP-IDF: v5.5.2
- Target: `esp32c6`
- Flash size: 4 MB, detected read-only with `esptool flash_id` on COM9
- Project: `esp32/`
- ESP-IDF v5.5.2 includes NimBLE central-mode support, mbedTLS X25519/HKDF-SHA-256/HMAC-SHA-256/AES-256-GCM, and encrypted-NVS support — standard components of this and prior ESP-IDF releases, confirmed rather than assumed from the fact that the version happened to already be installed. (Runtime sessions use AES-256-GCM, not AES-128-GCM as originally specified — see `docs/PLAN.md` step 6.)

## Heltec WiFi LoRa 32 V2 (Phase 4, in progress since 2026-09-16)

Second ESP32 target board for display and LoRa capabilities (see `docs/PLAN.md` step 7 capability roadmap). This is a different chip family from the pinned C6 baseline above, not a peripheral addition to it:

- Classic ESP32 (dual-core Xtensa), not the C6's RISC-V.
- SX1276 LoRa radio and SSD1306 OLED display onboard.
- No native USB — requires a USB-UART bridge chip to flash, unlike the C6's native USB-Serial/JTAG.

**Board bring-up confirmed read-only 2026-09-16** (Phase 4 step 1):

- Board revision: silkscreen reads **"WiFi LoRa 32 V2"** (not V2.1).
- Chip: ESP32-D0WDQ6, revision v1.0.
- Flash: **8 MB** (Winbond), detected read-only with `esptool flash_id` — matches the V2/V2.1 8MB expectation in the hardware doc below, rules out the older V1's 4MB.
- MAC: `a4:cf:12:03:ba:58`.
- USB-UART bridge: Silicon Labs CP210x, enumerated as COM10 (not stable across sessions/reboots — reconfirm before hardware work, same caveat as the C6's COM9/Flipper's COM8).
- Toolchain: the classic `esp32` (Xtensa) toolchain was not installed prior to this (this project had only ever installed `esp32c6`'s RISC-V toolchain); installed via `idf_tools.py install --targets=esp32`. `idf-env.json` now lists both `esp32` and `esp32c6` as selected targets.
- An unmodified `hello_world` example built and flashed cleanly for the `esp32` target, confirmed booting over serial.

Do not carry forward C6 pin mappings to this board — see `docs/hardware/esp32-c6-devkitc-1/README.md`, which already documents this distinction.

**Hardware verification pass 2026-09-27 (second physical unit, MAC `a4:cf:12:03:b1:74`, board_id
`heltec-a4cf1203b174` — see `docs/hardware/heltec-wifi-lora-32-v2/README.md`'s "Second physical
unit" section, not the Phase 4 unit above):** read-only `esptool flash_id` on COM10 reconfirmed
ESP32-D0WDQ6 rev v1.0, 8 MB Winbond flash. A clean `idf.py build` from a fresh `build/` directory
(current tree: `mesh_log`/`meshtastic_scan`/`lora_shared_radio` all present) succeeded — project
binary 47% free of the 2 MB factory partition, bootloader 8% free. `idf.py size` reports DRAM
124500/124580 bytes used, **80 bytes free** (previously reported as 72 bytes by the implementing
session; a small, unexplained drift, not investigated further — treat both as "essentially zero,
razor-thin" rather than reconciling the exact delta). The build does emit ~67 "`RADIOLIB_EXCLUDE_
STM32WLX` redefined" preprocessor warnings from the pinned `jgromes/radiolib` managed component
(scoped to that component's own compile units via `heltec/CMakeLists.txt`'s `target_compile_
definitions`) — pre-existing since `meshcore_scan` was added 2026-09-26, not introduced by this
session, and harmless (RadioLib's own `BuildOpt.h` apparently already default-defines this symbol
for non-STM32 targets), but a real deviation from a literal "0 warnings" build worth noting.

Flashed and boot-logged (COM10, ~90s captured via the `ESP_IDF_MONITOR_TEST=1` workaround, with
`PYTHONIOENCODING=utf-8`/`PYTHONUTF8=1` also needed — without them `idf_monitor` crashed with a
`UnicodeEncodeError` decoding the classic ESP32's 74880-baud ROM boot preamble against this
machine's cp1251 console codepage, a quirk specific to classic-ESP32's ROM UART bootstrap, not
seen on the C6/C5's native-USB boot path). Boot was clean for the full ~90s capture: no crash,
no reset loop, no Guru Meditation/backtrace. `mesh_log_init()` logged "mesh log resumed: active
sector 0 (gen 1), 0 pending record(s)" (not a fresh-init message, meaning this partition already
had valid state from an earlier flash of this same unit — not a partition-not-found/sector-count
error either way). `lora_shared_radio_init()` logged a successful SX127x identify (SX1276/SX1279-
class silicon) and `begin()`/`startReceive()` succeeding at 869.525 MHz/250 kHz BW/SF11/CR4/5;
the mode-switch multiplexer fired exactly once at ~60.7s uptime ("switched listen mode ->
meshtastic"), confirming the 60-second dwell timer runs correctly. NimBLE started its central
scan and logged ~200-240 scan reports per 10-second window throughout, with no reconnect churn.
No MeshCore/Meshtastic sighting was logged (expected — no real node in range).

**The real number this session existed to get:** `esp_get_free_heap_size()` logged **121808
bytes** free. Important caveat found while reading `main.c`: this log line runs *before*
`start_wifi_subsystem()`/`nimble_port_init()` (Wi-Fi/BLE stack init happens afterward in
`app_main()`), not after, despite the code comment beside it claiming "Wi-Fi+BLE+LoRa+GPS all
already initialized" — so 121808 bytes is an upper bound on steady-state free heap, not the true
number with the Wi-Fi+BLE stacks (which reserve a nontrivial chunk of heap themselves) also
running. Still directly useful for BL24's open question (a heap-allocated `mesh_log` dedup table
is clearly not constrained by DRAM the way `.dram0.bss` is — even a few hundred bytes would be
trivial against 121 KB), but a true worst-case reading (a second `esp_get_free_heap_size()` log
placed after both stacks are up) is still open for a future session. See `docs/BACKLOG.md` BL24.

**Same day, second pass: `mesh_log` dedup switched from flash-scan to a heap-allocated table**
against that measurement (128 entries, ~2.2 KB, `malloc()`'d once in `mesh_log_init()` —
`heltec/main/mesh_log.c`'s top comment has the full rationale). `idf.py build` clean; `idf.py
size` DRAM headroom moved from 80 to **72 bytes** (the predicted ~8-byte cost of the new
pointer/count/flag statics — the bulk table itself lives on the heap, not `.bss`). Host-native
`tests/esp32/build.ps1` suite reconfirmed passing (no shared-codec files touched). Re-flashed to
the same physical unit (COM10) and re-captured ~51s of boot log: clean boot, no crash/reset
loop, `mesh_log_init()`/`lora_shared_radio_init()`/wardriving_log/NimBLE scan all still succeed.
`esp_get_free_heap_size()` at the same log point now reads **119500 bytes** (down ~2.3 KB from
121808, consistent with the new table's ~2.2 KB allocation plus small heap-allocator overhead —
confirms the `malloc()` succeeded and is costing roughly what was predicted). No "dedup table
allocation failed" or "dedup table full" warning appeared (expected — no real MeshCore/
Meshtastic traffic was in range to populate the table). See `docs/BACKLOG.md` BL24 for the full
eviction-policy/reboot-caveat writeup.

Full hardware reference (pinout, board-revision ambiguity, per-vendor-documentation caveats): [docs/hardware/heltec-wifi-lora-32-v2/README.md](hardware/heltec-wifi-lora-32-v2/README.md). Phase 4 design and step-by-step status (architecture decision confirmed, gate-override decision, step tracking): `docs/PLAN.md`'s "Phase 4: Heltec WiFi LoRa 32 V2 board support" section.

## OLIMEX MOD-ESP32-C5 (started 2026-09-25)

Third ESP32 target board, `esp32c5/`. A different chip family from both prior boards: RISC-V
like the C6, but with a **dual-band Wi-Fi 6 (2.4 GHz + 5 GHz)** radio. Initial pass (Phase 8 Step
2) scoped to 2.4 GHz only; lifted the same day (Step 3) — this board now scans both bands and
has `wardriving` ported (see `docs/PLAN.md`'s Phase 8 section for the scope-reversal narrative).

- Board: OLIMEX MOD-ESP32-C5 Rev. A, module ESP32-C5-WROOM-1-N8R4.
- Chip: ESP32-C5, revision v1.0, single core + LP core, 240 MHz, BLE 5 (LE), IEEE 802.15.4.
- Flash: **8 MB**, detected read-only via `esptool flash_id` on COM11.
- MAC: `d0:cf:13:ff:fe:e0:88:40`.
- USB: native USB-Serial/JTAG (no bridge chip), same reset/flash handling as the C6.
- ESP-IDF `v5.5.2` already includes `esp32c5` SoC support; the `riscv32-esp-elf` toolchain is
  shared with the C6 target (`idf_tools.py install --targets=esp32c5` found nothing new to
  install beyond registering the target).
- GPS: ATGM336H wired to GPIO4 (ESP32 RX ← GPS TX) / GPIO5 (ESP32 TX → GPS RX) — the board's
  UEXT-connector UART pins, not GPIO2/GPIO3 (those are wired to UEXT I2C on this board and are
  also JTAG strapping pins).
- **No onboard pushbutton** — the factory-reset gesture (BOOT-hold 5s) used on the C6/Heltec
  has no hardware input here; deferred, see `docs/BACKLOG.md` BL15.
- Full hardware reference (pin map, schematic, strapping-pin notes):
  [docs/hardware/olimex-mod-esp32-c5/README.md](hardware/olimex-mod-esp32-c5/README.md).

## Flipper

- Distribution: Unleashed stable
- Release: `unlshd-092`
- Commit: `3c9be0fdd9d301a9436765099a2d1780b36a1795`
- Reported API: 88.4
- Repository: https://github.com/DarkFlippers/unleashed-firmware
- Project: `flipper/`

The FAP targets only this pinned API. Compatibility with later Unleashed API revisions is a future phase 2 enhancement.

## Baseline status

- [x] Install ESP-IDF v5.5.2 on Windows and repair the target RISC-V toolchain
- [x] Measure physical C6 flash size and confirm the partition table
- [x] Build the unmodified ESP32 baseline
- [x] Fetch the pinned Unleashed checkout
- [x] Build the standalone FAP baseline

The FAP was built with the pinned Unleashed Windows `fbt.cmd` wrapper using a temporary copy under `applications_user/flipper_esp32_over_ble`, because this FBT revision resolves `APPSRC` only from known app directories. The project-specific artifact is `build/f7-firmware/.extapps/flipper_esp32_over_ble.fap` in that checkout.

**FAP build type is pinned to release (`DEBUG=0`, `-Os`) as of 2026-09-28** — until then this project built and flashed FBT's default debug (`DEBUG=1`, `-Og`) artifact from `build/f7-firmware-D/`. This is a pinned baseline, not a convenience: a FAP's compiled sections are heap-resident for the app's whole lifetime, so build type is part of this app's runtime memory budget (see `CLAUDE.md`'s Build commands section and `docs/HARDENING_BACKLOG.md` H04 for the mechanism). Measured section sizes, same source tree, both configurations:

| Section | `-Og` (old default) | `-Os` (pinned) |
| --- | --- | --- |
| `.text` | 65,768 | 54,680 |
| `.rodata` | 12,632 | 11,944 |
| `.data` | 56 | 56 |
| `.bss` | 27,992 | 27,969 |
| **total heap held** | **106,448** | **94,649** |

`.fap` file size: 138,932 -> 115,756 bytes. All three Flipper host test suites (565 checks total) and `tools/check_shared_headers.py` pass against this build.

After the 2026-09-28 hardening pass ([HARDENING_PLAN.md](HARDENING_PLAN.md)) the FAP is `.text` 56,064 / `.rodata` 12,168 / `.data` 56 / `.bss` 27,985. That's +1,384 `.text` for `protocol_mutex`, batched scan events and the strict decoders. The Flipper host suites total 491 + 69 + 64 checks.

**ESP-IDF build config pinned for every ESP target as of 2026-09-28** (HARDENING_PLAN.md HP-12/HP-14/HP-27). `sdkconfig.defaults` in `esp32/`, `esp32/cluster_worker/`, `esp32c5/` and `heltec/` now set `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`, was `-Og`), `CONFIG_COMPILER_STACK_CHECK_MODE_NORM=y`, `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` and `CONFIG_ESP_TASK_WDT_PANIC=y`. Each tracked `sdkconfig` was regenerated, and the old/new diff was checked to contain only these options and their derived symbols. One known side effect: `CONFIG_FREERTOS_TASK_FUNCTION_WRAPPER` drops out, because its Kconfig depends on `COMPILER_OPTIMIZATION_DEBUG`, so a task function that `return`s instead of self-deleting no longer gets the wrapper's logged abort. The Heltec regeneration also finally applied `CONFIG_TOUCH_SUPPRESS_DEPRECATE_WARN`, which was in its defaults but missing from the stale sdkconfig. Measured `idf.py size`:

| Target | Before (`-Og`, no canary) | After |
| --- | --- | --- |
| C6 `esp32/` total image | 1,294,566 B | 1,204,940 B (DIRAM −9,032 B) |
| C6 `esp32/cluster_worker/` total image | 865,212 B | 796,792 B (DIRAM −8,540 B) |
| C5 `esp32c5/` total image | 1,329,234 B | 1,239,486 B (HP SRAM +10,026 B free; app partition 41% free) |
| Heltec DRAM / IRAM headroom | tree didn't link (−16 B DRAM) / — | **364 B / 7,309 B** (supersedes BACKLOG.md BL27's 8 B / 45 B) |
| Heltec DRAM headroom, after flush-window gate (2026-09-28, see PROJECT_HISTORY.md) | 364 B | **348 B / 7,309 B** (−16 B net; the mesh-dedup hash shrink in the same pass is heap-only, 0 B DRAM change) |

The ESP32 baseline build completed successfully with ESP-IDF v5.5.2. Verified artifacts are `esp32/build/flipper_esp32_over_ble.elf` (3,590,924 bytes), `esp32/build/flipper_esp32_over_ble.bin` (161,888 bytes), `esp32/build/flipper_esp32_over_ble.map` (2,828,886 bytes), `esp32/build/flasher_args.json` (959 bytes), and `esp32/build/project_description.json` (195,509 bytes).

The ESP32-C6 flash-ID query was read-only. COM4 and COM3 were busy; COM9 responded as an ESP32-C6 in USB-Serial/JTAG mode. `esptool flash_id` detected a 4 MB flash, confirming the 4 MB project configuration and current partition table.

## Step 2 transport verification

The fixed-payload BLE transport was verified on the physical ESP32-C6 and Flipper Zero on 2026-09-02.

- Flipper profile advertised successfully with the v2 service UUID; the earlier GAP error 146 was eliminated by using UUID-only advertising data.
- ESP32 discovered the peer advertisement and connected as the central.
- ATT MTU negotiation completed at 256 bytes.
- Service and both 128-bit characteristics were discovered.
- The notification CCCD was discovered and enabled.
- ESP32 wrote `ESP32-C6 transport smoke test`.
- Flipper logged receipt of the exact payload and returned `FLIPPER-ACK`.
- ESP32 received and verified the `FLIPPER-ACK` prefix in the fixed-size notification.
- After disconnect reason 08, the ESP32 reconnected and the Flipper received the payload again.

ESP32 build and flash passed with hash verification. Known remaining work at the time (step 1/2):
ESP32 logs still reported that NVS was not initialized before Bluetooth startup, although RF
calibration fell back successfully; pairing, persistence, encryption, CBOR framing, and
capabilities were still future roadmap work. All of that has since shipped — see
[docs/SESSION_MEMORY.md](SESSION_MEMORY.md) for current status.