/*
    doomBOX RFID Module (RC522 / MFRC522)
    https://github.com/jbohack/nyanBOX  (fork lineage)
    Copyright (c) 2026

    Licensed under the MIT License
    https://opensource.org/licenses/MIT

    SPDX-License-Identifier: MIT

    Features:
      - Nested RFID menu (Scanner / Dumper / Cloner / Saved Cards / Tools)
      - UID + data read, magic-card / rewritable detection
      - Full dump with progress bar, save to SPIFFS
      - Clone from saved card only (UID-only or full dump restore)
      - Format / factory-reset magic cards
      - Multi-card batch clone mode
      - Access-condition decoder
      - Scan log (ring buffer)
      - Simple key brute-force (common keys)
      - Card "emulator" UI (RC522 is reader-only – educational note shown)
      - Live field-strength indicator while scanning
      - Confirmation dialogs for destructive ops
      - XP awards via level_system
*/

#include "../include/rfid.h"
#include "../include/sleep_manager.h"
#include "../include/display_mirror.h"
#include "../include/level_system.h"
#include "../include/neopixel.h"
#include "../include/setting.h"
#include "../include/radio_manager.h"
#include "../include/pindefs.h"

#include <SPI.h>
#include <MFRC522.h>
#include <SPIFFS.h>
#include <FS.h>
#include <vector>
#include <string.h>

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
static MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);
static MFRC522::MIFARE_Key defaultKeyA;
static MFRC522::MIFARE_Key defaultKeyB;
static bool rfidReady = false;

// ---------------------------------------------------------------------------
// UI / state machine
// ---------------------------------------------------------------------------
enum RfidScreen {
  SCR_MENU = 0,
  SCR_SCANNER,
  SCR_DUMPER,
  SCR_CLONER_SELECT,      // pick saved card
  SCR_CLONER_MODE,        // UID-only vs Full
  SCR_CLONER_WRITE,       // waiting for target + writing
  SCR_SAVED_LIST,
  SCR_SAVED_DETAIL,
  SCR_TOOLS,
  SCR_BRUTE,
  SCR_ACCESS,
  SCR_SCANLOG,
  SCR_FORMAT,
  SCR_EMULATOR,
  SCR_BATCH,
  SCR_CONFIRM,
  SCR_MSG
};

static RfidScreen screen = SCR_MENU;
static RfidScreen prevScreen = SCR_MENU;
static int menuIndex = 0;
static int listIndex = 0;
static int listScroll = 0;
static bool needsRedraw = true;
static unsigned long lastButton = 0;
static const unsigned long DEBOUNCE = 180;

// Confirmation dialog
static const char* confirmTitle = nullptr;
static const char* confirmMsg   = nullptr;
static void (*confirmYesAction)() = nullptr;
static int confirmSel = 0;   // 0 = No, 1 = Yes

// Generic message overlay
static char msgLine1[22] = {0};
static char msgLine2[22] = {0};
static unsigned long msgUntil = 0;

// ---------------------------------------------------------------------------
// Card data structures
// ---------------------------------------------------------------------------
#define MAX_UID_LEN     10
#define MAX_BLOCKS      256          // 4K Classic
#define BLOCK_SIZE      16
#define MAX_SAVED_CARDS 12
#define SCANLOG_SIZE    16
#define SPIFFS_DIR      "/rfid"

struct CardDump {
  uint8_t  uid[MAX_UID_LEN];
  uint8_t  uidLen;
  uint8_t  sak;
  uint16_t atqa;
  char     typeName[18];
  bool     isMagic;          // Gen1a / rewritable UID detected
  bool     isClassic;
  uint16_t blockCount;       // 64 for 1K, 256 for 4K, etc.
  uint8_t  data[MAX_BLOCKS][BLOCK_SIZE];
  bool     blockValid[MAX_BLOCKS];
  char     name[16];         // user label / auto name
  uint32_t savedAt;          // millis at save (for ordering)
};

static CardDump currentCard;
static bool     hasCurrentCard = false;

// In-memory list of saved card metadata (full dumps live on SPIFFS)
struct SavedMeta {
  char     filename[32];
  char     name[16];
  uint8_t  uid[MAX_UID_LEN];
  uint8_t  uidLen;
  bool     isMagic;
  uint16_t blockCount;
};
static std::vector<SavedMeta> savedList;

// Scan log (UID history)
struct ScanEntry {
  uint8_t  uid[MAX_UID_LEN];
  uint8_t  uidLen;
  char     typeName[12];
  bool     isMagic;
  uint32_t seenAt;
};
static ScanEntry scanLog[SCANLOG_SIZE];
static int scanLogHead = 0;
static int scanLogCount = 0;

// Cloner / batch state
static int  selectedSavedIdx = -1;
static bool cloneUidOnly = true;
static bool batchMode = false;
static int  batchSuccess = 0;
static int  batchFail = 0;

// Brute-force
static const int NUM_COMMON_KEYS = 14;
static const byte COMMON_KEYS[NUM_COMMON_KEYS][6] = {
  {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
  {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5},
  {0xB0,0xB1,0xB2,0xB3,0xB4,0xB5},
  {0x00,0x00,0x00,0x00,0x00,0x00},
  {0xD3,0xF7,0xD3,0xF7,0xD3,0xF7},
  {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF},
  {0x01,0x02,0x03,0x04,0x05,0x06},
  {0x1A,0x2B,0x3C,0x4D,0x5E,0x6F},
  {0x12,0x34,0x56,0x78,0x9A,0xBC},
  {0x4D,0x3A,0x99,0xC3,0x51,0xDD},
  {0x1A,0x98,0x2C,0x7E,0x45,0x9A},
  {0x71,0x4C,0x5C,0x88,0x6E,0x97},
  {0x58,0x7E,0xE5,0xF9,0x35,0x0F},
  {0xA0,0x47,0x8C,0xC3,0x90,0x91}
};
static int bruteSector = 0;
static int bruteKeyIdx = 0;
static bool bruteRunning = false;
static char bruteFoundKey[18] = {0};

// Emulator (educational – RC522 cannot truly emulate)
static int emulateIdx = -1;
static bool emulateRandom = false;

// Progress
static int progressPct = 0;
static char progressLabel[20] = {0};

// Field strength (approximate – RC522 has no true RSSI; we pulse antenna gain)
static int fieldBars = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void showMsg(const char* l1, const char* l2 = "", unsigned long ms = 1800);
static void requestConfirm(const char* title, const char* msg, void (*yesFn)());
static void drawProgressBar(int y, int pct);
static void drawFieldBars(int x, int y, int bars);
static bool initReader();
static void haltReader();
static bool readCardBasic(CardDump& out);
static bool dumpCardFull(CardDump& out);
static bool detectMagic();
static bool writeUidOnly(const CardDump& src);
static bool writeFullDump(const CardDump& src);
static bool formatMagicCard();
static void decodeAccessBits(uint8_t* trailer, char* outA, char* outB, char* outAccess);
static void loadSavedList();
static bool saveCardToFS(const CardDump& c);
static bool loadCardFromFS(const char* filename, CardDump& out);
static bool deleteCardFromFS(const char* filename);
static void addToScanLog(const CardDump& c);
static void awardXP(int amount);
static void handleButtons();
static void drawMenu();
static void drawScanner();
static void drawDumper();
static void drawClonerSelect();
static void drawClonerMode();
static void drawClonerWrite();
static void drawSavedList();
static void drawSavedDetail();
static void drawTools();
static void drawBrute();
static void drawAccess();
static void drawScanLog();
static void drawFormat();
static void drawEmulator();
static void drawBatch();
static void drawConfirm();
static void drawMsg();

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void showMsg(const char* l1, const char* l2, unsigned long ms) {
  strncpy(msgLine1, l1, sizeof(msgLine1) - 1);
  msgLine1[sizeof(msgLine1) - 1] = 0;
  strncpy(msgLine2, l2, sizeof(msgLine2) - 1);
  msgLine2[sizeof(msgLine2) - 1] = 0;
  msgUntil = millis() + ms;
  prevScreen = screen;
  screen = SCR_MSG;
  needsRedraw = true;
}

static void requestConfirm(const char* title, const char* msg, void (*yesFn)()) {
  confirmTitle = title;
  confirmMsg = msg;
  confirmYesAction = yesFn;
  confirmSel = 0;
  prevScreen = screen;
  screen = SCR_CONFIRM;
  needsRedraw = true;
}

static void drawProgressBar(int y, int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  u8g2.drawFrame(4, y, 120, 8);
  int w = (pct * 116) / 100;
  if (w > 0) u8g2.drawBox(6, y + 2, w, 4);
}

static void drawFieldBars(int x, int y, int bars) {
  // 5 vertical bars
  for (int i = 0; i < 5; i++) {
    int h = 2 + i * 2;
    if (i < bars) {
      u8g2.drawBox(x + i * 5, y + (10 - h), 3, h);
    } else {
      u8g2.drawFrame(x + i * 5, y + (10 - h), 3, h);
    }
  }
}

static void awardXP(int amount) {
  addXP(amount);
  blinkColor(0, 80, 0);
}

static void uidToStr(const uint8_t* uid, uint8_t len, char* buf, size_t buflen) {
  size_t pos = 0;
  for (uint8_t i = 0; i < len && pos + 3 < buflen; i++) {
    pos += snprintf(buf + pos, buflen - pos, "%02X", uid[i]);
    if (i + 1 < len && pos + 1 < buflen) {
      buf[pos++] = ':';
      buf[pos] = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Reader init / low-level
// ---------------------------------------------------------------------------
static bool initReader() {
  // Ensure NRF24 is quiet and SPI is in a known state
  nrf24PowerDown();
  delay(5);

  SPI.begin(18, 19, 23, -1);   // same bus as NRF24
  SPI.setFrequency(4000000);   // 4 MHz safe for RC522
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);

  pinMode(RFID_SS_PIN, OUTPUT);
  digitalWrite(RFID_SS_PIN, HIGH);
  pinMode(RFID_RST_PIN, OUTPUT);
  digitalWrite(RFID_RST_PIN, HIGH);
  delay(10);

  mfrc522.PCD_Init();
  delay(20);

  // Soft self-test / version check
  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (v == 0x00 || v == 0xFF) {
    rfidReady = false;
    return false;
  }

  // Antenna on, max gain
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
  mfrc522.PCD_AntennaOn();

  for (byte i = 0; i < 6; i++) {
    defaultKeyA.keyByte[i] = 0xFF;
    defaultKeyB.keyByte[i] = 0xFF;
  }

  rfidReady = true;
  return true;
}

static void haltReader() {
  if (rfidReady) {
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
}

// Approximate field strength: try to see if a card answers quickly
static int measureFieldBars() {
  if (!rfidReady) return 0;
  // Simple heuristic: attempt RequestA; success + quick response = higher bars
  byte atqa[2];
  byte atqaLen = 2;
  MFRC522::StatusCode st = mfrc522.PICC_RequestA(atqa, &atqaLen);
  if (st == MFRC522::STATUS_OK) {
    // Card present – use a moderate/high reading
    return 4 + (millis() % 2);   // 4-5 bars when present
  }
  // No card – still show low activity if antenna is alive
  return (millis() / 400) % 2;   // flicker 0-1
}

// ---------------------------------------------------------------------------
// Gen1a magic backdoor (correct sequence from MFRC522Hack / Proxmark)
// Halt + 0x40 (7 bits) + 0x43 → then block 0 is writable without auth
// ---------------------------------------------------------------------------
static bool openGen1aBackdoor() {
  // 1. Halt the card (required by most Gen1a chips)
  mfrc522.PICC_HaltA();

  // 2. Send 0x40 as a 7-bit short frame
  byte cmd = 0x40;
  byte validBits = 7;
  byte response[32];
  byte responseLen = sizeof(response);
  MFRC522::StatusCode st = mfrc522.PCD_TransceiveData(
      &cmd, 1, response, &responseLen, &validBits, 0, false);
  if (st != MFRC522::STATUS_OK) {
    return false;
  }

  // 3. Send 0x43 (full byte)
  cmd = 0x43;
  validBits = 8;
  responseLen = sizeof(response);
  st = mfrc522.PCD_TransceiveData(
      &cmd, 1, response, &responseLen, &validBits, 0, false);
  return (st == MFRC522::STATUS_OK);
}

static bool detectMagic() {
  // Non-destructive probe: try the backdoor. Card must already be selected
  // or we halt first. After a successful open the card stays unlocked until
  // power cycle / new select.
  return openGen1aBackdoor();
}

// ---------------------------------------------------------------------------
// Basic card read (UID + type + ATQA/SAK)
// ---------------------------------------------------------------------------
static bool readCardBasic(CardDump& out) {
  memset(&out, 0, sizeof(out));

  if (!mfrc522.PICC_IsNewCardPresent()) return false;
  if (!mfrc522.PICC_ReadCardSerial()) return false;

  out.uidLen = mfrc522.uid.size;
  if (out.uidLen > MAX_UID_LEN) out.uidLen = MAX_UID_LEN;
  memcpy(out.uid, mfrc522.uid.uidByte, out.uidLen);
  out.sak = mfrc522.uid.sak;

  // ATQA is captured during anticollision inside PICC_ReadCardSerial.
  // Re-issuing RequestA after SELECT is unreliable; leave 0 if unknown.
  out.atqa = 0;

  MFRC522::PICC_Type t = mfrc522.PICC_GetType(out.sak);
  // PICC_GetTypeName returns const __FlashStringHelper* (PROGMEM) on MFRC522
  const __FlashStringHelper* fname = mfrc522.PICC_GetTypeName(t);
  if (fname) {
    strncpy_P(out.typeName, (PGM_P)fname, sizeof(out.typeName) - 1);
    out.typeName[sizeof(out.typeName) - 1] = '\0';
  } else {
    strncpy(out.typeName, "Unknown", sizeof(out.typeName) - 1);
    out.typeName[sizeof(out.typeName) - 1] = '\0';
  }

  out.isClassic = (t == MFRC522::PICC_TYPE_MIFARE_MINI ||
                   t == MFRC522::PICC_TYPE_MIFARE_1K ||
                   t == MFRC522::PICC_TYPE_MIFARE_4K);

  if (t == MFRC522::PICC_TYPE_MIFARE_1K || t == MFRC522::PICC_TYPE_MIFARE_MINI)
    out.blockCount = 64;
  else if (t == MFRC522::PICC_TYPE_MIFARE_4K)
    out.blockCount = 256;
  else
    out.blockCount = 16;  // Ultralight-ish fallback

  // Magic probe – uses correct Halt + 0x40(7bit) + 0x43 sequence
  out.isMagic = detectMagic();

  // Auto name from UID
  char uidStr[24];
  uidToStr(out.uid, out.uidLen, uidStr, sizeof(uidStr));
  snprintf(out.name, sizeof(out.name), "%.15s", uidStr);

  return true;
}

// ---------------------------------------------------------------------------
// Full dump (Classic)
// ---------------------------------------------------------------------------
static bool dumpCardFull(CardDump& out) {
  if (!out.isClassic) {
    // For non-Classic just mark UID block valid
    out.blockValid[0] = true;
    return true;
  }

  progressPct = 0;
  strncpy(progressLabel, "Dumping...", sizeof(progressLabel) - 1);

  MFRC522::MIFARE_Key key;
  memcpy(key.keyByte, defaultKeyA.keyByte, 6);

  for (uint16_t block = 0; block < out.blockCount; block++) {
    progressPct = (block * 100) / out.blockCount;
    // redraw occasionally
    if ((block & 3) == 0) {
      needsRedraw = true;
      // minimal redraw of progress only
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(4, 12, "Full Dump");
      u8g2.drawStr(4, 28, progressLabel);
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", progressPct);
      u8g2.drawStr(100, 28, pct);
      drawProgressBar(40, progressPct);
      u8g2.sendBuffer();
      displayMirrorSend(u8g2);
    }

    // Authenticate every sector
    byte sector = (block < 128) ? (block / 4) : (32 + (block - 128) / 16);
    byte firstBlock = (block < 128) ? (sector * 4) : (128 + (sector - 32) * 16);

    if (block == firstBlock || block == 0) {
      MFRC522::StatusCode st = mfrc522.PCD_Authenticate(
          MFRC522::PICC_CMD_MF_AUTH_KEY_A, firstBlock, &key, &(mfrc522.uid));
      if (st != MFRC522::STATUS_OK) {
        // try Key B
        st = mfrc522.PCD_Authenticate(
            MFRC522::PICC_CMD_MF_AUTH_KEY_B, firstBlock, &defaultKeyB, &(mfrc522.uid));
        if (st != MFRC522::STATUS_OK) {
          out.blockValid[block] = false;
          continue;
        }
      }
    }

    byte buffer[18];
    byte size = sizeof(buffer);
    MFRC522::StatusCode st = mfrc522.MIFARE_Read(block, buffer, &size);
    if (st == MFRC522::STATUS_OK) {
      memcpy(out.data[block], buffer, BLOCK_SIZE);
      out.blockValid[block] = true;
    } else {
      out.blockValid[block] = false;
    }
  }

  progressPct = 100;
  haltReader();
  return true;
}

// ---------------------------------------------------------------------------
// Write helpers (magic cards)
// ---------------------------------------------------------------------------
static bool writeUidOnly(const CardDump& src) {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return false;
  }

  // Prefer Gen1a backdoor
  if (!openGen1aBackdoor()) {
    // Some Chinese cards accept direct block-0 write after normal auth
    MFRC522::StatusCode st = mfrc522.PCD_Authenticate(
        MFRC522::PICC_CMD_MF_AUTH_KEY_A, 0, &defaultKeyA, &(mfrc522.uid));
    if (st != MFRC522::STATUS_OK) return false;
  }

  // Build block 0: UID + BCC + SAK + ATQA + manufacturer bytes
  byte block0[16];
  memset(block0, 0, 16);
  memcpy(block0, src.uid, src.uidLen);
  // BCC
  byte bcc = 0;
  for (uint8_t i = 0; i < src.uidLen; i++) bcc ^= src.uid[i];
  block0[src.uidLen] = bcc;
  // Rest left as 0 or copy from source if available
  if (src.blockValid[0]) {
    memcpy(block0 + src.uidLen + 1, src.data[0] + src.uidLen + 1, 16 - src.uidLen - 1);
  }

  MFRC522::StatusCode st = mfrc522.MIFARE_Write(0, block0, 16);
  haltReader();
  return st == MFRC522::STATUS_OK;
}

static bool writeFullDump(const CardDump& src) {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return false;
  }

  bool unlocked = openGen1aBackdoor();
  if (!unlocked) {
    // try normal auth path for Gen2 / CUID etc.
  }

  progressPct = 0;
  strncpy(progressLabel, "Writing...", sizeof(progressLabel) - 1);

  for (uint16_t block = 0; block < src.blockCount; block++) {
    if (!src.blockValid[block]) continue;

    progressPct = (block * 100) / src.blockCount;
    if ((block & 3) == 0) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(4, 12, "Cloning");
      u8g2.drawStr(4, 28, progressLabel);
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", progressPct);
      u8g2.drawStr(100, 28, pct);
      drawProgressBar(40, progressPct);
      u8g2.sendBuffer();
      displayMirrorSend(u8g2);
    }

    // Authenticate when needed (non-backdoor path)
    if (!unlocked) {
      byte sector = (block < 128) ? (block / 4) : (32 + (block - 128) / 16);
      byte firstBlock = (block < 128) ? (sector * 4) : (128 + (sector - 32) * 16);
      if (block == firstBlock) {
        mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A,
                                 firstBlock, &defaultKeyA, &(mfrc522.uid));
      }
    }

    byte buf[18];
    memcpy(buf, src.data[block], 16);
    MFRC522::StatusCode st = mfrc522.MIFARE_Write(block, buf, 16);
    if (st != MFRC522::STATUS_OK && block == 0) {
      // block 0 failure is critical for UID clone
      haltReader();
      return false;
    }
  }

  progressPct = 100;
  haltReader();
  return true;
}

static bool formatMagicCard() {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return false;
  }
  if (!openGen1aBackdoor()) return false;

  // Write empty/default trailers and zero data blocks
  byte empty[16];
  memset(empty, 0, 16);
  // Default trailer: FF keys + default AC
  byte trailer[16] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,  // Key A
    0xFF,0x07,0x80,0x69,            // Access bits + user
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF   // Key B
  };

  // Assume 1K for format
  for (uint16_t b = 0; b < 64; b++) {
    if ((b + 1) % 4 == 0) {
      mfrc522.MIFARE_Write(b, trailer, 16);
    } else if (b != 0) {
      mfrc522.MIFARE_Write(b, empty, 16);
    }
  }
  haltReader();
  return true;
}

// ---------------------------------------------------------------------------
// Access condition decoder (simplified human-readable)
// ---------------------------------------------------------------------------
static void decodeAccessBits(uint8_t* trailer, char* outA, char* outB, char* outAccess) {
  // trailer[6], [7], [8] hold C1 C2 C3 bits (inverted + non-inverted)
  uint8_t c1 = (trailer[7] & 0xF0) >> 4;
  uint8_t c2 = (trailer[8] & 0x0F);
  uint8_t c3 = (trailer[8] & 0xF0) >> 4;
  // For brevity we only show raw + common interpretation for data blocks
  snprintf(outA, 20, "KeyA %02X%02X%02X%02X%02X%02X",
           trailer[0], trailer[1], trailer[2],
           trailer[3], trailer[4], trailer[5]);
  snprintf(outB, 20, "KeyB %02X%02X%02X%02X%02X%02X",
           trailer[10], trailer[11], trailer[12],
           trailer[13], trailer[14], trailer[15]);
  snprintf(outAccess, 20, "AC %02X %02X %02X", trailer[6], trailer[7], trailer[8]);
}

// ---------------------------------------------------------------------------
// SPIFFS persistence
// ---------------------------------------------------------------------------
static void loadSavedList() {
  savedList.clear();
  if (!SPIFFS.begin(true)) return;

  // SPIFFS is flat; open root and accept any path that contains our prefix
  File root = SPIFFS.open("/");
  if (!root) return;

  File f = root.openNextFile();
  while (f) {
    const char* n = f.name();
    // Accept both "/rfid/xxx.dmp" and "rfid/xxx.dmp"
    bool match = (n && (strstr(n, "/rfid/") != nullptr || strstr(n, "rfid/") == n));
    if (!f.isDirectory() && match && f.size() > 22) {
      SavedMeta m;
      memset(&m, 0, sizeof(m));
      // Prefer full path when available (ESP32 core provides path())
#if defined(ESP32)
      if (f.path()) {
        strncpy(m.filename, f.path(), sizeof(m.filename) - 1);
      } else {
        strncpy(m.filename, n, sizeof(m.filename) - 1);
      }
#else
      strncpy(m.filename, n, sizeof(m.filename) - 1);
#endif
      // Ensure leading slash
      if (m.filename[0] != '/') {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "/%s", m.filename);
        strncpy(m.filename, tmp, sizeof(m.filename) - 1);
      }

      m.uidLen = f.read();
      if (m.uidLen > MAX_UID_LEN) m.uidLen = MAX_UID_LEN;
      f.read(m.uid, m.uidLen);
      f.read(); // sak
      m.isMagic = f.read() != 0;
      m.blockCount = f.read() | (f.read() << 8);
      f.read((uint8_t*)m.name, 16);
      m.name[15] = 0;
      savedList.push_back(m);
    }
    f = root.openNextFile();
  }
}

static bool saveCardToFS(const CardDump& c) {
  if (!SPIFFS.begin(true)) return false;
  SPIFFS.mkdir(SPIFFS_DIR);

  char path[48];
  char uidStr[24];
  uidToStr(c.uid, c.uidLen, uidStr, sizeof(uidStr));
  // replace ':' for filename safety
  for (char* p = uidStr; *p; p++) if (*p == ':') *p = '_';
  snprintf(path, sizeof(path), "%s/%s.dmp", SPIFFS_DIR, uidStr);

  File f = SPIFFS.open(path, FILE_WRITE);
  if (!f) return false;

  f.write(c.uidLen);
  f.write(c.uid, c.uidLen);
  f.write(c.sak);
  f.write(c.isMagic ? 1 : 0);
  f.write(c.blockCount & 0xFF);
  f.write((c.blockCount >> 8) & 0xFF);
  f.write((const uint8_t*)c.name, 16);

  for (uint16_t b = 0; b < c.blockCount; b++) {
    f.write(c.blockValid[b] ? 1 : 0);
    f.write(c.data[b], BLOCK_SIZE);
  }
  f.close();

  loadSavedList();
  return true;
}

// Static buffer avoids putting a second ~4 KB CardDump on the stack
static CardDump _loadBuf;

static bool loadCardFromFS(const char* filename, CardDump& out) {
  memset(&out, 0, sizeof(out));
  File f = SPIFFS.open(filename, FILE_READ);
  if (!f) {
    // Retry with leading slash if missing
    char alt[40];
    if (filename[0] != '/') {
      snprintf(alt, sizeof(alt), "/%s", filename);
      f = SPIFFS.open(alt, FILE_READ);
    }
    if (!f) return false;
  }

  out.uidLen = f.read();
  if (out.uidLen > MAX_UID_LEN) out.uidLen = MAX_UID_LEN;
  f.read(out.uid, out.uidLen);
  out.sak = f.read();
  out.isMagic = f.read() != 0;
  out.blockCount = f.read() | (f.read() << 8);
  if (out.blockCount > MAX_BLOCKS) out.blockCount = MAX_BLOCKS;
  f.read((uint8_t*)out.name, 16);
  out.name[15] = 0;
  out.isClassic = true;

  for (uint16_t b = 0; b < out.blockCount; b++) {
    out.blockValid[b] = f.read() != 0;
    f.read(out.data[b], BLOCK_SIZE);
  }
  f.close();
  return true;
}

static bool deleteCardFromFS(const char* filename) {
  bool ok = SPIFFS.remove(filename);
  loadSavedList();
  return ok;
}

// ---------------------------------------------------------------------------
// Scan log
// ---------------------------------------------------------------------------
static void addToScanLog(const CardDump& c) {
  ScanEntry& e = scanLog[scanLogHead];
  memcpy(e.uid, c.uid, c.uidLen);
  e.uidLen = c.uidLen;
  strncpy(e.typeName, c.typeName, sizeof(e.typeName) - 1);
  e.isMagic = c.isMagic;
  e.seenAt = millis();
  scanLogHead = (scanLogHead + 1) % SCANLOG_SIZE;
  if (scanLogCount < SCANLOG_SIZE) scanLogCount++;
}

// ---------------------------------------------------------------------------
// Button handling
// ---------------------------------------------------------------------------
static bool btnPressed(int pin) {
  if (digitalRead(pin) == LOW) {
    if (millis() - lastButton > DEBOUNCE) {
      lastButton = millis();
      updateLastActivity();
      return true;
    }
  }
  return false;
}

static void handleButtons() {
  // Global back / exit
  if (btnPressed(BUTTON_PIN_LEFT) || btnPressed(BUTTON_PIN_CENTER)) {
    if (screen == SCR_MENU) {
      // leave module – cleanup handled by main
      return;
    }
    if (screen == SCR_MSG) {
      screen = prevScreen;
      needsRedraw = true;
      return;
    }
    if (screen == SCR_CONFIRM) {
      screen = prevScreen;
      needsRedraw = true;
      return;
    }
    // default: return to main RFID menu
    screen = SCR_MENU;
    menuIndex = 0;
    needsRedraw = true;
    bruteRunning = false;
    return;
  }

  if (screen == SCR_CONFIRM) {
    if (btnPressed(BUTTON_PIN_UP) || btnPressed(BUTTON_PIN_DOWN)) {
      confirmSel = 1 - confirmSel;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      if (confirmSel == 1 && confirmYesAction) {
        confirmYesAction();
      }
      screen = prevScreen;
      needsRedraw = true;
    }
    return;
  }

  if (screen == SCR_MSG) {
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      screen = prevScreen;
      needsRedraw = true;
    }
    return;
  }

  // ---- Menu navigation ----
  if (screen == SCR_MENU) {
    const int MENU_COUNT = 6;
    if (btnPressed(BUTTON_PIN_UP)) {
      menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_DOWN)) {
      menuIndex = (menuIndex + 1) % MENU_COUNT;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      switch (menuIndex) {
        case 0: screen = SCR_SCANNER; break;
        case 1: screen = SCR_DUMPER; progressPct = 0; break;
        case 2:
          loadSavedList();
          listIndex = 0; listScroll = 0;
          screen = SCR_CLONER_SELECT;
          break;
        case 3:
          loadSavedList();
          listIndex = 0; listScroll = 0;
          screen = SCR_SAVED_LIST;
          break;
        case 4: screen = SCR_TOOLS; menuIndex = 0; break;
        case 5: // Batch mode entry
          loadSavedList();
          listIndex = 0;
          screen = SCR_BATCH;
          break;
      }
      needsRedraw = true;
    }
    return;
  }

  // ---- Scanner ----
  if (screen == SCR_SCANNER) {
    // continuous – no extra buttons needed
    return;
  }

  // ---- Dumper ----
  if (screen == SCR_DUMPER) {
    if (btnPressed(BUTTON_PIN_RIGHT) && hasCurrentCard) {
      // save current
      if (saveCardToFS(currentCard)) {
        awardXP(15);
        showMsg("Saved to flash", currentCard.name);
      } else {
        showMsg("Save failed", "SPIFFS error");
      }
    }
    return;
  }

  // ---- Cloner select saved ----
  if (screen == SCR_CLONER_SELECT) {
    int n = (int)savedList.size();
    if (n == 0) return;
    if (btnPressed(BUTTON_PIN_UP)) {
      listIndex = (listIndex - 1 + n) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_DOWN)) {
      listIndex = (listIndex + 1) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      selectedSavedIdx = listIndex;
      screen = SCR_CLONER_MODE;
      needsRedraw = true;
    }
    return;
  }

  // ---- Cloner mode ----
  if (screen == SCR_CLONER_MODE) {
    if (btnPressed(BUTTON_PIN_UP) || btnPressed(BUTTON_PIN_DOWN)) {
      cloneUidOnly = !cloneUidOnly;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      screen = SCR_CLONER_WRITE;
      needsRedraw = true;
    }
    return;
  }

  // ---- Cloner write ----
  if (screen == SCR_CLONER_WRITE) {
    // action is performed in loop when card present
    return;
  }

  // ---- Saved list ----
  if (screen == SCR_SAVED_LIST) {
    int n = (int)savedList.size();
    if (n == 0) return;
    if (btnPressed(BUTTON_PIN_UP)) {
      listIndex = (listIndex - 1 + n) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_DOWN)) {
      listIndex = (listIndex + 1) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      selectedSavedIdx = listIndex;
      screen = SCR_SAVED_DETAIL;
      needsRedraw = true;
    }
    return;
  }

  // ---- Saved detail ----
  if (screen == SCR_SAVED_DETAIL) {
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      // delete with confirm
      requestConfirm("Delete card?", "Cannot be undone", []() {
        if (selectedSavedIdx >= 0 && selectedSavedIdx < (int)savedList.size()) {
          deleteCardFromFS(savedList[selectedSavedIdx].filename);
          showMsg("Deleted", "");
          screen = SCR_SAVED_LIST;
        }
      });
    }
    return;
  }

  // ---- Tools menu ----
  if (screen == SCR_TOOLS) {
    const int TOOL_COUNT = 5;
    if (btnPressed(BUTTON_PIN_UP)) {
      menuIndex = (menuIndex - 1 + TOOL_COUNT) % TOOL_COUNT;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_DOWN)) {
      menuIndex = (menuIndex + 1) % TOOL_COUNT;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      switch (menuIndex) {
        case 0: screen = SCR_ACCESS; break;
        case 1: screen = SCR_SCANLOG; listIndex = 0; break;
        case 2: screen = SCR_BRUTE; bruteSector = 0; bruteKeyIdx = 0; bruteRunning = false; break;
        case 3: screen = SCR_FORMAT; break;
        case 4:
          loadSavedList();
          listIndex = 0;
          emulateIdx = -1;
          emulateRandom = false;
          screen = SCR_EMULATOR;
          break;
      }
      needsRedraw = true;
    }
    return;
  }

  // ---- Emulator ----
  if (screen == SCR_EMULATOR) {
    int n = (int)savedList.size() + 1; // +1 for random
    if (btnPressed(BUTTON_PIN_UP)) {
      listIndex = (listIndex - 1 + n) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_DOWN)) {
      listIndex = (listIndex + 1) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      if (listIndex == 0) {
        emulateRandom = true;
        emulateIdx = -1;
      } else {
        emulateRandom = false;
        emulateIdx = listIndex - 1;
      }
      showMsg("Emulate selected", "RC522=reader only");
    }
    return;
  }

  // ---- Batch ----
  if (screen == SCR_BATCH) {
    int n = (int)savedList.size();
    if (n == 0) return;
    if (btnPressed(BUTTON_PIN_UP)) {
      listIndex = (listIndex - 1 + n) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_DOWN)) {
      listIndex = (listIndex + 1) % n;
      needsRedraw = true;
    }
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      selectedSavedIdx = listIndex;
      batchMode = true;
      batchSuccess = 0;
      batchFail = 0;
      cloneUidOnly = false; // full by default for batch
      screen = SCR_CLONER_WRITE;
      needsRedraw = true;
    }
    return;
  }

  // ---- Format ----
  if (screen == SCR_FORMAT) {
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      requestConfirm("Format magic?", "Erases all data", []() {
        if (formatMagicCard()) {
          awardXP(10);
          showMsg("Format OK", "Magic card reset");
        } else {
          showMsg("Format fail", "Not magic / no card");
        }
      });
    }
    return;
  }

  // ---- Brute ----
  if (screen == SCR_BRUTE) {
    if (btnPressed(BUTTON_PIN_RIGHT)) {
      bruteRunning = !bruteRunning;
      needsRedraw = true;
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawHeader(const char* title) {
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(2, 10, title);
  u8g2.drawHLine(0, 12, 128);
}

static void drawMenu() {
  u8g2.clearBuffer();
  drawHeader("RFID Menu");
  const char* items[] = {
    "1.Scanner",
    "2.Dumper",
    "3.Cloner",
    "4.Saved Cards",
    "5.Tools",
    "6.Batch Clone"
  };
  u8g2.setFont(u8g2_font_6x10_tr);
  for (int i = 0; i < 6; i++) {
    int y = 24 + i * 7;
    if (i == menuIndex) {
      u8g2.drawBox(0, y - 6, 128, 8);
      u8g2.setDrawColor(0);
      u8g2.drawStr(4, y, items[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(4, y, items[i]);
    }
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawScanner() {
  u8g2.clearBuffer();
  drawHeader("Scanner");
  u8g2.setFont(u8g2_font_6x10_tr);

  if (!rfidReady) {
    u8g2.drawStr(4, 30, "RC522 not found");
    u8g2.drawStr(4, 42, "Check wiring");
  } else if (!hasCurrentCard) {
    u8g2.drawStr(4, 28, "Present card...");
    drawFieldBars(90, 20, fieldBars);
    u8g2.drawStr(4, 48, "Field:");
  } else {
    char line[22];
    uidToStr(currentCard.uid, currentCard.uidLen, line, sizeof(line));
    u8g2.drawStr(4, 24, line);

    snprintf(line, sizeof(line), "SAK:%02X ATQA:%04X", currentCard.sak, currentCard.atqa);
    u8g2.drawStr(4, 34, line);

    u8g2.drawStr(4, 44, currentCard.typeName);

    if (currentCard.isMagic) {
      u8g2.drawStr(4, 54, "UID: REWRITABLE");
    } else {
      u8g2.drawStr(4, 54, "UID: locked");
    }
    drawFieldBars(100, 48, fieldBars);
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawDumper() {
  u8g2.clearBuffer();
  drawHeader("Dumper");
  u8g2.setFont(u8g2_font_6x10_tr);

  if (!hasCurrentCard) {
    u8g2.drawStr(4, 30, "Scan a card first");
    u8g2.drawStr(4, 42, "(use Scanner)");
  } else {
    char line[22];
    uidToStr(currentCard.uid, currentCard.uidLen, line, sizeof(line));
    u8g2.drawStr(4, 24, line);
    snprintf(line, sizeof(line), "Blocks: %u", currentCard.blockCount);
    u8g2.drawStr(4, 34, line);

    if (progressPct > 0 && progressPct < 100) {
      drawProgressBar(42, progressPct);
      u8g2.drawStr(4, 60, progressLabel);
    } else {
      u8g2.drawStr(4, 50, "RIGHT=Save dump");
    }
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawClonerSelect() {
  u8g2.clearBuffer();
  drawHeader("Cloner - Select");
  u8g2.setFont(u8g2_font_6x10_tr);

  if (savedList.empty()) {
    u8g2.drawStr(4, 30, "No saved cards");
    u8g2.drawStr(4, 42, "Dump & save first");
  } else {
    int visible = 4;
    if (listIndex < listScroll) listScroll = listIndex;
    if (listIndex >= listScroll + visible) listScroll = listIndex - visible + 1;

    for (int i = 0; i < visible && (listScroll + i) < (int)savedList.size(); i++) {
      int idx = listScroll + i;
      int y = 24 + i * 10;
      char line[22];
      snprintf(line, sizeof(line), "%s%s",
               savedList[idx].isMagic ? "*" : " ",
               savedList[idx].name);
      if (idx == listIndex) {
        u8g2.drawBox(0, y - 7, 128, 9);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, line);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(2, y, line);
      }
    }
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawClonerMode() {
  u8g2.clearBuffer();
  drawHeader("Clone Mode");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(4, 28, cloneUidOnly ? "> UID only" : "  UID only");
  u8g2.drawStr(4, 40, !cloneUidOnly ? "> Full dump" : "  Full dump");
  u8g2.drawStr(4, 58, "RIGHT=Continue");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawClonerWrite() {
  u8g2.clearBuffer();
  drawHeader(batchMode ? "Batch Clone" : "Write Target");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(4, 26, "Present TARGET card");
  if (selectedSavedIdx >= 0 && selectedSavedIdx < (int)savedList.size()) {
    u8g2.drawStr(4, 38, savedList[selectedSavedIdx].name);
  }
  if (batchMode) {
    char line[22];
    snprintf(line, sizeof(line), "OK:%d Fail:%d", batchSuccess, batchFail);
    u8g2.drawStr(4, 50, line);
  }
  if (progressPct > 0 && progressPct < 100) {
    drawProgressBar(54, progressPct);
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawSavedList() {
  u8g2.clearBuffer();
  drawHeader("Saved Cards");
  u8g2.setFont(u8g2_font_6x10_tr);
  if (savedList.empty()) {
    u8g2.drawStr(4, 30, "Empty");
  } else {
    int visible = 4;
    if (listIndex < listScroll) listScroll = listIndex;
    if (listIndex >= listScroll + visible) listScroll = listIndex - visible + 1;
    for (int i = 0; i < visible && (listScroll + i) < (int)savedList.size(); i++) {
      int idx = listScroll + i;
      int y = 24 + i * 10;
      if (idx == listIndex) {
        u8g2.drawBox(0, y - 7, 128, 9);
        u8g2.setDrawColor(0);
        u8g2.drawStr(2, y, savedList[idx].name);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawStr(2, y, savedList[idx].name);
      }
    }
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawSavedDetail() {
  u8g2.clearBuffer();
  drawHeader("Card Detail");
  u8g2.setFont(u8g2_font_6x10_tr);
  if (selectedSavedIdx >= 0 && selectedSavedIdx < (int)savedList.size()) {
    const SavedMeta& m = savedList[selectedSavedIdx];
    char line[22];
    uidToStr(m.uid, m.uidLen, line, sizeof(line));
    u8g2.drawStr(4, 24, line);
    snprintf(line, sizeof(line), "Blocks:%u Magic:%s", m.blockCount, m.isMagic ? "Y" : "N");
    u8g2.drawStr(4, 36, line);
    u8g2.drawStr(4, 54, "RIGHT=Delete");
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawTools() {
  u8g2.clearBuffer();
  drawHeader("Tools");
  const char* items[] = {
    "Access Conditions",
    "Scan Log",
    "Brute Keys",
    "Format Magic",
    "Emulator"
  };
  u8g2.setFont(u8g2_font_6x10_tr);
  for (int i = 0; i < 5; i++) {
    int y = 24 + i * 8;
    if (i == menuIndex) {
      u8g2.drawBox(0, y - 6, 128, 8);
      u8g2.setDrawColor(0);
      u8g2.drawStr(4, y, items[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(4, y, items[i]);
    }
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawAccess() {
  u8g2.clearBuffer();
  drawHeader("Access Bits");
  u8g2.setFont(u8g2_font_5x7_tr);
  if (!hasCurrentCard || !currentCard.blockValid[3]) {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(4, 30, "Dump card first");
    u8g2.drawStr(4, 42, "(sector 0 trailer)");
  } else {
    char a[22], b[22], ac[22];
    decodeAccessBits(currentCard.data[3], a, b, ac);
    u8g2.drawStr(2, 24, a);
    u8g2.drawStr(2, 34, b);
    u8g2.drawStr(2, 44, ac);
    u8g2.drawStr(2, 56, "Sector 0 trailer");
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawScanLog() {
  u8g2.clearBuffer();
  drawHeader("Scan Log");
  u8g2.setFont(u8g2_font_5x7_tr);
  if (scanLogCount == 0) {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(4, 30, "Log empty");
  } else {
    int start = (scanLogHead - scanLogCount + SCANLOG_SIZE) % SCANLOG_SIZE;
    int visible = 5;
    for (int i = 0; i < visible && i < scanLogCount; i++) {
      int idx = (start + scanLogCount - 1 - i + SCANLOG_SIZE) % SCANLOG_SIZE;
      char line[24];
      char uid[16];
      uidToStr(scanLog[idx].uid, scanLog[idx].uidLen, uid, sizeof(uid));
      snprintf(line, sizeof(line), "%s%s", scanLog[idx].isMagic ? "*" : " ", uid);
      u8g2.drawStr(2, 22 + i * 8, line);
    }
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawBrute() {
  u8g2.clearBuffer();
  drawHeader("Brute Keys");
  u8g2.setFont(u8g2_font_6x10_tr);
  char line[22];
  snprintf(line, sizeof(line), "Sector %d Key %d/%d", bruteSector, bruteKeyIdx + 1, NUM_COMMON_KEYS);
  u8g2.drawStr(4, 26, line);
  if (bruteFoundKey[0]) {
    u8g2.drawStr(4, 38, "Found:");
    u8g2.drawStr(4, 48, bruteFoundKey);
  } else if (bruteRunning) {
    u8g2.drawStr(4, 42, "Running...");
  } else {
    u8g2.drawStr(4, 42, "RIGHT=Start/Stop");
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawFormat() {
  u8g2.clearBuffer();
  drawHeader("Format Magic");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(4, 28, "Present magic card");
  u8g2.drawStr(4, 40, "RIGHT=Format");
  u8g2.drawStr(4, 56, "Destructive!");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawEmulator() {
  u8g2.clearBuffer();
  drawHeader("Emulator");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 22, "NOTE: RC522 cannot");
  u8g2.drawStr(2, 30, "truly emulate cards.");
  u8g2.setFont(u8g2_font_6x10_tr);

  int n = (int)savedList.size() + 1;
  const char* label = (listIndex == 0) ? "Random UID" :
                      (listIndex - 1 < (int)savedList.size() ?
                       savedList[listIndex - 1].name : "?");
  char line[22];
  snprintf(line, sizeof(line), "> %s", label);
  u8g2.drawStr(4, 48, line);
  u8g2.drawStr(4, 60, "RIGHT=Select");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawBatch() {
  u8g2.clearBuffer();
  drawHeader("Batch Clone");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(4, 24, "Select source:");
  if (savedList.empty()) {
    u8g2.drawStr(4, 40, "No saved cards");
  } else {
    char line[22];
    snprintf(line, sizeof(line), "%s", savedList[listIndex].name);
    u8g2.drawStr(4, 40, line);
    u8g2.drawStr(4, 56, "RIGHT=Start batch");
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawConfirm() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  if (confirmTitle) u8g2.drawStr(4, 14, confirmTitle);
  if (confirmMsg) u8g2.drawStr(4, 28, confirmMsg);
  u8g2.drawStr(20, 50, confirmSel == 0 ? "> No" : "  No");
  u8g2.drawStr(80, 50, confirmSel == 1 ? "> Yes" : "  Yes");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawMsg() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  int w1 = u8g2.getStrWidth(msgLine1);
  u8g2.drawStr((128 - w1) / 2, 28, msgLine1);
  if (msgLine2[0]) {
    int w2 = u8g2.getStrWidth(msgLine2);
    u8g2.drawStr((128 - w2) / 2, 42, msgLine2);
  }
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
  if (millis() > msgUntil) {
    screen = prevScreen;
    needsRedraw = true;
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void rfidSetup() {
  pinMode(BUTTON_PIN_UP, INPUT_PULLUP);
  pinMode(BUTTON_PIN_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_CENTER, INPUT_PULLUP);

  screen = SCR_MENU;
  menuIndex = 0;
  hasCurrentCard = false;
  needsRedraw = true;
  bruteRunning = false;
  batchMode = false;
  progressPct = 0;
  scanLogCount = 0;
  scanLogHead = 0;

  SPIFFS.begin(true);
  loadSavedList();

  if (!initReader()) {
    showMsg("RC522 not found", "Check SS/RST pins");
  } else {
    blinkColor(0, 0, 60);
  }
}

void rfidCleanup() {
  haltReader();
  nrf24PowerDown();   // leave bus clean
  stopBlinking();
}

void rfidLoop() {
  handleButtons();

  // Auto-dismiss message
  if (screen == SCR_MSG && millis() > msgUntil) {
    screen = prevScreen;
    needsRedraw = true;
  }

  // ---- Scanner continuous read ----
  if (screen == SCR_SCANNER && rfidReady) {
    fieldBars = measureFieldBars();
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll > 250) {
      lastPoll = millis();
      CardDump tmp;
      if (readCardBasic(tmp)) {
        currentCard = tmp;
        hasCurrentCard = true;
        addToScanLog(tmp);
        awardXP(2);          // small XP for each unique-ish scan
        blinkColor(0, 40, 0);
        needsRedraw = true;
        haltReader();
        delay(300);          // debounce card
      } else {
        needsRedraw = true;  // update field bars
      }
    }
  }

  // ---- Dumper: perform full dump when card already known ----
  if (screen == SCR_DUMPER && hasCurrentCard && rfidReady && progressPct == 0) {
    // wait for user to present same card again for dump
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      // verify UID matches
      bool match = (mfrc522.uid.size == currentCard.uidLen);
      if (match) {
        for (uint8_t i = 0; i < currentCard.uidLen; i++) {
          if (mfrc522.uid.uidByte[i] != currentCard.uid[i]) { match = false; break; }
        }
      }
      if (match) {
        dumpCardFull(currentCard);
        awardXP(20);
        needsRedraw = true;
      }
      haltReader();
    }
  }

  // ---- Cloner write / batch ----
  if (screen == SCR_CLONER_WRITE && rfidReady && selectedSavedIdx >= 0) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 600) {
      lastTry = millis();
      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        // Use static _loadBuf to avoid ~4 KB stack allocation
        if (loadCardFromFS(savedList[selectedSavedIdx].filename, _loadBuf)) {
          bool ok = false;
          if (cloneUidOnly) {
            ok = writeUidOnly(_loadBuf);
          } else {
            ok = writeFullDump(_loadBuf);
          }
          if (ok) {
            awardXP(cloneUidOnly ? 25 : 40);
            blinkColor(0, 80, 0);
            if (batchMode) {
              batchSuccess++;
              showMsg("Clone OK", "Next card...");
            } else {
              showMsg("Clone success", _loadBuf.name);
              screen = SCR_MENU;
            }
          } else {
            blinkColor(80, 0, 0);
            if (batchMode) {
              batchFail++;
              showMsg("Clone fail", "Try again");
            } else {
              showMsg("Clone failed", "Not magic?");
            }
          }
        }
        haltReader();
        needsRedraw = true;
      }
    }
  }

  // ---- Brute force loop ----
  if (screen == SCR_BRUTE && bruteRunning && rfidReady) {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
      // wait for card
    } else {
      MFRC522::MIFARE_Key key;
      memcpy(key.keyByte, COMMON_KEYS[bruteKeyIdx], 6);
      byte block = bruteSector * 4;
      MFRC522::StatusCode st = mfrc522.PCD_Authenticate(
          MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid));
      if (st == MFRC522::STATUS_OK) {
        snprintf(bruteFoundKey, sizeof(bruteFoundKey),
                 "%02X%02X%02X%02X%02X%02X",
                 key.keyByte[0], key.keyByte[1], key.keyByte[2],
                 key.keyByte[3], key.keyByte[4], key.keyByte[5]);
        bruteRunning = false;
        awardXP(30);
        blinkColor(0, 80, 0);
      } else {
        bruteKeyIdx++;
        if (bruteKeyIdx >= NUM_COMMON_KEYS) {
          bruteKeyIdx = 0;
          bruteSector++;
          if (bruteSector > 15) {
            bruteRunning = false;
            showMsg("Brute done", "No key found");
          }
        }
      }
      haltReader();
      needsRedraw = true;
      delay(30);
    }
  }

  // ---- Draw ----
  if (needsRedraw || screen == SCR_SCANNER || screen == SCR_MSG ||
      screen == SCR_CLONER_WRITE || screen == SCR_BRUTE) {
    needsRedraw = false;
    switch (screen) {
      case SCR_MENU:          drawMenu(); break;
      case SCR_SCANNER:       drawScanner(); break;
      case SCR_DUMPER:        drawDumper(); break;
      case SCR_CLONER_SELECT: drawClonerSelect(); break;
      case SCR_CLONER_MODE:   drawClonerMode(); break;
      case SCR_CLONER_WRITE:  drawClonerWrite(); break;
      case SCR_SAVED_LIST:    drawSavedList(); break;
      case SCR_SAVED_DETAIL:  drawSavedDetail(); break;
      case SCR_TOOLS:         drawTools(); break;
      case SCR_BRUTE:         drawBrute(); break;
      case SCR_ACCESS:        drawAccess(); break;
      case SCR_SCANLOG:       drawScanLog(); break;
      case SCR_FORMAT:        drawFormat(); break;
      case SCR_EMULATOR:      drawEmulator(); break;
      case SCR_BATCH:         drawBatch(); break;
      case SCR_CONFIRM:       drawConfirm(); break;
      case SCR_MSG:           drawMsg(); break;
    }
  }
}
