#include "meshcore_radio.h"

#include <cstdio>

#include <RadioLib.h>

#include "meshcore_esp_hal.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "meshcore_proto.h"
#include "meshcore_table.h"
}

/* SX1276 SPI pins (docs/hardware/heltec-wifi-lora-32-v2/README.md's "SX1276/SX1278 LoRa SPI
   pin mapping" table) -- confirmed unclaimed elsewhere in this firmware (no other source
   file under heltec/main touches SPI as of this port). DIO1 is not physically wired on this
   board;
   RadioLib's Module constructor defaults its optional gpio (DIO1) parameter to RADIOLIB_NC
   when omitted, which is correct here -- DIO1 is only needed for some TX/timeout-driven
   features this passive-listen-only capability doesn't use. */
#define MESHCORE_RADIO_PIN_SCK 5
#define MESHCORE_RADIO_PIN_MISO 19
#define MESHCORE_RADIO_PIN_MOSI 27
#define MESHCORE_RADIO_PIN_NSS 18
#define MESHCORE_RADIO_PIN_RST 14
#define MESHCORE_RADIO_PIN_DIO0 26

/* MeshCore's EU-868 default preset (docs/PLAN.md's "MeshCore Scan Capability" design plan) --
   a single fixed compile-time configuration, no frequency-hopping/multi-preset scanning.
   Unvalidated against this board's separate Wi-Fi/BT combo radio's own coexistence bounds
   (see docs/BACKLOG.md's new item on this) -- this capability only ever listens (no
   transmit), which is lower risk than the bidirectional/duty-cycled traffic docs/LESSONS.md's
   coexistence writeups are about, but the gap is real and unmeasured, not just theoretical. */
#define MESHCORE_RADIO_FREQ_MHZ 869.525f
#define MESHCORE_RADIO_BW_KHZ 250.0f
#define MESHCORE_RADIO_SF 11u
#define MESHCORE_RADIO_CR 5u /* RadioLib's cr argument is the "4/x" denominator; 5 == 4/5 */

/* Own dedicated stack, same rationale as location.c's FEB_GPS_TASK_STACK_SIZE comment (this
   project's repeated BLE-callback-adjacent task-stack-overflow bug class) -- this task never
   touches the NimBLE host task's own stack budget. Larger than location.c's 3072 bytes
   because RadioLib's C++ call chain (virtual dispatch through RadioLibHal/Module/SX127x) is
   less predictable to hand-account than nmea_parser.c's plain-C parsing; not yet confirmed
   against a real -fstack-usage measurement (no hardware to flash against this session -- see
   this capability's own "not hardware-verified" caveat), so treat this constant as a
   conservative starting point, not a measured budget. */
#define MESHCORE_RADIO_TASK_STACK_SIZE 4096u
#define MESHCORE_RADIO_TASK_POLL_MS 20u

/* MeshCore's own documented maximum total frame size is 1 (header) + 4 (transport codes,
   worst case) + 1 (path length) + 64 (path, worst case) + 184 (payload, worst case) = 254
   bytes; 256 leaves a small margin without meaningfully changing this task's RAM footprint. */
#define MESHCORE_RADIO_MAX_FRAME_LEN 256u

static const char *TAG = "feb_meshcore_radio";

static EspHal *meshcore_hal;
static Module *meshcore_module;
static SX1276 *meshcore_radio;

/* SX1276-vs-SX1278 silicon identification (docs/hardware/heltec-wifi-lora-32-v2/README.md's
   "LoRa radio" bullet was unconfirmed against the physical unit -- Heltec sells both an
   SX1276 863-928 MHz SKU and an SX1278 433 MHz-only SKU under the same "V2" name). RegVersion
   (0x42) reads the same silicon-revision byte (0x12) on both variants, so it cannot
   distinguish them (SX127x.h's own getChipVersion() doc comment says as much). What genuinely
   differs between the two dies is the frequency synthesizer's tuning range: the SX1278 VCO
   physically cannot lock above ~525 MHz. FSK mode exposes a hardware "PllLock" status bit
   (RegIrqFlags1 bit 4) that LoRa mode does not; entering FSRX ("frequency synthesis RX", a
   receive-prep mode that runs the synthesizer without enabling the LNA, antenna switch, or
   PA -- no RF is emitted or received) at a candidate frequency and reading that bit back is a
   direct, no-TX, no-antenna-required readout of whether the physical PLL actually locked
   there, unlike a Frf-register readback (a register write/read always round-trips regardless
   of whether the synthesizer can produce that frequency, so it can't tell the two chips
   apart on its own).
   These register-address/bit constants mirror RadioLib's own (internal, not part of the
   public SX1276.h/Module.h surface this file otherwise uses) RADIOLIB_SX127X_REG_OP_MODE /
   RADIOLIB_SX127X_REG_IRQ_FLAGS_1 / RADIOLIB_SX127X_FLAG_PLL_LOCK constants in
   managed_components/jgromes__radiolib/src/modules/SX127x/SX127x.h -- cross-checked against
   that file rather than re-derived from the datasheet alone. The LoRa<->FSK OpMode dance
   below (LoRa+Sleep -> FSK+Sleep -> FSK+Standby -> FSK+FSRX) matches the sequence RadioLib's
   own SX127x::getTempRaw() uses to leave LoRa mode safely (LongRangeMode, RegOpMode bit 7,
   can only change while the mode bits are Sleep, per the SX127x datasheet). */
#define MESHCORE_RADIO_REG_OP_MODE 0x01u
#define MESHCORE_RADIO_REG_IRQ_FLAGS_1 0x3Eu
#define MESHCORE_RADIO_OPMODE_LORA_SLEEP 0x80u
#define MESHCORE_RADIO_OPMODE_FSK_SLEEP 0x00u
#define MESHCORE_RADIO_OPMODE_FSK_FSRX 0x04u
#define MESHCORE_RADIO_FLAG_PLL_LOCK 0x10u
#define MESHCORE_RADIO_IDENTIFY_CONTROL_FREQ_MHZ 433.775f
#define MESHCORE_RADIO_IDENTIFY_CANDIDATE_FREQ_MHZ 869.618f

static bool meshcore_radio_pll_locks_at(float freq_mhz)
{
    int16_t state;

    state = meshcore_radio->setFrequency(freq_mhz);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "identify: setFrequency(%.3f) failed: %d", (double)freq_mhz, state);
        return false;
    }

    meshcore_module->SPIsetRegValue(MESHCORE_RADIO_REG_OP_MODE, MESHCORE_RADIO_OPMODE_FSK_FSRX);
    meshcore_hal->delay(2); /* margin beyond SPIsetRegValue's own 2 ms write-verify wait, for PLL settle */

    uint8_t irq1 = (uint8_t)meshcore_module->SPIgetRegValue(MESHCORE_RADIO_REG_IRQ_FLAGS_1);

    meshcore_module->SPIsetRegValue(MESHCORE_RADIO_REG_OP_MODE, MESHCORE_RADIO_OPMODE_FSK_SLEEP);

    return (irq1 & MESHCORE_RADIO_FLAG_PLL_LOCK) != 0;
}

static void meshcore_radio_identify_chip(void)
{
    uint8_t prev_opmode = (uint8_t)meshcore_module->SPIgetRegValue(MESHCORE_RADIO_REG_OP_MODE);
    int16_t version = meshcore_radio->getChipVersion();

    meshcore_module->SPIsetRegValue(MESHCORE_RADIO_REG_OP_MODE, MESHCORE_RADIO_OPMODE_LORA_SLEEP);
    meshcore_module->SPIsetRegValue(MESHCORE_RADIO_REG_OP_MODE, MESHCORE_RADIO_OPMODE_FSK_SLEEP);

    bool locked_control = meshcore_radio_pll_locks_at(MESHCORE_RADIO_IDENTIFY_CONTROL_FREQ_MHZ);
    bool locked_candidate = meshcore_radio_pll_locks_at(MESHCORE_RADIO_IDENTIFY_CANDIDATE_FREQ_MHZ);

    meshcore_module->SPIsetRegValue(MESHCORE_RADIO_REG_OP_MODE, MESHCORE_RADIO_OPMODE_LORA_SLEEP);
    meshcore_module->SPIsetRegValue(MESHCORE_RADIO_REG_OP_MODE, prev_opmode);
    meshcore_radio->setFrequency(MESHCORE_RADIO_FREQ_MHZ);

    if (!locked_control) {
        ESP_LOGW(TAG, "identify: PLL did not lock at the %.3f MHz control frequency either -- "
                       "test inconclusive, not a chip-identity result",
                  (double)MESHCORE_RADIO_IDENTIFY_CONTROL_FREQ_MHZ);
    }

    ESP_LOGI(TAG, "SX127x identify: RegVersion=0x%02X, PLL lock @%.3f MHz=%d, PLL lock @%.3f MHz=%d -> %s",
              (unsigned)version,
              (double)MESHCORE_RADIO_IDENTIFY_CONTROL_FREQ_MHZ, (int)locked_control,
              (double)MESHCORE_RADIO_IDENTIFY_CANDIDATE_FREQ_MHZ, (int)locked_candidate,
              !locked_control ? "inconclusive (see warning above)"
              : locked_candidate ? "SX1276/SX1279-class silicon (863-1020 MHz PLL range; 868/915 MHz usable)"
                                  : "SX1278-class silicon (433 MHz-only PLL; 868/915 MHz NOT usable)");
}

/* Set only from meshcore_on_packet() (IRAM ISR context, via RadioLib's
   setPacketReceivedAction()); cleared only from meshcore_radio_task(). volatile, not
   spinlock-guarded -- a single-writer-single-reader boolean flag needs no stronger
   synchronization than that, same convention RadioLib's own examples use. */
static volatile bool meshcore_packet_flag;

/* Static/file-scope per this project's stack-budget convention (see
   MESHCORE_RADIO_TASK_STACK_SIZE's comment above) -- never stack-resident. Only
   meshcore_radio_task() ever touches this; it is not reentrant and there is exactly one
   instance of this task. */
static uint8_t meshcore_frame_buf[MESHCORE_RADIO_MAX_FRAME_LEN];

static void IRAM_ATTR meshcore_on_packet(void)
{
    meshcore_packet_flag = true;
}

/* Temporary diagnostic logging -- remove once real-world MeshCore reception is confirmed
   working (see docs/BACKLOG.md's "remove meshcore_radio.cpp RX-path diagnostic logging"
   item). Renders up to the first MESHCORE_RADIO_DIAG_HEX_BYTES of a raw received frame as
   space-separated hex, into a caller-owned stack buffer -- bounded by len and by the
   destination buffer size, never reads past frame[0..len). */
#define MESHCORE_RADIO_DIAG_HEX_BYTES 16u

static void meshcore_radio_diag_hex(const uint8_t *frame, size_t len, char *out, size_t out_size)
{
    size_t dump_len = len < MESHCORE_RADIO_DIAG_HEX_BYTES ? len : MESHCORE_RADIO_DIAG_HEX_BYTES;
    size_t pos = 0;

    out[0] = '\0';
    for (size_t i = 0; i < dump_len && pos + 3 < out_size; i++) {
        int written = snprintf(out + pos, out_size - pos, "%02x ", (unsigned)frame[i]);
        if (written <= 0) {
            break;
        }
        pos += (size_t)written;
    }
}

static void meshcore_radio_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (meshcore_packet_flag) {
            size_t len;

            meshcore_packet_flag = false;
            len = meshcore_radio->getPacketLength();

            /* Diagnostic: proves the DIO0 IRQ fired and the FIFO had something in it,
               independent of whether it turns out to be a valid MeshCore frame. */
            ESP_LOGI(TAG, "diag: RX IRQ fired, getPacketLength()=%u", (unsigned)len);

            if (len > 0 && len <= sizeof(meshcore_frame_buf)) {
                int state = meshcore_radio->readData(meshcore_frame_buf, len);

                if (state == RADIOLIB_ERR_NONE) {
                    meshcore_advert_t advert;
                    char hex_buf[MESHCORE_RADIO_DIAG_HEX_BYTES * 3u + 1u];

                    /* Diagnostic: raw frame evidence before any MeshCore-specific parsing is
                       attempted, so plausible-looking LoRa traffic can be distinguished from
                       noise/garbage even if meshcore_proto_parse() rejects it below. */
                    meshcore_radio_diag_hex(meshcore_frame_buf, len, hex_buf, sizeof(hex_buf));
                    ESP_LOGI(TAG, "diag: readData() ok, len=%u, rssi=%d dBm, bytes: %s",
                              (unsigned)len, (int)meshcore_radio->getRSSI(), hex_buf);

                    if (meshcore_proto_parse(meshcore_frame_buf, len, &advert)) {
                        int32_t rssi_dbm = (int32_t)meshcore_radio->getRSSI();
                        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

                        ESP_LOGI(TAG, "diag: meshcore_proto_parse() succeeded (valid ADVERT)");
                        meshcore_table_upsert(&advert, rssi_dbm, now_ms);
                        ESP_LOGI(TAG, "diag: meshcore_table_upsert() done, node_id=%s",
                                  advert.node_id_hex);
                    } else {
                        ESP_LOGI(TAG, "diag: meshcore_proto_parse() failed (not a valid ADVERT)");
                    }
                } else {
                    ESP_LOGW(TAG, "readData() failed: %d", state);
                }
            } else if (len > 0) {
                /* Bigger than MeshCore's own documented max frame size (already margined in
                   MESHCORE_RADIO_MAX_FRAME_LEN) -- not a valid MeshCore frame, but the
                   module's FIFO/IRQ state still needs clearing via readData() (with a
                   truncated length) so continuous receive can resume for the next frame. */
                meshcore_radio->readData(meshcore_frame_buf, sizeof(meshcore_frame_buf));
                ESP_LOGW(TAG, "dropped oversized frame (%u bytes)", (unsigned)len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(MESHCORE_RADIO_TASK_POLL_MS));
    }
}

void meshcore_radio_init(void)
{
    int state;

    meshcore_hal = new EspHal(MESHCORE_RADIO_PIN_SCK, MESHCORE_RADIO_PIN_MISO, MESHCORE_RADIO_PIN_MOSI);
    meshcore_module = new Module(meshcore_hal, MESHCORE_RADIO_PIN_NSS, MESHCORE_RADIO_PIN_DIO0,
                                  MESHCORE_RADIO_PIN_RST);
    meshcore_radio = new SX1276(meshcore_module);

    state = meshcore_radio->begin(MESHCORE_RADIO_FREQ_MHZ, MESHCORE_RADIO_BW_KHZ,
                                   MESHCORE_RADIO_SF, MESHCORE_RADIO_CR);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1276 begin() failed: %d; meshcore_scan will report an empty table", state);
        return;
    }

    meshcore_radio_identify_chip();

    meshcore_radio->setPacketReceivedAction(meshcore_on_packet);
    state = meshcore_radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1276 startReceive() failed: %d; meshcore_scan will report an empty table", state);
        return;
    }

    if (xTaskCreate(meshcore_radio_task, "meshcore_rx", MESHCORE_RADIO_TASK_STACK_SIZE, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start meshcore_scan RX task; meshcore_scan will report an empty table");
        return;
    }

    ESP_LOGI(TAG, "SX1276 initialized (%.3f MHz, BW %.0f kHz, SF%u, CR4/%u); listening for MeshCore ADVERTs",
              (double)MESHCORE_RADIO_FREQ_MHZ, (double)MESHCORE_RADIO_BW_KHZ,
              (unsigned)MESHCORE_RADIO_SF, (unsigned)MESHCORE_RADIO_CR);
}
