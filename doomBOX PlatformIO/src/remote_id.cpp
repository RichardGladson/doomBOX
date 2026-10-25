// doomBOX Superscanner — ASTM F3411 Remote ID decoder (BLE legacy form)
// See remote_id.h for scope and provenance.
#include "remote_id.h"
#include <string.h>

namespace RemoteId {

// ---- the wire format ----------------------------------------------------
// A Bluetooth Legacy advert is a run of AD structures, each one a length
// byte, a type byte, then length-1 bytes of data. Remote ID rides in the
// Service Data structure (type 0x16) under UUID 0xFFFA, and inside that
// sits a one-byte application code, a one-byte message counter, then
// exactly 25 bytes of message.
static const uint8_t AD_SERVICE_DATA_16 = 0x16;
static const uint16_t ODID_UUID         = 0xFFFA;
static const uint8_t ODID_APP_CODE      = 0x0D;
static const uint8_t MSG_SIZE           = 25;

// Message types live in the TOP nibble of byte 0. The bottom nibble is the
// protocol version, which is not checked here: the field layouts read have
// been stable across every published version, and refusing to decode a
// drone because it advertised version 3 would be worse than decoding it.
static const uint8_t MSG_BASIC_ID = 0x0;
static const uint8_t MSG_LOCATION = 0x1;
static const uint8_t MSG_SYSTEM   = 0x4;

// Fixed-point scales, from the reference implementation (opendroneid-core-c):
// coordinates are degrees x 10^7, and altitude is half-metres offset by
// 1000m so that below-sea-level values still fit an unsigned 16-bit field.
static const double LATLON_MULT = 10000000.0;
static const float  ALT_DIV     = 0.5f;
static const float  ALT_ADDER   = 1000.0f;

// Little-endian, because the standard's structs are packed and every
// device implementing it is little-endian. Read byte by byte rather than
// cast: the payload comes off the radio unaligned, and a 32-bit load at an
// odd address is a LoadStoreError exception on the ESP32 rather than a
// slow-but-safe read like it would be on a PC.
static int32_t rd32(const uint8_t* p) {
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

const char* uaTypeName(uint8_t t) {
  switch (t) {
    case 1:  return "PLANE";
    case 2:  return "QUAD";
    case 3:  return "HELI";
    case 4:  return "GYRO";
    case 5:  return "VTOL";
    case 6:  return "ORNITH";
    case 7:  return "GLIDER";
    case 8:  return "KITE";
    case 9:  return "FREEBAL";
    case 10: return "CAPBAL";
    case 11: return "AIRSHIP";
    case 12: return "PARA";
    case 13: return "ROCKET";
    case 14: return "TETHER";
    case 15: return "GROUND";
    default: return "UNKNOWN";
  }
}

void reset(Info& out) { out = Info(); }

// Decodes one 25-byte message body into `out`.
static bool mergeMessage(const uint8_t* m, Info& out, uint32_t now) {
  const uint8_t type = (uint8_t)((m[0] >> 4) & 0x0F);

  if (type == MSG_BASIC_ID) {
    // Byte 1 splits IDType (high) from UAType (low); the serial is the
    // next 20 bytes, space- or NUL-padded rather than terminated.
    out.uaType = (uint8_t)(m[1] & 0x0F);
    memcpy(out.serial, m + 2, 20);
    out.serial[20] = '\0';
    for (int i = 19; i >= 0; i--) {
      if (out.serial[i] == ' ' || out.serial[i] == '\0') out.serial[i] = '\0';
      else break;
    }
    out.haveBasic = true;
    out.at = now;
    return true;
  }

  if (type == MSG_LOCATION) {
    const int32_t la = rd32(m + 5);
    const int32_t lo = rd32(m + 9);
    // 0,0 is the standard's "unknown", and it is also a real place, so
    // treating it as no-fix is the lesser wrong of the two.
    if (la == 0 && lo == 0) return true;
    out.lat  = (float)((double)la / LATLON_MULT);
    out.lon  = (float)((double)lo / LATLON_MULT);
    out.altM = (float)rd16(m + 15) * ALT_DIV - ALT_ADDER;
    out.haveLoc = true;
    out.at = now;
    return true;
  }

  if (type == MSG_SYSTEM) {
    // Bytes 2-9 are where the operator (pilot) is standing -- the aircraft's
    // own location is reported separately in the Location message above.
    const int32_t la = rd32(m + 2);
    const int32_t lo = rd32(m + 6);
    if (la == 0 && lo == 0) return true;
    out.opLat = (float)((double)la / LATLON_MULT);
    out.opLon = (float)((double)lo / LATLON_MULT);
    out.haveOperator = true;
    out.at = now;
    return true;
  }

  // Auth, Self-ID, Operator-ID and Message Pack all reach here. Pack (0xF)
  // is the one worth naming: it wraps several messages at once, which is
  // what the WiFi Beacon form uses (not implemented in this port -- see
  // remote_id.h), but it does not fit in a 31-byte legacy advert, so
  // nothing arriving on the BLE path handled here can be one.
  return false;
}

// The Remote ID service data in a legacy advert: the pointer just past the
// AD type (UUID, app code, counter, message) and its size, or null.
static const uint8_t* findServiceData(const uint8_t* payload, uint8_t len, uint8_t& size) {
  if (!payload || len < 4) return nullptr;
  uint8_t i = 0;
  while (i < len) {
    const uint8_t adLen = payload[i];
    if (adLen == 0) break;
    if ((uint16_t)i + 1u + adLen > (uint16_t)len) break;
    const uint8_t  adType = payload[i + 1];
    const uint8_t* adData = payload + i + 2;
    const uint8_t  adSize = (uint8_t)(adLen - 1);
    if (adType == AD_SERVICE_DATA_16 && adSize >= 3 &&
        rd16(adData) == ODID_UUID && adData[2] == ODID_APP_CODE) {
      size = adSize;
      return adData;
    }
    i = (uint8_t)(i + 1 + adLen);
  }
  return nullptr;
}

bool present(const uint8_t* payload, uint8_t len) {
  uint8_t size = 0;
  return findServiceData(payload, len, size) != nullptr;
}

bool merge(const uint8_t* payload, uint8_t len, Info& out, uint32_t now) {
  // UUID, app code, counter, then the message itself.
  uint8_t size = 0;
  const uint8_t* d = findServiceData(payload, len, size);
  if (!d || size < 4 + MSG_SIZE) return false;
  return mergeMessage(d + 4, out, now);
}

}  // namespace RemoteId
