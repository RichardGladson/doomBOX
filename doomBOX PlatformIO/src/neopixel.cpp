/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/

#include "../include/neopixel.h"
#include "../include/setting.h"
#include <EEPROM.h>
#include "../include/pindefs.h"

extern Adafruit_NeoPixel pixels;

void neopixelSetup() {
  EEPROM.begin(512); 
  neoPixelActive = EEPROM.read(0);
  
 if (neoPixelActive) {
  pixels.begin();
  pixels.setBrightness(8);
  pixels.clear();
  pixels.show();
  }
}

static bool isBlinking = false;
static uint8_t baseRed = 0;
static uint8_t baseGreen = 0;
static uint8_t baseBlue = 0;
static unsigned long lastBlinkTime = 0;
static bool blinkState = false;
const int blinkSpeed = 800;

void blinkColor(uint8_t r, uint8_t g, uint8_t b) {
  if (!neoPixelActive) return;

  isBlinking = true;
  baseRed = r;
  baseGreen = g;
  baseBlue = b;
  blinkState = true;
  lastBlinkTime = millis();
}

void stopBlinking() {
  isBlinking = false;
  if (neoPixelActive) {
    pixels.setPixelColor(0, 0, 0, 0);
    pixels.show();
  }
}

void neopixelLoop() {
  if (!neoPixelActive || !isBlinking) return;

  unsigned long now = millis();
  if (now - lastBlinkTime >= blinkSpeed) {
    lastBlinkTime = now;
    blinkState = !blinkState;

    if (blinkState) {
      pixels.setPixelColor(0, pixels.Color(baseRed, baseGreen, baseBlue));
    } else {
      pixels.setPixelColor(0, 0, 0, 0);
    }

    pixels.show();
  }
}