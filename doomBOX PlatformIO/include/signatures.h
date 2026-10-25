/*
    doomBOX Superscanner — signature tables
    Device-detection data, separate from scan/parse logic.

    Phase 1 note: every entry in signatures.cpp right now reproduces a check
    that already existed in superscanner.cpp before this refactor -- same
    UUIDs, same MAC prefixes, same name/SSID substrings. Nothing new is
    detected yet. This phase is "same behavior, new architecture," so it can
    be verified against the pre-refactor file before Phase 2 adds coverage
    on top of it.
*/

#ifndef SUPERSCANNER_SIGNATURES_H
#define SUPERSCANNER_SIGNATURES_H

#include <stdint.h>
#include "superscanner.h"   // SuperDeviceType

// ---------------------------------------------------------------------------
// Confidence
// ---------------------------------------------------------------------------
// What a match is actually worth, given what was matched against:
//
//   HIGH  the identifier is in the vendor's own registered/reserved space
//         (e.g. a BLE service UUID the vendor assigns to this product line),
//         or is otherwise not something a different, unrelated device would
//         plausibly produce by coincidence.
//   MED   the identifier is real and vendor-specific, but belongs to a
//         parent/platform wider than the one product (a company that makes
//         several product lines on the same radio stack), or is one of
//         several interchangeable signals rather than the strongest one.
//   LOW   the identifier is a free-text name/SSID substring, a
//         locally-administered or otherwise unregistered MAC, or a pattern
//         that's plausible but typable/spoofable by anything. A LOW entry
//         is not a mistake -- it's still evidence, just evidence that's
//         weaker and should be shown to the user as weaker, not hidden.
//
// Grading an entry LOW is not a reason to delete it later; it's a reason to
// display it honestly. See signatures.cpp for the grade assigned to each
// entry carried over from the original superscanner.cpp, and why.
enum Confidence {
  LOW_CONF = 0,
  MED_CONF,
  HIGH_CONF
};

// ---------------------------------------------------------------------------
// Table row types
// ---------------------------------------------------------------------------
struct UuidEntry {
  uint16_t uuid;
  SuperDeviceType type;
  Confidence conf;
  const char* note;   // why this grade -- shown in comments/debug, not UI
};

struct MacPrefixEntry {
  uint8_t prefix[3];       // first 3 bytes of the BD_ADDR, in order
  SuperDeviceType type;
  Confidence conf;
  const char* note;
};

// A full IEEE OUI (vendor-registered first 3 octets), used for the Phase 2
// kOuiTable. Distinct from MacPrefixEntry (Phase 1, used for the handful of
// prefixes -- Flipper, Pwnagotchi -- that were already hardcoded per-device
// before any table existed) only in that lookupOui() below is NOT filtered
// by wantType: the caller doesn't know in advance what kind of device a
// WiFi frame's BSSID belongs to, that's the question being asked. Order in
// the table matters -- see signatures.cpp's header comment on precedence,
// carried over from SquatchWatch's own design notes.
struct OuiEntry {
  uint8_t prefix[3];
  const char* vendorLabel;  // e.g. "Ring", "Flock-MA-L" -- for debug/logging
  SuperDeviceType type;
  Confidence conf;
  const char* note;
};

// Free-text substring match against a BLE advertised name or WiFi SSID.
// `caseInsensitive` mirrors the original code's use of strcasestr for all
// existing entries; kept as a field rather than assumed so future entries
// can opt out if a pattern is case-sensitive by nature.
struct NameEntry {
  const char* pattern;
  SuperDeviceType type;
  Confidence conf;
  const char* note;
};

extern const UuidEntry       kUuidTable[];
extern const uint16_t        kUuidCount;
extern const MacPrefixEntry  kMacPrefixTable[];
extern const uint16_t        kMacPrefixCount;
extern const NameEntry       kBleNameTable[];
extern const uint16_t        kBleNameCount;
extern const NameEntry       kSsidTable[];
extern const uint16_t        kSsidCount;
extern const OuiEntry        kOuiTable[];
extern const uint16_t        kOuiCount;

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------
// Each returns true and (optionally) fills outConf/outNote on a match.
// First-match-wins against the table order given in signatures.cpp.

bool lookupUuid16(uint16_t uuid, SuperDeviceType wantType,
                   Confidence* outConf = nullptr, const char** outNote = nullptr);

// Convenience for the common case: does ANY uuid in this list (e.g. a
// ParsedAdv's uuids[]) match wantType in the table. Avoids every call site
// re-writing the same "loop and call lookupUuid16 until one hits" pattern.
bool lookupAnyUuid16(const uint16_t* uuids, uint8_t count, SuperDeviceType wantType,
                      Confidence* outConf = nullptr, const char** outNote = nullptr);

bool lookupMacPrefix(const uint8_t bda[6], SuperDeviceType wantType,
                      Confidence* outConf = nullptr, const char** outNote = nullptr);

bool lookupBleName(const char* name, SuperDeviceType wantType,
                    Confidence* outConf = nullptr, const char** outNote = nullptr);

bool lookupSsidName(const char* ssid, SuperDeviceType wantType,
                     Confidence* outConf = nullptr, const char** outNote = nullptr);

// First-match-wins walk of kOuiTable against a full 6-byte MAC (only the
// first 3 bytes are compared). Returns true and fills outType/outConf/
// outNote/outVendor on a match; false (outputs untouched) otherwise. Unlike
// the lookups above, there's no wantType filter -- the caller is asking
// "what, if anything, does this OUI belong to," not confirming a guess.
bool lookupOui(const uint8_t mac[6], SuperDeviceType* outType,
               Confidence* outConf = nullptr, const char** outNote = nullptr,
               const char** outVendor = nullptr);

#endif
