#ifndef FEB_STATUS_DISPLAY_H
#define FEB_STATUS_DISPLAY_H

typedef enum {
    FEB_DISPLAY_BLE_DISCONNECTED = 0,
    FEB_DISPLAY_BLE_SCANNING,
    FEB_DISPLAY_BLE_PAIRING_MODE,
    FEB_DISPLAY_BLE_CONNECTING,
    FEB_DISPLAY_BLE_PAIRING,
    FEB_DISPLAY_BLE_AUTHENTICATING,
    FEB_DISPLAY_BLE_AUTHENTICATED,
    FEB_DISPLAY_BLE_RADIO_OFF,
    /* HARDENING_PLAN.md HP-05: the kill-switch's Wi-Fi restart (esp_wifi_set_mode/
       esp_wifi_start) can fail after BLE has already come back up -- previously invisible on
       both the OLED and the wire. Distinct from FEB_DISPLAY_BLE_RADIO_OFF (which means "both
       radios intentionally off"): this means "BLE is up, Wi-Fi scanning/wardriving is not". */
    FEB_DISPLAY_BLE_WIFI_RESTART_FAILED
} feb_display_ble_state_t;

void feb_status_display_set_ble_state(feb_display_ble_state_t state);
void feb_status_display_start(void);

#endif