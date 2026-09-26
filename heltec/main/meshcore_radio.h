/* SX1276 (RadioLib-driven) MeshCore listener -- pure-C interface; the implementation
   (meshcore_radio.cpp) is C++ because RadioLib itself is a C++-only library (no C API), so
   this is the one file in this firmware that isn't plain C. Modeled directly on location.h/
   location.c's shape (docs/PLAN.md's "MeshCore Scan Capability" design plan): a single
   init-once-at-boot entry point that starts a dedicated background task; the task's parsed
   output lands in meshcore_table.h, not returned through this header, matching how
   location.c hands its parsed fixes to location_get_fix() rather than a radio-layer getter. */
#ifndef FEB_MESHCORE_RADIO_H
#define FEB_MESHCORE_RADIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the SX1276 on this board's already-wired, previously-unclaimed LoRa SPI pins
   (docs/hardware/heltec-wifi-lora-32-v2/README.md's "SX1276/SX1278 LoRa SPI pin mapping":
   SCK GPIO5, MISO GPIO19, MOSI GPIO27, NSS GPIO18, RST GPIO14, DIO0 GPIO26 -- see
   meshcore_radio.cpp for the compile-time pin/radio-parameter constants), starts RadioLib's
   interrupt-driven continuous receive, and starts a dedicated FreeRTOS task that hands each
   received frame to meshcore_proto_parse() and upserts a successful ADVERT decode into
   meshcore_table.h. Call once at boot, unconditionally, regardless of BLE connection state --
   the same "always-on background driver" shape as location_init() (location.h). Fixed
   compile-time radio parameters only (MeshCore's EU-868 default: 869.525 MHz, 250 kHz
   bandwidth, SF11, CR 4/5) -- no runtime configuration, matching this project's GPS-driver
   precedent. A radio/task-init failure is logged and leaves the table permanently empty;
   `meshcore_scan`'s `status` query still answers (node_count=0, total_known_nodes=0) rather
   than failing outright -- this is optional peripheral hardware, not required for the board
   to boot (same posture as location_init()'s own failure handling). */
void meshcore_radio_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FEB_MESHCORE_RADIO_H */
