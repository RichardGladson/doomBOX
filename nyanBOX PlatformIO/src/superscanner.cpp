/*
    doomBOX Superscanner
    Continuously scans for hacking tools, security tools, and gadgets.
    Presets filter which device types are matched.
*/

#include <Arduino.h>
#include <U8g2lib.h>
#include <vector>
#include <cstring>
#include <WiFi.h>
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_bt_main.h"
#include "esp_wifi.h"

#include "../include/superscanner.h"
#include "../include/radio_manager.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "../include/pindefs.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// ---------------------------------------------------------------------------
// Device types & presets
// ---------------------------------------------------------------------------
enum SuperDeviceType {
  DEV_FLIPPER = 0,
  DEV_PINEAPPLE,
  DEV_PWNAGOTCHI,
  DEV_NYANBOX,
  DEV_AXON,
  DEV_MESHTASTIC,
  DEV_MESHCORE,
  DEV_SKIMMER,
  DEV_AIRTAG,
  DEV_SMARTTAG,
  DEV_TILE,
  DEV_RAYBAN,
  DEV_DRONE,
  DEV_FLOCK,
  DEV_TYPE_COUNT
};

static const char* deviceTypeNames[] = {
  "Flipper Zero",
  "Pineapple",
  "Pwnagotchi",
  "nyanBOX",
  "Axon",
  "Meshtastic",
  "MeshCore",
  "Skimmer",
  "AirTag",
  "SmartTag",
  "Tile",
  "Ray-Ban",
  "Drone",
  "Flock"
};

enum SuperPreset {
  PRESET_HACKING = 0,   // Flipper, Pineapple, Pwnagotchi, nyanBOX
  PRESET_SECURITY,      // Axon, Meshtastic, MeshCore, Skimmer
  PRESET_GADGETS,       // Tile, Ray-Ban, Flock, Drone
  PRESET_ALL,
  PRESET_COUNT
};

static const char* presetTitles[] = {
  "Hacking tools",
  "Security tools",
  "Other gadgets",
  "All"
};

// Bitmask of device types enabled per preset
static uint16_t presetMasks[PRESET_COUNT] = {
  (1 << DEV_FLIPPER) | (1 << DEV_PINEAPPLE) | (1 << DEV_PWNAGOTCHI) | (1 << DEV_NYANBOX),
  (1 << DEV_AXON) | (1 << DEV_MESHTASTIC) | (1 << DEV_MESHCORE) | (1 << DEV_SKIMMER),
  (1 << DEV_TILE) | (1 << DEV_RAYBAN) | (1 << DEV_FLOCK) | (1 << DEV_DRONE),
  0xFFFF  // all
};

// ---------------------------------------------------------------------------
// Found device record
// ---------------------------------------------------------------------------
struct SuperDevice {
  char typeName[20];
  char detail[32];   // MAC or SSID
  int8_t rssi;
  unsigned long lastSeen;
  SuperDeviceType type;
};

static std::vector<SuperDevice> foundDevices;
static const int MAX_FOUND = 40;

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
enum SuperPhase { PHASE_PRESET_SELECT, PHASE_SCANNING };
static SuperPhase phase = PHASE_PRESET_SELECT;
static int presetIndex = 0;
static int listIndex = 0;
static int listStart = 0;
static bool needsRedraw = true;
static unsigned long lastButton = 0;
static const unsigned long debounceMs = 200;
static bool bleReady = false;
static bool scanning = false;
static unsigned long lastWifiScan = 0;
static const unsigned long WIFI_SCAN_INTERVAL = 12000;
static unsigned long lastBleRestart = 0;
static const unsigned long BLE_SCAN_RESTART = 10000;

static uint16_t activeMask = 0;

// Meshtastic 128-bit service UUID (little-endian bytes as used in adv)
static const uint8_t MESHTASTIC_UUID[16] = {
  0x6b, 0x6a, 0x41, 0x6c, 0x69, 0x6e, 0x6b, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
// Actual Meshtastic service UUID is often advertised; also match name "Meshtastic"

static esp_ble_scan_params_t ble_scan_params = {
  .scan_type              = BLE_SCAN_TYPE_ACTIVE,
  .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
  .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
  .scan_interval          = 0x50,
  .scan_window            = 0x30,
  .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void bdaToStr(uint8_t *bda, char *str, size_t size) {
  if (!bda || !str || size < 18) return;
  snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
           bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool typeEnabled(SuperDeviceType t) {
  return (activeMask & (1 << t)) != 0;
}

static void addOrUpdate(SuperDeviceType type, const char* detail, int8_t rssi) {
  if (!typeEnabled(type)) return;
  unsigned long now = millis();

  for (auto &d : foundDevices) {
    if (d.type == type && strcmp(d.detail, detail) == 0) {
      d.rssi = rssi;
      d.lastSeen = now;
      needsRedraw = true;
      return;
    }
  }

  if ((int)foundDevices.size() >= MAX_FOUND) {
    // Drop oldest
    size_t oldest = 0;
    for (size_t i = 1; i < foundDevices.size(); i++) {
      if (foundDevices[i].lastSeen < foundDevices[oldest].lastSeen) oldest = i;
    }
    foundDevices.erase(foundDevices.begin() + oldest);
  }

  SuperDevice d;
  strncpy(d.typeName, deviceTypeNames[type], sizeof(d.typeName) - 1);
  d.typeName[sizeof(d.typeName) - 1] = '\0';
  strncpy(d.detail, detail, sizeof(d.detail) - 1);
  d.detail[sizeof(d.detail) - 1] = '\0';
  d.rssi = rssi;
  d.lastSeen = now;
  d.type = type;
  foundDevices.push_back(d);
  needsRedraw = true;
}

static bool hasUuid16(uint8_t *adv, uint8_t len, uint16_t target) {
  uint8_t ulen = 0;
  uint8_t *data = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_16SRV_CMPL, &ulen);
  if (data && ulen >= 2) {
    for (int i = 0; i + 2 <= ulen; i += 2) {
      uint16_t u = data[i] | (data[i + 1] << 8);
      if (u == target) return true;
    }
  }
  ulen = 0;
  data = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_16SRV_PART, &ulen);
  if (data && ulen >= 2) {
    for (int i = 0; i + 2 <= ulen; i += 2) {
      uint16_t u = data[i] | (data[i + 1] << 8);
      if (u == target) return true;
    }
  }
  return false;
}

static bool getAdvName(uint8_t *adv, uint8_t len, char *out, size_t outSize) {
  uint8_t nlen = 0;
  uint8_t *name = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_CMPL, &nlen);
  if (!name || nlen == 0) {
    nlen = 0;
    name = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_SHORT, &nlen);
  }
  if (!name || nlen == 0 || nlen >= outSize) {
    out[0] = '\0';
    return false;
  }
  memcpy(out, name, nlen);
  out[nlen] = '\0';
  return true;
}

static bool isAirTagPayload(uint8_t *payload, uint8_t payload_len) {
  if (!payload || payload_len < 4) return false;
  for (int i = 0; i <= payload_len - 4; i++) {
    if (payload[i] == 0x1E && payload[i+1] == 0xFF &&
        payload[i+2] == 0x4C && payload[i+3] == 0x00) return true;
    if (payload[i] == 0x4C && payload[i+1] == 0x00 &&
        payload[i+2] == 0x12 && payload[i+3] == 0x19) return true;
  }
  return false;
}

// Known skimmer BLE names (subset)
static bool isSkimmerName(const char* name) {
  if (!name || !name[0]) return false;
  static const char* patterns[] = {
    "HC-03", "HC-05", "HC-06", "HC-08", "BT-SPP",
    "JDY-", "MLT-BT", "SKIM", "CardReader", nullptr
  };
  for (int i = 0; patterns[i]; i++) {
    if (strcasestr(name, patterns[i])) return true;
  }
  return false;
}

// Flock SSID patterns
static bool isFlockSsid(const char* ssid) {
  if (!ssid || !ssid[0]) return false;
  static const char* pats[] = { "flock", "FS Ext Battery", "Penguin", "Pigvision", nullptr };
  for (int i = 0; pats[i]; i++) {
    if (strcasestr(ssid, pats[i])) return true;
  }
  return false;
}

// Pineapple OUI check on BSSID string "xx:13:37:..."
static bool isPineappleBssid(const char* bssid) {
  if (!bssid || strlen(bssid) < 8) return false;
  return (strncasecmp(&bssid[3], "13", 2) == 0) &&
         (strncasecmp(&bssid[6], "37", 2) == 0);
}

// ---------------------------------------------------------------------------
// BLE GAP callback – classify advertisements
// ---------------------------------------------------------------------------
static void processBleAdv(esp_ble_gap_cb_param_t *scan_result) {
  uint8_t *adv = scan_result->scan_rst.ble_adv;
  uint8_t advLen = scan_result->scan_rst.adv_data_len;
  int8_t rssi = scan_result->scan_rst.rssi;
  char addr[18];
  bdaToStr(scan_result->scan_rst.bda, addr, sizeof(addr));

  char name[32] = {0};
  getAdvName(adv, advLen, name, sizeof(name));

  // Flipper Zero – UUID 0x308x or MAC 80:e1:26
  if (typeEnabled(DEV_FLIPPER)) {
    bool byMac = (strncasecmp(addr, "80:e1:26", 8) == 0) ||
                 (strncasecmp(addr, "80:e1:27", 8) == 0);
    bool byUuid = hasUuid16(adv, advLen, 0x3081) ||
                  hasUuid16(adv, advLen, 0x3082) ||
                  hasUuid16(adv, advLen, 0x3083);
    if (byMac || byUuid) {
      addOrUpdate(DEV_FLIPPER, addr, rssi);
    }
  }

  // nyanBOX / doomBOX – name contains nyanBOX or doomBOX, or manuf FF FF
  if (typeEnabled(DEV_NYANBOX)) {
    if ((name[0] && (strcasestr(name, "nyanBOX") || strcasestr(name, "doomBOX"))) ||
        (advLen >= 4 && adv[0] == 0xFF && adv[1] == 0xFF)) {
      // Also check manufacturer data via resolve
      uint8_t mlen = 0;
      uint8_t *mdata = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &mlen);
      if ((mdata && mlen >= 2 && mdata[0] == 0xFF && mdata[1] == 0xFF) ||
          (name[0] && (strcasestr(name, "nyanBOX") || strcasestr(name, "doomBOX")))) {
        addOrUpdate(DEV_NYANBOX, addr, rssi);
      }
    }
  }

  // Axon – name match
  if (typeEnabled(DEV_AXON) && name[0] && strcasestr(name, "Axon")) {
    addOrUpdate(DEV_AXON, addr, rssi);
  }

  // Meshtastic
  if (typeEnabled(DEV_MESHTASTIC)) {
    if ((name[0] && strcasestr(name, "Meshtastic")) ||
        hasUuid16(adv, advLen, 0x6BA1)) {  // common short form if present
      addOrUpdate(DEV_MESHTASTIC, addr, rssi);
    } else if (name[0] && strcasestr(name, "mesh")) {
      // broader mesh name match only if Meshtastic-like
      if (strcasestr(name, "Meshtastic") || strcasestr(name, "mt-")) {
        addOrUpdate(DEV_MESHTASTIC, addr, rssi);
      }
    }
  }

  // MeshCore – name
  if (typeEnabled(DEV_MESHCORE) && name[0] &&
      (strcasestr(name, "MeshCore") || strcasestr(name, "meshcore"))) {
    addOrUpdate(DEV_MESHCORE, addr, rssi);
  }

  // Skimmer
  if (typeEnabled(DEV_SKIMMER) && isSkimmerName(name)) {
    addOrUpdate(DEV_SKIMMER, addr, rssi);
  }

  // AirTag
  if (typeEnabled(DEV_AIRTAG) && isAirTagPayload(adv, advLen)) {
    addOrUpdate(DEV_AIRTAG, addr, rssi);
  }

  // SmartTag (Samsung) UUID 0xFD5A
  if (typeEnabled(DEV_SMARTTAG) && hasUuid16(adv, advLen, 0xFD5A)) {
    addOrUpdate(DEV_SMARTTAG, addr, rssi);
  }

  // Tile UUID 0xFEED / 0xFEEC
  if (typeEnabled(DEV_TILE) &&
      (hasUuid16(adv, advLen, 0xFEED) || hasUuid16(adv, advLen, 0xFEEC))) {
    addOrUpdate(DEV_TILE, addr, rssi);
  }

  // Ray-Ban UUID 0xFD5F
  if (typeEnabled(DEV_RAYBAN) && hasUuid16(adv, advLen, 0xFD5F)) {
    addOrUpdate(DEV_RAYBAN, addr, rssi);
  }

  // Drone Remote ID (BLE) – company ID pattern FA FF 0D
  if (typeEnabled(DEV_DRONE) && advLen > 6) {
    for (int i = 0; i + 4 < advLen; i++) {
      if (adv[i] == 0x16 && adv[i+1] == 0xFA && adv[i+2] == 0xFF && adv[i+3] == 0x0D) {
        addOrUpdate(DEV_DRONE, addr, rssi);
        break;
      }
    }
  }
}

static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      esp_ble_gap_start_scanning(0);  // continuous
      scanning = true;
      break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      scanning = true;
      break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        processBleAdv(param);
      }
      break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      scanning = false;
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// WiFi scan pass (Pineapple, Pwnagotchi, Flock, some drones via SSID)
// ---------------------------------------------------------------------------
static void runWifiScanPass() {
  // Use blocking short scan – OK because we call infrequently
  int n = WiFi.scanNetworks(false, true, false, 300);
  if (n <= 0) {
    WiFi.scanDelete();
    return;
  }

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    String bssid = WiFi.BSSIDstr(i);
    int32_t rssi = WiFi.RSSI(i);
    char detail[32];

    // Pineapple
    if (typeEnabled(DEV_PINEAPPLE) && isPineappleBssid(bssid.c_str())) {
      snprintf(detail, sizeof(detail), "%s", bssid.c_str());
      addOrUpdate(DEV_PINEAPPLE, detail, (int8_t)rssi);
    }

    // Pwnagotchi – characteristic MAC de:ad:be:ef:de:ad
    if (typeEnabled(DEV_PWNAGOTCHI) &&
        strcasecmp(bssid.c_str(), "de:ad:be:ef:de:ad") == 0) {
      const char* nm = ssid.length() ? ssid.c_str() : "pwnagotchi";
      snprintf(detail, sizeof(detail), "%s", nm);
      addOrUpdate(DEV_PWNAGOTCHI, detail, (int8_t)rssi);
    }

    // Flock
    if (typeEnabled(DEV_FLOCK) && isFlockSsid(ssid.c_str())) {
      snprintf(detail, sizeof(detail), "%s", ssid.c_str());
      addOrUpdate(DEV_FLOCK, detail, (int8_t)rssi);
    }

    // Drone – common OpenDroneID / DJI-ish SSIDs
    if (typeEnabled(DEV_DRONE) && ssid.length() > 0) {
      if (strcasestr(ssid.c_str(), "DJI") ||
          strcasestr(ssid.c_str(), "RemoteID") ||
          strcasestr(ssid.c_str(), "UAV")) {
        snprintf(detail, sizeof(detail), "%s", ssid.c_str());
        addOrUpdate(DEV_DRONE, detail, (int8_t)rssi);
      }
    }
  }
  WiFi.scanDelete();
}

// ---------------------------------------------------------------------------
// Start / stop BLE scan
// ---------------------------------------------------------------------------
static void startBleScan() {
  if (!bleReady) {
    if (!initBLE()) return;
    esp_ble_gap_register_callback(gapCallback);
    bleReady = true;
  }
  esp_ble_gap_set_scan_params(&ble_scan_params);
  lastBleRestart = millis();
}

static void stopBleScan() {
  if (scanning) {
    esp_ble_gap_stop_scanning();
    scanning = false;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawPresetSelect() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(28, 10, "Superscanner");

  u8g2.setFont(u8g2_font_6x10_tr);
  for (int i = 0; i < PRESET_COUNT; i++) {
    int y = 22 + i * 11;
    if (i == presetIndex) {
      u8g2.drawStr(2, y, ">");
    }
    u8g2.drawStr(12, y, presetTitles[i]);
  }
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(2, 63, "SEL=start  LEFT=back");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawScanScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);

  char header[32];
  snprintf(header, sizeof(header), "SS:%s", presetTitles[presetIndex]);
  u8g2.drawStr(0, 9, header);

  char countStr[16];
  snprintf(countStr, sizeof(countStr), "%d", (int)foundDevices.size());
  u8g2.drawStr(110, 9, countStr);

  // Scanning indicator
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(0, 16, scanning ? "scanning..." : "restarting...");

  u8g2.setFont(u8g2_font_5x8_tr);
  int visible = 5;
  if (listIndex < listStart) listStart = listIndex;
  if (listIndex >= listStart + visible) listStart = listIndex - visible + 1;

  for (int i = 0; i < visible; i++) {
    int idx = listStart + i;
    if (idx >= (int)foundDevices.size()) break;
    int y = 25 + i * 8;
    if (idx == listIndex) {
      u8g2.drawStr(0, y, ">");
    }
    const SuperDevice &d = foundDevices[idx];
    char line[40];
    snprintf(line, sizeof(line), "%s %ddB", d.typeName, d.rssi);
    u8g2.drawStr(8, y, line);
  }

  if (foundDevices.empty()) {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(20, 40, "No devices yet");
  }

  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(0, 63, "UP/DN scroll  LEFT=back");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void superscannerSetup() {
  phase = PHASE_PRESET_SELECT;
  presetIndex = 0;
  listIndex = 0;
  listStart = 0;
  foundDevices.clear();
  needsRedraw = true;
  bleReady = false;
  scanning = false;
  lastButton = 0;
  lastWifiScan = 0;
  activeMask = 0;
}

void superscannerCleanup() {
  stopBleScan();
  cleanupBLE();
  cleanupWiFi();
  bleReady = false;
  foundDevices.clear();
}

void superscannerLoop() {
  unsigned long now = millis();
  bool up = digitalRead(BUTTON_PIN_UP) == LOW;
  bool down = digitalRead(BUTTON_PIN_DOWN) == LOW;
  bool sel = digitalRead(BUTTON_PIN_CENTER) == LOW;
  bool left = digitalRead(BUTTON_PIN_LEFT) == LOW;

  if (now - lastButton < debounceMs) {
    // still draw if needed
  } else {
    if (phase == PHASE_PRESET_SELECT) {
      if (up) {
        lastButton = now;
        presetIndex = (presetIndex - 1 + PRESET_COUNT) % PRESET_COUNT;
        needsRedraw = true;
      } else if (down) {
        lastButton = now;
        presetIndex = (presetIndex + 1) % PRESET_COUNT;
        needsRedraw = true;
      } else if (sel) {
        lastButton = now;
        activeMask = presetMasks[presetIndex];
        foundDevices.clear();
        listIndex = 0;
        listStart = 0;
        phase = PHASE_SCANNING;
        needsRedraw = true;

        // Bring up WiFi STA for scanNetworks
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(50);
        startBleScan();
        lastWifiScan = now;
      }
      // LEFT is handled by main runApp (select button to exit)
    } else {
      // PHASE_SCANNING
      if (up) {
        lastButton = now;
        if (listIndex > 0) {
          listIndex--;
          needsRedraw = true;
        }
      } else if (down) {
        lastButton = now;
        if (listIndex < (int)foundDevices.size() - 1) {
          listIndex++;
          needsRedraw = true;
        }
      }

      // Periodic WiFi scan pass
      if (now - lastWifiScan >= WIFI_SCAN_INTERVAL) {
        lastWifiScan = now;
        // Briefly stop BLE to reduce RF contention (optional)
        runWifiScanPass();
      }

      // Keep BLE scanning alive
      if (!scanning && now - lastBleRestart > 2000) {
        startBleScan();
      }
    }
  }

  if (needsRedraw) {
    needsRedraw = false;
    if (phase == PHASE_PRESET_SELECT) {
      drawPresetSelect();
    } else {
      drawScanScreen();
    }
  }
}
