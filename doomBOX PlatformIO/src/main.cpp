/*
    doomBOX

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <stdint.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <esp_wifi.h>
#include "esp_bt_main.h"
#include <math.h>

#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

#include "../include/icon.h"
#include "../include/doombox_logo.h"
#include "../include/neopixel.h"
#include "../include/setting.h"

#include "../include/sourapple.h"
#include "../include/reaper_ui.h"
#include "../include/sourdroid.h"
#include "../include/blescan.h"
#include "../include/ble_inspector.h"
#include "../include/ble_spammer.h"
#include "../include/ble_spoofer.h"
#include "../include/swiftpair.h"
#include "../include/flipperzero_detector.h"
#include "../include/meshtastic_detector.h"
#include "../include/meshcore_detector.h"
#include "../include/airtag_detector.h"
#include "../include/airtag_spoofer.h"
#include "../include/tile_detector.h"
#include "../include/smarttag_detector.h"
#include "../include/rayban_detector.h"
#include "../include/wifiscan.h"
#include "../include/client_sniffer.h"
#include "../include/deauth.h"
#include "../include/deauth_scanner.h"
#include "../include/handshake_capture.h"
#include "../include/beacon_spam.h"
#include "../include/pwnagotchi_detector.h"
#include "../include/pindefs.h"
#include "../include/about.h"
#include "../include/pwnagotchi_spam.h"
#include "../include/level_system.h"
#include "../include/nyanbox_detector.h"
#include "../include/nyanbox_advertiser.h"
#include "../include/evil_portal.h"
#include "../include/legal_disclaimer.h"
#include "../include/cardskimmer_detector.h"
#include "../include/axon_detector.h"
#include "../include/drone_detector.h"
#include "../include/drone_spoofer.h"
#include "../include/flock_detector.h"
#include "../include/device_scout.h"
#include "../include/pineapple_detector.h"
#include "../include/display_mirror.h"
#include "../include/password.h"
#include "../include/radio_manager.h"
#include "../include/superscanner.h"
#include "../include/rfid.h"

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Adafruit_NeoPixel pixels(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
extern uint8_t oledBrightness;

struct MenuItem {
  const char* name;
  const unsigned char* icon;
  void (*setup)();
  void (*loop)();
  void (*cleanup)();
};

bool dangerousActionsEnabled = true;  // permanently enabled

const char* nyanboxVersion = NYANBOX_VERSION;
static unsigned long lastActivity = 0;
const unsigned long MAX_XP_IDLE_TIME = 120000;

unsigned long upLastMillis    = 0;
unsigned long upNextRepeat    = 0;
bool         upPressed        = false;
unsigned long upDebounceTime  = 0;

unsigned long downLastMillis  = 0;
unsigned long downNextRepeat  = 0;
bool         downPressed      = false;
unsigned long downDebounceTime = 0;

bool selPrev = false;
bool rightPrev = false;
bool leftPrev = false;
unsigned long selDebounceTime = 0;
unsigned long rightDebounceTime = 0;
unsigned long leftDebounceTime = 0;

const unsigned long initialDelay   = 500;
const unsigned long repeatInterval = 250;
const unsigned long debounceDelay  = 200;

static bool needsRedraw = true;
static bool advertiserBootPending = true;

void updateLastActivity() {
  lastActivity = millis();
}

// Sleep feature removed – stubs kept so existing includes still compile
void updateSleepTimeout(unsigned long newTimeout) {
  (void)newTimeout;
}

bool anyButtonPressed() {
  return digitalRead(BUTTON_PIN_UP)    == LOW ||
        digitalRead(BUTTON_PIN_DOWN)  == LOW ||
        digitalRead(BUTTON_PIN_CENTER)== LOW ||
        digitalRead(BUTTON_PIN_RIGHT) == LOW ||
        digitalRead(BUTTON_PIN_LEFT)  == LOW;
}

void wakeDisplay() {
  // no-op (sleep removed)
}

void checkIdle() {
  // no-op (sleep removed)
}

const int ITEM_HEIGHT = 16;
const int ITEM_SPACING = 2;
const int TEXT_X = 28;
const int SELECTION_X = 4;
const int SELECTION_WIDTH = 120;
const int ICON_X = 8;

void drawSelection(int x, int y, int width, int height, bool selected) {
  if (selected) {
    u8g2.drawBox(x, y+2, 2, height-4);
  }
}

enum AppMenuState { APP_MAIN, APP_BLE, APP_WIFI, APP_OTHER, APP_LEVEL, APP_OPTIONS };

// ========== Premium Options Menu (all logic lives in main.cpp) ==========
#define EEPROM_ADDR_AIRPLANE   20
#define EEPROM_ADDR_TROLL      21
#define EEPROM_ADDR_BRIGHTNESS 1
#define OPTIONS_HOLD_MS        1000
#define TROLL_DISABLE_HOLD_MS  4000
#define BRIGHTNESS_MIN_PCT     5   // never fully black
#define BRIGHTNESS_MAX_PCT     100

bool airplaneModeEnabled = false;
bool trollModeEnabled    = false;
bool optionsMenuActive   = false;
int  optionsSelected     = 0;   // 0-5 (2 blank)
int  optionsSubMenu      = 0;   // 0=grid, 1=brightness
int  brightnessPercent   = 50;
unsigned long optionsOpenAnimStart = 0;
const unsigned long OPTIONS_ANIM_MS = 320;

// Long-press tracking for entering options (UP) / exiting (DOWN)
unsigned long upHoldStart   = 0;
bool          upHolding     = false;
unsigned long downHoldStart = 0;
bool          downHolding   = false;
unsigned long selHoldStart  = 0;
bool          selHolding    = false;

// 16x16 trollface (classic meme outline) – for troll mode icon
// Row-major, LSB left, 2 bytes per row
static const unsigned char bitmap_trollface[] PROGMEM = {
  0xE0, 0x07, // ....###.......
  0xF8, 0x1F, // ..#####.......
  0xFC, 0x3F, // .#######......
  0x1E, 0x78, // ####..####....
  0x0F, 0xF0, // ####....####..
  0x67, 0xE6, // ##..##..##..##
  0x67, 0xE6, // ##..##..##..##
  0x0F, 0xF0, // ####....####..
  0x1E, 0x78, // ####..####....
  0xFC, 0x3F, // .#######......
  0xF8, 0x1F, // ..#####.......
  0xE0, 0x07, // ....###.......
  0xC0, 0x03, // .....##.......
  0x80, 0x01, // ......#.......
  0x00, 0x00,
  0x00, 0x00
};

// Draw one option circle:
//  - selected: thick outer ring
//  - enabled (toggle on): filled disc + inverted glyph
//  - off / neutral: outline + normal glyph
//  - blank: faint outline only
static void drawOptionCircle(int cx, int cy, int r, bool selected, bool enabled, bool blank) {
  if (blank) {
    u8g2.drawCircle(cx, cy, r, U8G2_DRAW_ALL);
    return;
  }
  if (selected) {
    u8g2.drawCircle(cx, cy, r + 3, U8G2_DRAW_ALL);
    u8g2.drawCircle(cx, cy, r + 2, U8G2_DRAW_ALL);
  }
  if (enabled) {
    u8g2.drawDisc(cx, cy, r);
  } else {
    u8g2.drawCircle(cx, cy, r, U8G2_DRAW_ALL);
    u8g2.drawCircle(cx, cy, r - 1, U8G2_DRAW_ALL);
  }
}

static void drawOptionsIconGlyph(int idx, int cx, int cy, bool inverted) {
  u8g2.setDrawColor(inverted ? 0 : 1);
  switch (idx) {
    case 0: { // Brightness – sun
      u8g2.drawDisc(cx, cy, 3);
      for (int a = 0; a < 8; a++) {
        float ang = a * 0.785398f;
        int x1 = cx + (int)(5.0f * cos(ang));
        int y1 = cy + (int)(5.0f * sin(ang));
        int x2 = cx + (int)(8.0f * cos(ang));
        int y2 = cy + (int)(8.0f * sin(ang));
        u8g2.drawLine(x1, y1, x2, y2);
      }
      break;
    }
    case 1: // Password – lock
      u8g2.drawFrame(cx - 4, cy - 1, 8, 7);
      u8g2.drawBox(cx - 1, cy + 1, 2, 3);
      u8g2.drawCircle(cx, cy - 3, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case 2: // Airplane
      u8g2.drawLine(cx - 7, cy, cx + 7, cy);
      u8g2.drawLine(cx - 2, cy - 5, cx + 5, cy);
      u8g2.drawLine(cx - 2, cy + 5, cx + 5, cy);
      u8g2.drawLine(cx + 5, cy - 3, cx + 7, cy);
      u8g2.drawLine(cx + 5, cy + 3, cx + 7, cy);
      u8g2.drawLine(cx - 5, cy - 2, cx - 2, cy);
      u8g2.drawLine(cx - 5, cy + 2, cx - 2, cy);
      break;
    case 3: { // Trollface – wide grin, small eyes, chin (classic meme silhouette)
      // Head outline
      u8g2.drawCircle(cx, cy - 1, 7, U8G2_DRAW_ALL);
      // Eyes (small, close)
      u8g2.drawPixel(cx - 3, cy - 3);
      u8g2.drawPixel(cx - 2, cy - 3);
      u8g2.drawPixel(cx + 2, cy - 3);
      u8g2.drawPixel(cx + 3, cy - 3);
      // Eyebrow arches
      u8g2.drawLine(cx - 5, cy - 5, cx - 2, cy - 4);
      u8g2.drawLine(cx + 2, cy - 4, cx + 5, cy - 5);
      // Huge grin
      u8g2.drawLine(cx - 5, cy + 1, cx - 3, cy + 3);
      u8g2.drawLine(cx - 3, cy + 3, cx + 3, cy + 3);
      u8g2.drawLine(cx + 3, cy + 3, cx + 5, cy + 1);
      u8g2.drawLine(cx - 4, cy + 2, cx + 4, cy + 2);
      // Chin point
      u8g2.drawLine(cx - 2, cy + 5, cx, cy + 7);
      u8g2.drawLine(cx, cy + 7, cx + 2, cy + 5);
      break;
    }
    default:
      break;
  }
  u8g2.setDrawColor(1);
}

static void loadOptionsFromEEPROM() {
  uint8_t air = EEPROM.read(EEPROM_ADDR_AIRPLANE);
  uint8_t tr  = EEPROM.read(EEPROM_ADDR_TROLL);
  airplaneModeEnabled = (air == 1);
  trollModeEnabled    = (tr == 1);

  uint8_t b = EEPROM.read(EEPROM_ADDR_BRIGHTNESS);
  if (b == 0xFF) b = 128;
  oledBrightness = b;
  brightnessPercent = (int)map(oledBrightness, 0, 255, 0, 100);
  if (brightnessPercent < BRIGHTNESS_MIN_PCT) brightnessPercent = BRIGHTNESS_MIN_PCT;
  u8g2.setContrast(oledBrightness);
}

static void saveBrightness() {
  oledBrightness = (uint8_t)map(brightnessPercent, 0, 100, 0, 255);
  if (oledBrightness < 13) oledBrightness = 13; // ~5%
  u8g2.setContrast(oledBrightness);
  EEPROM.write(EEPROM_ADDR_BRIGHTNESS, oledBrightness);
  EEPROM.commit();
}

static void setAirplaneMode(bool on) {
  airplaneModeEnabled = on;
  EEPROM.write(EEPROM_ADDR_AIRPLANE, on ? 1 : 0);
  EEPROM.commit();
}

static void setTrollMode(bool on) {
  trollModeEnabled = on;
  EEPROM.write(EEPROM_ADDR_TROLL, on ? 1 : 0);
  EEPROM.commit();
}

static void setPasswordEnabled(bool on) {
  if (on) {
    // Fixed sequence: UP DOWN UP DOWN  (1=UP, 2=DOWN)  stored at EEPROM 6..
    EEPROM.write(6, 1);
    EEPROM.write(7, 2);
    EEPROM.write(8, 1);
    EEPROM.write(9, 2);
    // Clear remaining password slots (10-13) – password block only
    for (int i = 4; i < 8; i++) EEPROM.write(6 + i, 0);
  } else {
    clearPassword();
  }
  EEPROM.commit();
}

static void applyTrollOverlay() {
  if (!trollModeEnabled) return;
  // Random glitchy artifacts – still fully controllable
  for (int i = 0; i < 12; i++) {
    int x = random(0, 128);
    int y = random(0, 64);
    int w = random(1, 8);
    int h = random(1, 4);
    if (random(0, 3) == 0) u8g2.drawBox(x, y, w, h);
    else if (random(0, 2) == 0) u8g2.drawLine(x, y, x + random(-10, 10), y + random(-6, 6));
    else u8g2.drawPixel(x, y);
  }
  // Occasional horizontal tear
  if (random(0, 8) == 0) {
    int y = random(0, 64);
    u8g2.drawHLine(0, y, 128);
  }
}

static void drawHoldProgressBar(unsigned long heldMs, unsigned long targetMs) {
  int pct = (int)((heldMs * 100) / targetMs);
  if (pct > 100) pct = 100;
  u8g2.drawFrame(14, 58, 100, 5);
  int fill = (pct * 98) / 100;
  if (fill > 0) u8g2.drawBox(15, 59, fill, 3);
}

// Returns whether option index is currently "on" (for toggle visuals)
static bool optionIsEnabled(int idx) {
  switch (idx) {
    case 1: return passwordEnabled();
    case 2: return airplaneModeEnabled;
    case 3: return trollModeEnabled;
    default: return false; // brightness is not a toggle
  }
}

static void drawOptionsMenu(unsigned long animProgress) {
  float t = (float)animProgress / (float)OPTIONS_ANIM_MS;
  if (t > 1.0f) t = 1.0f;
  float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);

  u8g2.clearBuffer();

  // GUI-only 2x3 grid – no text labels
  // Tight layout that fits 128x64 cleanly
  const int positions[6][2] = {
    {24, 18}, {64, 18}, {104, 18},
    {24, 46}, {64, 46}, {104, 46}
  };
  const bool selectable[6] = {true, true, true, true, false, false};

  int scaleR = (int)(12 * e);
  if (scaleR < 2) scaleR = 2;

  for (int i = 0; i < 6; i++) {
    int cx = positions[i][0];
    int cy = positions[i][1] + (int)((1.0f - e) * 18);

    bool sel = (optionsSelected == i);
    bool blank = !selectable[i];
    bool enabled = !blank && optionIsEnabled(i);

    // Brightness is never "enabled" fill – only selected ring
    if (i == 0) enabled = false;

    drawOptionCircle(cx, cy, scaleR, sel && !blank, enabled, blank);

    if (!blank && e > 0.55f) {
      // Invert glyph when circle is filled (toggle ON)
      drawOptionsIconGlyph(i, cx, cy, enabled);
    }

    // For brightness when selected: small inner accent ring so selection is obvious
    if (i == 0 && sel && e > 0.7f) {
      u8g2.drawCircle(cx, cy, scaleR - 3, U8G2_DRAW_ALL);
    }
  }

  applyTrollOverlay();
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawBrightnessSubMenu() {
  u8g2.clearBuffer();

  // GUI-only brightness: big sun + slider + percent as graphic blocks
  // Sun icon top-center
  int cx = 64, cy = 16;
  u8g2.drawDisc(cx, cy, 5);
  for (int a = 0; a < 8; a++) {
    float ang = a * 0.785398f;
    int x1 = cx + (int)(8.0f * cos(ang));
    int y1 = cy + (int)(8.0f * sin(ang));
    int x2 = cx + (int)(12.0f * cos(ang));
    int y2 = cy + (int)(12.0f * sin(ang));
    u8g2.drawLine(x1, y1, x2, y2);
  }

  // Percent as filled blocks (no text dependency for identity – still show digits for value)
  char buf[8];
  sprintf(buf, "%d", brightnessPercent);
  u8g2.setFont(u8g2_font_helvB12_tr);
  int bw = u8g2.getUTF8Width(buf);
  u8g2.setCursor((128 - bw) / 2, 42);
  u8g2.print(buf);

  // Wide slider
  u8g2.drawFrame(10, 50, 108, 10);
  int fill = map(brightnessPercent, BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT, 0, 106);
  if (fill < 0) fill = 0;
  if (fill > 106) fill = 106;
  u8g2.drawBox(11, 51, fill, 8);

  applyTrollOverlay();
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

// XP: every second spent in any function awards ~3 XP
static const int XP_PER_SECOND = 3;

bool isReconApp(const char* appName) {
  return strstr(appName, "Scan") != nullptr || 
         strstr(appName, "Detector") != nullptr ||
         strstr(appName, "Scout") != nullptr ||
         strstr(appName, "Analyzer") != nullptr ||
         strstr(appName, "Inspect") != nullptr;
}

bool isDangerousApp(const char* appName) {
  return false;
}

bool isOffensiveApp(const char* appName) {
  if (isDangerousApp(appName)) {
    return true;
  }

  return strstr(appName, "Deauth") != nullptr ||
         strstr(appName, "Spam") != nullptr ||
         strstr(appName, "Swift Pair") != nullptr ||
         strstr(appName, "Sour Apple") != nullptr ||
         strstr(appName, "Sour Droid") != nullptr ||
         strstr(appName, "Spoofer") != nullptr ||
         strstr(appName, "Evil Portal") != nullptr;
}

bool isUtilityApp(const char* appName) {
  return strstr(appName, "Setting") != nullptr ||
         strstr(appName, "About") != nullptr;
}

AppMenuState currentState = APP_MAIN;
MenuItem*    currentMenuItems = nullptr;
int          currentMenuSize  = 0;
int          item_selected    = 0;
int          mainMenuColumn   = 0;  // 0 = categories (WiFi/BLE/Other), 1 = shortcuts (RFID)

constexpr uint8_t BUTTON_UP    = BUTTON_PIN_UP;
constexpr uint8_t BUTTON_SEL   = BUTTON_PIN_CENTER;
constexpr uint8_t BUTTON_DOWN  = BUTTON_PIN_DOWN;
constexpr uint8_t BUTTON_RIGHT = BUTTON_PIN_RIGHT;
constexpr uint8_t BUTTON_LEFT  = BUTTON_PIN_LEFT;

static unsigned long appStartTime = 0;
static unsigned long lastXPReward = 0;
static const char* currentAppName = "";
static bool inApplication = false;
static int pendingXP = 0;
const unsigned long XP_REWARD_INTERVAL = 1000;  // award XP every second

bool justPressed(uint8_t pin, bool &prev, unsigned long &debounceTime) {
  bool now = digitalRead(pin) == LOW;
  unsigned long currentTime = millis();

  if (now != prev && (currentTime - debounceTime) > debounceDelay) {
    debounceTime = currentTime;
    prev = now;
    return now;
  }

  return false;
}

bool shouldShowApp(const char* appName) {
  return !isDangerousApp(appName) || isDangerousActionsEnabled();
}

// Forward declarations – defined later with the rest of the menus
extern MenuItem mainMenu[];
extern MenuItem shortcutMenu[];
constexpr int MAIN_MENU_SIZE = 3;
constexpr int SHORTCUT_MENU_SIZE = 2;

int getVisibleMenuSize() {
  if (currentState == APP_MAIN) {
    if (mainMenuColumn == 1) {
      int count = 0;
      for (int i = 0; i < SHORTCUT_MENU_SIZE; i++) {
        if (shouldShowApp(shortcutMenu[i].name)) count++;
      }
      return count;
    }
    // Airplane mode hides WiFi (0) and BLE (1) → only "Other" remains
    if (airplaneModeEnabled) return 1;
    return MAIN_MENU_SIZE;
  }

  int count = 0;
  for (int i = 0; i < currentMenuSize; i++) {
    if (shouldShowApp(currentMenuItems[i].name)) {
      count++;
    }
  }
  return count;
}

MenuItem* getVisibleMenuItem(int visibleIndex) {
  if (currentState == APP_MAIN && mainMenuColumn == 1) {
    int visibleCount = 0;
    for (int i = 0; i < SHORTCUT_MENU_SIZE; i++) {
      if (shouldShowApp(shortcutMenu[i].name)) {
        if (visibleCount == visibleIndex) {
          return &shortcutMenu[i];
        }
        visibleCount++;
      }
    }
    return &shortcutMenu[0];
  }

  // Main menu left column with airplane mode: only "Other" is visible
  if (currentState == APP_MAIN && mainMenuColumn == 0 && airplaneModeEnabled) {
    return &mainMenu[2];  // Other
  }

  if (currentState == APP_MAIN && mainMenuColumn == 0) {
    if (visibleIndex >= 0 && visibleIndex < MAIN_MENU_SIZE) {
      return &mainMenu[visibleIndex];
    }
    return &mainMenu[0];
  }

  int visibleCount = 0;
  for (int i = 0; i < currentMenuSize; i++) {
    if (shouldShowApp(currentMenuItems[i].name)) {
      if (visibleCount == visibleIndex) {
        return &currentMenuItems[i];
      }
      visibleCount++;
    }
  }

  if (currentMenuItems == nullptr) return &mainMenu[0];
  return &currentMenuItems[0];
}


void startAppTracking(const char* appName) {
  currentAppName = appName;
  appStartTime = millis();
  lastXPReward = appStartTime;
  pendingXP = 0;
  inApplication = true;
}

void stopAppTracking() {
  if (inApplication) {
    // Flush any remaining pending XP
    if (pendingXP > 0) {
      addXP(pendingXP);
      pendingXP = 0;
    }

    inApplication = false;
    currentAppName = "";

    updateLastActivity();
  }
}

void updateAppXP() {
  if (!inApplication) return;

  // Award ~3 XP for every second spent in the current function
  if (millis() - lastXPReward >= XP_REWARD_INTERVAL) {
    pendingXP += XP_PER_SECOND;
    lastXPReward = millis();

    // Apply in small batches so progress feels responsive
    if (pendingXP >= XP_PER_SECOND) {
      addXP(pendingXP);
      pendingXP = 0;
    }
  }
}

void enterMenu(AppMenuState st);
void runApp(MenuItem &mi);

void noCleanup() {
}

MenuItem mainMenu[] = {
  { "WiFi",  bitmap_icon_wifi,    nullptr, nullptr, noCleanup },
  { "BLE",   bitmap_icon_ble,     nullptr, nullptr, noCleanup },
  { "Other", bitmap_icon_analyzer, nullptr, nullptr, noCleanup }
};

// Second column on main menu – RFID + Reaper link
MenuItem shortcutMenu[] = {
  { "RFID",   bitmap_icon_rfid,  rfidSetup,     rfidLoop,     rfidCleanup },
  { "Reaper", bitmap_icon_skull, reaperUiSetup, reaperUiLoop, reaperUiCleanup }
};

MenuItem wifiMenu[] = {
  { "Scanner",         nullptr, wifiscanSetup,           wifiscanLoop,           wifiscanCleanup },
  { "Deauther",        nullptr, deauthSetup,             deauthLoop,             cleanupWiFi },
  { "Handshake Capture", nullptr, handshakeCaptureSetup, handshakeCaptureLoop, cleanupWiFi },
  { "Client Sniffer",  nullptr, clientSnifferSetup,      clientSnifferLoop,      clientSnifferCleanup },
  { "Deauth Scanner",  nullptr, deauthScannerSetup,      deauthScannerLoop,      cleanupWiFi },
  { "Beacon Spam",     nullptr, beaconSpamSetup,         beaconSpamLoop,         cleanupWiFi },
  { "Evil Portal",     nullptr, evilPortalSetup,         evilPortalLoop,         cleanupEvilPortal },
  { "Pineapple Detector", nullptr, pineappleDetectorSetup, pineappleDetectorLoop, cleanupWiFi },
  { "Pwnagotchi Detector", nullptr, pwnagotchiDetectorSetup, pwnagotchiDetectorLoop, cleanupWiFi },
  { "Pwnagotchi Spam", nullptr, pwnagotchiSpamSetup,     pwnagotchiSpamLoop,     cleanupWiFi },
  { "Back",            nullptr, nullptr,                 nullptr,                noCleanup }
};
constexpr int WIFI_MENU_SIZE = sizeof(wifiMenu) / sizeof(wifiMenu[0]);

MenuItem bleMenu[] = {
  { "BLE Scan",     nullptr, blescanSetup,             blescanLoop,             cleanupBLE },
  { "BLE Inspector",   nullptr, bleInspectorSetup,            bleInspectorLoop,            cleanupBLE },
  { "doomBOX Detector", nullptr, nyanboxDetectorSetup,         nyanboxDetectorLoop,         cleanupBLE },
  { "Flipper Zero Detector", nullptr, flipperZeroDetectorSetup, flipperZeroDetectorLoop, cleanupBLE },
  { "Axon Detector", nullptr, axonDetectorSetup, axonDetectorLoop, cleanupBLE },
  { "Meshtastic Detector", nullptr, meshtasticDetectorSetup, meshtasticDetectorLoop, cleanupBLE },
  { "MeshCore Detector", nullptr, meshcoreDetectorSetup, meshcoreDetectorLoop, cleanupBLE },
  { "Skimmer Detector", nullptr, cardskimmerDetectorSetup, cardskimmerDetectorLoop, cleanupBLE },
  { "AirTag Detector", nullptr, airtagDetectorSetup,   airtagDetectorLoop,      cleanupBLE },
  { "AirTag Spoofer", nullptr, airtagSpooferSetup,     airtagSpooferLoop,       cleanupBLE },
  { "SmartTag Detector", nullptr, smarttagDetectorSetup, smarttagDetectorLoop, cleanupBLE },
  { "Tile Detector", nullptr, tileDetectorSetup,     tileDetectorLoop,       cleanupBLE },
  { "RayBan Detector", nullptr, raybanDetectorSetup, raybanDetectorLoop, cleanupBLE },
  { "BLE Spammer",  nullptr, bleSpamSetup,             bleSpamLoop,             cleanupBLE },
  { "Swift Pair",   nullptr, swiftpairSpamSetup,       swiftpairSpamLoop,       cleanupBLE },
  { "Sour Apple",   nullptr, sourappleSetup,           sourappleLoop,           cleanupBLE },
  { "Sour Droid",    nullptr, sourDroidSetup,          sourDroidLoop,          cleanupBLE },
  { "BLE Spoofer",  nullptr, bleSpooferSetup,          bleSpooferLoop,          cleanupBLE },
  { "Back",         nullptr, nullptr,                  nullptr,                 noCleanup }
};
constexpr int BLE_MENU_SIZE = sizeof(bleMenu) / sizeof(bleMenu[0]);

MenuItem otherMenu[] = {
  { "Superscanner", nullptr, superscannerSetup, superscannerLoop, superscannerCleanup },
  { "Drone Detector", nullptr, droneDetectorSetup, droneDetectorLoop, cleanupDroneDetector },
  { "Drone Spoofer", nullptr, droneSpooferSetup, droneSpooferLoop, cleanupDroneSpoofer },
  { "Flock Detector", nullptr, flockDetectorSetup, flockDetectorLoop, cleanupFlockDetector },
  { "Device Scout", nullptr, deviceScoutSetup, deviceScoutLoop, cleanupDeviceScout },
  { "Setting",      nullptr, settingSetup,    settingLoop,    noCleanup },
  { "About",        nullptr, aboutSetup,      aboutLoop,      aboutCleanup },
  { "Back",         nullptr, nullptr,         nullptr,        noCleanup }
};
constexpr int OTHER_MENU_SIZE = sizeof(otherMenu) / sizeof(otherMenu[0]);

void enterMenu(AppMenuState st) {
  // Save previous selection name only if a menu is already loaded
  const char* previousAppName = nullptr;
  if (currentMenuItems != nullptr && currentMenuSize > 0 &&
      item_selected >= 0 && item_selected < getVisibleMenuSize()) {
    MenuItem* prev = getVisibleMenuItem(item_selected);
    if (prev) previousAppName = prev->name;
  }

  currentState = st;

  // Assign menu pointers BEFORE any getVisibleMenuItem() calls
  switch (st) {
    case APP_MAIN:
      currentMenuItems = mainMenu;
      currentMenuSize  = MAIN_MENU_SIZE;
      mainMenuColumn   = 0;
      break;
    case APP_WIFI:
      currentMenuItems = wifiMenu;
      currentMenuSize  = WIFI_MENU_SIZE;
      break;
    case APP_BLE:
      currentMenuItems = bleMenu;
      currentMenuSize  = BLE_MENU_SIZE;
      break;
    case APP_OTHER:
      currentMenuItems = otherMenu;
      currentMenuSize  = OTHER_MENU_SIZE;
      break;
    default:
      break;
  }

  // Stop advertiser when leaving main. Restart when returning (after boot delay).
  if (st == APP_MAIN) {
    if (!advertiserBootPending) {
      startNyanboxAdvertiser();
    }
  } else {
    stopNyanboxAdvertiser();
  }

  item_selected = 0;
  if (previousAppName && st != APP_MAIN) {
    for (int i = 0; i < getVisibleMenuSize(); i++) {
      if (strcmp(getVisibleMenuItem(i)->name, previousAppName) == 0) {
        item_selected = i;
        break;
      }
    }
  }

  needsRedraw = true;
}

void runApp(MenuItem &mi) {
  if (!mi.setup) return;

  startAppTracking(mi.name);

  if (isReconApp(mi.name)) {
    blinkColor(0, 0, 255);  // Blue
  } else if (isOffensiveApp(mi.name)) {
    blinkColor(255, 0, 0); // Red
  }

  mi.setup();
  updateLastActivity();
  u8g2.setPowerSave(0);

  if (!mi.loop) return;
  while (digitalRead(BUTTON_SEL) == LOW);

  while (true) {
    updateAppXP();
    neopixelLoop();

    if (anyButtonPressed()) {
      updateLastActivity();
    }

    mi.loop();
    if (digitalRead(BUTTON_SEL) == LOW) {
      while (digitalRead(BUTTON_SEL) == LOW);

      if (mi.cleanup) {
        mi.cleanup();
      }

      break;
    }
  }

  stopBlinking();
  stopAppTracking();
  u8g2.clearBuffer();
}

void setup() {
  Serial.begin(115200);

  // Buttons first so password / menus never run without pins ready
  pinMode(BUTTON_PIN_UP, INPUT_PULLUP);
  pinMode(BUTTON_PIN_CENTER, INPUT_PULLUP);
  pinMode(BUTTON_PIN_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT, INPUT_PULLUP);

  neopixelSetup();

  EEPROM.begin(512);
  oledBrightness = EEPROM.read(1);
  if (oledBrightness == 0xFF) oledBrightness = 128;

  dangerousActionsEnabled = true;  // permanently enabled

  uint8_t continuousScanValue = EEPROM.read(4);
  if (continuousScanValue == 0xFF) {
    continuousScanEnabled = true;
  } else {
    continuousScanEnabled = (continuousScanValue == 1);
  }

  uint8_t privacyModeValue = EEPROM.read(5);
  if (privacyModeValue == 0xFF) {
    privacyModeEnabled = false;
  } else {
    privacyModeEnabled = (privacyModeValue == 1);
  }

  u8g2.begin();
  u8g2.setContrast(oledBrightness);
  u8g2.setBitmapMode(1);

  loadOptionsFromEEPROM();

  updateLastActivity();

  // ========== Opening Scene 1: RG logo (no name text) ==========
  u8g2.clearBuffer();
  u8g2.drawBitmap(0, 0, 16, 64, logo_doombox);  // RG logo only
  u8g2.sendBuffer();
  delay(2000);

  // ========== Opening Scene 2: "presents" ==========
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvR08_tr);
  {
    const char* p = "presents";
    int16_t pw = u8g2.getUTF8Width(p);
    u8g2.setCursor((128 - pw) / 2, 34);
    u8g2.print(p);
  }
  u8g2.sendBuffer();
  delay(1500);

  // ========== Opening Scene 3: new doomBOX logo bitmap ==========
  u8g2.clearBuffer();
  u8g2.drawBitmap(0, 0, 16, 64, doombox_bitmap);
  u8g2.sendBuffer();
  delay(2000);

  // (GitHub / credits screen removed – go straight to device)

  levelSystemSetup();

  // Defer BLE advertising until after the main menu is up.
  // Starting BLE immediately after the cutscene was a common cause of
  // boot loops (crash → reboot → cutscene again).
  initNyanboxAdvertiser();

  if (passwordEnabled()) { checkPasswordOnBoot(); }

  enterMenu(APP_MAIN);
  displayMirrorSetup();
  // BLE advertiser starts later from loop via updateNyanboxAdvertiser()
}

void loop() {
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "MIRROR_ON") {
      displayMirrorEnable(true);
      needsRedraw = true;
    } else if (cmd == "MIRROR_OFF") {
      displayMirrorEnable(false);
    }
  }

  updateAppXP();
  neopixelLoop();

  // Defer BLE advertiser until UI has been running a moment (avoids boot-loop crashes)
  if (advertiserBootPending && millis() > 8000) {
    advertiserBootPending = false;
    startNyanboxAdvertiser();
  }
  updateNyanboxAdvertiser();

  bool upNow   = (digitalRead(BUTTON_PIN_UP)     == LOW);
  bool downNow = (digitalRead(BUTTON_PIN_DOWN)   == LOW);
  bool selNow  = (digitalRead(BUTTON_PIN_CENTER) == LOW);
  unsigned long currentTime = millis();

  // ========== OPTIONS MENU STATE MACHINE ==========
  if (optionsMenuActive) {
    updateLastActivity();

    // Troll disable: hold SELECT 4 seconds (works globally while troll is on, also here)
    if (selNow && trollModeEnabled) {
      if (!selHolding) {
        selHolding = true;
        selHoldStart = currentTime;
      } else if (currentTime - selHoldStart >= TROLL_DISABLE_HOLD_MS) {
        setTrollMode(false);
        selHolding = false;
        while (digitalRead(BUTTON_PIN_CENTER) == LOW) delay(10);
        needsRedraw = true;
      }
    } else if (!selNow) {
      selHolding = false;
    }

    if (optionsSubMenu == 1) {
      // Brightness sub-menu — short presses adjust, hold DOWN 1s exits options entirely
      if (upNow && !upPressed) {
        if (brightnessPercent < BRIGHTNESS_MAX_PCT) {
          brightnessPercent++;
          saveBrightness();
        }
      }
      if (downNow) {
        if (!downHolding) {
          // Rising edge: decrease brightness immediately
          downHolding = true;
          downHoldStart = currentTime;
          if (brightnessPercent > BRIGHTNESS_MIN_PCT) {
            brightnessPercent--;
            saveBrightness();
          }
        } else if (currentTime - downHoldStart >= OPTIONS_HOLD_MS) {
          optionsMenuActive = false;
          optionsSubMenu = 0;
          downHolding = false;
          currentState = APP_MAIN;
          needsRedraw = true;
          while (digitalRead(BUTTON_PIN_DOWN) == LOW) delay(10);
          downPressed = false;
          return;
        }
      } else {
        downHolding = false;
      }
      if (justPressed(BUTTON_SEL, selPrev, selDebounceTime)) {
        optionsSubMenu = 0;
        needsRedraw = true;
      }
      upPressed = upNow;
      downPressed = downNow;
      drawBrightnessSubMenu();
      if (downHolding && (currentTime - downHoldStart) >= 500) {
        drawHoldProgressBar(currentTime - downHoldStart, OPTIONS_HOLD_MS);
        u8g2.sendBuffer();
        displayMirrorSend(u8g2);
      }
      return;
    }

    // Grid navigation — short press moves, hold DOWN >500ms shows bar, 1000ms exits
    if (upNow && !upPressed) {
      if (optionsSelected >= 3) optionsSelected -= 3;
      else optionsSelected = (optionsSelected + 3) % 6;
      if (optionsSelected >= 4) optionsSelected = 3;
      needsRedraw = true;
    }

    if (downNow) {
      if (!downHolding) {
        // Rising edge: navigate immediately
        downHolding = true;
        downHoldStart = currentTime;
        if (optionsSelected < 3) optionsSelected += 3;
        else optionsSelected = optionsSelected - 3;
        if (optionsSelected >= 4) optionsSelected = 3;
        needsRedraw = true;
      } else if (currentTime - downHoldStart >= OPTIONS_HOLD_MS) {
        optionsMenuActive = false;
        optionsSubMenu = 0;
        downHolding = false;
        currentState = APP_MAIN;
        needsRedraw = true;
        while (digitalRead(BUTTON_PIN_DOWN) == LOW) delay(10);
        downPressed = false;
        return;
      }
    } else {
      downHolding = false;
    }

    if (justPressed(BUTTON_LEFT, leftPrev, leftDebounceTime)) {
      if (optionsSelected % 3 > 0) optionsSelected--;
      else optionsSelected += 2;
      if (optionsSelected >= 4) optionsSelected = 3;
      needsRedraw = true;
    }
    if (justPressed(BUTTON_RIGHT, rightPrev, rightDebounceTime)) {
      if (optionsSelected % 3 < 2) optionsSelected++;
      else optionsSelected -= 2;
      if (optionsSelected >= 4) optionsSelected = 3;
      needsRedraw = true;
    }

    if (justPressed(BUTTON_SEL, selPrev, selDebounceTime)) {
      if (optionsSelected == 0) {
        optionsSubMenu = 1;
        needsRedraw = true;
      } else if (optionsSelected == 1) {
        bool nowOn = !passwordEnabled();
        setPasswordEnabled(nowOn);
        needsRedraw = true;
      } else if (optionsSelected == 2) {
        setAirplaneMode(!airplaneModeEnabled);
        needsRedraw = true;
      } else if (optionsSelected == 3) {
        setTrollMode(!trollModeEnabled);
        needsRedraw = true;
      }
      // 4 & 5 are blank – ignore
    }

    upPressed = upNow;
    downPressed = downNow;

    unsigned long animElapsed = currentTime - optionsOpenAnimStart;
    drawOptionsMenu(animElapsed);

    // Overlay hold-to-exit progress bar only after 500 ms continuous hold
    if (downHolding && (currentTime - downHoldStart) >= 500) {
      drawHoldProgressBar(currentTime - downHoldStart, OPTIONS_HOLD_MS);
      u8g2.sendBuffer();
      displayMirrorSend(u8g2);
    }
    return;
  }

  // ========== TROLL MODE GLOBAL DISABLE (hold SELECT 4s) ==========
  if (trollModeEnabled && selNow) {
    if (!selHolding) {
      selHolding = true;
      selHoldStart = currentTime;
    } else if (currentTime - selHoldStart >= TROLL_DISABLE_HOLD_MS) {
      setTrollMode(false);
      selHolding = false;
      while (digitalRead(BUTTON_PIN_CENTER) == LOW) delay(10);
      needsRedraw = true;
    }
  } else if (!selNow) {
    selHolding = false;
  }

  // ========== UP: short press = navigate, hold >500ms shows bar, 1000ms = options ==========
  // Progress bar only appears after 500 ms of continuous hold.
  if (currentState == APP_MAIN && !optionsMenuActive) {
    if (upNow) {
      updateLastActivity();
      if (!upHolding) {
        // Rising edge: immediate short-press navigation
        upHolding = true;
        upHoldStart = currentTime;
        upDebounceTime = currentTime;
        if (item_selected > 0) item_selected--;
        else item_selected = getVisibleMenuSize() - 1;
        upLastMillis = currentTime;
        upNextRepeat = currentTime + initialDelay;
        needsRedraw = true;
      } else {
        unsigned long held = currentTime - upHoldStart;
        if (held >= OPTIONS_HOLD_MS) {
          optionsMenuActive = true;
          optionsSelected = 0;
          optionsSubMenu = 0;
          optionsOpenAnimStart = currentTime;
          upHolding = false;
          while (digitalRead(BUTTON_PIN_UP) == LOW) delay(10);
          upPressed = false;
          needsRedraw = true;
          return;
        }
        // Auto-repeat scroll only before the options hold threshold
        if (held < 500 && currentTime >= upNextRepeat) {
          if (item_selected > 0) item_selected--;
          else item_selected = getVisibleMenuSize() - 1;
          upNextRepeat += repeatInterval;
          needsRedraw = true;
        }
      }
    } else {
      upHolding = false;
    }
    upPressed = upNow;
  } else {
    // Normal UP handling when not on main / not holding for options
    if (upNow) {
      updateLastActivity();
      if (!upPressed && (currentTime - upDebounceTime) > debounceDelay) {
        upDebounceTime = currentTime;
        if (item_selected > 0) {
          item_selected--;
        } else {
          item_selected = getVisibleMenuSize() - 1;
        }
        upLastMillis = currentTime;
        upNextRepeat = upLastMillis + initialDelay;
        needsRedraw = true;
      } else if (upPressed && currentTime >= upNextRepeat) {
        if (item_selected > 0) {
          item_selected--;
        } else {
          item_selected = getVisibleMenuSize() - 1;
        }
        upNextRepeat += repeatInterval;
        needsRedraw = true;
      }
    }
    upPressed = upNow;
  }

  // DOWN handling (normal, when not in options)
  if (downNow) {
    updateLastActivity();
    if (!downPressed && (currentTime - downDebounceTime) > debounceDelay) {
      downDebounceTime = currentTime;
      if (item_selected < getVisibleMenuSize() - 1) {
        item_selected++;
      } else {
        item_selected = 0;
      }
      downLastMillis = currentTime;
      downNextRepeat = downLastMillis + initialDelay;
      needsRedraw = true;
    } else if (downPressed && currentTime >= downNextRepeat) {
      if (item_selected < getVisibleMenuSize() - 1) {
        item_selected++;
      } else {
        item_selected = 0;
      }
      downNextRepeat += repeatInterval;
      needsRedraw = true;
    }
  }
  downPressed = downNow;

  if (justPressed(BUTTON_SEL, selPrev, selDebounceTime)) {
    updateLastActivity();
    if (currentState != APP_LEVEL) {
      MenuItem *sel = getVisibleMenuItem(item_selected);
      if (currentState == APP_MAIN) {
        if (mainMenuColumn == 0) {
          if (strcmp(sel->name, "WiFi") == 0) {
            if (!airplaneModeEnabled) enterMenu(APP_WIFI);
          } else if (strcmp(sel->name, "BLE") == 0) {
            if (!airplaneModeEnabled) enterMenu(APP_BLE);
          } else if (strcmp(sel->name, "Other") == 0) enterMenu(APP_OTHER);
        } else {
          // Second column – launch RFID (or other shortcuts)
          runApp(*sel);
          needsRedraw = true;
        }
      } else {
        if (strcmp(sel->name, "Back") == 0) {
          enterMenu(APP_MAIN);
        } else {
          runApp(*sel);
          needsRedraw = true;
        }
      }
    }
  }

  if (justPressed(BUTTON_LEFT, leftPrev, leftDebounceTime)) {
    updateLastActivity();
    if (currentState == APP_MAIN) {
      if (mainMenuColumn == 1) {
        mainMenuColumn = 0;
        item_selected = 0;
        needsRedraw = true;
      }
    } else if (currentState == APP_LEVEL ||
               currentState == APP_BLE   ||
               currentState == APP_WIFI  ||
               currentState == APP_OTHER) {
      enterMenu(APP_MAIN);
    }
  }

  if (justPressed(BUTTON_RIGHT, rightPrev, rightDebounceTime)) {
    updateLastActivity();
    if (currentState == APP_MAIN) {
      if (mainMenuColumn == 0) {
        mainMenuColumn = 1;
        item_selected = 0;
        needsRedraw = true;
      } else {
        // Already on second column → open Level menu
        currentState = APP_LEVEL;
        levelSystemSetup();
        needsRedraw = true;
      }
    }
  }

  if (currentState == APP_LEVEL) {
    levelSystemLoop();
  } else {
    if (currentState == APP_MAIN) {
      updateNyanboxAdvertiser();
    }

    // Redraw when holding UP long enough for progress bar, or when needsRedraw
    bool showHoldBar = (currentState == APP_MAIN && upHolding &&
                        (currentTime - upHoldStart) >= 500);
    bool forceDraw = showHoldBar;
    if (needsRedraw || forceDraw || trollModeEnabled) {
      u8g2.clearBuffer();

      if (currentState == APP_MAIN) {
        // ===== Dual-column main menu =====
        // Left: WiFi / BLE / Other  (hidden when airplane mode)
        // Right: RFID

        const int leftX = 2;
        const int rightX = 66;

        // Draw left column items (skip WiFi/BLE when airplane mode)
        {
          int drawIdx = 0;
          for (int i = 0; i < MAIN_MENU_SIZE; i++) {
            if (airplaneModeEnabled && (i == 0 || i == 1)) continue;
            int itemY = 6 + (drawIdx * (ITEM_HEIGHT + ITEM_SPACING));
            int textY = itemY + 11;
            bool focused = (mainMenuColumn == 0 && item_selected == drawIdx);

            if (focused) {
              u8g2.drawBox(leftX, itemY + 2, 2, ITEM_HEIGHT - 4);
            }

            if (mainMenu[i].icon) {
              u8g2.drawXBMP(leftX + 4, itemY, 16, 16, mainMenu[i].icon);
            }
            u8g2.setFont(u8g2_font_helvR08_tr);
            u8g2.drawStr(leftX + 22, textY, mainMenu[i].name);
            drawIdx++;
          }
        }

        // Draw right (shortcut) column items
        for (int i = 0; i < SHORTCUT_MENU_SIZE; i++) {
          if (!shouldShowApp(shortcutMenu[i].name)) continue;
          int itemY = 6 + (i * (ITEM_HEIGHT + ITEM_SPACING));
          int textY = itemY + 11;
          bool focused = (mainMenuColumn == 1 && item_selected == i);

          if (focused) {
            u8g2.drawBox(rightX, itemY + 2, 2, ITEM_HEIGHT - 4);
          }

          if (shortcutMenu[i].icon) {
            u8g2.drawXBMP(rightX + 4, itemY, 16, 16, shortcutMenu[i].icon);
          }
          u8g2.setFont(u8g2_font_helvR08_tr);
          u8g2.drawStr(rightX + 22, textY, shortcutMenu[i].name);
        }

        // Hold-to-open progress bar — only after 500 ms continuous hold
        if (showHoldBar) {
          unsigned long held = currentTime - upHoldStart;
          drawHoldProgressBar(held, OPTIONS_HOLD_MS);
        }
      } else {
        // ===== Standard single-column submenu =====
        int start;
        if (item_selected == 0) start = 0;
        else if (item_selected == getVisibleMenuSize() - 1) start = max(0, getVisibleMenuSize() - 3);
        else start = item_selected - 1;

        int highlight = item_selected - start;

        int selectionY = 6 + (highlight * (ITEM_HEIGHT + ITEM_SPACING));
        drawSelection(SELECTION_X, selectionY, SELECTION_WIDTH, ITEM_HEIGHT, true);

        for (int i = 0; i < 3; i++) {
          int idx = start + i;
          if (idx < getVisibleMenuSize()) {
            MenuItem *item = getVisibleMenuItem(idx);
            int itemY = 6 + (i * (ITEM_HEIGHT + ITEM_SPACING));
            int textY = itemY + 11;

            u8g2.setFont(u8g2_font_helvR08_tr);
            u8g2.drawStr(TEXT_X, textY, item->name);

            if (item->icon) {
              int iconY = itemY;
              u8g2.drawXBMP(ICON_X, iconY, 16, 16, item->icon);
            }
          }
        }
      }

      applyTrollOverlay();
      u8g2.sendBuffer();
      displayMirrorSend(u8g2);
      needsRedraw = false;
    }
  }
}