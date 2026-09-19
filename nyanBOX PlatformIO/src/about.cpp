/*
    nyanBOX by Nyan Devices
    https://github.com/jbohack/nyanBOX
    Copyright (c) 2025 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#include <Arduino.h>
#include "about.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "snake.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
const char* nyanboxVersion = NYANBOX_VERSION;

#define KONAMI_LENGTH 10
static const uint8_t konamiSequence[KONAMI_LENGTH] = {
  BUTTON_PIN_UP,
  BUTTON_PIN_UP,
  BUTTON_PIN_DOWN,
  BUTTON_PIN_DOWN,
  BUTTON_PIN_LEFT,
  BUTTON_PIN_RIGHT,
  BUTTON_PIN_LEFT,
  BUTTON_PIN_RIGHT,
  BUTTON_PIN_LEFT,
  BUTTON_PIN_RIGHT
};

static uint8_t konamiIndex = 0;
static bool    snakeMode   = false;

static bool needsRedraw = true;

void aboutSetup() {
  pinMode(BUTTON_PIN_UP,    INPUT_PULLUP);
  pinMode(BUTTON_PIN_DOWN,  INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT,  INPUT_PULLUP);
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);

  snakeSetup();
  snakeMode = false;
  needsRedraw = true;
}

void aboutLoop() {
  const uint8_t arrows[] = {
    BUTTON_PIN_UP,
    BUTTON_PIN_DOWN,
    BUTTON_PIN_LEFT,
    BUTTON_PIN_RIGHT
  };
  for (auto pin : arrows) {
    if (digitalRead(pin) == LOW) {
      if (pin == konamiSequence[konamiIndex]) {
        konamiIndex++;
        if (konamiIndex == KONAMI_LENGTH) {
          snakeMode   = true;
          konamiIndex = 0;
        }
      } else {
        konamiIndex = (pin == konamiSequence[0]) ? 1 : 0;
      }
      delay(150);
      break;
    }
  }

  if (snakeMode) {
    snakeLoop();
    return;
  }

  if (!needsRedraw) {
    return;
  }

  needsRedraw = false;
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_helvB14_tr);
  const char* title = "doomBOX";
  int16_t titleW = u8g2.getUTF8Width(title);
  u8g2.setCursor((128 - titleW) / 2, 14);
  u8g2.print(title);

  u8g2.setFont(u8g2_font_5x8_tr);
  const char* url = "github.com/richardgladson/doomBOX";
  int16_t urlW = u8g2.getUTF8Width(url);
  u8g2.setCursor((128 - urlW) / 2, 28);
  u8g2.print(url);

  u8g2.setFont(u8g2_font_helvR08_tr);
  const char* credit1 = "made by Richard Gladson";
  int16_t c1W = u8g2.getUTF8Width(credit1);
  u8g2.setCursor((128 - c1W) / 2, 42);
  u8g2.print(credit1);

  const char* credit2 = "inspired by nyanBOX";
  int16_t c2W = u8g2.getUTF8Width(credit2);
  u8g2.setCursor((128 - c2W) / 2, 52);
  u8g2.print(credit2);

  u8g2.setFont(u8g2_font_5x8_tr);
  const char* tagline = "wireless pentesting device";
  int16_t tagW = u8g2.getUTF8Width(tagline);
  u8g2.setCursor((128 - tagW) / 2, 62);
  u8g2.print(tagline);

  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

void aboutCleanup() {
  if (snakeMode) {
    snakeCleanup();
  }
}
