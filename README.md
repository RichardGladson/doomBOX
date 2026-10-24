# doomBOX
<img width="1976" height="796" alt="doomBOX banner" src="doomBOX banner.png" />

A compact ESP32 wireless toolkit for security research and Wi-Fi, Bluetooth and RFID related work.  

Forked from [nyanBOX](https://github.com/jbohack/nyanBOX) by Nyan Devices, itself originally forked from [nRFbox](https://github.com/cifertech/nrfbox) by CiferTech.

**Repository:** [github.com/richardGladson/doomBOX](https://github.com/richardGladson/doomBOX)

---
## Acknowledgements & Licensing

doomBOX is a derivative project built upon [nyanBOX](https://github.com/jbohack/nyanBOX) by **jbohack**. The original project provided the foundation for doomBOX, and its original authorship and contributions are acknowledged and respected.

While doomBOX includes my own additions and modifications, it remains derived from nyanBOX. The original MIT licence and copyright notices are preserved in accordance with its terms.

My modifications are licensed under the **GNU General Public License (GPL)**. This does not replace or revoke the original MIT licence applicable to the upstream code. The applicable licence terms depend on which portions of the code are being used or redistributed.

Full credit for the original nyanBOX project belongs to its respective author(s).

---

## System overview

doomBOX is the **controller / UI node** of a two-device system:

| Role | Repo | Hardware focus |
|------|------|----------------|
| **doomBOX (this repo)** | [richardGladson/doomBOX](https://github.com/richardGladson/doomBOX) | ESP32 + OLED UI, Wi‑Fi/BLE tools, RFID |
| **ESP32-Reaper** | [RichardGladson/ESP32-reaper](https://github.com/RichardGladson/ESP32-reaper) | ESP32-C3 + **3× nRF24L01** field radio, passive 2.4 GHz survey, ESP‑NOW slave |

- **Standalone:** doomBOX runs all onboard Wi‑Fi / BLE / RFID tools without Reaper.
- **With Reaper:** open the **Reaper** shortcut on the main menu to discover, pair, and command the remote triple-radio node over ESP‑NOW. Passive channel survey (BT / Wi‑Fi ranges) is offloaded to Reaper; results and status come back to the OLED.

Full multi-radio spectrum work lives on Reaper. This firmware has no onboard nRF24 support.

---

## Hardware

| Component | Notes |
|-----------|--------|
| MCU | ESP32 (PlatformIO board: `esp32dev`) |
| Display | 0.96″ 128×64 OLED (SSD1306, I²C / U8g2) |
| Buttons | Up, Down, Center (exit), Left (back), Right |
| Status LED | NeoPixel / RGB (GPIO 14) |
| RFID (optional) | MFRC522 on SPI |

### Pin map (`pindefs.h`)

| Function | GPIO |
|----------|------|
| Button Up | 26 |
| Button Down | 33 |
| Button Center (SEL / exit) | 32 |
| Button Left (back) | 25 |
| Button Right | 27 |
| NeoPixel | 14 |
| RFID SS | 21 |
| RFID RST | 22 |
| SPI (RFID) | SCK 18, MISO 19, MOSI 23 |

---

## Features

### 📶 Wi‑Fi
- Access-point / client scanner
- Deauther
- Handshake capture
- Client sniffer
- Deauth-frame scanner
- Beacon spam
- Evil Portal (captive portal)
- Pineapple detector
- Pwnagotchi detector & spam

### 🔵 Bluetooth / BLE
- BLE scanner
- BLE advertising inspector
- doomBOX / nyanBOX detector
- Flipper Zero, Axon, Meshtastic, MeshCore detectors
- Bluetooth skimmer detector
- AirTag detector & spoofer
- Samsung SmartTag, Tile, Ray-Ban Meta detectors
- BLE spammer, Swift Pair spam, Sour Apple, Sour Droid, BLE spoofer

### 📡 Other / RF-related
- Superscanner
- Drone detector (Remote ID) & drone spoofer (ODID)
- Flock Safety detector
- Device Scout
- **Reaper link** — ESP‑NOW control of [ESP32-Reaper](https://github.com/RichardGladson/ESP32-reaper) for multi-radio passive survey

> Use **Reaper → BT Survey / WiFi Survey** when a Reaper node is paired for spectrum work.

### 🏷️ RFID
- MFRC522 tools (main menu, right column)

### 🖥️ Interface
- Dual-column main menu: **WiFi / BLE / Other** (left) · **RFID / Reaper** (right)
- Optional boot password
- Level / XP system (Right again on the right column)
- NeoPixel status
- Display mirroring
- Snake easter egg (Konami code on About: ↑↑↓↓←→←→←→)

---

## Main menu layout

```
Left column              Right column
─────────────            ─────────────
  WiFi                     RFID
  BLE                      Reaper   ← ESP-NOW to ESP32-Reaper
  Other
```

**Navigation:** Up/Down select · Right enter column or open Level · Left back · Center (SEL) launch / exit app.

### Reaper UI (when opened)
1. Searches for a Reaper node (ESP‑NOW discovery beacon).
2. On pair: shows MAC, mode, R1/R2/R3 health.
3. Actions (Up/Down, Right to run): **BT Survey** · **WiFi Survey** · **Stop** · **Refresh Status** · **Disconnect**.
4. Center exits to the main menu.

Protocol: versioned binary packets (magic `0xDB`), heartbeats, mode/scan/stop/status. See Reaper’s `protocol.h` and this repo’s `reaper_protocol` sources.

---

## Build

**Toolchain:** [PlatformIO](https://platformio.org/)

```bash
cd "doomBOX PlatformIO"   # or your project root containing platformio.ini
pio run
pio run -t upload
pio device monitor
```

**Environment:** `doomBOX` · board `esp32dev` · partition `huge_app.csv`

**Libraries** (from `platformio.ini`):
- `olikraus/U8g2`
- `adafruit/Adafruit NeoPixel`
- `bblanchon/ArduinoJson`
- `miguelbalboa/MFRC522`

---

## Project layout (high level)

```
doomBOX PlatformIO/
  platformio.ini
  include/          # headers (apps, pindefs, icons, reaper_*)
  src/              # main.cpp + one module per tool
```

Notable modules: `wifiscan`, `deauth`, `handshake_capture`, `evil_portal`, `blescan`, `ble_*`, detectors, `radio_manager`, `rfid`, `reaper_manager` / `reaper_ui` / `reaper_protocol`.

---

## Companion: ESP32-Reaper

For 3× nRF24 passive survey and Core-controlled channel stepping, use:

**[github.com/RichardGladson/ESP32-reaper](https://github.com/RichardGladson/ESP32-reaper)**

- MCU: ESP32-C3 Super Mini
- 3× nRF24L01 on shared SPI
- Standalone button + LED, or paired as ESP‑NOW slave to doomBOX
- No continuous carrier / jamming (listen-only survey)

Flash Reaper, power it near doomBOX, open **Reaper** on the main menu, and wait for **PAIRED**.

---

## Differences from stock nyanBOX

**Not carried over (examples):** camera detector/deauther, passive 802.11 CSV monitor, FindMy/HID beepers, KARR, iBeacon tools, LE gear detector, jam detector, battery pack / enclosure, triple-nRF24 on the same board.

**doomBOX-specific:**
- Focus on onboard Wi‑Fi/BLE + OLED (no onboard nRF24)
- RFID column
- **ESP‑NOW Reaper integration** for remote multi-radio survey
- Snake easter egg, level system, legal disclaimer flow

---

## Disclaimer

doomBOX is for **education, security research, and authorized testing only**. Do not scan, deauth, spoof, or interfere with networks or devices you do not own or lack explicit permission to test. Follow local radio and computer-crime laws. You are solely responsible for how you use this software and hardware; authors and contributors accept no liability for misuse.

---

## License

GNU GPL-3.0 License — see [LICENSE](LICENSE).

This project is a derivative of nyanBOX (MIT). Changing the license to GPL from the original MIT ensures that all derivative firmware is distrubuted and used properly. Original copyright notices in source headers must be retained.
