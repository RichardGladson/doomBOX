/*
doomBOX by Richard Gladson
https://github.com/RichardGladson/doomBOX
Copyright (c) 2026 Richard Gladson
Licensed under the GNU General Public License v3.0
SPDX-License-Identifier: GPL-3.0-only
*/
#include "../include/airtag_detector.h"
#include "../include/radio_manager.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "../include/setting.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include <vector>

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

#define BTN_UP BUTTON_PIN_UP
#define BTN_DOWN BUTTON_PIN_DOWN
#define BTN_RIGHT BUTTON_PIN_RIGHT
#define BTN_BACK BUTTON_PIN_LEFT
#define BTN_CENTER BUTTON_PIN_CENTER

// Advanced Find My payload structure
struct FindMyData {
    uint8_t publicKey[22];
    uint8_t statusByte;
    bool isValid;
};

std::vector<AirTagDeviceData> airtagDevices;
const int MAX_DEVICES = 100;
int currentIndex = 0;
int listStartIndex = 0;
bool isDetailView = false;
bool isLocateMode = false;
char locateTargetAddress[18] = {0};
unsigned long lastButtonPress = 0;
const unsigned long debounceTime = 200;
static bool needsRedraw = true;
static int lastDeviceCount = 0;
static unsigned long lastLocateUpdate = 0;
const unsigned long locateUpdateInterval = 1000;
static unsigned long lastCountdownUpdate = 0;
const unsigned long countdownUpdateInterval = 1000;
static bool wasScanning = false;
static bool isScanning = false;
static unsigned long lastScanTime = 0;
const unsigned long scanInterval = 30000;
const unsigned long scanDuration = 8;
static bool bleInitialized = false;
static bool scanCompleted = false;

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x100,
    .scan_window            = 0xA0,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

static void bda_to_string(uint8_t *bda, char *str, size_t size) {
    if (bda == NULL || str == NULL || size < 18) return;
    snprintf(str, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

// Strict Find My payload parser. Rejects non-AirTag Apple devices.
bool parseFindMyPayload(uint8_t *payload, uint8_t len, FindMyData &out) {
    out.isValid = false;
    // Looking for: Length(0x1E), Type(0xFF), Company(0x4C, 0x00), FindMyType(0x12)
    for (int i = 0; i <= len - 27; i++) {
        if (payload[i] == 0x1E && payload[i+1] == 0xFF && 
            payload[i+2] == 0x4C && payload[i+3] == 0x00 && 
            payload[i+4] == 0x12) {
            out.statusByte = payload[i+5];
            memcpy(out.publicKey, &payload[i+6], 22);
            out.isValid = true;
            return true;
        }
    }
    return false;
}

static void process_scan_result(esp_ble_gap_cb_param_t *scan_result) {
    uint8_t *payload = scan_result->scan_rst.ble_adv;
    uint8_t payload_len = scan_result->scan_rst.adv_data_len;
    
    FindMyData fmd;
    if (!parseFindMyPayload(payload, payload_len, fmd)) {
        return; // Not a valid AirTag/Find My device
    }

    uint8_t *bda = scan_result->scan_rst.bda;
    char addrStr[18];
    bda_to_string(bda, addrStr, sizeof(addrStr));
    if (strlen(addrStr) < 12) return;

    if (isLocateMode && strlen(locateTargetAddress) > 0) {
        if (strcmp(addrStr, locateTargetAddress) != 0) return;
    } else if (airtagDevices.size() >= MAX_DEVICES) {
        return;
    }

    for (size_t i = 0; i < airtagDevices.size(); i++) {
        if (strcmp(airtagDevices[i].address, addrStr) == 0) {
            airtagDevices[i].rssi = (int8_t)(((int)airtagDevices[i].rssi * 3 + (int)scan_result->scan_rst.rssi) / 4);
            airtagDevices[i].lastSeen = millis();
            
            // Store the parsed Find My data for the spoofer to use
            airtagDevices[i].statusByte = fmd.statusByte;
            memcpy(airtagDevices[i].publicKey, fmd.publicKey, 22);
            airtagDevices[i].hasValidFindMyData = true;
            
            if (payload_len > 0 && payload_len < 64) {
                memcpy(airtagDevices[i].payload, payload, payload_len);
                airtagDevices[i].payloadLength = payload_len;
            }
            return;
        }
    }

    AirTagDeviceData newDev = {};
    strncpy(newDev.address, addrStr, 17);
    newDev.address[17] = '\0';
    newDev.rssi = scan_result->scan_rst.rssi;
    newDev.lastSeen = millis();
    newDev.isAirTag = true;
    strcpy(newDev.name, "AirTag");
    
    newDev.statusByte = fmd.statusByte;
    memcpy(newDev.publicKey, fmd.publicKey, 22);
    newDev.hasValidFindMyData = true;

    if (payload_len > 0 && payload_len < 64) {
        memcpy(newDev.payload, payload, payload_len);
        newDev.payloadLength = payload_len;
    }

    airtagDevices.push_back(newDev);
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            isScanning = true;
            esp_ble_gap_start_scanning(scanDuration);
            lastScanTime = millis();
        }
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) isScanning = false;
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            process_scan_result(param);
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            lastScanTime = millis();
            scanCompleted = true;
            if (isLocateMode) {
                isScanning = true;
                esp_ble_gap_start_scanning(scanDuration);
            } else {
                isScanning = false;
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        isScanning = false;
        scanCompleted = true;
        break;
    default:
        break;
    }
}

void airtagDetectorSetup() {
    airtagDevices.clear();
    airtagDevices.reserve(MAX_DEVICES);
    currentIndex = listStartIndex = 0;
    isDetailView = false;
    isLocateMode = false;
    memset(locateTargetAddress, 0, sizeof(locateTargetAddress));
    lastButtonPress = 0;
    isScanning = true;
    scanCompleted = false;
    needsRedraw = true;
    lastDeviceCount = 0;
    
    u8g2.begin();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.clearBuffer();
    u8g2.drawStr(0, 10, "Scanning for");
    u8g2.drawStr(0, 20, "AirTags...");
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);

    initBLE();
    esp_ble_gap_register_callback(esp_gap_cb);
    esp_ble_gap_set_scan_params(&ble_scan_params);
    bleInitialized = true;

    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_CENTER, INPUT_PULLUP);
}

void airtagDetectorLoop() {
    if (!bleInitialized) return;
    unsigned long now = millis();

    if (isScanning && !isLocateMode) {
        if (lastDeviceCount != (int)airtagDevices.size() || wasScanning != isScanning) {
            lastDeviceCount = (int)airtagDevices.size();
            wasScanning = isScanning;
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.drawStr(0, 10, "Scanning for");
            u8g2.drawStr(0, 20, "AirTags...");
            char countStr[32];
            snprintf(countStr, sizeof(countStr), "%d/%d devices", (int)airtagDevices.size(), MAX_DEVICES);
            u8g2.drawStr(0, 35, countStr);
            u8g2.sendBuffer();
            displayMirrorSend(u8g2);
        }
        return;
    }

    if (wasScanning != isScanning) {
        wasScanning = isScanning;
        needsRedraw = true;
    }

    unsigned long effectiveScanInterval = scanInterval;
    uint32_t effectiveScanDuration = scanDuration;
    if (airtagDevices.empty() && isContinuousScanEnabled()) {
        effectiveScanInterval = 500;
        effectiveScanDuration = 3;
    }

    if (!isScanning && scanCompleted && now - lastScanTime > effectiveScanInterval && !isDetailView && !isLocateMode) {
        if (airtagDevices.size() >= MAX_DEVICES) {
            int devicesToRemove = MAX_DEVICES / 4;
            if (devicesToRemove > 0) airtagDevices.erase(airtagDevices.begin(), airtagDevices.begin() + devicesToRemove);
            currentIndex = listStartIndex = 0;
        }
        scanCompleted = false;
        isScanning = true;
        esp_ble_gap_start_scanning(effectiveScanDuration);
        lastScanTime = now;
        return;
    }

    if (scanCompleted && now - lastButtonPress > debounceTime) {
        if (!isDetailView && !isLocateMode && digitalRead(BTN_UP) == LOW && currentIndex > 0) {
            --currentIndex;
            if (currentIndex < listStartIndex) --listStartIndex;
            lastButtonPress = now; needsRedraw = true;
        } else if (!isDetailView && !isLocateMode && digitalRead(BTN_DOWN) == LOW && currentIndex < (int)airtagDevices.size() - 1) {
            ++currentIndex;
            if (currentIndex >= listStartIndex + 5) ++listStartIndex;
            lastButtonPress = now; needsRedraw = true;
        } else if (!isDetailView && !isLocateMode && digitalRead(BTN_CENTER) == LOW && !airtagDevices.empty()) {
            isDetailView = true;
            if (isScanning) { esp_ble_gap_stop_scanning(); isScanning = false; }
            lastButtonPress = now; needsRedraw = true;
        } else if (isDetailView && !isLocateMode && digitalRead(BTN_CENTER) == LOW && !airtagDevices.empty()) {
            isLocateMode = true;
            strncpy(locateTargetAddress, airtagDevices[currentIndex].address, sizeof(locateTargetAddress) - 1);
            if (!isScanning) { isScanning = true; esp_ble_gap_start_scanning(scanDuration); }
            lastButtonPress = now; lastLocateUpdate = now; needsRedraw = true;
        } else if (isLocateMode && digitalRead(BTN_BACK) == LOW) {
            isLocateMode = false;
            memset(locateTargetAddress, 0, sizeof(locateTargetAddress));
            if (isScanning) { esp_ble_gap_stop_scanning(); isScanning = false; }
            lastButtonPress = now; needsRedraw = true;
            while (digitalRead(BTN_BACK) == LOW) delay(10);
        } else if (isDetailView && !isLocateMode && digitalRead(BTN_BACK) == LOW) {
            isDetailView = false;
            if (isScanning) { esp_ble_gap_stop_scanning(); isScanning = false; }
            lastButtonPress = now; needsRedraw = true;
            while (digitalRead(BTN_BACK) == LOW) delay(10);
        }
    }

    if (airtagDevices.empty()) {
        currentIndex = listStartIndex = 0;
        isDetailView = false; isLocateMode = false;
        memset(locateTargetAddress, 0, sizeof(locateTargetAddress));
    } else {
        currentIndex = constrain(currentIndex, 0, (int)airtagDevices.size() - 1);
        listStartIndex = constrain(listStartIndex, 0, max(0, (int)airtagDevices.size() - 5));
    }

    if ((isDetailView || isLocateMode) && now - lastLocateUpdate >= locateUpdateInterval) {
        lastLocateUpdate = now; needsRedraw = true;
    }

    if (!needsRedraw) return;
    needsRedraw = false;
    u8g2.clearBuffer();
    
    // UI rendering remains identical to your original, just relying on the new parsed data
    if (airtagDevices.empty()) {
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawStr(0, 10, "No AirTags found");
        u8g2.drawStr(0, 30, "Scanning...");
    } else if (isLocateMode) {
        auto &dev = airtagDevices[currentIndex];
        u8g2.setFont(u8g2_font_5x8_tr);
        char buf[32];
        snprintf(buf, sizeof(buf), "RSSI: %d dBm", dev.rssi);
        u8g2.drawStr(0, 28, buf);
        snprintf(buf, sizeof(buf), "Motion: %s", (dev.statusByte & 0x10) ? "YES" : "NO");
        u8g2.drawStr(0, 38, buf);
    } else if (isDetailView) {
        auto &dev = airtagDevices[currentIndex];
        u8g2.setFont(u8g2_font_5x8_tr);
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr: %s", dev.address);
        u8g2.drawStr(0, 20, buf);
        snprintf(buf, sizeof(buf), "Key Valid: %s", dev.hasValidFindMyData ? "YES" : "NO");
        u8g2.drawStr(0, 30, buf);
        u8g2.drawStr(0, 60, "L=Back SEL=Locate");
    } else {
        u8g2.setFont(u8g2_font_6x10_tr);
        char header[32];
        snprintf(header, sizeof(header), "AirTags: %d/%d", (int)airtagDevices.size(), MAX_DEVICES);
        u8g2.drawStr(0, 10, header);
        for (int i = 0; i < 5; ++i) {
            int idx = listStartIndex + i;
            if (idx >= (int)airtagDevices.size()) break;
            auto &d = airtagDevices[idx];
            if (idx == currentIndex) u8g2.drawStr(0, 20 + i * 10, ">");
            char line[32];
            snprintf(line, sizeof(line), "%.8s | RSSI %d", d.name, d.rssi);
            u8g2.drawStr(10, 20 + i * 10, line);
        }
    }
    u8g2.sendBuffer();
    displayMirrorSend(u8g2);
}