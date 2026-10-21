/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
// pindefs.h
#ifndef PINDEFS_H
#define PINDEFS_H

// Button Pin Definitions
#define BUTTON_PIN_UP      26
#define BUTTON_PIN_DOWN    33
#define BUTTON_PIN_CENTER  32  // Exit
#define BUTTON_PIN_LEFT    25  // Back
#define BUTTON_PIN_RIGHT   27  // Select

// NeoPixel
#define NEOPIXEL_PIN       14

// RC522 RFID (SPI bus: SCK=18, MISO=19, MOSI=23)
#define RFID_SS_PIN        21
#define RFID_RST_PIN       22

#endif // PINDEFS_H
