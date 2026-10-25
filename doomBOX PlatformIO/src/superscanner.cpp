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
#include "signatures.h"
#include "remote_id.h"

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
  // --- Phase 2 additions ---
  DEV_CAMERA,    // general consumer/commercial cameras (Wyze, Hikvision, Axis, Arlo, Blink, etc.)
  DEV_RING,      // Ring LLC specifically -- SquatchWatch splits this from generic CAMERA; kept separate per that precedent
  DEV_ALPR,      // automated license-plate readers (Motorola Solutions/Vigilant, Genetec AutoVu)
  DEV_RAVEN,     // gunshot-detection sensors (ShotSpotter/SoundThinking)
  // --- Phase 3+ addition ---
  DEV_FMDN,      // Google Find Hub + Chipolo -- both participate in Google's
                 // Find My Device Network rather than running separate
                 // ecosystems, so one type per SquatchWatch-CYD's own
                 // GOOGLE_TAG modeling (user decision: single type, not split)
  DEV_TYPE_COUNT
};

// NOTE: DEV_TYPE_COUNT is now 19. presetMasks/activeMask below were widened
// from uint16_t to uint32_t in Phase 2 to accommodate 18 types; still fits
// comfortably with DEV_FMDN added (bit 18 of 32).
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
  "Flock",
  "Camera",
  "Ring",
  "ALPR",
  "Gunshot Sensor",
  "Find My Tag"  // DEV_FMDN: Google Find Hub + Chipolo (Find My Device Network)
};

// Preset scheme redesigned per user request: 4 categories, every device
// type placed exactly once across the 3 non-ALL presets (no orphans -- the
// previous scheme left DEV_SMARTTAG and DEV_AIRTAG in no preset at all,
// only reachable via PRESET_ALL; that gap is closed here).
enum SuperPreset {
  PRESET_TRACKERS = 0,  // Cameras, law-enforcement sensors, personal trackers:
                         // Camera, Ring, Axon, Flock, ALPR, Raven, AirTag,
                         // SmartTag, Tile, FMDN (10 types)
  PRESET_HACKING,        // Hacking tools + skimmers: Flipper, Pineapple,
                         // Pwnagotchi, nyanBOX, Skimmer (5 types)
  PRESET_OTHER,          // Drone, Ray-Ban, MeshCore, Meshtastic (4 types)
  PRESET_ALL,
  PRESET_COUNT
};

static const char* presetTitles[] = {
  "Trackers",
  "Hacking tools",
  "Other",
  "All"
};

// Bitmask of device types enabled per preset. uint32_t (see prior phase
// notes): DEV_TYPE_COUNT is 18, a 16-bit mask would silently drop bits
// 16-17 (DEV_ALPR/DEV_RAVEN) from every mask including "all".
static uint32_t presetMasks[PRESET_COUNT] = {
  // Trackers: cameras, LE sensors, personal trackers
  (1UL << DEV_CAMERA) | (1UL << DEV_RING) |
  (1UL << DEV_AXON) | (1UL << DEV_FLOCK) | (1UL << DEV_ALPR) | (1UL << DEV_RAVEN) |
  (1UL << DEV_AIRTAG) | (1UL << DEV_SMARTTAG) | (1UL << DEV_TILE) | (1UL << DEV_FMDN),

  // Hacking tools + skimmers
  (1UL << DEV_FLIPPER) | (1UL << DEV_PINEAPPLE) | (1UL << DEV_PWNAGOTCHI) |
  (1UL << DEV_NYANBOX) | (1UL << DEV_SKIMMER),

  // Other: drone, Ray-Ban, mesh radios
  (1UL << DEV_DRONE) | (1UL << DEV_RAYBAN) | (1UL << DEV_MESHCORE) | (1UL << DEV_MESHTASTIC),

  0xFFFFFFFFUL  // all -- DEV_TYPE_COUNT is 18, fits comfortably in 32 bits
};

// ---------------------------------------------------------------------------
// Found device record
// ---------------------------------------------------------------------------
struct SuperDevice {
  char typeName[20];
  char detail[32];       // what's shown: MAC/SSID normally, a Remote ID summary for DEV_DRONE
  char rssiKeyShadow[32]; // stable identity for dedup -- equals detail for every existing
                          // call site (so their behavior is unchanged), but for DEV_DRONE
                          // this holds the MAC specifically, so detail can keep changing
                          // (MAC -> "TYPE SERIAL" -> "TYPE SERIAL ALTm") as Remote ID fields
                          // arrive without each change reading as a new device. Sized to
                          // match detail[32] -- some existing callers pass SSIDs up to 32
                          // chars as detail/matchKey (Flock, Pineapple, drone-by-SSID), and
                          // a shorter key here would silently truncate and could collide
                          // two distinct long SSIDs into one dedup bucket.
  int8_t rssi;
  unsigned long lastSeen;
  SuperDeviceType type;
};

static std::vector<SuperDevice> foundDevices;
static const int MAX_FOUND = 40;

// ---------------------------------------------------------------------------
// Remote ID tracking (Phase 3+)
// ---------------------------------------------------------------------------
// A separate small side-table rather than fields on SuperDevice: Remote ID
// carries meaningfully more state (serial, altitude, the operator's own
// coordinates) than any other tracked device, and every other DEV_* type
// would carry that struct's weight in the vector for nothing. Keyed by MAC
// like everything else; capped and aged independently of MAX_FOUND so a
// handful of drones can't starve the main device list or vice versa.
struct RidTrack {
  char addr[18];
  RemoteId::Info info;
};
static std::vector<RidTrack> ridTracks;
static const int MAX_RID_TRACKS = 8;  // small on purpose: few aircraft are ever overhead at once

static RidTrack& ridTrackFor(const char* addr) {
  for (auto &t : ridTracks) {
    if (strcmp(t.addr, addr) == 0) return t;
  }
  if ((int)ridTracks.size() >= MAX_RID_TRACKS) {
    // Drop the stalest track to make room, same policy as addOrUpdate()'s
    // overflow handling for foundDevices.
    size_t oldest = 0;
    for (size_t i = 1; i < ridTracks.size(); i++) {
      if (ridTracks[i].info.at < ridTracks[oldest].info.at) oldest = i;
    }
    ridTracks.erase(ridTracks.begin() + oldest);
  }
  RidTrack fresh;
  strncpy(fresh.addr, addr, sizeof(fresh.addr) - 1);
  fresh.addr[sizeof(fresh.addr) - 1] = '\0';
  RemoteId::reset(fresh.info);
  ridTracks.push_back(fresh);
  return ridTracks.back();
}

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

static uint32_t activeMask = 0;  // widened alongside presetMasks -- see note above DEV_TYPE_COUNT's definition

// Which device types kOuiTable can actually match (see signatures.cpp):
// DEV_CAMERA, DEV_RING, DEV_AXON, DEV_ALPR, DEV_RAVEN, DEV_DRONE, DEV_SKIMMER.
// lookupOui() is an unconditional 78-entry linear walk; without this guard
// it ran on every single BLE advert and every WiFi scan result regardless
// of whether any OUI-backed type was even enabled in the active preset --
// with the "Hacking tools" and "Other" presets (neither includes an
// OUI-backed type except Skimmer in Hacking tools), most of that work was
// pure waste. Checked once per advert/result as `activeMask & kOuiRelevantMask`
// before calling lookupOui() at all, rather than only filtering its result
// afterward via typeEnabled().
static const uint32_t kOuiRelevantMask =
  (1UL << DEV_CAMERA) | (1UL << DEV_RING) | (1UL << DEV_AXON) |
  (1UL << DEV_ALPR) | (1UL << DEV_RAVEN) | (1UL << DEV_DRONE) | (1UL << DEV_SKIMMER);

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
  return (activeMask & (1UL << t)) != 0;
}

// matchKey is what identifies "the same device" across repeated sightings.
// Every existing call site passes matchKey == detail (dedup by the string
// actually shown), preserving current behavior exactly. The drone path
// below is the one exception: its detail string changes as more Remote ID
// fields arrive (MAC -> "TYPE SERIAL" -> "TYPE SERIAL ALTm"), so matching
// on detail would read each change as a new device rather than an update
// to the same one. Passing the MAC as matchKey while detail carries the
// current summary fixes that without touching any other call site's
// behavior.
static void addOrUpdateKeyed(SuperDeviceType type, const char* matchKey,
                              const char* detail, int8_t rssi) {
  if (!typeEnabled(type)) return;
  unsigned long now = millis();

  for (auto &d : foundDevices) {
    if (d.type == type && strcmp(d.rssiKeyShadow, matchKey) == 0) {
      d.rssi = rssi;
      d.lastSeen = now;
      strncpy(d.detail, detail, sizeof(d.detail) - 1);
      d.detail[sizeof(d.detail) - 1] = '\0';
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
  strncpy(d.rssiKeyShadow, matchKey, sizeof(d.rssiKeyShadow) - 1);
  d.rssiKeyShadow[sizeof(d.rssiKeyShadow) - 1] = '\0';
  d.rssi = rssi;
  d.lastSeen = now;
  d.type = type;
  foundDevices.push_back(d);
  needsRedraw = true;
}

static void addOrUpdate(SuperDeviceType type, const char* detail, int8_t rssi) {
  addOrUpdateKeyed(type, detail, detail, rssi);
}

// ---------------------------------------------------------------------------
// Single-pass advertisement parse
// ---------------------------------------------------------------------------
// Every device-type check used to call esp_ble_resolve_adv_data() on its own
// (name lookup, complete-UUID list, partial-UUID list, manufacturer data),
// each one a fresh linear walk over the same AD structures. With up to 14
// device types enabled at once (PRESET_ALL), that was dozens of redundant
// walks over the same handful of bytes for every single advertisement the
// radio handed over -- on the task that's also servicing the BLE stack.
//
// This walks the AD structures exactly once per advertisement and pulls out
// everything any matcher below needs: the device name, every 16-bit service
// UUID (complete or partial list, since callers never cared which), and the
// manufacturer-data block. Matchers then just read from this struct instead
// of re-parsing.
#define MAX_PARSED_UUIDS 12

struct ParsedAdv {
  char name[32];
  uint16_t uuids[MAX_PARSED_UUIDS];
  uint8_t uuidCount;
  const uint8_t* mfgData;   // points into the original adv buffer; not owned
  uint8_t mfgLen;
};

static void parseAdv(uint8_t *adv, uint8_t len, ParsedAdv &out) {
  out.name[0] = '\0';
  out.uuidCount = 0;
  out.mfgData = nullptr;
  out.mfgLen = 0;

  if (!adv || len == 0) return;

  for (uint8_t i = 0; i + 1 < len; ) {
    const uint8_t fieldLen = adv[i];
    if (fieldLen == 0 || i + 1 + fieldLen > len) break;  // malformed, stop rather than overrun
    const uint8_t adType = adv[i + 1];
    const uint8_t *data = &adv[i + 2];
    const uint8_t dataLen = fieldLen - 1;  // fieldLen includes the type byte

    switch (adType) {
      case ESP_BLE_AD_TYPE_NAME_CMPL:
      case ESP_BLE_AD_TYPE_NAME_SHORT:
        // Prefer the complete name if we see both; only take SHORT if we
        // haven't already captured a name.
        if (out.name[0] == '\0' || adType == ESP_BLE_AD_TYPE_NAME_CMPL) {
          uint8_t n = dataLen;
          if (n >= sizeof(out.name)) n = sizeof(out.name) - 1;
          memcpy(out.name, data, n);
          out.name[n] = '\0';
        }
        break;

      case ESP_BLE_AD_TYPE_16SRV_CMPL:
      case ESP_BLE_AD_TYPE_16SRV_PART:
        for (uint8_t j = 0; j + 2 <= dataLen && out.uuidCount < MAX_PARSED_UUIDS; j += 2) {
          out.uuids[out.uuidCount++] = data[j] | (data[j + 1] << 8);
        }
        break;

      case ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE:
        // Keep the first one; a device sending more than one mfg-data block
        // in a single advert is rare, and every current matcher only wants
        // one.
        if (!out.mfgData) {
          out.mfgData = data;
          out.mfgLen = dataLen;
        }
        break;

      default:
        break;
    }

    i += 1 + fieldLen;
  }
}

static bool parsedHasUuid16(const ParsedAdv &p, uint16_t target) {
  for (uint8_t i = 0; i < p.uuidCount; i++) {
    if (p.uuids[i] == target) return true;
  }
  return false;
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

// isSkimmerName() and isFlockSsid() removed here -- both fully replaced by
// kBleNameTable/kSsidTable lookups (see lookupBleName()/lookupSsidName()
// call sites in processBleAdv()/processWifiScanResults()). Same patterns,
// now data-driven with confidence grades instead of hardcoded in a
// function body.

// Phase 3+: Axon body-worn-camera payload match, replacing the old bare
// "Axon" name-substring check (which is still in kBleNameTable as a LOW-
// confidence fallback -- this is the real signal). Ported from
// SquatchWatch-CYD's isAxonBwcPayload(): the literal bytes "BWCDEVICE"
// appear in the advert, but inside a little-endian 128-bit UUID field
// (AD type 0x21), so the byte order comes out reversed on the wire.
// SquatchWatch's comment notes this was owner-confirmed against a real
// device (2026-09-19); checking both directions here since that's the
// documented wire behavior, not a guess.
static bool isAxonBwcPayload(uint8_t *payload, uint8_t payload_len) {
  static const char FWD[] = "BWCDEVICE";
  const uint8_t n = 9;  // strlen("BWCDEVICE"), not counting the terminator
  if (!payload || payload_len < n) return false;
  for (uint8_t i = 0; i + n <= payload_len; i++) {
    bool fwd = true, rev = true;
    for (uint8_t k = 0; k < n && (fwd || rev); k++) {
      if (payload[i + k] != (uint8_t)FWD[k]) fwd = false;
      if (payload[i + k] != (uint8_t)FWD[n - 1 - k]) rev = false;
    }
    if (fwd || rev) return true;
  }
  return false;
}

// Phase 3+: Google Find Hub frame detection. Find Hub rides inside an
// Eddystone-shaped service-data AD structure (0x16, UUID 0xFEAA) but with a
// frame-type byte of 0x40 or 0x41 rather than Eddystone's own frame types
// (0x00 UID, 0x10 URL, 0x20 TLM, 0x30 EID). Checking the frame-type byte
// specifically -- not just "is 0xFEAA present" -- is what distinguishes an
// actual Find Hub tag from any ordinary Eddystone beacon (retail/transit
// beacons use 0xFEAA constantly and are not trackers). Ported from
// SquatchWatch-CYD's isFmdnPayload().
static bool isFmdnPayload(uint8_t *adv, uint8_t advLen) {
  if (!adv || advLen < 4) return false;
  for (uint8_t i = 0; i + 1 < advLen; ) {
    const uint8_t fieldLen = adv[i];
    if (fieldLen == 0 || i + 1 + fieldLen > advLen) break;
    const uint8_t adType = adv[i + 1];
    const uint8_t *data = &adv[i + 2];
    const uint8_t dataLen = fieldLen - 1;
    if (adType == 0x16 && dataLen >= 3 &&
        data[0] == 0xAA && data[1] == 0xFE &&
        (data[2] == 0x40 || data[2] == 0x41)) {
      return true;
    }
    i += 1 + fieldLen;
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

  // Parse the AD structures exactly once. Every check below that needs the
  // name, a 16-bit UUID, or the manufacturer-data block reads from this
  // instead of re-walking the raw advert.
  ParsedAdv p;
  parseAdv(adv, advLen, p);
  const char *name = p.name;

  // OUI table lookup (Phase 2). Unlike the per-type checks below, the
  // matched device type isn't known ahead of time here, so this runs once
  // and checks typeEnabled() against whatever lookupOui() returns -- most
  // BLE-advertising OUI-table entries (cameras, Ring, drones) use a random/
  // resolvable BLE address rather than their WiFi MAC in practice, so in
  // the field this will mostly fire from the WiFi scan path below instead;
  // kept here too since a public BLE address matching the table is still a
  // real, if less common, signal worth catching.
  {
    SuperDeviceType ouiType;
    if ((activeMask & kOuiRelevantMask) &&
        lookupOui(scan_result->scan_rst.bda, &ouiType) && typeEnabled(ouiType)) {
      addOrUpdate(ouiType, addr, rssi);
    }
  }

  // Flipper Zero – UUID 0x308x or registered OUI. Routed through
  // signatures.cpp's tables; raw bda bytes go to lookupMacPrefix() directly
  // rather than round-tripping through the formatted address string.
  if (typeEnabled(DEV_FLIPPER)) {
    bool byMac = lookupMacPrefix(scan_result->scan_rst.bda, DEV_FLIPPER);
    bool byUuid = lookupAnyUuid16(p.uuids, p.uuidCount, DEV_FLIPPER);
    if (byMac || byUuid) {
      addOrUpdate(DEV_FLIPPER, addr, rssi);
    }
  }

  // nyanBOX / doomBOX – name match goes through the table; the manufacturer
  // FF FF pattern stays inline since it's a fixed-byte check on mfg data,
  // not a name/UUID/MAC lookup the table types cover.
  if (typeEnabled(DEV_NYANBOX)) {
    bool byName = lookupBleName(name, DEV_NYANBOX);
    bool byMfg = p.mfgData && p.mfgLen >= 2 && p.mfgData[0] == 0xFF && p.mfgData[1] == 0xFF;
    if (byName || byMfg) {
      addOrUpdate(DEV_NYANBOX, addr, rssi);
    }
  }

  // Axon – real payload signature first (HIGH-grade signal: the literal
  // BWCDEVICE marker), name substring as a LOW-grade fallback for anything
  // that doesn't carry the marker but still says "Axon" in its name.
  if (typeEnabled(DEV_AXON)) {
    if (isAxonBwcPayload(adv, advLen) || lookupBleName(name, DEV_AXON)) {
      addOrUpdate(DEV_AXON, addr, rssi);
    }
  }

  // Meshtastic – name table now covers "Meshtastic" and the "mt-" prefix
  // (both already in kBleNameTable), plus the UUID short-form guess.
  if (typeEnabled(DEV_MESHTASTIC)) {
    bool byName = lookupBleName(name, DEV_MESHTASTIC);
    bool byUuid = lookupAnyUuid16(p.uuids, p.uuidCount, DEV_MESHTASTIC);
    if (byName || byUuid) {
      addOrUpdate(DEV_MESHTASTIC, addr, rssi);
    }
  }

  // MeshCore – name
  if (typeEnabled(DEV_MESHCORE) && lookupBleName(name, DEV_MESHCORE)) {
    addOrUpdate(DEV_MESHCORE, addr, rssi);
  }

  // Skimmer – name table
  if (typeEnabled(DEV_SKIMMER) && lookupBleName(name, DEV_SKIMMER)) {
    addOrUpdate(DEV_SKIMMER, addr, rssi);
  }

  // AirTag – scans the raw advert, not the parsed fields: the Find My
  // pattern can span AD-structure boundaries in a way that doesn't match
  // any single parsed field cleanly (see isAirTagPayload's own comments
  // upstream in SquatchWatch for why this one stays a raw byte scan). Not
  // table-driven since it isn't a plain UUID/name/MAC match.
  if (typeEnabled(DEV_AIRTAG) && isAirTagPayload(adv, advLen)) {
    addOrUpdate(DEV_AIRTAG, addr, rssi);
  }

  // SmartTag (Samsung) UUID 0xFD5A
  if (typeEnabled(DEV_SMARTTAG) && lookupAnyUuid16(p.uuids, p.uuidCount, DEV_SMARTTAG)) {
    addOrUpdate(DEV_SMARTTAG, addr, rssi);
  }

  // Tile UUID 0xFEED / 0xFEEC
  if (typeEnabled(DEV_TILE) && lookupAnyUuid16(p.uuids, p.uuidCount, DEV_TILE)) {
    addOrUpdate(DEV_TILE, addr, rssi);
  }

  // Ray-Ban UUID 0xFD5F
  if (typeEnabled(DEV_RAYBAN) && lookupAnyUuid16(p.uuids, p.uuidCount, DEV_RAYBAN)) {
    addOrUpdate(DEV_RAYBAN, addr, rssi);
  }

  // FMDN (Google Find Hub + Chipolo) – frame-type-qualified Eddystone scan
  // (not table-driven: needs the service-data frame-type byte, which
  // parseAdv() doesn't extract) OR Chipolo's two dedicated UUIDs (which
  // are table-driven).
  if (typeEnabled(DEV_FMDN)) {
    bool byFrame = isFmdnPayload(adv, advLen);
    bool byUuid = lookupAnyUuid16(p.uuids, p.uuidCount, DEV_FMDN);
    if (byFrame || byUuid) {
      addOrUpdate(DEV_FMDN, addr, rssi);
    }
  }

  // Drone Remote ID (BLE legacy form) – Phase 3+: full ASTM F3411 decode
  // via RemoteId::merge(), replacing the old bare company-ID byte scan
  // (which only flagged presence, same bytes RemoteId::present() checks
  // internally, just less precisely -- it didn't verify the app-code byte).
  //
  // detail is built from whichever fields have actually arrived so far
  // (Basic ID and Location rotate in over roughly a second, see
  // remote_id.h): starts as just the MAC like every other device, upgrades
  // to "TYPE SERIAL" once Basic ID lands, then adds altitude once Location
  // lands. The operator/pilot location (haveOperator, opLat/opLon) is
  // decoded and held in ridTracks, but deliberately NOT written into
  // detail or anywhere else the UI reads -- this project currently has no
  // detail/drill-down screen (only the single-line scrolling list), so
  // there's nowhere to show it without adding one, and that's a product
  // decision for a later phase, not something to fold in silently here.
  if (typeEnabled(DEV_DRONE) && RemoteId::present(adv, advLen)) {
    RidTrack &t = ridTrackFor(addr);
    RemoteId::merge(adv, advLen, t.info, millis());

    char detail[32];
    if (t.info.haveBasic && t.info.haveLoc) {
      snprintf(detail, sizeof(detail), "%s %s %.0fm",
                RemoteId::uaTypeName(t.info.uaType), t.info.serial, t.info.altM);
    } else if (t.info.haveBasic) {
      snprintf(detail, sizeof(detail), "%s %s",
                RemoteId::uaTypeName(t.info.uaType), t.info.serial);
    } else {
      snprintf(detail, sizeof(detail), "%s", addr);
    }
    // Keyed by addr (the MAC), not by detail: detail changes as Remote ID
    // fields arrive, and dedup needs to track "same aircraft," not "same
    // string" -- see addOrUpdateKeyed()'s comment.
    addOrUpdateKeyed(DEV_DRONE, addr, detail, rssi);
  }
}

static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      esp_ble_gap_start_scanning(0);  // continuous
      if (!scanning) {
        scanning = true;
        needsRedraw = true;
      }
      break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      if (!scanning) {
        scanning = true;
        needsRedraw = true;
      }
      break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        processBleAdv(param);
      }
      break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      if (scanning) {
        scanning = false;
        needsRedraw = true;
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// WiFi scan pass (Pineapple, Pwnagotchi, Flock, some drones via SSID)
// ---------------------------------------------------------------------------
// Split into start/poll rather than one blocking call. WiFi.scanNetworks()
// with its first argument `false` runs synchronously on the calling task --
// for the ~300ms-per-channel sweep configured below (channel_dwell=300ms
// over up to 14 channels) that's potentially several seconds where
// superscannerLoop() can't read buttons, can't redraw the display, and
// can't service the BLE restart logic below it. The async form (`true`)
// kicks the scan off on the WiFi driver and returns immediately;
// WiFi.scanComplete() is polled each loop() tick to pick up the results
// once they're ready, same as any other non-blocking state check here.
static bool wifiScanPending = false;

static void processWifiScanResults(int n) {
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    String bssid = WiFi.BSSIDstr(i);
    uint8_t* bssidRaw = WiFi.BSSID(i);   // raw 6 bytes, for OUI table lookup
    int32_t rssi = WiFi.RSSI(i);
    char detail[32];

    // OUI table lookup (Phase 2) – this is the primary path for cameras,
    // Ring, ALPR, Raven and the drone-manufacturer OUIs: a WiFi BSSID is
    // the device's real, vendor-assigned MAC far more reliably than a BLE
    // address, which is commonly randomized/resolvable on modern devices.
    if (bssidRaw && (activeMask & kOuiRelevantMask)) {
      SuperDeviceType ouiType;
      if (lookupOui(bssidRaw, &ouiType) && typeEnabled(ouiType)) {
        snprintf(detail, sizeof(detail), "%s", bssid.c_str());
        addOrUpdate(ouiType, detail, (int8_t)rssi);
      }
    }

    // Pineapple – NOT yet routed through kMacPrefixTable. The original
    // isPineappleBssid() checks string offsets 3 and 6 of "xx:yy:zz:...",
    // i.e. octets 2-3, not a true 3-octet OUI prefix (octets 1-3). Folding
    // it into kMacPrefixEntry as-is would require either misrepresenting
    // what it actually checks or quietly widening/narrowing the match --
    // either way, a behavior change Phase 1 isn't supposed to make. Left
    // as its own function; Phase 2 is the right place to decide whether to
    // replace this with SquatchWatch's documented 02:13:37 locally-
    // administered OUI pattern instead (see Phase 1 handoff notes).
    if (typeEnabled(DEV_PINEAPPLE) && isPineappleBssid(bssid.c_str())) {
      snprintf(detail, sizeof(detail), "%s", bssid.c_str());
      addOrUpdate(DEV_PINEAPPLE, detail, (int8_t)rssi);
    }

    // Pwnagotchi – full 6-byte MAC match, not a 3-octet prefix. Kept as its
    // own check rather than forced into kMacPrefixTable's prefix-only shape
    // (a 3-octet "DE:AD:BE" prefix would be strictly broader than what the
    // original code matched, which is a behavior change this phase isn't
    // making).
    if (typeEnabled(DEV_PWNAGOTCHI) &&
        strcasecmp(bssid.c_str(), "de:ad:be:ef:de:ad") == 0) {
      const char* nm = ssid.length() ? ssid.c_str() : "pwnagotchi";
      snprintf(detail, sizeof(detail), "%s", nm);
      addOrUpdate(DEV_PWNAGOTCHI, detail, (int8_t)rssi);
    }

    // Flock – now table-driven
    if (typeEnabled(DEV_FLOCK) && lookupSsidName(ssid.c_str(), DEV_FLOCK)) {
      snprintf(detail, sizeof(detail), "%s", ssid.c_str());
      addOrUpdate(DEV_FLOCK, detail, (int8_t)rssi);
    }

    // Drone – common OpenDroneID / DJI-ish SSIDs, now table-driven
    if (typeEnabled(DEV_DRONE) && lookupSsidName(ssid.c_str(), DEV_DRONE)) {
      snprintf(detail, sizeof(detail), "%s", ssid.c_str());
      addOrUpdate(DEV_DRONE, detail, (int8_t)rssi);
    }
  }
  WiFi.scanDelete();
}

// Kicks off an async scan. Safe to call even if one is already running
// (WiFi.scanNetworks returns WIFI_SCAN_RUNNING immediately in that case
// rather than queueing a second one or blocking).
static void startWifiScanPass() {
  if (wifiScanPending) return;
  WiFi.scanNetworks(true /* async */, true /* show hidden */, false /* no active-scan-only */, 300);
  wifiScanPending = true;
}

// Call every loop() tick while a scan is pending. Returns once results
// (or an error) have been consumed; does nothing otherwise.
static void pollWifiScanPass() {
  if (!wifiScanPending) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;        // still in progress, check again next tick
  if (n == WIFI_SCAN_FAILED) {
    wifiScanPending = false;
    WiFi.scanDelete();
    return;
  }
  // n >= 0: scan finished, n networks found (0 is a valid, completed result)
  processWifiScanResults(n);
  wifiScanPending = false;
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
  // Optimistically mark as scanning so UI doesn't stick on "restarting..."
  // while waiting for the async GAP callback.
  scanning = true;
  needsRedraw = true;
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
  u8g2.drawStr(0, 16, scanning ? "scanning..." : "starting...");

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
  // If a prior session left an async scan in flight (e.g. the user backed
  // out of the scan screen mid-scan), drop it rather than carry the flag
  // forward -- nothing would otherwise call pollWifiScanPass() again to
  // clear it, which would permanently block future startWifiScanPass()
  // calls.
  if (wifiScanPending) {
    WiFi.scanDelete();
    wifiScanPending = false;
  }
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

      // Periodic WiFi scan pass – non-blocking: start() kicks it off,
      // poll() picks up results on whichever later tick they're ready.
      // Neither call stalls superscannerLoop(), so button handling, the
      // BLE-restart check right below, and the display redraw at the
      // bottom of this function all keep running while a scan is in
      // flight.
      if (!wifiScanPending && now - lastWifiScan >= WIFI_SCAN_INTERVAL) {
        lastWifiScan = now;
        startWifiScanPass();
      }
      pollWifiScanPass();

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
