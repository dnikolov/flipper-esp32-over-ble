#include "status_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "location.h"
#include "meshcore_table.h"
#include "meshtastic_table.h"

#define DISPLAY_TAG "feb_status_display"
#define OLED_I2C_ADDRESS 0x3Cu
#define OLED_SDA_GPIO GPIO_NUM_4
#define OLED_SCL_GPIO GPIO_NUM_15
#define OLED_RESET_GPIO GPIO_NUM_16
#define OLED_WIDTH 128u
#define OLED_HEIGHT 64u
#define OLED_PAGE_HEIGHT 8u
#define OLED_FRAMEBUFFER_SIZE (OLED_WIDTH * OLED_HEIGHT / OLED_PAGE_HEIGHT)
#define OLED_REFRESH_MS 1000u
#define OLED_ROTATE_FRAMES 4u

typedef struct {
    char character;
    uint8_t rows[7];
} display_glyph_t;

static const display_glyph_t display_glyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}}, {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}}, {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}}, {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}}, {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}}, {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
    {':', {0, 4, 4, 0, 4, 4, 0}}, {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'.', {0, 0, 0, 0, 0, 12, 12}}, {'_', {0, 0, 0, 0, 0, 0, 31}},
    {'/', {1, 2, 2, 4, 8, 8, 16}}, {'?', {14, 17, 1, 2, 4, 0, 4}},
};

static portMUX_TYPE display_state_lock = portMUX_INITIALIZER_UNLOCKED;
static feb_display_ble_state_t display_ble_state = FEB_DISPLAY_BLE_DISCONNECTED;

void feb_status_display_set_ble_state(feb_display_ble_state_t state)
{
    portENTER_CRITICAL(&display_state_lock);
    display_ble_state = state;
    portEXIT_CRITICAL(&display_state_lock);
}

static feb_display_ble_state_t get_ble_state(void)
{
    feb_display_ble_state_t state;

    portENTER_CRITICAL(&display_state_lock);
    state = display_ble_state;
    portEXIT_CRITICAL(&display_state_lock);
    return state;
}

static const display_glyph_t *find_glyph(char character)
{
    size_t index;

    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    for (index = 0; index < sizeof(display_glyphs) / sizeof(display_glyphs[0]); index++) {
        if (display_glyphs[index].character == character) {
            return &display_glyphs[index];
        }
    }
    return &display_glyphs[sizeof(display_glyphs) / sizeof(display_glyphs[0]) - 1u];
}

static void draw_text(uint8_t *framebuffer, unsigned int y, const char *text)
{
    unsigned int x = 0;

    while (*text != '\0' && x + 5u <= OLED_WIDTH) {
        const display_glyph_t *glyph = find_glyph(*text++);
        unsigned int row;

        for (row = 0; row < 7u; row++) {
            unsigned int column;
            for (column = 0; column < 5u; column++) {
                if ((glyph->rows[row] & (1u << (4u - column))) != 0) {
                    unsigned int pixel_y = y + row;
                    framebuffer[(pixel_y / OLED_PAGE_HEIGHT) * OLED_WIDTH + x + column] |=
                        (uint8_t)(1u << (pixel_y % OLED_PAGE_HEIGHT));
                }
            }
        }
        x += 6u;
    }
}

static const char *ble_state_text(feb_display_ble_state_t state)
{
    switch (state) {
    case FEB_DISPLAY_BLE_SCANNING:
        return "FLIPPER: SCANNING";
    case FEB_DISPLAY_BLE_PAIRING_MODE:
        return "PAIR MODE: SCANNING";
    case FEB_DISPLAY_BLE_CONNECTING:
        return "FLIPPER: CONNECTING";
    case FEB_DISPLAY_BLE_PAIRING:
        return "LINK: PAIRING";
    case FEB_DISPLAY_BLE_AUTHENTICATING:
        return "LINK: AUTHENTICATING";
    case FEB_DISPLAY_BLE_AUTHENTICATED:
        return "LINK: AUTHENTICATED";
    case FEB_DISPLAY_BLE_DISCONNECTED:
    default:
        return "FLIPPER: DISCONNECTED";
    }
}

static void format_seen_age(uint32_t last_seen_ms, uint32_t now_ms, char *out, size_t out_size)
{
    uint32_t elapsed_seconds = (now_ms - last_seen_ms) / 1000u;

    if (elapsed_seconds < 60u) {
        (void)snprintf(out, out_size, "SEEN: %luS AGO", (unsigned long)elapsed_seconds);
    } else {
        (void)snprintf(out, out_size, "SEEN: %luM AGO", (unsigned long)(elapsed_seconds / 60u));
    }
}

static void render_meshcore(char lines[8][32], uint32_t now_ms)
{
    meshcore_table_entry_t entry = {0};
    uint64_t meshcore_total = 0;
    uint64_t meshtastic_total = 0;
    size_t recent_count = meshcore_table_snapshot(&entry, 1u, &meshcore_total);

    (void)meshtastic_table_snapshot(NULL, 0, &meshtastic_total);
    (void)snprintf(lines[1], sizeof(lines[1]), "MC:%u MT:%u",
                   (unsigned int)meshcore_total, (unsigned int)meshtastic_total);
    (void)snprintf(lines[2], sizeof(lines[2]), "MESHCORE");

    if (recent_count == 0) {
        (void)snprintf(lines[3], sizeof(lines[3]), "NO NODES HEARD");
        return;
    }
    (void)snprintf(lines[3], sizeof(lines[3]), "NAME: %.15s", entry.has_name ? entry.name : "UNKNOWN");
    (void)snprintf(lines[4], sizeof(lines[4]), "ID: %s", entry.node_id_hex);
    (void)snprintf(lines[5], sizeof(lines[5]), "RSSI: %ld DBM", (long)entry.rssi_dbm);
    format_seen_age(entry.last_seen_ms, now_ms, lines[6], sizeof(lines[6]));
}

static void render_meshtastic(char lines[8][32], uint32_t now_ms)
{
    meshtastic_table_entry_t entry = {0};
    uint64_t meshcore_total = 0;
    uint64_t meshtastic_total = 0;
    size_t recent_count = meshtastic_table_snapshot(&entry, 1u, &meshtastic_total);

    (void)meshcore_table_snapshot(NULL, 0, &meshcore_total);
    (void)snprintf(lines[1], sizeof(lines[1]), "MC:%u MT:%u",
                   (unsigned int)meshcore_total, (unsigned int)meshtastic_total);
    (void)snprintf(lines[2], sizeof(lines[2]), "MESHTASTIC");

    if (recent_count == 0) {
        (void)snprintf(lines[3], sizeof(lines[3]), "NO NODES HEARD");
        return;
    }
    (void)snprintf(lines[3], sizeof(lines[3]), "NAME: %.15s", entry.has_name ? entry.name : "UNKNOWN");
    (void)snprintf(lines[4], sizeof(lines[4]), "ID: %s", entry.node_id_hex);
    (void)snprintf(lines[5], sizeof(lines[5]), "RSSI: %ld DBM", (long)entry.rssi_dbm);
    format_seen_age(entry.last_seen_ms, now_ms, lines[6], sizeof(lines[6]));
}

static void render_gps(char lines[8][32])
{
    feb_location_t fix = {0};

    if (location_get_fix(&fix) != FEB_LOCATION_FIX) {
        return;
    }
    (void)snprintf(lines[7], sizeof(lines[7]), "%02u:%02u:%02uZ %u.%uKMH",
                   (unsigned int)((fix.utc_timestamp_s / 3600u) % 24u),
                   (unsigned int)((fix.utc_timestamp_s / 60u) % 60u),
                   (unsigned int)(fix.utc_timestamp_s % 60u),
                   (unsigned int)(fix.speed_e1_kmh / 10u),
                   (unsigned int)(fix.speed_e1_kmh % 10u));
}

static void render_status(uint8_t *framebuffer, bool show_meshcore, uint32_t now_ms)
{
    char lines[8][32] = {{0}};

    (void)snprintf(lines[0], sizeof(lines[0]), "%s", ble_state_text(get_ble_state()));
    if (show_meshcore) {
        render_meshcore(lines, now_ms);
    } else {
        render_meshtastic(lines, now_ms);
    }
    render_gps(lines);

    memset(framebuffer, 0, OLED_FRAMEBUFFER_SIZE);
    for (unsigned int line = 0; line < 8u; line++) {
        draw_text(framebuffer, line * 8u, lines[line]);
    }
}

static esp_err_t write_frame(i2c_master_dev_handle_t device, const uint8_t *framebuffer)
{
    uint8_t command[4];
    uint8_t page_data[OLED_WIDTH + 1u];

    page_data[0] = 0x40;
    for (uint8_t page = 0; page < OLED_HEIGHT / OLED_PAGE_HEIGHT; page++) {
        command[0] = 0x00;
        command[1] = (uint8_t)(0xB0u | page);
        command[2] = 0x00;
        command[3] = 0x10;
        if (i2c_master_transmit(device, command, sizeof(command), 100) != ESP_OK) {
            return ESP_FAIL;
        }
        memcpy(&page_data[1], &framebuffer[(size_t)page * OLED_WIDTH], OLED_WIDTH);
        if (i2c_master_transmit(device, page_data, sizeof(page_data), 100) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static void destroy_i2c(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t device)
{
    if (device != NULL) {
        (void)i2c_master_bus_rm_device(device);
    }
    if (bus != NULL) {
        (void)i2c_del_master_bus(bus);
    }
}

static void display_task(void *arg)
{
    static const uint8_t init_commands[] = {
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14,
        0x20, 0x02, 0xA0, 0xC0, 0xDA, 0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB,
        0x40, 0xA4, 0xA6, 0xAF,
    };
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t device = NULL;
    uint8_t *framebuffer;
    bool show_meshcore = true;
    unsigned int frame_count = 0;
    esp_err_t err;

    (void)arg;
    framebuffer = malloc(OLED_FRAMEBUFFER_SIZE);
    if (framebuffer == NULL) {
        ESP_LOGE(DISPLAY_TAG, "framebuffer allocation failed; OLED disabled");
        vTaskDelete(NULL);
        return;
    }

    gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << OLED_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&reset_config);
    if (err == ESP_OK) {
        err = gpio_set_level(OLED_RESET_GPIO, 0);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = gpio_set_level(OLED_RESET_GPIO, 1);
    }
    if (err != ESP_OK) {
        ESP_LOGE(DISPLAY_TAG, "OLED reset setup failed: %s", esp_err_to_name(err));
        free(framebuffer);
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_config, &bus);
    if (err == ESP_OK) {
        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_I2C_ADDRESS,
            .scl_speed_hz = 400000,
        };
        err = i2c_master_bus_add_device(bus, &device_config, &device);
    }
    if (err == ESP_OK) {
        err = i2c_master_transmit(device, init_commands, sizeof(init_commands), 100);
    }
    if (err != ESP_OK) {
        ESP_LOGE(DISPLAY_TAG, "OLED initialization failed: %s; display disabled",
                 esp_err_to_name(err));
        destroy_i2c(bus, device);
        free(framebuffer);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        render_status(framebuffer, show_meshcore,
                      (uint32_t)(esp_timer_get_time() / 1000));
        err = write_frame(device, framebuffer);
        if (err != ESP_OK) {
            ESP_LOGW(DISPLAY_TAG, "OLED transfer failed: %s; display disabled",
                     esp_err_to_name(err));
            break;
        }
        if (++frame_count >= OLED_ROTATE_FRAMES) {
            frame_count = 0;
            show_meshcore = !show_meshcore;
        }
        vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_MS));
    }

    destroy_i2c(bus, device);
    free(framebuffer);
    vTaskDelete(NULL);
}

void feb_status_display_start(void)
{
    BaseType_t result = xTaskCreate(display_task, "feb_oled", 4096, NULL,
                                    tskIDLE_PRIORITY + 1, NULL);
    if (result != pdPASS) {
        ESP_LOGE(DISPLAY_TAG, "task creation failed; OLED disabled");
    }
}