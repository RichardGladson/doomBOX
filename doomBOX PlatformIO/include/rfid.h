/*
    doomBOX RFID Module (RC522 / MFRC522)
    Based on nyanBOX / doomBOX architecture
    Copyright (c) 2026

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#ifndef RFID_H
#define RFID_H

#include <Arduino.h>
#include <U8g2lib.h>
#include "pindefs.h"

// RC522 pins (shared SPI bus: SCK=18, MISO=19, MOSI=23)
// Choose free GPIOs that do not conflict with buttons / NRF24 / NeoPixel
#ifndef RFID_SS_PIN
#define RFID_SS_PIN   21
#endif
#ifndef RFID_RST_PIN
#define RFID_RST_PIN  22
#endif

void rfidSetup();
void rfidLoop();
void rfidCleanup();

#endif
