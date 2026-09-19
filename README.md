# doomBOX

Compact ESP32 wireless toolkit based on nyanBOX, configured for a **single NRF24 module** and the ESP32's onboard 2.4 GHz radio.

## Features

### Wi-Fi
- Wi-Fi scanner with access-point and client detection
- Wi-Fi channel analyzer
- Camera detector
- Camera deauther
- Wi-Fi deauther
- Deauthentication-frame scanner
- 802.11 packet monitor
- Beacon spam
- Evil Portal
- Pineapple detector
- Pwnagotchi detector
- Pwnagotchi spam

### Bluetooth / BLE
- BLE scanner
- BLE advertising-packet inspector
- nyanBOX detector
- Flipper Zero detector
- Axon detector
- Meshtastic detector
- MeshCore detector
- Bluetooth skimmer detector
- AirTag detector
- AirTag spoofer
- Find My beeper
- Samsung SmartTag detector
- Tile detector
- KARR detector
- Ray-Ban Meta detector
- iBeacon detector
- iBeacon spoofer
- BLE spammer
- Windows Swift Pair
- Sour Apple
- Sour Droid
- BLE spoofer

### RF / Wireless Analysis
- 2.4 GHz channel scanner
- Real-time spectrum analyzer
- Jam detector
- Drone detector
- Drone spoofer
- Flock Safety detector
- LE Gear detector
- Device Scout

### Interface
- 0.96" 128×64 OLED interface
- Menu-driven navigation
- On-device tool selection and status display
- ESP32 Wi-Fi + Bluetooth/BLE radio
- NRF24-based 2.4 GHz functions

## Added

Changes specific to doomBOX that are not part of the original nyanBOX hardware configuration:

- **Single NRF24 support** — designed to operate with only **one NRF24 module** instead of nyanBOX's multi-NRF24 configuration.
- **No external 2.4 GHz antenna required** — doomBOX is built around the ESP32's onboard 2.4 GHz radio/antenna for its Wi-Fi and Bluetooth/BLE functions.
- **Simplified hardware build** — the project is intended for a smaller, easier-to-build setup using one NRF24 module and a standalone 0.96" OLED.

## Hardware

| Component | doomBOX configuration |
|---|---|
| Microcontroller | ESP32 WROOM / compatible ESP32 |
| NRF24 | **1× NRF24L01** |
| Display | **0.96" 128×64 OLED** |
| 2.4 GHz external antenna | **Not required** |
| Wireless radios | ESP32 Wi-Fi + Bluetooth/BLE, NRF24 |
