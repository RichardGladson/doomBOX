// language: C++, file: reaper.cpp, target: ESP32-WROOM (doomBOX), Arduino framework
// *Consolidated reaper protocol, manager, and UI — replaces reaper_protocol.h/cpp,
//  reaper_manager.h/cpp, and reaper_ui.cpp. Binary-compatible with ESP32-Reaper.*
/*
doomBOX by Richard Gladson
https://github.com/RichardGladson/doomBOX
Copyright (c) 2026 Richard Gladson
Licensed under the GNU General Public License v3.0
https://www.gnu.org/licenses/gpl-3.0.html
SPDX-License-Identifier: GPL-3.0-only
*/

#include <Arduino.h>
#include <string.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <esp_now.h>

#include "../include/pindefs.h"
#include "../include/display_mirror.h"
#include "../include/sleep_manager.h"

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// ============================================================================
// PROTOCOL DEFINITIONS
// ============================================================================

#define PROTOCOL_MAGIC            0xDB
#define REAPER_PROTOCOL_VERSION   0x01
#define DEVICE_ID_LEN             6
#define PROTOCOL_MAX_PAYLOAD      200
#define ESPNOW_CHANNEL            1

#pragma pack(push, 1)

enum class PacketType : uint8_t {
  DISCOVERY_BEACON      = 0x01,
  PAIRING_REQUEST       = 0x02,
  PAIRING_RESPONSE      = 0x03,
  HEARTBEAT             = 0x04,
  DEVICE_IDENT          = 0x10,
  STATUS_REQUEST        = 0x11,
  STATUS_RESPONSE       = 0x12,
  ERROR_RESPONSE        = 0x13,
  MODE_COMMAND          = 0x20,
  RADIO_CONFIG_COMMAND  = 0x21,
  SCAN_TEST_COMMAND     = 0x22,
  STOP_COMMAND          = 0x23,
  CUSTOM_PAYLOAD        = 0xF0,
};

enum class DeviceRole : uint8_t {
  UNKNOWN   = 0x00,
  REAPER    = 0x01,
  CORE      = 0x02,
  WRAITH    = 0x03,
};

enum class OperatingMode : uint8_t {
  IDLE              = 0x00,
  BLUETOOTH_SURVEY  = 0x01,
  WIFI_SURVEY       = 0x02,
  RADIO_TEST        = 0x03,
  STOPPED           = 0x04,
};

enum class ErrorCode : uint8_t {
  NONE                  = 0x00,
  UNSUPPORTED_VERSION   = 0x01,
  MALFORMED_PACKET      = 0x02,
  INVALID_COMMAND       = 0x03,
  RADIO_FAILURE         = 0x04,
  NOT_PAIRED            = 0x05,
  BUSY                  = 0x06,
  TIMEOUT               = 0x07,
};

struct ProtocolHeader {
  uint8_t    magic;
  uint8_t    version;
  PacketType type;
  DeviceRole sender_role;
  uint8_t    sender_id[DEVICE_ID_LEN];
  uint16_t   sequence;
  uint8_t    payload_len;
  uint8_t    checksum;
};

#define PROTOCOL_HEADER_SIZE sizeof(ProtocolHeader)

struct ProtocolPacket {
  ProtocolHeader header;
  uint8_t        payload[PROTOCOL_MAX_PAYLOAD];
};

struct DiscoveryBeaconPayload {
  uint8_t core_id[DEVICE_ID_LEN];
  uint8_t protocol_version;
  uint8_t flags;
};

struct PairingRequestPayload {
  uint8_t reaper_id[DEVICE_ID_LEN];
  uint8_t fw_major;
  uint8_t fw_minor;
  uint8_t radio_count;
};

struct PairingResponsePayload {
  uint8_t status;
  uint8_t core_id[DEVICE_ID_LEN];
  uint8_t assigned_slot;
};

struct HeartbeatPayload {
  uint32_t uptime_ms;
  uint8_t  mode;
  uint8_t  radio_status;
};

struct StatusResponsePayload {
  uint8_t  mode;
  uint8_t  radio_status;
  uint8_t  core_connected;
  uint16_t free_heap;
  uint32_t uptime_ms;
};

struct ModeCommandPayload {
  uint8_t mode;
};

struct RadioConfigPayload {
  uint8_t radio_mask;
  uint8_t channel;
  uint8_t power_level;
  uint8_t data_rate;
};

struct ScanTestPayload {
  uint8_t survey_type;
  uint8_t start_channel;
  uint8_t end_channel;
  uint8_t hop_step;
};

#pragma pack(pop)

namespace Protocol {

uint8_t computeChecksum(const ProtocolHeader& hdr) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&hdr);
  uint8_t cs = 0;
  for (size_t i = 0; i < offsetof(ProtocolHeader, checksum); ++i) {
    cs ^= p[i];
  }
  return cs;
}

bool isValidHeader(const ProtocolHeader& hdr) {
  if (hdr.magic != PROTOCOL_MAGIC) return false;
  if (hdr.version == 0 || hdr.version > REAPER_PROTOCOL_VERSION + 2) return false;
  if (hdr.payload_len > PROTOCOL_MAX_PAYLOAD) return false;
  if (hdr.checksum != computeChecksum(hdr)) return false;
  return true;
}

void fillHeader(ProtocolHeader& hdr, PacketType type, DeviceRole role,
                const uint8_t* sender_id, uint16_t seq, uint8_t payload_len) {
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = PROTOCOL_MAGIC;
  hdr.version = REAPER_PROTOCOL_VERSION;
  hdr.type = type;
  hdr.sender_role = role;
  if (sender_id) {
    memcpy(hdr.sender_id, sender_id, DEVICE_ID_LEN);
  }
  hdr.sequence = seq;
  hdr.payload_len = payload_len;
  hdr.checksum = computeChecksum(hdr);
}

size_t packPacket(uint8_t* out, size_t out_len,
                  PacketType type, DeviceRole role,
                  const uint8_t* sender_id, uint16_t seq,
                  const void* payload, uint8_t payload_len) {
  if (!out || out_len < PROTOCOL_HEADER_SIZE + payload_len) return 0;
  ProtocolHeader hdr;
  fillHeader(hdr, type, role, sender_id, seq, payload_len);
  memcpy(out, &hdr, PROTOCOL_HEADER_SIZE);
  if (payload && payload_len > 0) {
    memcpy(out + PROTOCOL_HEADER_SIZE, payload, payload_len);
  }
  return PROTOCOL_HEADER_SIZE + payload_len;
}

bool unpackPacket(const uint8_t* data, size_t len, ProtocolPacket& pkt) {
  if (!data || len < PROTOCOL_HEADER_SIZE) return false;
  memcpy(&pkt.header, data, PROTOCOL_HEADER_SIZE);
  if (!isValidHeader(pkt.header)) return false;
  if (len < PROTOCOL_HEADER_SIZE + pkt.header.payload_len) return false;
  if (pkt.header.payload_len > 0) {
    memcpy(pkt.payload, data + PROTOCOL_HEADER_SIZE, pkt.header.payload_len);
  }
  return true;
}

const char* packetTypeToString(PacketType t) {
  switch (t) {
    case PacketType::DISCOVERY_BEACON:     return "DISCOVERY_BEACON";
    case PacketType::PAIRING_REQUEST:      return "PAIRING_REQUEST";
    case PacketType::PAIRING_RESPONSE:     return "PAIRING_RESPONSE";
    case PacketType::HEARTBEAT:            return "HEARTBEAT";
    case PacketType::DEVICE_IDENT:         return "DEVICE_IDENT";
    case PacketType::STATUS_REQUEST:       return "STATUS_REQUEST";
    case PacketType::STATUS_RESPONSE:      return "STATUS_RESPONSE";
    case PacketType::ERROR_RESPONSE:       return "ERROR_RESPONSE";
    case PacketType::MODE_COMMAND:         return "MODE_COMMAND";
    case PacketType::RADIO_CONFIG_COMMAND: return "RADIO_CONFIG_COMMAND";
    case PacketType::SCAN_TEST_COMMAND:    return "SCAN_TEST_COMMAND";
    case PacketType::STOP_COMMAND:         return "STOP_COMMAND";
    case PacketType::CUSTOM_PAYLOAD:       return "CUSTOM_PAYLOAD";
    default:                               return "UNKNOWN";
  }
}

const char* modeToString(OperatingMode m) {
  switch (m) {
    case OperatingMode::IDLE:             return "IDLE";
    case OperatingMode::BLUETOOTH_SURVEY: return "BT SURVEY";
    case OperatingMode::WIFI_SURVEY:      return "WIFI SURVEY";
    case OperatingMode::RADIO_TEST:       return "RADIO TEST";
    case OperatingMode::STOPPED:          return "STOPPED";
    default:                              return "UNKNOWN";
  }
}

} // namespace Protocol

// ============================================================================
// REAPER MANAGER
// ============================================================================

enum class ReaperLinkState : uint8_t {
  IDLE = 0,
  INITIALIZING,
  SEARCHING,
  PAIRED,
  ERROR,
};

struct ReaperStatus {
  bool     valid;
  uint8_t  mode;
  uint8_t  radio_status;
  uint8_t  core_connected;
  uint16_t free_heap;
  uint32_t uptime_ms;
  uint8_t  fw_major;
  uint8_t  fw_minor;
  uint8_t  radio_count;
};

class ReaperManager {
public:
  ReaperManager();
  bool begin();
  void update();
  void stop();

  ReaperLinkState getState() const { return state_; }
  bool isPaired() const { return state_ == ReaperLinkState::PAIRED; }
  const uint8_t* getReaperMac() const { return reaper_mac_; }
  const uint8_t* getLocalMac() const { return local_mac_; }
  const ReaperStatus& getStatus() const { return status_; }

  bool sendModeCommand(OperatingMode mode);
  bool sendScanCommand(OperatingMode surveyType,
                       uint8_t startCh, uint8_t endCh, uint8_t hopStep);
  bool sendStopCommand();
  bool sendStatusRequest();
  bool sendRadioConfig(uint8_t mask, uint8_t channel,
                       uint8_t power, uint8_t dataRate);

  void startDiscovery();
  void disconnect();

private:
  ReaperLinkState state_;
  uint8_t local_mac_[6];
  uint8_t reaper_mac_[6];
  bool reaper_mac_valid_;
  uint16_t sequence_;
  uint32_t last_beacon_ms_;
  uint32_t last_heartbeat_rx_ms_;
  uint32_t last_heartbeat_tx_ms_;
  ReaperStatus status_;

  static ReaperManager* instance_;

  static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);
  static void onDataSent(const uint8_t* mac, esp_now_send_status_t status);

  void handleReceived(const uint8_t* mac, const uint8_t* data, int len);
  void processPairingRequest(const ProtocolPacket& pkt, const uint8_t* mac);
  void processHeartbeat(const ProtocolPacket& pkt);
  void processStatusResponse(const ProtocolPacket& pkt);
  void processDeviceIdent(const ProtocolPacket& pkt);

  bool sendToReaper(PacketType type, const void* payload, uint8_t payload_len);
  bool sendBroadcast(PacketType type, const void* payload, uint8_t payload_len);
  bool addPeer(const uint8_t* mac);
  void removePeer(const uint8_t* mac);
};

ReaperManager* ReaperManager::instance_ = nullptr;

#define CORE_BEACON_INTERVAL_MS     1000
#define CORE_HEARTBEAT_INTERVAL_MS  1000
#define CORE_HEARTBEAT_TIMEOUT_MS   5000

ReaperManager::ReaperManager()
  : state_(ReaperLinkState::IDLE),
    reaper_mac_valid_(false),
    sequence_(0),
    last_beacon_ms_(0),
    last_heartbeat_rx_ms_(0),
    last_heartbeat_tx_ms_(0) {
  memset(local_mac_, 0, 6);
  memset(reaper_mac_, 0, 6);
  memset(&status_, 0, sizeof(status_));
  instance_ = this;
}

bool ReaperManager::begin() {
  state_ = ReaperLinkState::INITIALIZING;
  Serial.println("[REAPER] ESP-NOW initializing");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  memcpy(local_mac_, mac, 6);
  Serial.printf("[REAPER] local MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[REAPER] esp_now_init failed");
    state_ = ReaperLinkState::ERROR;
    return false;
  }

  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, bcast, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  state_ = ReaperLinkState::SEARCHING;
  last_beacon_ms_ = millis();
  Serial.println("[REAPER] searching for Reaper node");
  return true;
}

void ReaperManager::stop() {
  if (reaper_mac_valid_) {
    removePeer(reaper_mac_);
    reaper_mac_valid_ = false;
  }
  esp_now_deinit();
  state_ = ReaperLinkState::IDLE;
  status_.valid = false;
}

void ReaperManager::startDiscovery() {
  if (reaper_mac_valid_) {
    removePeer(reaper_mac_);
    reaper_mac_valid_ = false;
  }
  status_.valid = false;
  state_ = ReaperLinkState::SEARCHING;
  last_beacon_ms_ = millis();
  Serial.println("[REAPER] restarting discovery");
}

void ReaperManager::disconnect() {
  startDiscovery();
}

void ReaperManager::update() {
  if (state_ == ReaperLinkState::IDLE || state_ == ReaperLinkState::ERROR) return;

  uint32_t now = millis();

  if (state_ == ReaperLinkState::SEARCHING) {
    if (now - last_beacon_ms_ >= CORE_BEACON_INTERVAL_MS) {
      last_beacon_ms_ = now;
      DiscoveryBeaconPayload beacon = {};
      memcpy(beacon.core_id, local_mac_, DEVICE_ID_LEN);
      beacon.protocol_version = REAPER_PROTOCOL_VERSION;
      beacon.flags = 0;
      sendBroadcast(PacketType::DISCOVERY_BEACON, &beacon, sizeof(beacon));
    }
  } else if (state_ == ReaperLinkState::PAIRED) {
    if (now - last_heartbeat_rx_ms_ > CORE_HEARTBEAT_TIMEOUT_MS) {
      Serial.println("[REAPER] heartbeat timeout");
      startDiscovery();
      return;
    }
    if (now - last_heartbeat_tx_ms_ >= CORE_HEARTBEAT_INTERVAL_MS) {
      last_heartbeat_tx_ms_ = now;
      HeartbeatPayload hb = {};
      hb.uptime_ms = now;
      hb.mode = 0;
      hb.radio_status = 0;
      sendToReaper(PacketType::HEARTBEAT, &hb, sizeof(hb));
    }
  }
}

bool ReaperManager::sendModeCommand(OperatingMode mode) {
  ModeCommandPayload m = {};
  m.mode = static_cast<uint8_t>(mode);
  return sendToReaper(PacketType::MODE_COMMAND, &m, sizeof(m));
}

bool ReaperManager::sendScanCommand(OperatingMode surveyType,
                                     uint8_t startCh, uint8_t endCh, uint8_t hopStep) {
  ScanTestPayload sc = {};
  sc.survey_type = static_cast<uint8_t>(surveyType);
  sc.start_channel = startCh;
  sc.end_channel = endCh;
  sc.hop_step = hopStep ? hopStep : 1;
  return sendToReaper(PacketType::SCAN_TEST_COMMAND, &sc, sizeof(sc));
}

bool ReaperManager::sendStopCommand() {
  return sendToReaper(PacketType::STOP_COMMAND, nullptr, 0);
}

bool ReaperManager::sendStatusRequest() {
  return sendToReaper(PacketType::STATUS_REQUEST, nullptr, 0);
}

bool ReaperManager::sendRadioConfig(uint8_t mask, uint8_t channel,
                                     uint8_t power, uint8_t dataRate) {
  RadioConfigPayload cfg = {};
  cfg.radio_mask = mask;
  cfg.channel = channel;
  cfg.power_level = power;
  cfg.data_rate = dataRate;
  return sendToReaper(PacketType::RADIO_CONFIG_COMMAND, &cfg, sizeof(cfg));
}

bool ReaperManager::sendToReaper(PacketType type, const void* payload, uint8_t payload_len) {
  if (!reaper_mac_valid_ || state_ != ReaperLinkState::PAIRED) return false;
  uint8_t buf[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD];
  size_t len = Protocol::packPacket(buf, sizeof(buf), type, DeviceRole::CORE,
                                     local_mac_, sequence_++, payload, payload_len);
  if (len == 0) return false;
  return esp_now_send(reaper_mac_, buf, len) == ESP_OK;
}

bool ReaperManager::sendBroadcast(PacketType type, const void* payload, uint8_t payload_len) {
  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t buf[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD];
  size_t len = Protocol::packPacket(buf, sizeof(buf), type, DeviceRole::CORE,
                                     local_mac_, sequence_++, payload, payload_len);
  if (len == 0) return false;
  return esp_now_send(bcast, buf, len) == ESP_OK;
}

void ReaperManager::onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (instance_) instance_->handleReceived(mac, data, len);
}

void ReaperManager::onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
  (void)mac;
  (void)status;
}

void ReaperManager::handleReceived(const uint8_t* mac, const uint8_t* data, int len) {
  ProtocolPacket pkt;
  if (!Protocol::unpackPacket(data, len, pkt)) return;
  if (pkt.header.version > REAPER_PROTOCOL_VERSION + 2) return;

  switch (pkt.header.type) {
    case PacketType::PAIRING_REQUEST:
      processPairingRequest(pkt, mac);
      break;
    case PacketType::HEARTBEAT:
      processHeartbeat(pkt);
      break;
    case PacketType::STATUS_RESPONSE:
      processStatusResponse(pkt);
      break;
    case PacketType::DEVICE_IDENT:
      processDeviceIdent(pkt);
      break;
    default:
      break;
  }
}

void ReaperManager::processPairingRequest(const ProtocolPacket& pkt, const uint8_t* mac) {
  if (state_ != ReaperLinkState::SEARCHING && state_ != ReaperLinkState::PAIRED) return;
  if (pkt.header.sender_role != DeviceRole::REAPER) return;

  PairingRequestPayload req = {};
  if (pkt.header.payload_len >= sizeof(PairingRequestPayload)) {
    memcpy(&req, pkt.payload, sizeof(req));
  }

  Serial.printf("[REAPER] pairing request from %02X:%02X:%02X:%02X:%02X:%02X "
                "fw=%u.%u radios=%u\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                req.fw_major, req.fw_minor, req.radio_count);

  addPeer(mac);
  memcpy(reaper_mac_, mac, 6);
  reaper_mac_valid_ = true;

  PairingResponsePayload resp = {};
  resp.status = 0;
  memcpy(resp.core_id, local_mac_, DEVICE_ID_LEN);
  resp.assigned_slot = 0;

  uint8_t buf[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD];
  size_t len = Protocol::packPacket(buf, sizeof(buf), PacketType::PAIRING_RESPONSE,
                                     DeviceRole::CORE, local_mac_, sequence_++,
                                     &resp, sizeof(resp));
  if (len > 0) {
    esp_now_send(mac, buf, len);
  }

  state_ = ReaperLinkState::PAIRED;
  last_heartbeat_rx_ms_ = millis();
  last_heartbeat_tx_ms_ = millis();
  status_.fw_major = req.fw_major;
  status_.fw_minor = req.fw_minor;
  status_.radio_count = req.radio_count;
  status_.valid = true;
  Serial.println("[REAPER] PAIRED");
}

void ReaperManager::processHeartbeat(const ProtocolPacket& pkt) {
  if (state_ != ReaperLinkState::PAIRED) return;
  last_heartbeat_rx_ms_ = millis();
  if (pkt.header.payload_len >= sizeof(HeartbeatPayload)) {
    HeartbeatPayload hb;
    memcpy(&hb, pkt.payload, sizeof(hb));
    status_.mode = hb.mode;
    status_.radio_status = hb.radio_status;
    status_.uptime_ms = hb.uptime_ms;
    status_.valid = true;
  }
}

void ReaperManager::processStatusResponse(const ProtocolPacket& pkt) {
  if (pkt.header.payload_len < sizeof(StatusResponsePayload)) return;
  StatusResponsePayload st;
  memcpy(&st, pkt.payload, sizeof(st));
  status_.mode = st.mode;
  status_.radio_status = st.radio_status;
  status_.core_connected = st.core_connected;
  status_.free_heap = st.free_heap;
  status_.uptime_ms = st.uptime_ms;
  status_.valid = true;
  last_heartbeat_rx_ms_ = millis();
}

void ReaperManager::processDeviceIdent(const ProtocolPacket& pkt) {
  if (pkt.header.payload_len >= sizeof(PairingRequestPayload)) {
    PairingRequestPayload id;
    memcpy(&id, pkt.payload, sizeof(id));
    status_.fw_major = id.fw_major;
    status_.fw_minor = id.fw_minor;
    status_.radio_count = id.radio_count;
  }
}

bool ReaperManager::addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void ReaperManager::removePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }
}

// ============================================================================
// UI LAYER
// ============================================================================

static ReaperManager g_reaper;
static bool needsRedraw = true;
static int menuIndex = 0;
static unsigned long lastStatusReq = 0;
static unsigned long lastDraw = 0;

static const char* pairedActions[] = {
  "BT Survey",
  "WiFi Survey",
  "Stop",
  "Refresh Status",
  "Disconnect"
};
static const int PAIRED_ACTION_COUNT = 5;

#define BT_START   2
#define BT_END     80
#define WIFI_START 1
#define WIFI_END   14
#define HOP_STEP   2

void reaperUiSetup() {
  pinMode(BUTTON_PIN_UP, INPUT_PULLUP);
  pinMode(BUTTON_PIN_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_PIN_LEFT, INPUT_PULLUP);
  pinMode(BUTTON_PIN_RIGHT, INPUT_PULLUP);
  menuIndex = 0;
  needsRedraw = true;
  lastStatusReq = 0;
  lastDraw = 0;
  g_reaper.begin();
}

void reaperUiCleanup() {
  g_reaper.stop();
}

static void drawSearching() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(8, 14, "REAPER LINK");
  u8g2.drawHLine(0, 18, 128);
  u8g2.drawStr(10, 36, "Searching...");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(4, 52, "Power on ESP32-Reaper");
  u8g2.drawStr(4, 62, "SEL to exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawError() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(8, 14, "REAPER LINK");
  u8g2.drawHLine(0, 18, 128);
  u8g2.drawStr(16, 40, "ESP-NOW ERROR");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(4, 58, "SEL to exit");
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

static void drawPaired() {
  const ReaperStatus& st = g_reaper.getStatus();
  const uint8_t* mac = g_reaper.getReaperMac();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvR08_tr);
  u8g2.drawStr(8, 12, "REAPER PAIRED");
  u8g2.drawHLine(0, 15, 128);

  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 24, macStr);

  char line[32];
  snprintf(line, sizeof(line), "Mode:%s",
           Protocol::modeToString(static_cast<OperatingMode>(st.mode)));
  u8g2.drawStr(2, 33, line);

  snprintf(line, sizeof(line), "R1:%s R2:%s R3:%s",
           (st.radio_status & 0x01) ? "OK" : "--",
           (st.radio_status & 0x02) ? "OK" : "--",
           (st.radio_status & 0x04) ? "OK" : "--");
  u8g2.drawStr(2, 42, line);

  const char* act = pairedActions[menuIndex];
  u8g2.setFont(u8g2_font_helvR08_tr);
  int tw = u8g2.getStrWidth(act);
  u8g2.drawBox(0, 48, 128, 16);
  u8g2.setDrawColor(0);
  u8g2.drawStr((128 - tw) / 2, 60, act);
  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
  displayMirrorSend(u8g2);
}

void reaperUiLoop() {
  g_reaper.update();

  if (g_reaper.isPaired() && (millis() - lastStatusReq > 2000)) {
    lastStatusReq = millis();
    g_reaper.sendStatusRequest();
  }

  static unsigned long lastBtn = 0;
  if (millis() - lastBtn > 180) {
    if (digitalRead(BUTTON_PIN_UP) == LOW) {
      lastBtn = millis();
      if (g_reaper.isPaired()) {
        menuIndex = (menuIndex - 1 + PAIRED_ACTION_COUNT) % PAIRED_ACTION_COUNT;
        needsRedraw = true;
      }
    } else if (digitalRead(BUTTON_PIN_DOWN) == LOW) {
      lastBtn = millis();
      if (g_reaper.isPaired()) {
        menuIndex = (menuIndex + 1) % PAIRED_ACTION_COUNT;
        needsRedraw = true;
      }
    } else if (digitalRead(BUTTON_PIN_RIGHT) == LOW) {
      lastBtn = millis();
      if (g_reaper.isPaired()) {
        switch (menuIndex) {
          case 0:
            g_reaper.sendScanCommand(OperatingMode::BLUETOOTH_SURVEY,
                                      BT_START, BT_END, HOP_STEP);
            break;
          case 1:
            g_reaper.sendScanCommand(OperatingMode::WIFI_SURVEY,
                                      WIFI_START, WIFI_END, HOP_STEP);
            break;
          case 2:
            g_reaper.sendStopCommand();
            break;
          case 3:
            g_reaper.sendStatusRequest();
            break;
          case 4:
            g_reaper.disconnect();
            menuIndex = 0;
            break;
        }
        needsRedraw = true;
      }
    }
  }

  static ReaperLinkState prevState = ReaperLinkState::IDLE;
  ReaperLinkState st = g_reaper.getState();
  if (st != prevState) {
    prevState = st;
    needsRedraw = true;
  }

  if (needsRedraw || (millis() - lastDraw > 500)) {
    lastDraw = millis();
    needsRedraw = false;
    switch (st) {
      case ReaperLinkState::SEARCHING:
      case ReaperLinkState::INITIALIZING:
        drawSearching();
        break;
      case ReaperLinkState::PAIRED:
        drawPaired();
        break;
      case ReaperLinkState::ERROR:
        drawError();
        break;
      default:
        drawSearching();
        break;
    }
  }
}