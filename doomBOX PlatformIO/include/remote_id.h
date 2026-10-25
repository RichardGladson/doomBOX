// doomBOX Superscanner — ASTM F3411 "Remote ID" decoder (BLE legacy form)
//
// Ported from SquatchWatch-CYD's remote_id.h/.cpp (GPLv3), which credits
// opendroneid-core-c (https://github.com/opendroneid/opendroneid-core-c) as
// the source for every offset and constant below.
//
// What this decodes, and why it matters more than a presence flag: the
// DEV_DRONE check before this port only tested for the company-ID bytes
// "16 FA FF 0D" in the advert -- i.e. "something ASTM-compliant is
// broadcasting nearby," with no further detail. The advert actually
// carries one of several message types, rotated over a second or two:
//   - Basic ID: the aircraft's serial number and type (quad/plane/heli/...)
//   - Location: the aircraft's own GPS position and altitude
//   - System:   the OPERATOR's GPS position -- where the person flying it
//               is standing, not the aircraft
// This is a standardized, publicly-broadcast safety field (the FAA/ASTM
// standard requires it precisely so the public and authorities can
// identify who is flying a given aircraft), not something this decoder
// discovers by accident -- the same category of thing as reading an
// aircraft's ADS-B transponder. It's still the most sensitive single field
// this project surfaces, so it's worth being deliberate about how it's
// shown in the UI rather than letting it appear as an incidental side
// effect of this port.
//
// Scope, honestly, matching SquatchWatch's own caveat: this decodes the
// Bluetooth LEGACY advertising form only (AD type 0x16 service data under
// UUID 0xFFFA, one message per advert). It does NOT decode the WiFi Beacon
// form here -- that requires raw 802.11 frame access via
// esp_wifi_set_promiscuous(), which this project's WiFi path does not use
// (WiFi.scanNetworks() discards vendor information elements before this
// code ever sees them). Adding promiscuous-mode WiFi sniffing is a
// separate architectural change, not a drop-in addition to this file.
// It also does not decode Bluetooth 5 Long Range / Coded PHY (the ESP32
// BLE stack this project targets doesn't support it in hardware), nor the
// WiFi NAN form (rides action frames, not beacons or scan results either).
#pragma once
#include <stdint.h>

namespace RemoteId {

// One aircraft, assembled over several adverts. A drone does not send
// everything at once -- Basic ID, Location and System are broadcast in
// rotation, so this fills in over a second or two. Each `have` flag says
// whether that part has actually arrived; a field that hasn't arrived yet
// must not be drawn as zero, since (0,0) is a real coordinate (Gulf of
// Guinea) as well as the standard's "unknown" sentinel.
struct Info {
  bool     haveBasic    = false;
  bool     haveLoc      = false;
  bool     haveOperator = false;
  char     serial[21]   = {0};   // 20 bytes + terminator
  uint8_t  uaType        = 0;
  float    lat = 0, lon = 0;     // the aircraft, degrees
  float    altM          = 0;    // geodetic altitude, metres
  float    opLat = 0, opLon = 0; // the operator/pilot, degrees
  uint32_t at             = 0;   // millis() of the last message merged in
};

// Short aircraft-type label ("QUAD", "PLANE", "HELI", ...) for a narrow
// display column.
const char* uaTypeName(uint8_t t);

// Pulls one Remote ID message out of a raw BLE advertisement and merges it
// into `out`, leaving fields the message didn't carry alone. Returns true
// if this advert actually contained a Remote ID message. Call once per
// advert for a given MAC; keep one Info per aircraft (keyed by MAC, same
// as every other tracked device in this project) and feed every advert
// from that MAC through it.
bool merge(const uint8_t* payload, uint8_t len, Info& out, uint32_t now);

// Whether a legacy advert carries Remote ID at all, without decoding it.
// Cheap presence check for a path that just wants to flag DEV_DRONE; merge()
// is still called separately to actually fill in Info.
bool present(const uint8_t* payload, uint8_t len);

void reset(Info& out);

}  // namespace RemoteId
