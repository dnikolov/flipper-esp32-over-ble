# OLIMEX MOD-ESP32-C5 (Rev. A) Reference Bundle

Bare module board carrying an **ESP32-C5-WROOM-1-N8R4** (8 MB flash), plus an externally
wired **ATGM336H** GPS module. Board added to this project 2026-09-25.

## Local vendor materials

- `vendor/MOD-ESP32-C5_Rev_A_schematic.pdf`: full board schematic (KiCad export, single sheet).

## Official online sources

- Board repository: https://github.com/OLIMEX/MOD-ESP32-C5
- Schematic (same file as above): https://github.com/OLIMEX/MOD-ESP32-C5/blob/main/HARDWARE/MOD-ESP32-C5.Rev.A/MOD-ESP32-C5_Rev_A.pdf

## Verified board facts (read-only, `esptool` on COM11, 2026-09-25)

- Chip: **ESP32-C5, revision v1.0** — Espressif's first RISC-V chip with a native **dual-band
  Wi-Fi 6 (2.4 GHz + 5 GHz)** radio, BLE 5 (LE), and IEEE 802.15.4. Single core + LP core, 240 MHz,
  48 MHz crystal.
- Flash: **8 MB**, matching the module's `N8` marking.
- MAC: `d0:cf:13:ff:fe:e0:88:40`.
- USB: native USB-Serial/JTAG only (no separate UART-bridge chip) — same reset/flash handling
  as the ESP32-C6 DevKitC, not the Heltec's CP210x bridge path.
- Known port from this session: **COM11** — reconfirm every session, not stable across
  reboots/replugs.

## This project's scope decision: 2.4 GHz only initially, lifted the same day

The ESP32-C5's headline feature over the C6 is native 5 GHz Wi-Fi. This project's first pass
at this board **deliberately restricted Wi-Fi scanning to the 2.4 GHz band only** (explicit user
decision, 2026-09-25) for the initial transport/pairing/capability port. **That restriction was
lifted later the same day** — explicit user request to add 5 GHz scanning and port
`wardriving` once the 2.4GHz-only path was hardware-verified. See `docs/PLAN.md`'s Phase 8 step
3 for the current state.

**Mechanism (confirmed 2026-09-25 against the installed ESP-IDF v5.5.2 headers, not guessed):**
this chip's Wi-Fi band mode defaults to `WIFI_BAND_MODE_AUTO` (2.4G+5G) because
`CONFIG_SOC_WIFI_SUPPORT_5G=y` in this board's generated sdkconfig — `esp32c5/main/main.c`'s
`start_wifi_subsystem()` originally called `esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY)`
right after `esp_wifi_start()` succeeded; now switched to `WIFI_BAND_MODE_AUTO` (both bands) per
the scope reversal above.

**No radio-coexistence sweep run for this chip** (tracked as `docs/BACKLOG.md` BL16) — the
`wifi_scan`/`ble_scan`/`wardriving` capabilities were all ported using the C6's step-4 interval
bounds as an unvalidated borrow, matching the same stopgap the Heltec port used for its own
different combo radio. `wardriving` is this board's first exposure to *sustained* concurrent
Wi-Fi+BLE load (a manual `wifi_scan`/`ble_scan` is a bounded, short action; wardriving runs
continuously) — do not treat those bounds as verified for this chip's radio, and watch for
instability under real wardriving duty cycles specifically, not just short manual scans.

**2026-09-26: dual-band scanning made configurable via `wifi_band` (docs/PROTOCOL.md).**
Hardware-verified dual-band wardriving (above) turned out slow for a manual `wifi_scan` —
`WIFI_BAND_MODE_AUTO` sweeps every 5 GHz channel including the DFS range, which requires slow
passive listening for radar-detection compliance. The user's explicit choice was "make it
configurable" rather than defaulting to skip DFS or leaving it slow. `wardriving`'s `start`
action now carries a required-when-`"wifi"`-in-`sources` `wifi_band` field
(`"2.4ghz"` / `"5ghz_fast"` / `"5ghz_full"`), applied once at wardriving start (same "global
radio setting, read back by a later manual `wifi_scan`" pattern `country` already uses):

- `"2.4ghz"` → `esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY)`, this board's original
  pre-dual-band default.
- `"5ghz_full"` → `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)`, yesterday's shipped
  default (full sweep, every channel, DFS included).
- `"5ghz_fast"` → `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)` **plus** an explicit
  `wifi_scan_config_t.channel_bitmap` (channels 1-14 on 2.4 GHz; 36/40/44/48/149/153/157/
  161/165 — the non-DFS UNII-1-low/UNII-3 5 GHz channels — on 5 GHz), restricting the scan
  to that subset while still issuing exactly one `esp_wifi_scan_start()` call.

**`5ghz_fast` mechanism, confirmed against the installed ESP-IDF v5.5.2 headers (not
assumed):** `wifi_scan_config_t` (`esp_wifi_types_generic.h`) has a
`wifi_scan_channel_bitmap_t channel_bitmap` field (`ghz_2_channels` a `uint16_t`,
`ghz_5_channels` a `uint32_t`, bit-indexed per `wifi_2g_channel_bit_t`/`wifi_5g_channel_bit_t`)
that restricts a single scan to an explicit channel subset — `channel` must stay `0` for the
bitmap to take effect (a nonzero `channel` instead requests one single specific channel, the
mutually-exclusive alternative). This is a real, documented, non-guessed mechanism: it's
exercised by `esp-idf/examples/wifi/scan/main/scan.c`'s `array_2_channel_bitmap()` and by
`wpa_supplicant`'s own `esp_scan.c` (`get_scan_channel_bitmap()`), and a zeroed bitmap (this
board's existing `2.4ghz`/`5ghz_full` scan_cfg, always freshly `memset(0)`) is confirmed to
mean "no per-call restriction, full sweep of whatever band(s) `band_mode` allows" rather than
"scan nothing" — so `2.4ghz`/`5ghz_full` needed no `channel_bitmap` change at all, only
`5ghz_fast` does. **No multi-scan/sequential-per-channel restructuring was needed** — this
stays a single `esp_wifi_scan_start()` call yielding one `WIFI_EVENT_SCAN_DONE`, so
`wifi_scan_done_handler()`/wardriving's scan-cycle timing are unchanged. See
`esp32c5/main/main.c`'s `wardriving_apply_wifi_band()`/`wardriving_set_wifi_band_mode()`.

The C6/Heltec's shared `cbor_wardriving.c` decoder now decodes `wifi_band` like `country`
(optional at the codec level; required-with-`wifi` enforcement is a caller/`main.c` concern),
so those boards accept all three values on the wire without error and simply never read the
decoded value — no C6/Heltec `main.c` change was needed, since neither board's command handler
references it.

Boot default (before any `wardriving start` ever runs this boot) stays `WIFI_BAND_MODE_AUTO`/
`5ghz_full`, matching yesterday's already-verified behavior — `wardriving_wifi_band`'s
in-memory default is `WARDRIVING_BAND_5GHZ_FULL` for this reason. Wardriving's own boot
autostart (from persisted enabled/wifi/ble state) is the one exception: it hardcodes
`WARDRIVING_BAND_2_4GHZ`, the conservative choice, the same way it already hardcodes
`WARDRIVING_SWELLING_NORMAL`/`WARDRIVING_COUNTRY_ROW` rather than resuming whatever a prior
live `start` last chose — an unattended boot is not the place to default into the
never-swept dual-band+BLE coexistence combo flagged above.

## Pin map (from the schematic; module pin numbers in parens)

| Signal | Pin | Notes |
| --- | --- | --- |
| `EN` | pin 3 | Reset/enable, pulled up via R3 10k. |
| `GPIO2` (pin 4) | `MTMS` / `LP_I2C_SDA` | Wired to UEXT connector pin 6 (I2C SDA). Also a JTAG strap (Table 3-1: default floating) — do not force its reset-time level. **Not used for GPS on this board's wiring** (see below). |
| `GPIO3` (pin 5) | `MTDI` / `LP_I2C_SCL` | Wired to UEXT connector pin 5 (I2C SCL). Same JTAG-strap caution as GPIO2. Free/unused by this project. |
| `GPIO4` (pin 17) | `LP_UART_RXD` | Wired to UEXT connector pin 4 (UART RXD). **User-wired to the ATGM336H GPS module's TX line** — this is the ESP32's UART RX pin. |
| `GPIO5` (pin 16) | `LP_UART_TXD` | Wired to UEXT connector pin 3 (UART TXD). **User-wired to the ATGM336H GPS module's RX line** — this is the ESP32's UART TX pin (GPS config commands only; NMEA output doesn't need it). |
| `GPIO11`/`GPIO12` | `U0TXD`/`U0RXD` | Also routed to the ESP-PROG programming header — this is the console/log UART, not free for other use while ESP-PROG is attached. |
| `GPIO13`/`GPIO14` | `USB_D-`/`USB_D+` | Native USB-Serial/JTAG (as used for the COM11 connection above). |
| `GPIO26` | `USER_LED2` (red) | Driven via 2.2k resistor. **Also a boot-mode strapping pin** (Table 3-3): default floating, sampled at reset alongside GPIO27/GPIO28 to select SPI Boot vs. Joint Download Boot. Safe to drive as an output post-boot; do not add external circuitry that could hold it low/high across a reset. |
| `GPIO27` | `USER_LED1` (green) | Driven via 2.2k resistor. Also a boot-mode strapping pin (default pull-up, bit value 1) — same caution as GPIO26. |
| `GPIO28` | `BOOT` | Boot-mode strap (default pull-up, bit value 1 = SPI Boot). Routed to the ESP-PROG header for entering download mode; **no physical pushbutton is broken out on this board** for user-triggered boot/reset gestures. |

**No onboard pushbutton exists on this board** (confirmed from the schematic — only LEDs, UEXT,
USB-C, LDO, and the ESP-PROG header are present). This project's factory-reset gesture
(BOOT-hold 5s, used on both the C6 DevKitC and the Heltec) has **no hardware input to bind to
here**. Explicit user decision 2026-09-25: **defer factory-reset support for this board** rather
than guess at a substitute mechanism — tracked in `docs/BACKLOG.md`.

## Status LED mapping (two plain LEDs, chosen 2026-09-25)

Unlike the C6's single addressable WS2812 (color-encoded state) or the Heltec's single plain
LED (blink-cadence-encoded state), this board has two independent plain GPIOs to spend, so the
two states this project already tracks get one LED each instead of being folded onto one:

- **Green (USER_LED1, GPIO27):** connection/session state — off/slow-blink while scanning or
  connected-but-not-yet-authenticated, solid on once the runtime session is authenticated.
  Directly mirrors the C6/Heltec's primary connection indicator.
- **Red (USER_LED2, GPIO26):** backlog-flushing activity — fast blink while a capability's
  backlog batch is actively draining to the Flipper, off otherwise. Wired up 2026-09-25 (Phase 8
  Step 3) once `wardriving` was ported — `wardriving_maybe_kick_send()`/
  `wardriving_send_next_batch()` (`esp32c5/main/main.c`) drive it exactly like the C6/Heltec
  builds.

Unlike the C6/Heltec, this board's status LED has no third visual state for "a wardriving
capture is currently active" (both those boards render that as a color/blink-rate change on
their single LED) — `feb_status_led_set_wardriving_active()` exists for call-site parity with
the shared `wardriving_sync_status_led()` call pattern but is a deliberate no-op here (records
the flag, applies nothing). Tracked as `docs/BACKLOG.md` BL17 for the user to confirm or
override, mirroring the Heltec's own BL14 judgment call for its single plain LED.

See `esp32c5/main/status_led.c`/`.h` for the implementation.

## GPS module: ATGM336H

Wired by the user directly to `GPIO4` (ESP32 RX ← GPS TX) / `GPIO5` (ESP32 TX → GPS RX), i.e.
the same physical pins the schematic labels as the UEXT connector's dedicated UART (not the
UEXT I2C pins on GPIO2/GPIO3). ATGM336H is a UART NMEA-0183 GPS/GNSS module (typically 9600
baud default) — same driver shape as the C6/Heltec's existing `location.c`/`nmea_parser.c`,
retargeted to these two pins.
