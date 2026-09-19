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

  // Title - fits fully on screen
  u8g2.setFont(u8g2_font_helvB14_tr);
  const char* title = "doomBOX";
  int16_t titleW = u8g2.getUTF8Width(title);
  u8g2.setCursor((128 - titleW) / 2, 14);
  u8g2.print(title);

  // All text below title uses the same tiny font
  u8g2.setFont(u8g2_font_5x8_tr);

  const char* url1 = "github.com/";
  int16_t url1W = u8g2.getUTF8Width(url1);
  u8g2.setCursor((128 - url1W) / 2, 26);
  u8g2.print(url1);

  const char* url2 = "richardgladson/doomBOX";
  int16_t url2W = u8g2.getUTF8Width(url2);
  u8g2.setCursor((128 - url2W) / 2, 34);
  u8g2.print(url2);

  const char* credit1 = "made by richard gladson";
  int16_t c1W = u8g2.getUTF8Width(credit1);
  u8g2.setCursor((128 - c1W) / 2, 44);
  u8g2.print(credit1);

  const char* credit2 = "inspired by nyanBOX";
  int16_t c2W = u8g2.getUTF8Width(credit2);
  u8g2.setCursor((128 - c2W) / 2, 52);
  u8g2.print(credit2);

  const char* tag1 = "wireless penetration";
  int16_t t1W = u8g2.getUTF8Width(tag1);
  u8g2.setCursor((128 - t1W) / 2, 58);
  u8g2.print(tag1);

  const char* tag2 = "testing tool";
  int16_t t2W = u8g2.getUTF8Width(tag2);
  u8g2.setCursor((128 - t2W) / 2, 63);
  u8g2.print(tag2);

  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

void aboutCleanup() {
  if (snakeMode) {
    snakeCleanup();
  }
}
