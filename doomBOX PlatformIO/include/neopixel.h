/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/
#ifndef NEOPIXEL_H
#define NEOPIXEL_H

#include <Adafruit_NeoPixel.h>

extern Adafruit_NeoPixel pixels;

void neopixelSetup();
void neopixelLoop();

void blinkColor(uint8_t r, uint8_t g, uint8_t b);
void stopBlinking();

#endif // NEOPIXEL_H
