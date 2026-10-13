/*
    doomBOX
    https://github.com/richardgladson/doomBOX
    Copyright (c) 2025 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#include <EEPROM.h>
#include <U8g2lib.h>
#include "../include/level_system.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "../include/pindefs.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// Efficient 4-byte uptime store (seconds) + 1-byte magic
// Reuses the old level EEPROM region (100-104)
#define EEPROM_ADDRESS_UPTIME_MAGIC 100
#define EEPROM_ADDRESS_UPTIME_0     101  // little-endian uint32_t
#define EEPROM_ADDRESS_UPTIME_1     102
#define EEPROM_ADDRESS_UPTIME_2     103
#define EEPROM_ADDRESS_UPTIME_3     104
#define UPTIME_MAGIC_NUMBER         0xAC

// Persist to EEPROM at most once per minute to limit wear
#define UPTIME_SAVE_INTERVAL_MS     60000UL

// baseSeconds = value loaded from / last written to EEPROM
// sessionOriginMs = millis() at the moment baseSeconds was set
// Live total = baseSeconds + (millis() - sessionOriginMs) / 1000
static uint32_t baseSeconds = 0;
static unsigned long sessionOriginMs = 0;
static unsigned long lastSaveMs = 0;
static bool uptimeInitialized = false;
static bool needsRedraw = true;

static void saveUptimeData(uint32_t seconds) {
  EEPROM.write(EEPROM_ADDRESS_UPTIME_MAGIC, UPTIME_MAGIC_NUMBER);
  EEPROM.write(EEPROM_ADDRESS_UPTIME_0, (seconds) & 0xFF);
  EEPROM.write(EEPROM_ADDRESS_UPTIME_1, (seconds >> 8) & 0xFF);
  EEPROM.write(EEPROM_ADDRESS_UPTIME_2, (seconds >> 16) & 0xFF);
  EEPROM.write(EEPROM_ADDRESS_UPTIME_3, (seconds >> 24) & 0xFF);
  EEPROM.commit();
}

static void loadUptimeData() {
  uint8_t magic = EEPROM.read(EEPROM_ADDRESS_UPTIME_MAGIC);
  if (magic != UPTIME_MAGIC_NUMBER) {
    baseSeconds = 0;
    saveUptimeData(0);
    return;
  }
  baseSeconds =
      ((uint32_t)EEPROM.read(EEPROM_ADDRESS_UPTIME_0)) |
      ((uint32_t)EEPROM.read(EEPROM_ADDRESS_UPTIME_1) << 8) |
      ((uint32_t)EEPROM.read(EEPROM_ADDRESS_UPTIME_2) << 16) |
      ((uint32_t)EEPROM.read(EEPROM_ADDRESS_UPTIME_3) << 24);
}

uint32_t getTotalUptimeSeconds() {
  if (!uptimeInitialized) return baseSeconds;
  unsigned long elapsedMs = millis() - sessionOriginMs;
  return baseSeconds + (elapsedMs / 1000UL);
}

void updateUptimeTracking() {
  if (!uptimeInitialized) return;

  unsigned long now = millis();
  if (now - lastSaveMs < UPTIME_SAVE_INTERVAL_MS) return;

  // Fold live elapsed into base and realign the origin so we never
  // double-count, then persist the absolute total.
  unsigned long elapsedMs = now - sessionOriginMs;
  uint32_t addSec = elapsedMs / 1000UL;
  if (addSec > 0) {
    baseSeconds += addSec;
    sessionOriginMs = now - (elapsedMs % 1000UL);
    saveUptimeData(baseSeconds);
  }
  lastSaveMs = now;
  needsRedraw = true;
}

void levelSystemSetup() {
  // First call (boot): load EEPROM and start the session clock.
  // Later calls (opening the Uptime screen): only request a redraw —
  // do NOT reset the session clock or the total would appear to restart.
  if (!uptimeInitialized) {
    EEPROM.begin(512);
    loadUptimeData();
    sessionOriginMs = millis();
    lastSaveMs = millis();
    uptimeInitialized = true;
  }
  needsRedraw = true;
}

void displayLevelScreen() {
  u8g2.clearBuffer();

  uint32_t secs = getTotalUptimeSeconds();
  uint32_t days = secs / 86400UL;
  uint32_t hours = (secs % 86400UL) / 3600UL;
  uint32_t mins = (secs % 3600UL) / 60UL;
  uint32_t s = secs % 60UL;

  u8g2.setFont(u8g2_font_helvB14_tr);
  const char* title = "Uptime";
  int tw = u8g2.getUTF8Width(title);
  u8g2.drawStr((128 - tw) / 2, 18, title);

  u8g2.setFont(u8g2_font_helvR08_tr);
  char line1[32];
  if (days > 0) {
    snprintf(line1, sizeof(line1), "%lu d  %02lu:%02lu:%02lu",
             (unsigned long)days, (unsigned long)hours,
             (unsigned long)mins, (unsigned long)s);
  } else {
    snprintf(line1, sizeof(line1), "%02lu:%02lu:%02lu",
             (unsigned long)hours, (unsigned long)mins, (unsigned long)s);
  }
  int w1 = u8g2.getUTF8Width(line1);
  u8g2.drawStr((128 - w1) / 2, 36, line1);

  u8g2.setFont(u8g2_font_6x10_tr);
  char line2[28];
  snprintf(line2, sizeof(line2), "Total: %lu sec", (unsigned long)secs);
  int w2 = u8g2.getUTF8Width(line2);
  u8g2.drawStr((128 - w2) / 2, 50, line2);

  u8g2.drawStr(0, 64, "<- Back");

  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

void levelSystemLoop() {
  updateUptimeTracking();
  if (needsRedraw) {
    displayLevelScreen();
    needsRedraw = false;
  } else {
    static unsigned long lastTick = 0;
    if (millis() - lastTick >= 1000) {
      lastTick = millis();
      displayLevelScreen();
    }
  }
}

// --- stubs (leveling removed) ---
void addXP(int amount) { (void)amount; }
int getCurrentLevel() { return 0; }
int getCurrentXP() { return 0; }
int getXPForNextLevel() { return 0; }
void resetXPData() {}
void setMinimumLevel(int level) { (void)level; }
