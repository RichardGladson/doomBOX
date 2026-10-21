/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef RFID_H
#define RFID_H

#include <Arduino.h>
#include <U8g2lib.h>
#include "pindefs.h"

// RC522 pins (SPI bus: SCK=18, MISO=19, MOSI=23)
// Choose free GPIOs that do not conflict with buttons / NeoPixel
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
