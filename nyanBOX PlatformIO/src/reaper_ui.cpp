/**
 * Reaper control UI – discover, pair, command surveys, show status
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include "../include/reaper_ui.h"
#include "../include/reaper_manager.h"
#include "../include/reaper_protocol.h"
#include "../include/pindefs.h"
#include "../include/display_mirror.h"
#include "../include/sleep_manager.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

static ReaperManager g_reaper;
static bool needsRedraw = true;
static int menuIndex = 0;
static unsigned long lastStatusReq = 0;
static unsigned long lastDraw = 0;

// Menu actions when paired
static const char* pairedActions[] = {
    "BT Survey",
    "WiFi Survey",
    "Stop",
    "Refresh Status",
    "Disconnect"
};
static const int PAIRED_ACTION_COUNT = 5;

// Channel ranges matching Reaper defaults
#define BT_START   2
#define BT_END     80
#define WIFI_START 1
#define WIFI_END   14
#define HOP_STEP   2

void reaperUiSetup() {
    pinMode(BUTTON_PIN_UP, INPUT_PULLUP);
    pinMode(BUTTON_PIN_DOWN, INPUT_PULLUP);
    pinMode(BUTTON_PIN_LEFT, INPUT_PULLUP);
    pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);

    menuIndex = 0;
    needsRedraw = true;
    lastStatusReq = 0;
    lastDraw = 0;

    g_reaper.begin();
}

void reaperUiCleanup() {
    g_reaper.stop();
}

static void drawSearching() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(8, 14, "REAPER LINK");
    u8g2.drawHLine(0, 18, 128);

    u8g2.drawStr(10, 36, "Searching...");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(4, 52, "Power on ESP32-Reaper");
    u8g2.drawStr(4, 62, "SEL to exit");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawError() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(8, 14, "REAPER LINK");
    u8g2.drawHLine(0, 18, 128);
    u8g2.drawStr(16, 40, "ESP-NOW ERROR");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(4, 58, "SEL to exit");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawPaired() {
    const ReaperStatus& st = g_reaper.getStatus();
    const uint8_t* mac = g_reaper.getReaperMac();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(8, 12, "REAPER PAIRED");
    u8g2.drawHLine(0, 15, 128);

    // MAC (short)
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 24, macStr);

    // Mode + radios
    char line[32];
    snprintf(line, sizeof(line), "Mode:%s",
             Protocol::modeToString(static_cast<OperatingMode>(st.mode)));
    u8g2.drawStr(2, 33, line);

    snprintf(line, sizeof(line), "R1:%s R2:%s R3:%s",
             (st.radio_status & 0x01) ? "OK" : "--",
             (st.radio_status & 0x02) ? "OK" : "--",
             (st.radio_status & 0x04) ? "OK" : "--");
    u8g2.drawStr(2, 42, line);

    // Action menu (one highlighted)
    const char* act = pairedActions[menuIndex];
    u8g2.setFont(u8g2_font_helvR08_tr);
    int tw = u8g2.getStrWidth(act);
    u8g2.drawBox(0, 48, 128, 16);
    u8g2.setDrawColor(0);
    u8g2.drawStr((128 - tw) / 2, 60, act);
    u8g2.setDrawColor(1);

    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

void reaperUiLoop() {
    g_reaper.update();

    // Periodic status request when paired
    if (g_reaper.isPaired() && (millis() - lastStatusReq > 2000)) {
        lastStatusReq = millis();
        g_reaper.sendStatusRequest();
    }

    // Button handling (non-blocking style with short delay for debounce)
    static unsigned long lastBtn = 0;
    if (millis() - lastBtn > 180) {
        if (digitalRead(BUTTON_PIN_UP) == LOW) {
            lastBtn = millis();
            if (g_reaper.isPaired()) {
                menuIndex = (menuIndex - 1 + PAIRED_ACTION_COUNT) % PAIRED_ACTION_COUNT;
                needsRedraw = true;
            }
        } else if (digitalRead(BUTTON_PIN_DOWN) == LOW) {
            lastBtn = millis();
            if (g_reaper.isPaired()) {
                menuIndex = (menuIndex + 1) % PAIRED_ACTION_COUNT;
                needsRedraw = true;
            }
        } else if (digitalRead(BUTTON_PIN_RIGHT) == LOW) {
            lastBtn = millis();
            if (g_reaper.isPaired()) {
                switch (menuIndex) {
                    case 0: // BT Survey
                        g_reaper.sendScanCommand(OperatingMode::BLUETOOTH_SURVEY,
                                                 BT_START, BT_END, HOP_STEP);
                        break;
                    case 1: // WiFi Survey
                        g_reaper.sendScanCommand(OperatingMode::WIFI_SURVEY,
                                                 WIFI_START, WIFI_END, HOP_STEP);
                        break;
                    case 2: // Stop
                        g_reaper.sendStopCommand();
                        break;
                    case 3: // Refresh
                        g_reaper.sendStatusRequest();
                        break;
                    case 4: // Disconnect
                        g_reaper.disconnect();
                        menuIndex = 0;
                        break;
                }
                needsRedraw = true;
            }
        }
    }

    // Redraw on state change or periodically
    static ReaperLinkState prevState = ReaperLinkState::IDLE;
    ReaperLinkState st = g_reaper.getState();
    if (st != prevState) {
        prevState = st;
        needsRedraw = true;
    }

    if (needsRedraw || (millis() - lastDraw > 500)) {
        lastDraw = millis();
        needsRedraw = false;
        switch (st) {
            case ReaperLinkState::SEARCHING:
            case ReaperLinkState::INITIALIZING:
                drawSearching();
                break;
            case ReaperLinkState::PAIRED:
                drawPaired();
                break;
            case ReaperLinkState::ERROR:
                drawError();
                break;
            default:
                drawSearching();
                break;
        }
    }
}
