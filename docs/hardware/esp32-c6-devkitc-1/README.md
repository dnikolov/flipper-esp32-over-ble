# ESP32-C6-WROOM-1 DevKitC-1-N4 Reference Bundle

This directory is the local reference bundle for the Espressif ESP32-C6-DevKitC-1 populated with an ESP32-C6-WROOM-1-N4 module. Consult the exact board revision schematic before assigning a peripheral pin.

## Local vendor materials

- `vendor/esp32-c6-wroom-1_datasheet_en.pdf`: ESP32-C6-WROOM-1/WROOM-1U module datasheet.
- `vendor/esp32-c6_datasheet_en.pdf`: ESP32-C6 chip datasheet.
- `source/guide/`: upstream reStructuredText source for the Espressif DevKitC-1 v1.2 hardware guide.
- `source/static/`: board photographs, block diagrams, and pin-layout artwork used by the upstream guide.

The guide source identifies the DevKitC-1 v1.2 as an ESP32-C6-WROOM-1(U) board with 8 MB SPI flash. Confirm that a physical board marked `N4` has the expected 4 MB flash before selecting partition sizes or OTA layout.

## Official online sources

- Board guide: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/hw-reference/esp32c6/user-guide-devkitc-1.html
- ESP32-C6 ESP-IDF documentation: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/
- ESP32-C6-DevKitC-1 guide source: https://github.com/espressif/esp-dev-kits/tree/master/docs/en/esp32-c6-devkitc-1
- Board source repository: https://github.com/espressif/esp-dev-kits
- Module datasheet: https://www.espressif.com/sites/default/files/documentation/esp32-c6-wroom-1_wroom-1u_datasheet_en.pdf
- Chip datasheet: https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf

The public `esp-dev-kits` repository currently provides the board-guide source and rendered hardware assets for this target. No KiCad, schematic, PCB, or mechanical CAD source matching the board was present in the retrieved repository revision. Use the guide's schematic link or obtain the release-specific schematic from Espressif before a hardware design depends on a board net.

## Verified board facts

- Target SoC: ESP32-C6, with 2.4 GHz Wi-Fi 6, Bluetooth LE 5, and IEEE 802.15.4 for Zigbee/Thread.
- The WROOM-1 module uses a PCB antenna. WROOM-1U is the external-antenna variant; do not exchange their RF assumptions.
- The board has two USB Type-C paths:
  - a USB-to-UART bridge port for serial flashing and bridge-based communication;
  - a native ESP32-C6 USB 2.0 full-speed device/Serial-JTAG port, connected to GPIO12 (`USB_D-`) and GPIO13 (`USB_D+`), for USB device protocols and JTAG.
- Native USB data pins do not make the ESP32-C6 a USB host.
- UART0 is exposed as GPIO16 (`U0TXD`) and GPIO17 (`U0RXD`) on header J3. Verify its bridge connections and boot behavior against the chosen revision before using it for serial development.
- GPIO8 drives the onboard RGB LED, which is an **addressable WS2812** (confirmed in
  `source/guide/user_guide.rst`: "RGB LED - Addressable RGB LED, driven by GPIO8", plus a note
  on the board's WS2812 driving circuit) — not a plain digital LED. `gpio_set_level()` alone
  cannot light it; driving it requires the WS2812 one-wire serial protocol (e.g. via
  `driver/rmt_tx.h`, as used in `esp32/main/factory_reset.c`'s factory-reset hold-to-blink
  feedback).
- GPIO0, GPIO4, GPIO5, GPIO8, GPIO9, and GPIO15 have strapping/JTAG-related functions. Do not connect external circuitry that can force their reset-time level without checking the chip datasheet.
- The guide lists USB Type-C power, 5 V/GND headers, and 3V3/GND headers as power options. Treat these as mutually exclusive unless the board schematic explicitly approves the combined configuration.
- J5 is provided for current measurement. Its jumper changes the module power path; retain it for normal operation unless performing a planned current measurement.

## Firmware baseline

Use ESP-IDF with the `esp32c6` target. Do not carry forward classic ESP32 or Heltec V2 pin mappings: this board has no onboard LoRa or OLED, includes IEEE 802.15.4, and has native USB hardware.

For the Flipper protocol project, pairing is BLE-only. The current BLE-role decision means the ESP32-C6 operates as the BLE central and the Flipper advertises the GATT service. Keep BLE connected while scheduling Wi-Fi scans conservatively because Wi-Fi and BLE share the 2.4 GHz radio. Establish the board revision, flash size, USB data path, and any attached peripherals before creating the ESP-IDF partition table or choosing GPIOs.