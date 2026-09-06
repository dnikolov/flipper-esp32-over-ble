# flipper-esp32-over-ble

This project is a minimal framework for pairing a Flipper Zero with an ESP32 board over BLE, then exposing board-specific capabilities through a capability registry.

## Goal

- Pair a Flipper app to an ESP32 over BLE
- Pair in a trusted environment using an ephemeral BLE key exchange
- Use AES-128 + CBOR for BLE communication
- Expose different capabilities depending on the connected board model and attached hardware

## Pairing model

1. User chooses Add ESP32 board in the Flipper app.
2. The board enters a short, locally authorized BLE pairing window.
3. The app and board exchange ephemeral public keys and derive shared pairing material over BLE.
4. The app and board confirm the pairing transcript before storing the pairing material.
5. The pairing material is stored securely on both ends.
6. The app continues communication over BLE using encrypted CBOR messages.

## Capability model

Each connected board advertises a capability set such as:

- Wi-Fi scan
- GPS read
- GPIO access
- BLE relay
- sensor readout
- custom extension support

The capability list is dynamic and depends on the board model and attached peripherals.

## Security model

- BLE pairing authenticates and establishes the initial pairing material.
- BLE traffic uses AES-128 with CBOR payloads.
- The protocol includes sequence numbers and integrity checks.
- The BLE session remains authenticated using the negotiated shared secret.

## Project layout

- esp32/: ESP32 firmware skeleton for device-side BLE + capability registry
- flipper/: Flipper app skeleton for pairing, connectivity, and capability UI
- docs/: design docs for pairing, protocol, and capability negotiation

## Implementation roadmap

The ordered implementation phases and acceptance criteria are in [docs/PLAN.md](docs/PLAN.md).
