#include "lora_shared_radio.h"

#include <RadioLib.h>

#include "meshcore_esp_hal.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "mesh_log.h"
#include "meshcore_proto.h"
#include "meshcore_table.h"
#include "meshtastic_proto.h"
#include "meshtastic_table.h"
}

/* SX1276 SPI pins (docs/hardware/heltec-wifi-lora-32-v2/README.md's "SX1276/SX1278 LoRa SPI
   pin mapping" table) -- confirmed unclaimed elsewhere in this firmware. DIO1 is not
   physically wired on this board; RadioLib's Module constructor defaults its optional gpio
   (DIO1) parameter to RADIOLIB_NC when omitted, which is correct here. */
#define LORA_RADIO_PIN_SCK 5
#define LORA_RADIO_PIN_MISO 19
#define LORA_RADIO_PIN_MOSI 27
#define LORA_RADIO_PIN_NSS 18
#define LORA_RADIO_PIN_RST 14
#define LORA_RADIO_PIN_DIO0 26

/* Shared RF modem preset -- MeshCore's own EU-868 default AND Meshtastic's EU_868 "LongFast"
   default happen to be identical (869.525 MHz, 250 kHz BW, SF11, CR4/5; confirmed via this
   session's research into Meshtastic's public radio-settings docs) -- see
   lora_shared_radio.h's radio-sharing decision for why this is exactly what makes
   time-multiplexing here cheap (only the sync-word register differs between the two "listen
   modes", not the whole modem configuration). Unvalidated against this board's separate
   Wi-Fi/BT combo radio's own coexistence bounds (docs/BACKLOG.md) -- listen-only (no LoRa TX
   from this capability pair) is lower risk than bidirectional/duty-cycled traffic, but the gap
   is real and unmeasured. */
#define LORA_RADIO_FREQ_MHZ 869.525f
#define LORA_RADIO_BW_KHZ 250.0f
#define LORA_RADIO_SF 11u
#define LORA_RADIO_CR 5u /* RadioLib's cr argument is the "4/x" denominator; 5 == 4/5 */

/* Sync-word values: MeshCore uses RadioLib's own default private-network sync word
   (RADIOLIB_SX127X_SYNC_WORD, 0x12) -- this project never overrides it, so `begin()` below is
   called without an explicit syncWord argument, same as before this file's meshtastic_scan
   extension. Meshtastic's firmware explicitly configures 0x2B (`radio.setSyncWord(0x2B)`,
   per meshtastic/firmware's RadioInterface config, confirmed via this session's research) --
   distinct from both LoRa's generic "private" (0x12) and "public"/LoRaWAN (0x34) conventions,
   specifically so a Meshtastic radio doesn't accidentally demodulate unrelated LoRa traffic
   using either of those defaults, and vice versa. */
#define LORA_SYNC_WORD_MESHTASTIC 0x2Bu

/* How long each protocol gets exclusive use of the radio before switching to the other --
   see lora_shared_radio.h's radio-sharing decision for why time-multiplexing was chosen over
   a runtime mode-select command. This value is an engineering guess, not a measured bound:
   neither MeshCore's nor Meshtastic's real-world node broadcast interval has been observed by
   this project (no hardware for either), so there is no data to size a "catch both protocols'
   broadcasts often enough without switching so fast it wastes airtime-listening-margin"
   tradeoff against. 60 seconds per side was picked as a moderate middle ground purely by
   engineering judgment -- flagged here, and in docs/BACKLOG.md, as unvalidated rather than
   silently presented as tuned. Whichever protocol is not the active listen mode simply cannot
   receive at all during the other's window -- a broadcast arriving then is missed outright,
   not delayed. */
#define LORA_SHARED_RADIO_DWELL_MS 60000u

#define MESHCORE_RADIO_TASK_STACK_SIZE 4096u
#define MESHCORE_RADIO_TASK_POLL_MS 20u

/* Larger of MeshCore's own documented max frame size (254 bytes) and Meshtastic's (256 bytes,
   see meshtastic_proto.c) -- shared by both listen modes since only one is ever active at a
   time. */
#define LORA_RADIO_MAX_FRAME_LEN 256u

static const char *TAG = "feb_lora_shared_radio";

static EspHal *meshcore_hal;
static Module *meshcore_module;
static SX1276 *meshcore_radio;

/* SX1276-vs-SX1278 silicon identification -- unchanged from this file's original meshcore-only
   version (meshcore_radio.cpp, before the 2026-09-27 meshtastic_scan rename/extension); see
   git history for the full derivation comment if needed again. Runs once at init, before
   continuous receive or sync-word multiplexing starts. */
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
    meshcore_hal->delay(2);

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
    meshcore_radio->setFrequency(LORA_RADIO_FREQ_MHZ);

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

typedef enum {
    LORA_MODE_MESHCORE = 0,
    LORA_MODE_MESHTASTIC = 1,
} lora_listen_mode_t;

/* lora_active_mode is only ever written by lora_shared_radio_task(); lora_on_packet() (IRAM
   ISR context) only reads it, to stamp which mode a just-arrived packet belongs to before the
   task gets around to processing it (protects against the task switching modes in the small
   window between an interrupt firing and the task noticing lora_packet_flag). Both are
   volatile, not spinlock-guarded -- same single-writer-single-reader-boolean-flag convention
   RadioLib's own examples use for meshcore_packet_flag below. */
static volatile lora_listen_mode_t lora_active_mode = LORA_MODE_MESHCORE;
static volatile lora_listen_mode_t lora_packet_mode;
static volatile bool meshcore_packet_flag;

static uint8_t lora_frame_buf[LORA_RADIO_MAX_FRAME_LEN];

static void IRAM_ATTR meshcore_on_packet(void)
{
    lora_packet_mode = lora_active_mode;
    meshcore_packet_flag = true;
}

static void lora_handle_meshcore_frame(size_t len)
{
    meshcore_advert_t advert;

    if (meshcore_proto_parse(lora_frame_buf, len, &advert)) {
        int32_t rssi_dbm = (int32_t)meshcore_radio->getRSSI();
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

        meshcore_table_upsert(&advert, rssi_dbm, now_ms);
        /* docs/WARDRIVING_PUBLISH.md "Mesh node publishing": only a sighting that carries a
           position is ever recorded here -- wdgwars.pl rejects positionless nodes anyway
           (`no_gps`), and this is the same has_location gate cbor_meshcore.h's own
           lat_e7_offset/lon_e7_offset optionality already documents. */
        if (advert.has_location) {
            (void)mesh_log_record_sighting(advert.node_id_hex, strlen(advert.node_id_hex),
                                     MESH_LOG_NETWORK_MESHCORE, advert.lat_e7, advert.lon_e7);
        }
    }
}

static void lora_handle_meshtastic_frame(size_t len)
{
    meshtastic_advert_t advert;

    if (meshtastic_proto_parse(lora_frame_buf, len, &advert)) {
        int32_t rssi_dbm = (int32_t)meshcore_radio->getRSSI();
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

        meshtastic_table_upsert(&advert, rssi_dbm, now_ms);
        /* docs/WARDRIVING_PUBLISH.md "Mesh node publishing": same has_location gate as the
           MeshCore call above -- a Meshtastic node only ever carries a position when it
           broadcasts a POSITION_APP payload on the default channel (meshtastic_proto.c's
           decrypt_default_channel_payload()), added by this task; previously this file never
           called mesh_log_record_sighting() for Meshtastic at all because
           meshtastic_advert_t had no lat/lon field. */
        if (advert.has_location) {
            (void)mesh_log_record_sighting(advert.node_id_hex, strlen(advert.node_id_hex),
                                     MESH_LOG_NETWORK_MESHTASTIC, advert.lat_e7, advert.lon_e7);
        }
    }
}

static void lora_shared_radio_switch_mode(void)
{
    lora_listen_mode_t next_mode = (lora_active_mode == LORA_MODE_MESHCORE) ? LORA_MODE_MESHTASTIC : LORA_MODE_MESHCORE;
    int16_t state;

    if (next_mode == LORA_MODE_MESHTASTIC) {
        state = meshcore_radio->setSyncWord(LORA_SYNC_WORD_MESHTASTIC);
    } else {
        state = meshcore_radio->setSyncWord(RADIOLIB_SX127X_SYNC_WORD);
    }
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "setSyncWord() failed switching to %s mode: %d -- staying on previous mode",
                  next_mode == LORA_MODE_MESHTASTIC ? "meshtastic" : "meshcore", state);
        return;
    }

    state = meshcore_radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "startReceive() failed after switching to %s mode: %d",
                  next_mode == LORA_MODE_MESHTASTIC ? "meshtastic" : "meshcore", state);
        return;
    }

    lora_active_mode = next_mode;
    ESP_LOGI(TAG, "switched listen mode -> %s", next_mode == LORA_MODE_MESHTASTIC ? "meshtastic" : "meshcore");
}

/* HARDENING_PLAN.md HP-15: this task's stack (MESHCORE_RADIO_TASK_STACK_SIZE) has never been
   measured on hardware since the Meshtastic AES+protobuf decode path was added to its call
   chain. Logged only on a new minimum (self-rate-limiting, no separate frame counter needed) --
   0 extra .bss beyond this one word, since uxTaskGetStackHighWaterMark(NULL) needs no stored
   task handle when called from the task being measured. */
static UBaseType_t lora_rx_stack_min_words = (UBaseType_t)-1;

static void lora_rx_log_stack_headroom(void)
{
    UBaseType_t words = uxTaskGetStackHighWaterMark(NULL);

    if (words < lora_rx_stack_min_words) {
        lora_rx_stack_min_words = words;
        ESP_LOGI(TAG, "lora_shared_rx stack high-water mark: %u words (%u bytes) free",
                  (unsigned)words, (unsigned)(words * sizeof(StackType_t)));
    }
}

static void lora_shared_radio_task(void *arg)
{
    uint64_t last_switch_ms = (uint64_t)(esp_timer_get_time() / 1000);

    (void)arg;
    for (;;) {
        if (meshcore_packet_flag) {
            lora_listen_mode_t mode = lora_packet_mode;
            size_t len;

            meshcore_packet_flag = false;
            len = meshcore_radio->getPacketLength();

            if (len > 0 && len <= sizeof(lora_frame_buf)) {
                int state = meshcore_radio->readData(lora_frame_buf, len);

                if (state == RADIOLIB_ERR_NONE) {
                    if (mode == LORA_MODE_MESHTASTIC) {
                        lora_handle_meshtastic_frame(len);
                    } else {
                        lora_handle_meshcore_frame(len);
                    }
                }
            } else if (len > 0) {
                meshcore_radio->readData(lora_frame_buf, sizeof(lora_frame_buf));
                ESP_LOGW(TAG, "dropped oversized frame (%u bytes)", (unsigned)len);
            }
            lora_rx_log_stack_headroom();
        }

        {
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

            if (now_ms - last_switch_ms >= LORA_SHARED_RADIO_DWELL_MS) {
                lora_shared_radio_switch_mode();
                last_switch_ms = now_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MESHCORE_RADIO_TASK_POLL_MS));
    }
}

void lora_shared_radio_init(void)
{
    int state;

    meshcore_hal = new EspHal(LORA_RADIO_PIN_SCK, LORA_RADIO_PIN_MISO, LORA_RADIO_PIN_MOSI);
    meshcore_module = new Module(meshcore_hal, LORA_RADIO_PIN_NSS, LORA_RADIO_PIN_DIO0,
                                  LORA_RADIO_PIN_RST);
    meshcore_radio = new SX1276(meshcore_module);

    /* No explicit syncWord argument -- defaults to RADIOLIB_SX127X_SYNC_WORD (0x12), i.e.
       this boots into MeshCore listen mode, matching lora_active_mode's initializer. */
    state = meshcore_radio->begin(LORA_RADIO_FREQ_MHZ, LORA_RADIO_BW_KHZ, LORA_RADIO_SF, LORA_RADIO_CR);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1276 begin() failed: %d; meshcore_scan/meshtastic_scan will report empty tables", state);
        return;
    }

    meshcore_radio_identify_chip();

    meshcore_radio->setPacketReceivedAction(meshcore_on_packet);
    state = meshcore_radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1276 startReceive() failed: %d; meshcore_scan/meshtastic_scan will report empty tables", state);
        return;
    }

    if (xTaskCreate(lora_shared_radio_task, "lora_shared_rx", MESHCORE_RADIO_TASK_STACK_SIZE, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start shared LoRa RX task; meshcore_scan/meshtastic_scan will report empty tables");
        return;
    }

    ESP_LOGI(TAG, "SX1276 initialized (%.3f MHz, BW %.0f kHz, SF%u, CR4/%u); time-multiplexing "
                   "MeshCore/Meshtastic listen modes every %u ms",
              (double)LORA_RADIO_FREQ_MHZ, (double)LORA_RADIO_BW_KHZ,
              (unsigned)LORA_RADIO_SF, (unsigned)LORA_RADIO_CR, (unsigned)LORA_SHARED_RADIO_DWELL_MS);
}
