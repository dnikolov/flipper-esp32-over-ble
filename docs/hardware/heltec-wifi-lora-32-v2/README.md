# Heltec WiFi LoRa 32 V2 Reference Bundle

**No physical board has been acquired for this project yet.** Every fact below is taken from
Heltec's and Espressif's own published documentation/source, not from a locally-measured
unit. Do not wire anything to a real board using this doc without first re-confirming the
exact revision in hand (see "Board revision ambiguity" below) — the same discipline this
project already applies to the C6 (see
[docs/hardware/esp32-c6-devkitc-1/README.md](../esp32-c6-devkitc-1/README.md), where the
vendor guide's 8 MB flash default turned out wrong for the physical N4 unit's 4 MB).

## Board revision ambiguity — resolve before wiring anything

"Heltec WiFi LoRa 32" is a product line with several electrically-different revisions. This
doc is about **V2** specifically (Heltec's own part/line designation, first released
2018-09-15 per their hardware update log). Do not use it for:

- **V1** (2017-06-01): 4 MB flash (not 8 MB), 26 MHz crystal (not 40 MHz), no Vext power-gate
  pin.
- **V2.1** (2019-06-15): per Heltec's hardware update log, changes only the RF-switch
  component (PE4259 → UPG2179) and moves the battery-voltage-detect ADC pin from GPIO13 to
  GPIO37 — the GPIO/SPI/I2C pin map below is otherwise unchanged from V2, but this must be
  confirmed against the physical unit, not assumed from this note.
- **V3 / V3.1 / V3.2 / V4**: a different SoC entirely (ESP32-S3, not classic ESP32), a
  different LoRa chip (SX1262, not SX1276/SX1278), USB-C with no CP2102 bridge (V3+), and a
  materially different GPIO layout. None of this doc's pin numbers apply.

Heltec's own product page lists V2 as "gradually being phased out" in favor of V3, so a
newly-purchased unit today may not actually be a V2 — check the board's silkscreen/label
against Heltec's revision photos before trusting anything here.

## Official online sources

- Heltec product/phaseout page (V2): https://heltec.org/project/wifi-lora-32v2/
- Heltec hardware update log (V1 → V4 revision history):
  https://docs.heltec.org/en/node/esp32/wifi_lora_32/hardware_update_log.html
- Heltec docs index for this board line: https://docs.heltec.org/en/node/esp32/wifi_lora_32/index.html
- Older WiFi LoRa 32 manual PDF: https://resource.heltec.cn/download/Manual%20Old/WiFi%20Lora32Manual.pdf
- Authoritative pin definitions (Espressif's Arduino core board-variant file — used as the
  primary source for the pin table below):
  https://github.com/espressif/arduino-esp32/blob/master/variants/heltec_wifi_lora_32_V2/pins_arduino.h
- Heltec's own Arduino library/examples repo: https://github.com/HelTecAutomation/Heltec_ESP32
- Community forum, V1 vs V2 differences: http://community.heltec.cn/t/difference-between-wifi-lora-32-v1-and-v2/259
- Community forum, GPIO21/Vext double-duty: http://community.heltec.cn/t/gpio21-and-vext-wifi-lora32-v2/7065
- Espressif ESP32 Series datasheet (strapping pins): https://www.mouser.com/datasheet/2/813/esp32_datasheet_en_1223853-1919342.pdf
- esptool boot-mode-selection reference (classic ESP32 strapping behavior):
  https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html

No KiCad/schematic source or locally-cached vendor PDF bundle exists for this board yet
(unlike the C6's `vendor/` bundle) — fetch and cache the schematic once a physical unit is
acquired, so future sessions aren't re-deriving from web search each time.

## Facts per vendor documentation — not yet locally verified

Everything in this section is **per Heltec's/Espressif's published documentation**, cross-checked
across at least two independent sources where noted. None of it has been confirmed against a
physical board (no unit owned yet).

- **SoC:** classic ESP32 (dual-core Xtensa LX6 @ 240 MHz, 520 KB SRAM) — RISC-V-free, unlike
  the C6. Wi-Fi 4 (802.11 b/g/n, 2.4 GHz only) + Bluetooth Classic + BLE 4.2, sharing one
  combo radio — a different radio combination from the C6's Wi-Fi 6 + BLE 5 + 802.15.4, so
  none of the C6's step-4 coexistence bounds carry over (see `docs/PLAN.md`'s Phase 4 design
  section).
- **Flash:** Heltec's hardware update log states V2 upgraded from V1's 4 MB to **8 MB** SPI
  flash. **Not yet physically confirmed** — per this project's own C6 precedent (vendor
  default said 8 MB, the actual N4 unit was 4 MB), do not size a partition table against this
  number until `esptool flash_id` is run read-only against the real unit.
- **LoRa radio:** SX1276 (863–928 MHz region SKU) or SX1278 (433 MHz SKU), depending on which
  regional variant was purchased — Heltec sells both under the "V2" name. Confirm which chip
  and frequency band is physically populated before any LoRa capability design, since
  regulatory TX constraints differ by band.
- **OLED:** SSD1306 driver, 0.96", 128×64, I2C, address `0x3C` — confirmed via the
  arduino-esp32 pin variant file, RIOT-OS's board documentation, and community references,
  independently agreeing on the address.
- **USB-UART bridge:** CP2102 (Silicon Labs), not a native-USB device. This is a real
  operational difference from the C6: the C6 exposes its own native USB-Serial/JTAG (no
  bridge chip, no separate VCP driver beyond `usbser.sys`); this board will enumerate purely
  as a CP2102 serial port and needs Silicon Labs' CP210x VCP driver on a machine that doesn't
  already have one, and flashing goes through the classic esptool DTR/RTS auto-reset sequence
  rather than the C6's native-USB reset handling.
- **Antenna:** onboard 2.4 GHz metal-spring antenna for Wi-Fi/BT; separate U.FL/IPEX connector
  for the LoRa radio — an external LoRa antenna must be attached before any LoRa TX activity.
- **Battery:** JST-SH 1.25 mm 2-pin LiPo connector with onboard charge/discharge management;
  not present/relevant on the C6 DevKitC-1.

## SX1276/SX1278 LoRa SPI pin mapping (per vendor documentation)

Source: arduino-esp32's `heltec_wifi_lora_32_V2/pins_arduino.h`, cross-checked against
espboards.dev's and RIOT-OS's independent pin references (all three agree).

| Signal | GPIO |
| --- | --- |
| SCK | GPIO5 |
| MISO | GPIO19 |
| MOSI | GPIO27 |
| NSS / CS (`SS`) | GPIO18 |
| RST | GPIO14 |
| DIO0 | GPIO26 |
| DIO1 | GPIO35 |
| DIO2 | GPIO34 |

## SSD1306 OLED I2C pin mapping (per vendor documentation)

The OLED is wired to its **own dedicated I2C pins**, not the ESP32 Arduino core's default
`Wire` pins (GPIO21/22, also broken out on this board for general use) — a documented,
easy-to-confuse-in-practice point in the Heltec ecosystem.

| Signal | GPIO |
| --- | --- |
| SDA (OLED) | GPIO4 |
| SCL (OLED) | GPIO15 |
| RST (OLED) | GPIO16 |
| I2C address | `0x3C` |

General-purpose I2C (Arduino `Wire` default, a separate bus from the OLED above):

| Signal | GPIO |
| --- | --- |
| SDA | GPIO21 |
| SCL | GPIO22 |

**GPIO21 double-duty:** per Heltec's own community forum, GPIO21 is also the **Vext**
power-gate control for external peripherals (`LOW` = Vext on, `HIGH` = Vext off) on V2 and
later — introduced in V2, not present on V1. Any design using the general-purpose I2C bus on
this board must account for GPIO21 also carrying Vext semantics; this is a real, vendor-confirmed
gotcha, not a guess.

## Other pins worth recording

| Signal | GPIO | Note |
| --- | --- | --- |
| UART0 TX | GPIO1 | via CP2102 bridge |
| UART0 RX | GPIO3 | via CP2102 bridge |
| Onboard LED (`LED_BUILTIN`) | GPIO25 | plain LED, also DAC1 — **not** an addressable
WS2812 like the C6's GPIO8 RGB LED; do not carry that driving code over. |
| Boot/PRG button (`KEY_BUILTIN`) | GPIO0 | standard classic-ESP32 boot-mode strapping pin |
| External GPS module RX | GPIO36 (`SENSOR_VP`/ADC1_CH0) | user-wired 2026-09-17, not a
Heltec onboard pin — see note below. |

**External GPS module wired to GPIO36 (2026-09-17):** an unlabeled/unidentified ("electronic
scrap") GPS module, user-wired with its TX line to GPIO36 — no return line, since GPIO36 has no
output driver (input-only, no internal pull resistor, per Espressif's datasheet) and can't drive
anything back to the module anyway. Confirmed working via a throwaway UART-sniffer firmware
(`heltec/gps_probe/`, not part of the real project firmware): valid NMEA 0183 sentences
(`GPGGA`/`GPGLL`/`GPGSA`/`GPGSV`/`GPRMC`/`GPVTG`/`GPZDA`) at **9600 baud**, 100% printable bytes,
occasionally showing a real fix (quality=1, 3-4 satellites). Module identity, exact chipset, and
logic-level (3.3 V vs 5 V TTL) are unconfirmed — GPIO36 is not 5 V-tolerant, so this was wired at
the user's own risk without that check. This removes the "no GPS wiring documented for this
board" blocker noted against a future `gps` capability port (see `docs/PLAN.md`'s Phase 4 step
7 and `.claude/agents/heltec-developer.md`), but a real driver integration (mirroring the C6's
`nmea_parser.c`/`location.c`) has not been done — this was a wiring/UART-sanity check only.

## Strapping / boot-sampled pins — treat carefully

Classic ESP32's strapping pins (sampled into the `GPIO_STRAP` register at reset, per
Espressif's ESP32 Series datasheet and esptool's boot-mode-selection reference) are a
**different set from the C6's**: **GPIO0, GPIO2, GPIO4, GPIO5, GPIO12 (`MTDI`), GPIO15
(`MTDO`)** — six pins, not the C6's GPIO0/4/5/8/9/15. Do not assume the C6's strapping set
carries over.

On this specific board, **three of those six are already committed by Heltec's own design to
onboard peripherals**, not by anything this project would choose:

- GPIO4 — OLED SDA
- GPIO5 — LoRa SCK
- GPIO15 — OLED SCL (`MTDO`; also controls boot log verbosity)

This is inherent to the board and not a hazard this project introduced, but it means any
future `display`/`lora` capability code touching those buses is implicitly touching strapping
pins — no additional external circuitry should ever be added to those lines that could force
their reset-time level. The two strapping pins **not** claimed by onboard peripherals still
need the same caution as on any classic ESP32 design:

- GPIO12 (`MTDI`) — selects flash voltage (1.8 V vs 3.3 V); an externally forced level here is
  the single most common way to soft-brick a classic ESP32 board.
- GPIO2 — must be low or floating for normal SPI-flash boot.
- GPIO0 — boot-mode select (also the board's PRG button, a normal/expected use).

## Firmware baseline notes

- ESP-IDF target for this board is `esp32` (classic dual-core Xtensa) — **not** `esp32c6`
  and **not** `esp32s3`. See `docs/PLAN.md`'s Phase 4 design section for the open question of
  whether this becomes a second top-level ESP-IDF project or a separate config within the
  existing `esp32/` tree.
- Do not carry forward this board's pin mappings to the C6, or the C6's pin mappings here —
  see `docs/hardware/esp32-c6-devkitc-1/README.md`, which already states the same caution in
  the other direction.
- Not yet pinned to a specific ESP-IDF partition table, sdkconfig target, or flash-size
  assumption — that starts only once a physical unit's actual flash size is measured
  read-only via `esptool flash_id`, matching this project's hardware-safety convention (no
  flashing/erasing/writing without explicit user request).
