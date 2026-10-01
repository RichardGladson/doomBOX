# doomBOX 💀📡

A compact ESP32 wireless toolkit, forked from [nyanBOX](https://github.com/jbohack/nyanBOX) by Nyan Devices.

> 🚧 **Work in progress** — doomBOX is under active development. Not every feature listed below is guaranteed to work as expected yet. Expect rough edges, half-finished tools, and the occasional crash. Bug reports and PRs welcome.

---

## 🔱 Two Hardware Variants: Wraith & Reaper

doomBOX now splits into two hardware builds:

| Variant | NRF24 modules | Repo |
|---|---|---|
| 👻 **doomBOX Wraith** | **0** — ESP32 onboard radio only | this repo |
| 💀 **doomBOX Reaper** | **3×** NRF24L01 | separate repo (coming soon) |

**This repository covers Wraith.** Wraith is the stripped-down, antenna-free, NRF24-less build — it runs entirely off the ESP32's built-in Wi-Fi/BLE radio. Reaper is the fuller-featured sibling with a triple-NRF24 setup and lives in its own repo.

> ⚠️ **Notice:** Because Wraith has **zero NRF24 modules**, any feature in this codebase that depends on NRF24 hardware **will not function** on Wraith. Right now that's **SigKill**, which drives the NRF24 directly for its jamming/carrier functions. It still appears in the "Other" menu but will fail to initialize (`radioReady = false`) since there's no NRF24 chip to talk to. SigKill — and any future NRF24-exclusive tools — belong on **Reaper**, not Wraith.

---

## 🧰 Features

### 📶 Wi-Fi
- 📡 Wi-Fi scanner with access-point and client detection
- 📊 Wi-Fi channel analyzer
- 🧨 Wi-Fi deauther
- 🔎 Deauthentication-frame scanner
- 🎏 Beacon spam
- 🪝 Evil Portal (captive portal)
- 🍍 Pineapple detector
- 🤖 Pwnagotchi detector
- 📢 Pwnagotchi spam
- 🤝 Handshake capture + viewer

### 🔵 Bluetooth / BLE
- 🛰️ BLE scanner
- 🔍 BLE advertising-packet inspector
- 👻 doomBOX detector (spots other doomBOX/nyanBOX units)
- 🐬 Flipper Zero detector
- ⚡ Axon detector
- 🕸️ Meshtastic detector
- 🕸️ MeshCore detector
- 💳 Bluetooth skimmer detector
- 🏷️ AirTag detector
- 🏷️ AirTag spoofer
- 🏷️ Samsung SmartTag detector
- 🔵 Tile detector
- 🕶️ Ray-Ban Meta detector
- 📣 BLE spammer
- 🪟 Windows Swift Pair spam
- 🍎 Sour Apple
- 🤖 Sour Droid
- 🎭 BLE spoofer

### 📡 RF / Wireless Analysis
- 📶 2.4 GHz channel scanner *(ESP32 radio — works on Wraith)*
- 📈 Signal analyzer *(ESP32 radio — works on Wraith)*
- 🚁 Drone detector (Remote ID)
- 🚁 Drone spoofer (ODID)
- 🚨 Flock Safety detector
- 🕵️ Device Scout
- ☠️ SigKill *(⚠️ **NRF24-only — non-functional on Wraith, requires Reaper**)*

### 🖥️ Interface
- 🖤 0.96" 128×64 OLED display
- 🧭 Menu-driven navigation across WiFi / BLE / Other categories
- 🔒 Optional boot-time password lock
- 🎮 Hidden level/XP system (press **right** on the main menu)
- 🌈 NeoPixel/RGB status LED support
- 📺 Display-mirroring output
- 🐍 Konami-code-unlocked Snake game, tucked inside the About screen

---

## ❌ What's Missing From the Original nyanBOX

doomBOX Wraith trims nyanBOX down to a zero-NRF24, antenna-free build. The following nyanBOX tools and hardware **did not make the cut**:

- 📷 Camera Detector (Ring/Blink/Nest/Arlo/etc. OUI fingerprinting)
- 📷 Camera Deauther
- 🕵️ Passive 802.11 Packet Monitor (promiscuous capture + CSV export)
- 🔑 FindMy Beeper (triggers the speaker on Apple FindMy accessories)
- 🚪 HID Beeper (targets Bluetooth-enabled access-control readers)
- 🚗 KARR Detector (anti-theft system vulnerability scanner)
- 🛰️ iBeacon Detector
- 🛰️ iBeacon Spoofer
- 👮 LE Gear Detector (law-enforcement equipment fingerprinting)
- 📡 Jam Detector (interference/baseline anomaly alerts)
- 🔋 2500mAh rechargeable battery + full-day runtime
- 📦 Protective hardware enclosure
- 🎛️ Triple-NRF24 module support *(this now lives on doomBOX Reaper instead)*
- 📶 External 2.4 GHz antenna

---

## ➕ What's New in doomBOX

Changes and additions specific to doomBOX that aren't part of stock nyanBOX:

- 🔱 **Wraith / Reaper split** — doomBOX is now two hardware variants instead of one: Wraith (0× NRF24, this repo) and Reaper (3× NRF24, separate repo)
- 👻 **Zero-NRF24 Wraith build** — runs entirely on the ESP32's onboard Wi-Fi/BLE radio, no NRF24 module needed at all
- 📻 **No external antenna required** — relies entirely on the ESP32's onboard radio
- 🪶 **Simplified, lighter build** — smaller footprint, fewer parts, easier to assemble
- 🖥️ **Standalone 0.96" OLED** — streamlined display integration
- 🐍 **Snake game easter egg** — Konami code (↑↑↓↓←→←→←→) on the About screen drops you straight into a playable round of Snake

---

## ⚠️ Disclaimer

doomBOX is intended for **educational purposes, security research, and authorized testing only**. Don't scan, deauth, spoof, or otherwise mess with networks or devices you don't own or don't have explicit permission to test. Know your local laws — some of these tools may be restricted or illegal to operate where you live. You are solely responsible for how you use this device; the authors and contributors accept no liability for misuse.
