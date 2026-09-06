# ESP32-C6 BLE transport

This is the roadmap Step 2 transport slice for the ESP32-C6-DevKitC-1-N4. At boot it scans for the protocol v2 service, connects one peer, discovers the write and notify characteristics, enables notifications, and writes a fixed ASCII smoke-test payload. Notifications are copied and logged with a 64-byte bound.

The transport uses NimBLE central mode from ESP-IDF v5.5.2. It does not implement CBOR, pairing, encryption, or capabilities. Disconnects and failed connection attempts use exponential delays of 1, 2, 4, 8, and 16 seconds, with at most five automatic retries per connection cycle.

## Build

From an ESP-IDF v5.5.2 PowerShell environment:

```powershell
idf.py set-target esp32c6
idf.py build
```

The implementation expects the Flipper peripheral to advertise the v2 service UUID `9c3f7e6a-f403-4c31-9ea2-58a7a20fb811`, with write characteristic ending in `812` and notify characteristic ending in `813`.

The initial partition table assumes a 4 MB device because the board is marked N4. Confirm the physical flash size before relying on this layout.