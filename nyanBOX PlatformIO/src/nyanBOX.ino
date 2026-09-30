/*
    doomBOX
    https://github.com/jbohack/nyanBOX
    Copyright (c) 2026 jbohack

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

#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif
#include <RF24.h>

#include "../include/icon.h"
#include "../include/doombox_logo.h"
#include "../include/neopixel.h"
#include "../include/setting.h"

#include "../include/scanner.h"
#include "../include/analyzer.h"
#include "../include/sourapple.h"
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
#include "../include/sigkill.h"
#include "../include/about.h"
#include "../include/channel_analyzer.h"
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

RF24 radios[] = {
  RF24(RADIO_CE_PIN_1, RADIO_CSN_PIN_1)
};

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

enum AppMenuState { APP_MAIN, APP_BLE, APP_WIFI, APP_OTHER, APP_LEVEL };

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
  return strstr(appName, "SigKill") != nullptr;
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
int          mainMenuColumn   = 0;  // 0 = categories (WiFi/BLE/Other), 1 = shortcuts

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
constexpr int SHORTCUT_MENU_SIZE = 3;

int getVisibleMenuSize() {
  // On main menu, size depends on which column is focused
  if (currentState == APP_MAIN) {
    if (mainMenuColumn == 1) {
      int count = 0;
      for (int i = 0; i < SHORTCUT_MENU_SIZE; i++) {
        if (shouldShowApp(shortcutMenu[i].name)) count++;
      }
      return count;
    }
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

// Shortcut column on main menu – full entry points (not mere redirects)
MenuItem shortcutMenu[] = {
  { "WiFi Scan",       bitmap_icon_wifi,  wifiscanSetup,         wifiscanLoop,         wifiscanCleanup },
  { "BT SigKill",      bitmap_icon_kill,  sigkillBleSetup,       sigkillLoop,          cleanupRadio },
  { "AirTag",          bitmap_icon_apple, airtagDetectorSetup,   airtagDetectorLoop,   cleanupBLE }
};

MenuItem wifiMenu[] = {
  { "WiFi Scan",       nullptr, wifiscanSetup,           wifiscanLoop,           wifiscanCleanup },
  { "Client Sniffer",  nullptr, clientSnifferSetup,      clientSnifferLoop,      clientSnifferCleanup },
  { "Channel Analyzer", nullptr, channelAnalyzerSetup,   channelAnalyzerLoop,    cleanupWiFi },
  { "WiFi Deauther",   nullptr, deauthSetup,             deauthLoop,             cleanupWiFi },
  { "Deauth Scanner",  nullptr, deauthScannerSetup,      deauthScannerLoop,      cleanupWiFi },
  { "Beacon Spam",     nullptr, beaconSpamSetup,         beaconSpamLoop,         cleanupWiFi },
  { "Evil Portal",     nullptr, evilPortalSetup,         evilPortalLoop,         cleanupEvilPortal },
  { "Pineapple Detector", nullptr, pineappleDetectorSetup, pineappleDetectorLoop, cleanupWiFi },
  { "Pwnagotchi Detector", nullptr, pwnagotchiDetectorSetup, pwnagotchiDetectorLoop, cleanupWiFi },
  { "Pwnagotchi Spam", nullptr, pwnagotchiSpamSetup,     pwnagotchiSpamLoop,     cleanupWiFi },
  { "Handshake Capture", nullptr, handshakeCaptureSetup, handshakeCaptureLoop, cleanupWiFi },
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
  { "SigKill",   nullptr, sigkillSetup,   sigkillLoop,   cleanupRadio },
  { "Superscanner", nullptr, superscannerSetup, superscannerLoop, superscannerCleanup },
  { "Drone Detector", nullptr, droneDetectorSetup, droneDetectorLoop, cleanupDroneDetector },
  { "Drone Spoofer", nullptr, droneSpooferSetup, droneSpooferLoop, cleanupDroneSpoofer },
  { "Flock Detector", nullptr, flockDetectorSetup, flockDetectorLoop, cleanupFlockDetector },
  { "Device Scout", nullptr, deviceScoutSetup, deviceScoutLoop, cleanupDeviceScout },
  { "Scanner",      nullptr, scannerSetup,    scannerLoop,    cleanupRadio },
  { "Analyzer",     nullptr, analyzerSetup,   analyzerLoop,   cleanupRadio },
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

  // Unified single-module NRF24 bring-up
  nrf24Begin();  // ignores result at boot; features re-init later

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

  bool upNow = (digitalRead(BUTTON_PIN_UP) == LOW);
  bool downNow = (digitalRead(BUTTON_PIN_DOWN) == LOW);
  unsigned long currentTime = millis();

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
          // Category column
          if (strcmp(sel->name, "WiFi") == 0) enterMenu(APP_WIFI);
          else if (strcmp(sel->name, "BLE") == 0) enterMenu(APP_BLE);
          else if (strcmp(sel->name, "Other") == 0) enterMenu(APP_OTHER);
        } else {
          // Shortcut column – launch the real function
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
        // Already on shortcut column → open Level menu
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

    if (needsRedraw) {
      u8g2.clearBuffer();

      if (currentState == APP_MAIN) {
        // ===== Dual-column main menu (clean) =====
        // Left: WiFi / BLE / Other
        // Right: WiFi Scan / BT SigKill / AirTag

        const int leftX = 2;
        const int rightX = 66;

        // Draw left column items
        for (int i = 0; i < MAIN_MENU_SIZE; i++) {
          int itemY = 6 + (i * (ITEM_HEIGHT + ITEM_SPACING));
          int textY = itemY + 11;
          bool focused = (mainMenuColumn == 0 && item_selected == i);

          if (focused) {
            u8g2.drawBox(leftX, itemY + 2, 2, ITEM_HEIGHT - 4);
          }

          if (mainMenu[i].icon) {
            u8g2.drawXBMP(leftX + 4, itemY, 16, 16, mainMenu[i].icon);
          }
          u8g2.setFont(u8g2_font_helvR08_tr);
          u8g2.drawStr(leftX + 22, textY, mainMenu[i].name);
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
          u8g2.setFont(u8g2_font_5x8_tr);
          const char* label = shortcutMenu[i].name;
          if (strcmp(label, "BT SigKill") == 0) label = "BT Kill";
          u8g2.drawStr(rightX + 22, textY, label);
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

      u8g2.sendBuffer();
      displayMirrorSend(u8g2);
      needsRedraw = false;
    }
  }
}