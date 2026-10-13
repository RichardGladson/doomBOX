/**
 * CORE-side ESP-NOW manager implementation
 */

#include "../include/reaper_manager.h"
#include <string.h>

#define CORE_BEACON_INTERVAL_MS     1000
#define CORE_HEARTBEAT_INTERVAL_MS  1000
#define CORE_HEARTBEAT_TIMEOUT_MS   5000

ReaperManager* ReaperManager::instance_ = nullptr;

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
