/*
    doomBOX Superscanner — signature tables (implementation)

    Phase 1: every row below reproduces an existing check from
    superscanner.cpp before this refactor. Grades were assigned based on
    what the original check actually matched against (a vendor-reserved
    UUID vs. a free-text name substring vs. an unregistered MAC pattern),
    not re-derived from scratch -- the goal here is an honest label on
    behavior that already existed, not a change in what gets detected.
*/

#include "signatures.h"
#include <string.h>
#include <strings.h>   // strncasecmp, strcasecmp

// ---------------------------------------------------------------------------
// UUID table (16-bit BLE service UUIDs)
// ---------------------------------------------------------------------------
const UuidEntry kUuidTable[] = {
  // Flipper Zero advertises these three as its own service UUIDs. They are
  // not a shared/platform range -- a match is Flipper software running on
  // Flipper hardware, not "some nRF52 dev board." HIGH.
  { 0x3081, DEV_FLIPPER, HIGH_CONF, "Flipper-specific service UUID" },
  { 0x3082, DEV_FLIPPER, HIGH_CONF, "Flipper-specific service UUID" },
  { 0x3083, DEV_FLIPPER, HIGH_CONF, "Flipper-specific service UUID" },

  // Samsung's registered 16-bit UUID for SmartTag/SmartTag+ advertisements.
  // HIGH: this is Samsung's own assignment, not a shared range.
  { 0xFD5A, DEV_SMARTTAG, HIGH_CONF, "Samsung SmartTag service UUID" },

  // Tile's two in-use service UUIDs (older and newer tag hardware both
  // appear in the wild). Both are Tile-registered. HIGH.
  { 0xFEED, DEV_TILE, HIGH_CONF, "Tile service UUID (legacy)" },
  { 0xFEEC, DEV_TILE, HIGH_CONF, "Tile service UUID (current)" },
  { 0xFD84, DEV_TILE, HIGH_CONF, "Tile service UUID (third, SquatchWatch-CYD-confirmed Bluetooth SIG assignment)" },

  // Meta/Ray-Ban smart glasses service UUID. HIGH: Meta-registered, not a
  // shared block.
  { 0xFD5F, DEV_RAYBAN, HIGH_CONF, "Ray-Ban Meta smart glasses service UUID" },

  // Carried over from the pre-refactor file's own comment: "common short
  // form if present" -- i.e. the original author flagged this as not fully
  // certain. LOW until someone confirms this against an actual Meshtastic
  // device/firmware source; kept rather than dropped since name-based
  // Meshtastic detection still works independently of this entry.
  { 0x6BA1, DEV_MESHTASTIC, LOW_CONF, "Unverified -- carried over from original code's own uncertain comment" },

  // --- Phase 3+: DEV_FMDN (Chipolo's two SIG-assigned UUIDs) ---
  // Chipolo participates in Google's Find My Device Network; these are
  // Chipolo's own Bluetooth SIG-assigned UUIDs, not a shared/platform
  // range. HIGH, same rationale as Tile's UUIDs above.
  { 0xFE33, DEV_FMDN, HIGH_CONF, "SquatchWatch-CYD: Chipolo's SIG-assigned service UUID" },
  { 0xFE65, DEV_FMDN, HIGH_CONF, "SquatchWatch-CYD: Chipolo's second SIG-assigned service UUID" },
};
const uint16_t kUuidCount = sizeof(kUuidTable) / sizeof(kUuidTable[0]);

// ---------------------------------------------------------------------------
// MAC prefix table (first 3 octets of the BD_ADDR)
// ---------------------------------------------------------------------------
// NOTE: the original code's Flipper MAC check and the new MeshCore-adjacent
// entries below are 3-octet (OUI-style) prefixes compared against the full
// address string. The original Pineapple check is NOT a 3-octet prefix --
// see kBleNameTable/isPineappleBssidLegacy below, ported separately so this
// phase changes nothing about what matches.
const MacPrefixEntry kMacPrefixTable[] = {
  // Flipper Devices Inc.'s registered OUI block. HIGH: this is Flipper's
  // own IEEE-registered range, not a shared module vendor.
  { {0x80, 0xE1, 0x26}, DEV_FLIPPER, HIGH_CONF, "Flipper Devices Inc. registered OUI" },
  { {0x80, 0xE1, 0x27}, DEV_FLIPPER, HIGH_CONF, "Flipper Devices Inc. registered OUI" },

  // Pwnagotchi's well-known fixed MAC (de:ad:be:ef:de:ad). This is an
  // unregistered, hand-picked address baked into the default Pwnagotchi
  // image -- anyone can set a BLE/WiFi MAC to this value, and the project
  // itself tells users it's safe to randomize it. LOW: real signal when
  // seen (nobody else picks this by accident), but trivially spoofable and
  // trivially avoided, which is a different risk profile than an
  // IEEE-registered block.
  { {0xDE, 0xAD, 0xBE}, DEV_PWNAGOTCHI, LOW_CONF, "Pwnagotchi default fixed MAC (unregistered, user-changeable)" },
};
const uint16_t kMacPrefixCount = sizeof(kMacPrefixTable) / sizeof(kMacPrefixTable[0]);

// ---------------------------------------------------------------------------
// BLE advertised-name substring table
// ---------------------------------------------------------------------------
const NameEntry kBleNameTable[] = {
  // nyanBOX/doomBOX self-identification by advertised name. MED: it's a
  // specific, deliberately-chosen string (not a generic word), but it's
  // still free text that any BLE device could advertise.
  { "nyanBOX", DEV_NYANBOX, MED_CONF, "doomBOX/nyanBOX self-ID string" },
  { "doomBOX", DEV_NYANBOX, MED_CONF, "doomBOX/nyanBOX self-ID string" },

  // Axon body-cam name substring. LOW: free-text name field, easily typed
  // by any device; no UUID or MAC backing this one up in the original code.
  { "Axon", DEV_AXON, LOW_CONF, "Free-text name substring only, no UUID/MAC corroboration" },

  // Meshtastic. MED for the exact project name, LOW for the generic "mt-"
  // prefix and bare "mesh" substring, which could belong to unrelated
  // mesh-networking gear.
  { "Meshtastic", DEV_MESHTASTIC, MED_CONF, "Project's own default device-name string" },
  { "mt-", DEV_MESHTASTIC, LOW_CONF, "Short prefix also used by unrelated devices" },

  { "MeshCore", DEV_MESHCORE, MED_CONF, "Project's own default device-name string" },
  { "meshcore", DEV_MESHCORE, MED_CONF, "Project's own default device-name string (lowercase form)" },

  // Skimmer-associated Bluetooth module names. LOW across the board: these
  // are generic Bluetooth serial module name prefixes (HC-05 etc. ship on
  // thousands of unrelated hobbyist boards) -- real skimmers use them, but
  // so does nearly everything else with a cheap BT-SPP module.
  { "HC-03", DEV_SKIMMER, LOW_CONF, "Generic BT-SPP module name, used far beyond skimmers" },
  { "HC-05", DEV_SKIMMER, LOW_CONF, "Generic BT-SPP module name, used far beyond skimmers" },
  { "HC-06", DEV_SKIMMER, LOW_CONF, "Generic BT-SPP module name, used far beyond skimmers" },
  { "HC-08", DEV_SKIMMER, LOW_CONF, "Generic BT-SPP module name, used far beyond skimmers" },
  { "BT-SPP", DEV_SKIMMER, LOW_CONF, "Generic BT-SPP module name, used far beyond skimmers" },
  { "JDY-", DEV_SKIMMER, LOW_CONF, "Generic BT module family name prefix" },
  { "MLT-BT", DEV_SKIMMER, LOW_CONF, "Generic BT module name" },
  { "SKIM", DEV_SKIMMER, MED_CONF, "Deliberately skimmer-specific substring, not a generic module name" },
  { "CardReader", DEV_SKIMMER, MED_CONF, "Deliberately skimmer-descriptive substring" },
};
const uint16_t kBleNameCount = sizeof(kBleNameTable) / sizeof(kBleNameTable[0]);

// ---------------------------------------------------------------------------
// WiFi SSID substring table
// ---------------------------------------------------------------------------
const NameEntry kSsidTable[] = {
  // Flock patterns: all free-text SSID substrings in the original code,
  // none backed by a BSSID/OUI check. LOW.
  { "flock", DEV_FLOCK, LOW_CONF, "Free-text SSID substring only" },
  { "FS Ext Battery", DEV_FLOCK, LOW_CONF, "Free-text SSID substring only" },
  { "Penguin", DEV_FLOCK, LOW_CONF, "Free-text SSID substring only, also a common unrelated SSID word" },
  { "Pigvision", DEV_FLOCK, LOW_CONF, "Free-text SSID substring only" },

  // Drone SSID guesses. LOW: generic words/abbreviations, not a
  // manufacturer-specific string.
  { "DJI", DEV_DRONE, LOW_CONF, "Manufacturer name as SSID substring; easily coincidental" },
  { "RemoteID", DEV_DRONE, LOW_CONF, "Generic descriptive substring" },
  { "UAV", DEV_DRONE, LOW_CONF, "Generic 3-letter abbreviation, high false-positive potential" },
};
const uint16_t kSsidCount = sizeof(kSsidTable) / sizeof(kSsidTable[0]);

// ---------------------------------------------------------------------------
// OUI table (Phase 2)
// ---------------------------------------------------------------------------
// Every row below is ported from SquatchWatch-CYD's signatures.cpp (GPLv3),
// with its original vendor label, DetectionType-equivalent, confidence
// grade, and sourcing comment carried over verbatim/paraphrased and
// attributed. This is SquatchWatch's research, cited -- not independently
// re-verified against the IEEE registry by this project. Where SquatchWatch
// documented a correction (an entry they removed or re-labelled after
// finding it wrong), that history is kept in the comment, because it's the
// clearest evidence the surrounding entries were actually checked rather
// than copied from an unverified list.
//
// ORDER MATTERS. lookupOui() walks top-to-bottom and returns the first
// match. SquatchWatch's own precedence (their DESIGN.md §6.2, as described
// in their signatures.cpp header comment): Axon > Raven > Flock > ALPR >
// camera/Ring > drone > pentest-hardware. Some prefixes could plausibly
// read as more than one category (e.g. an Espressif-module camera vs. a
// generic ESP32 board), so entries for the more specific category must
// stay ahead of any broader catch-all -- there is no broad catch-all in
// this table, but the ordering is preserved in case one is ever added.
//
// doomBOX does not currently have a FLOCK OUI table -- DEV_FLOCK here is
// still SSID-only (see kSsidTable). SquatchWatch's Flock OUI rows (17+
// module-vendor prefixes, each graded LOW with its own removal history for
// the Espressif blocks that were pulled) are a substantial, actively-
// maintained list on their own; porting it is flagged as follow-up rather
// than included here, so this table stays reviewable in one sitting. Ask
// if you want that list ported now instead of deferred.
const OuiEntry kOuiTable[] = {
  // ---- Axon / body cameras (SquatchWatch: highest precedence) ----
  // Axon Enterprise's own registered OUI.
  { {0x00, 0x25, 0xDF}, "Axon",        DEV_AXON, HIGH_CONF,
    "SquatchWatch-CYD: Axon Enterprise's own IEEE MA-L block." },
  { {0xE4, 0x05, 0x40}, "Axon-Body",   DEV_AXON, LOW_CONF,
    "SquatchWatch-CYD, graded LOW there; sourcing not detailed in their comment." },
  { {0x28, 0x24, 0xFF}, "Axon-Signal", DEV_AXON, LOW_CONF,
    "SquatchWatch-CYD, graded LOW there; sourcing not detailed in their comment." },

  // Other body-camera makers, filed under DEV_AXON (same rationale
  // SquatchWatch gives: the UI category is "body camera," the vendor label
  // says whose). SquatchWatch's own comment: WatchGuard VIDEO (Motorola) is
  // NOT WatchGuard Technologies the firewall maker -- that company's
  // 00:01:21/00:90:7F blocks are deliberately excluded, since including
  // them would label every office firewall a police body camera.
  { {0x00, 0x1D, 0x96}, "WatchGuard",  DEV_AXON, MED_CONF,
    "SquatchWatch-CYD, 2026-10-14: WatchGuard VIDEO (Motorola), registry-checked; "
    "explicitly distinct from WatchGuard Technologies the firewall vendor." },
  { {0x00, 0x23, 0xBD}, "DigitalAlly", DEV_AXON, MED_CONF,
    "SquatchWatch-CYD, 2026-10-14, registry-checked." },

  // ---- Gunshot-detection sensors ----
  { {0xD4, 0x11, 0xD6}, "ShotSpotter", DEV_RAVEN, HIGH_CONF,
    "SquatchWatch-CYD: ShotSpotter/SoundThinking's own IEEE MA-L block." },

  // ---- ALPR (automated license-plate readers) ----
  // SquatchWatch's own correction history, kept verbatim because it's the
  // clearest evidence of real verification work: 00:0E:58 used to sit here
  // labelled "Vigilant." It is not Vigilant -- the IEEE registry gives that
  // block to SONOS, INC. (assigned Feb 2004), so every Sonos speaker in
  // earshot was being logged as a license-plate reader. Removed outright
  // rather than relabelled, since there was nothing to correct it to.
  // Motorola Solutions absorbed Vigilant, so Motorola's own blocks (below)
  // are what SquatchWatch calls "the nearest honest thing to the entry
  // they replace." All read from the IEEE registry directly, per their
  // comment, after the Sonos mistake was found.
  { {0x00, 0x04, 0x7D}, "ALPR-Mtrla",  DEV_ALPR, HIGH_CONF,
    "SquatchWatch-CYD: Motorola Solutions (absorbed Vigilant), IEEE-registry-checked." },
  { {0x00, 0x18, 0x85}, "ALPR-Mtrla",  DEV_ALPR, HIGH_CONF,
    "SquatchWatch-CYD: Motorola Solutions, IEEE-registry-checked." },
  { {0x00, 0x1F, 0x92}, "ALPR-Mtrla",  DEV_ALPR, HIGH_CONF,
    "SquatchWatch-CYD: Motorola Solutions, IEEE-registry-checked." },
  { {0x4C, 0xCC, 0x34}, "ALPR-Mtrla",  DEV_ALPR, HIGH_CONF,
    "SquatchWatch-CYD: Motorola Solutions, IEEE-registry-checked." },
  { {0xB8, 0xE2, 0x8C}, "ALPR-Mtrla",  DEV_ALPR, HIGH_CONF,
    "SquatchWatch-CYD: registered to Motorola Solutions Malaysia Sdn. Bhd. "
    "(separate registry line from the US parent)." },
  { {0x00, 0xBF, 0x15}, "ALPR-Gentec", DEV_ALPR, HIGH_CONF,
    "SquatchWatch-CYD: Genetec (AutoVu LPR platform), IEEE-registry-checked." },
  { {0x0C, 0xBF, 0x15}, "ALPR-Gentec", DEV_ALPR, HIGH_CONF,
    "SquatchWatch-CYD: Genetec (AutoVu LPR platform), IEEE-registry-checked." },

  // ---- Skimmer OUIs (Bluetooth Classic module prefixes) ----
  { {0x20, 0x13, 0x00}, "Skim-Linvor", DEV_SKIMMER, LOW_CONF,
    "SquatchWatch-CYD: BT Classic module vendor prefix, shared with non-skimmer use." },
  { {0x98, 0xD3, 0x31}, "Skim-HC05",   DEV_SKIMMER, LOW_CONF,
    "SquatchWatch-CYD: corrected from an earlier 98:D3:00 (unregistered) to "
    "Shenzhen Bolutek's actual IEEE MA-L block, 98:D3:31, maker of the HC-05/HC-06 modules." },
  { {0x00, 0x1A, 0x7D}, "Skim-CSR",    DEV_SKIMMER, LOW_CONF,
    "SquatchWatch-CYD: BT Classic module vendor (CSR) prefix, shared with non-skimmer use." },

  // ---- Cameras (general) ----
  { {0x2C, 0xAA, 0x8E}, "Wyze",        DEV_CAMERA, HIGH_CONF, "SquatchWatch-CYD: Wyze Labs own OUI." },
  { {0xD0, 0x3F, 0x27}, "Wyze",        DEV_CAMERA, HIGH_CONF, "SquatchWatch-CYD: Wyze Labs own OUI." },
  { {0x7C, 0x78, 0xB2}, "Wyze",        DEV_CAMERA, HIGH_CONF, "SquatchWatch-CYD: Wyze Labs own OUI." },
  { {0xB8, 0xD7, 0xAF}, "Wyze-Mod",    DEV_CAMERA, LOW_CONF,  "SquatchWatch-CYD: module-vendor prefix, not Wyze's own." },
  { {0x34, 0xD2, 0x70}, "Amazon",      DEV_CAMERA, MED_CONF,
    "SquatchWatch-CYD: previously mislabelled Hikvision; IEEE registry actually "
    "gives this to Amazon Technologies, a much broader company, hence MED not HIGH." },
  { {0xF0, 0x27, 0x2D}, "Amazon",      DEV_CAMERA, MED_CONF, "SquatchWatch-CYD: as above, Amazon Technologies." },
  { {0xC0, 0x56, 0xE3}, "Hikvision",   DEV_CAMERA, HIGH_CONF, "SquatchWatch-CYD: Hikvision's own OUI." },
  { {0x44, 0x19, 0xB6}, "Hikvision",   DEV_CAMERA, HIGH_CONF, "SquatchWatch-CYD: Hikvision's own OUI." },
  { {0x28, 0x57, 0xBE}, "Hikvision",   DEV_CAMERA, HIGH_CONF,
    "SquatchWatch-CYD: sold under the Reolink brand but registered to Hikvision." },
  { {0x00, 0xE0, 0x4C}, "Realtek",     DEV_CAMERA, LOW_CONF,  "SquatchWatch-CYD: module-vendor prefix, widely shared." },
  { {0xBC, 0xDD, 0xC2}, "Arlo",        DEV_CAMERA, LOW_CONF,  "SquatchWatch-CYD, graded LOW there." },
  { {0x4C, 0x69, 0x05}, "Blink",       DEV_CAMERA, LOW_CONF,  "SquatchWatch-CYD, graded LOW there." },
  { {0xA4, 0xC1, 0x38}, "Tuya",        DEV_CAMERA, LOW_CONF,  "SquatchWatch-CYD: Tuya is a white-label IoT platform vendor, widely shared." },
  { {0x3C, 0xA0, 0x70}, "Blink",       DEV_CAMERA, MED_CONF,  "SquatchWatch-CYD: Blink by Amazon's own IEEE MA-L block." },
  { {0x70, 0xAD, 0x43}, "Blink",       DEV_CAMERA, MED_CONF,  "SquatchWatch-CYD: Blink by Amazon's own IEEE MA-L block." },
  { {0x74, 0x13, 0x48}, "Blink",       DEV_CAMERA, MED_CONF,  "SquatchWatch-CYD: Blink by Amazon's own IEEE MA-L block." },
  { {0x74, 0xAB, 0x93}, "Blink",       DEV_CAMERA, MED_CONF,  "SquatchWatch-CYD: Blink by Amazon's own IEEE MA-L block." },
  { {0xC8, 0x19, 0xD8}, "Blink",       DEV_CAMERA, MED_CONF,  "SquatchWatch-CYD: Blink by Amazon's own IEEE MA-L block." },
  { {0xF0, 0x74, 0xC1}, "Blink",       DEV_CAMERA, MED_CONF,  "SquatchWatch-CYD: Blink by Amazon's own IEEE MA-L block." },
  { {0xD4, 0x2D, 0xC5}, "i-PRO",       DEV_CAMERA, HIGH_CONF,
    "SquatchWatch-CYD: i-PRO (Panasonic's security-camera spinoff, incl. body cams), own OUI." },

  // ---- Ring (kept separate from generic DEV_CAMERA, per user decision) ----
  { {0xFC, 0x65, 0xDE}, "Ring", DEV_RING, MED_CONF, "SquatchWatch-CYD, part of Ring LLC's registered block." },
  { {0x68, 0x37, 0xE9}, "Ring", DEV_RING, MED_CONF, "SquatchWatch-CYD, part of Ring LLC's registered block." },
  { {0xAC, 0x9F, 0xC3}, "Ring", DEV_RING, HIGH_CONF,
    "SquatchWatch-CYD: Ring LLC's full registered MA-L block, cross-checked via "
    "netify.ai and maclookup.app (13 prefixes, registered 2019-03-01)." },
  { {0x18, 0x7F, 0x88}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x34, 0x3E, 0xA4}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x54, 0xE0, 0x19}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x5C, 0x47, 0x5E}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x64, 0x9A, 0x63}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x90, 0x48, 0x6C}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x9C, 0x76, 0x13}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0xCC, 0x3B, 0xFB}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0xC4, 0xDB, 0xAD}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x24, 0x2B, 0xD6}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x00, 0xB4, 0x63}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },
  { {0x50, 0xE4, 0x67}, "Ring", DEV_RING, HIGH_CONF, "SquatchWatch-CYD: Ring LLC OUI, cross-checked as above." },

  // ---- Commercial / institutional camera vendors ----
  { {0xE0, 0xA7, 0x00}, "Verkada",  DEV_CAMERA, HIGH_CONF,
    "SquatchWatch-CYD: IEEE MA-L registry, registered 2016-09-22. "
    "Verkada also sells LPR, but SquatchWatch files it as CAMERA (general-purpose) "
    "rather than overstating it as an ALPR-only vendor; same choice kept here." },
  { {0x70, 0x1A, 0xD5}, "Avigilon", DEV_CAMERA, HIGH_CONF,
    "SquatchWatch-CYD: Avigilon Alta, IEEE registry, registered 2021-04-27." },
  { {0x00, 0x40, 0x8C}, "Axis",     DEV_CAMERA, HIGH_CONF,
    "SquatchWatch-CYD: Axis Communications, IEEE registry, registered 1998." },
  { {0xB8, 0xA4, 0x4F}, "Axis",     DEV_CAMERA, HIGH_CONF, "SquatchWatch-CYD: Axis Communications OUI." },

  // ---- Drones, by manufacturer (backs up the existing SSID/Remote-ID
  // checks for drones that don't broadcast Remote ID, or don't yet) ----
  { {0x60, 0x60, 0x1F}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI (drones/controllers)." },
  { {0x34, 0xD2, 0x62}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI." },
  { {0x48, 0x1C, 0xB9}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI." },
  { {0xE4, 0x7A, 0x2C}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI." },
  { {0x58, 0xB8, 0x58}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI." },
  { {0x04, 0xA8, 0x5A}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI." },
  { {0x8C, 0x58, 0x23}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI." },
  { {0x0C, 0x9A, 0xE6}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI." },
  { {0x88, 0x29, 0x85}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI, registered 2025-10." },
  { {0x4C, 0x43, 0xF6}, "DJI",    DEV_DRONE, MED_CONF, "SquatchWatch-CYD: DJI's own OUI, registered 2025-12." },
  { {0x9C, 0x5A, 0x8A}, "DJI-BW", DEV_DRONE, LOW_CONF,
    "SquatchWatch-CYD: Baiwang, a DJI subsidiary with a wider product range than just drones." },
  { {0xEC, 0x72, 0xF7}, "DJI-BW", DEV_DRONE, LOW_CONF, "SquatchWatch-CYD: DJI/Baiwang, as above." },
  { {0x34, 0x91, 0xF0}, "DJI-BW", DEV_DRONE, LOW_CONF, "SquatchWatch-CYD: DJI/Baiwang, as above." },
  { {0xC8, 0xA1, 0x62}, "DJI-BW", DEV_DRONE, LOW_CONF, "SquatchWatch-CYD: DJI/Baiwang, as above." },
  { {0x38, 0x1D, 0x14}, "Skydio", DEV_DRONE, HIGH_CONF, "SquatchWatch-CYD: Skydio's own OUI." },
  { {0x90, 0x03, 0xB7}, "Parrot", DEV_DRONE, MED_CONF, "SquatchWatch-CYD: Parrot's own OUI." },
  { {0x90, 0x3A, 0xE6}, "Parrot", DEV_DRONE, MED_CONF, "SquatchWatch-CYD: Parrot's own OUI." },
  { {0xA0, 0x14, 0x3D}, "Parrot", DEV_DRONE, MED_CONF, "SquatchWatch-CYD: Parrot's own OUI." },
  { {0x00, 0x12, 0x1C}, "Parrot", DEV_DRONE, LOW_CONF,
    "SquatchWatch-CYD: one of Parrot's two oldest blocks, mostly used on their car-kit products." },
  { {0x00, 0x26, 0x7E}, "Parrot", DEV_DRONE, LOW_CONF, "SquatchWatch-CYD: Parrot's other oldest block, same caveat." },

  // NOTE on Flipper/Pineapple: SquatchWatch deliberately does NOT carry a
  // Flipper OUI row here -- their comment explains Flipper Devices' actual
  // IEEE block (0C:FA:22) belongs to their desk accessory (the "BUSY Bar"),
  // not the Flipper Zero itself, which advertises from 80:E1:26, an
  // unregistered address. doomBOX already has 80:E1:26/80:E1:27 in
  // kMacPrefixTable (Phase 1) plus the UUID matches, which is the correct
  // signal per SquatchWatch's own finding -- so nothing is added here.
  // Pineapple is flagged separately (see Phase 1 handoff note) since
  // SquatchWatch's version (02:13:37 as a full locally-administered OUI)
  // differs from doomBOX's current byte-offset heuristic; not yet merged
  // into this table pending that decision.
};
const uint16_t kOuiCount = sizeof(kOuiTable) / sizeof(kOuiTable[0]);

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------
bool lookupUuid16(uint16_t uuid, SuperDeviceType wantType,
                   Confidence* outConf, const char** outNote) {
  for (uint16_t i = 0; i < kUuidCount; i++) {
    if (kUuidTable[i].uuid == uuid && kUuidTable[i].type == wantType) {
      if (outConf) *outConf = kUuidTable[i].conf;
      if (outNote) *outNote = kUuidTable[i].note;
      return true;
    }
  }
  return false;
}

bool lookupAnyUuid16(const uint16_t* uuids, uint8_t count, SuperDeviceType wantType,
                      Confidence* outConf, const char** outNote) {
  for (uint8_t i = 0; i < count; i++) {
    if (lookupUuid16(uuids[i], wantType, outConf, outNote)) return true;
  }
  return false;
}

bool lookupOui(const uint8_t mac[6], SuperDeviceType* outType,
               Confidence* outConf, const char** outNote, const char** outVendor) {
  if (!mac) return false;
  for (uint16_t i = 0; i < kOuiCount; i++) {
    const OuiEntry &e = kOuiTable[i];
    if (mac[0] == e.prefix[0] && mac[1] == e.prefix[1] && mac[2] == e.prefix[2]) {
      if (outType) *outType = e.type;
      if (outConf) *outConf = e.conf;
      if (outNote) *outNote = e.note;
      if (outVendor) *outVendor = e.vendorLabel;
      return true;
    }
  }
  return false;
}

bool lookupMacPrefix(const uint8_t bda[6], SuperDeviceType wantType,
                      Confidence* outConf, const char** outNote) {
  for (uint16_t i = 0; i < kMacPrefixCount; i++) {
    const MacPrefixEntry &e = kMacPrefixTable[i];
    if (e.type != wantType) continue;
    if (bda[0] == e.prefix[0] && bda[1] == e.prefix[1] && bda[2] == e.prefix[2]) {
      if (outConf) *outConf = e.conf;
      if (outNote) *outNote = e.note;
      return true;
    }
  }
  return false;
}

bool lookupBleName(const char* name, SuperDeviceType wantType,
                    Confidence* outConf, const char** outNote) {
  if (!name || !name[0]) return false;
  for (uint16_t i = 0; i < kBleNameCount; i++) {
    const NameEntry &e = kBleNameTable[i];
    if (e.type != wantType) continue;
    if (strcasestr(name, e.pattern)) {
      if (outConf) *outConf = e.conf;
      if (outNote) *outNote = e.note;
      return true;
    }
  }
  return false;
}

bool lookupSsidName(const char* ssid, SuperDeviceType wantType,
                     Confidence* outConf, const char** outNote) {
  if (!ssid || !ssid[0]) return false;
  for (uint16_t i = 0; i < kSsidCount; i++) {
    const NameEntry &e = kSsidTable[i];
    if (e.type != wantType) continue;
    if (strcasestr(ssid, e.pattern)) {
      if (outConf) *outConf = e.conf;
      if (outNote) *outNote = e.note;
      return true;
    }
  }
  return false;
}
