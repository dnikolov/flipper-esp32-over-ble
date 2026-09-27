#ifndef FEB_STATUS_DISPLAY_H
#define FEB_STATUS_DISPLAY_H

typedef enum {
    FEB_DISPLAY_BLE_DISCONNECTED = 0,
    FEB_DISPLAY_BLE_SCANNING,
    FEB_DISPLAY_BLE_PAIRING_MODE,
    FEB_DISPLAY_BLE_CONNECTING,
    FEB_DISPLAY_BLE_PAIRING,
    FEB_DISPLAY_BLE_AUTHENTICATING,
    FEB_DISPLAY_BLE_AUTHENTICATED
} feb_display_ble_state_t;

void feb_status_display_set_ble_state(feb_display_ble_state_t state);
void feb_status_display_start(void);

#endif