#include "location.h"
#include "nmea_parser.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real wardriving-record
   timestamps": compile-time pins/baud, hardware-verified by tools/test_gps_antenna.ps1
   (esp32/gps_antenna_test) -- runtime-configurable pins are deliberately out of scope (see
   that section's "Scope boundary" note). RX-only: the GPS module is never transmitted to. */
static const char *TAG = "feb_location";

#define FEB_GPS_UART_PORT UART_NUM_1
#define FEB_GPS_UART_RX_GPIO 18
#define FEB_GPS_UART_TX_GPIO 19
#define FEB_GPS_UART_BAUD 9600u
#define FEB_GPS_UART_RX_BUF_SIZE 2048u
#define FEB_GPS_LINE_BUF_SIZE 128u
#define FEB_GPS_READ_CHUNK_SIZE 128u
/* Own dedicated stack (docs/BACKLOG.md's cost-efficiency section flags this project's
   repeated history of stack-overflow bugs on BLE-callback-adjacent tasks) -- this task never
   touches the NimBLE host task's own stack budget. Parsing uses only small stack-local
   scalars/structs (nmea_gga_t/nmea_rmc_t are a handful of ints each); the line buffer itself
   is static (below), not stack-resident. 3072 bytes matches this codebase's existing
   lightweight-task precedent (main.c's reconnect_task, factory_reset.c's factory_reset_task). */
#define FEB_GPS_TASK_STACK_SIZE 3072u

static portMUX_TYPE location_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Guarded by location_spinlock; read by location_get_fix() (called from the NimBLE host
   task), written by location_task() (its own dedicated task) -- a short critical section
   avoids a torn read/write of this multi-field state on a preemptively-scheduled RTOS,
   independent of core count. */
static feb_location_state_t location_state = FEB_LOCATION_NO_SIGNAL;
static feb_location_t location_current;
static bool location_traffic_seen;
static bool location_have_gga;
static nmea_gga_t location_last_gga;
static bool location_have_rmc;
static nmea_rmc_t location_last_rmc;

/* Task-exclusive; not guarded by location_spinlock since only location_task() ever touches
   these. Static/file-scope per this project's stack-budget convention rather than
   task-stack-resident. */
static char location_line_buf[FEB_GPS_LINE_BUF_SIZE];
static uint8_t location_read_chunk[FEB_GPS_READ_CHUNK_SIZE];

/* Recomputes location_state/location_current from location_last_gga/location_last_rmc.
   Caller must hold location_spinlock. docs/PROTOCOL.md's `gps` status table: no_signal until
   at least one checksum-valid NMEA sentence of any type has been seen (location_traffic_seen
   -- broader than "GGA or RMC specifically" so a module emitting only e.g. VTG before its
   first GGA/RMC still reports acquiring, not no_signal); fix requires GGA fix_quality > 0
   AND RMC status 'A' simultaneously; acquiring otherwise. No fix-quality/HDOP/satellite
   threshold (docs/PLAN.md design decision 3). */
static void location_recompute_locked(void)
{
    bool gga_fix = location_have_gga && location_last_gga.fix_quality > 0u;
    bool rmc_valid = location_have_rmc && location_last_rmc.status_active;

    if (!location_traffic_seen) {
        location_state = FEB_LOCATION_NO_SIGNAL;
        return;
    }

    if (location_have_gga) {
        location_current.fix_quality = location_last_gga.fix_quality;
        location_current.satellites = location_last_gga.satellites;
        location_current.hdop_e1 = location_last_gga.hdop_e1;
        location_current.lat_e7 = location_last_gga.lat_e7;
        location_current.lon_e7 = location_last_gga.lon_e7;
        location_current.altitude_dm = location_last_gga.altitude_dm;
    }

    if (gga_fix && rmc_valid) {
        location_current.utc_timestamp_s = nmea_rmc_to_unix_time(&location_last_rmc);
        location_state = FEB_LOCATION_FIX;
    } else {
        location_state = FEB_LOCATION_ACQUIRING;
    }
}

static void location_process_line(const char *line, size_t len)
{
    nmea_gga_t gga;
    nmea_rmc_t rmc;
    bool got_gga;
    bool got_rmc;

    if (len == 0 || line[0] != '$' || !nmea_checksum_valid(line, len)) {
        return;
    }

    got_gga = nmea_parse_gga(line, len, &gga);
    got_rmc = !got_gga && nmea_parse_rmc(line, len, &rmc);

    portENTER_CRITICAL(&location_spinlock);
    location_traffic_seen = true;
    if (got_gga) {
        location_have_gga = true;
        location_last_gga = gga;
    }
    if (got_rmc) {
        location_have_rmc = true;
        location_last_rmc = rmc;
    }
    location_recompute_locked();
    portEXIT_CRITICAL(&location_spinlock);
}

static void location_task(void *arg)
{
    size_t line_len = 0;

    (void)arg;
    while (1) {
        int n = uart_read_bytes(FEB_GPS_UART_PORT, location_read_chunk,
                                 sizeof(location_read_chunk), pdMS_TO_TICKS(200));
        int i;

        for (i = 0; i < n; i++) {
            char c = (char)location_read_chunk[i];

            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                if (line_len > 0) {
                    location_process_line(location_line_buf, line_len);
                    line_len = 0;
                }
                continue;
            }
            if (line_len < sizeof(location_line_buf) - 1u) {
                location_line_buf[line_len++] = c;
            } else {
                line_len = 0; /* oversized/malformed line: drop and resync */
            }
        }
    }
}

void location_init(void)
{
    uart_config_t cfg = {
        .baud_rate = (int)FEB_GPS_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err;

    err = uart_driver_install(FEB_GPS_UART_PORT, (int)FEB_GPS_UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s; GPS disabled", esp_err_to_name(err));
        return;
    }
    err = uart_param_config(FEB_GPS_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s; GPS disabled", esp_err_to_name(err));
        return;
    }
    /* RX-only: TX pin is configured (a floating/unconfigured UART TX pin is bad practice)
       but location_task() never calls uart_write_bytes() -- an earlier wake/cold-start
       command burst was proven actively harmful in the antenna smoke test, forcing
       re-acquisition on every reconnect. */
    err = uart_set_pin(FEB_GPS_UART_PORT, FEB_GPS_UART_TX_GPIO, FEB_GPS_UART_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s; GPS disabled", esp_err_to_name(err));
        return;
    }

    if (xTaskCreate(location_task, "gps_parse", FEB_GPS_TASK_STACK_SIZE, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start GPS parse task; GPS disabled");
    }
}

feb_location_state_t location_get_fix(feb_location_t *out)
{
    feb_location_state_t state;

    portENTER_CRITICAL(&location_spinlock);
    state = location_state;
    if (out != NULL) {
        *out = location_current;
    }
    portEXIT_CRITICAL(&location_spinlock);
    return state;
}
