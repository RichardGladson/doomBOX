/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/

/**
 * CORE-side ESP-NOW manager for automatic Reaper discovery, pairing and control.
 */

#ifndef DOOMBOX_REAPER_MANAGER_H
#define DOOMBOX_REAPER_MANAGER_H

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "reaper_protocol.h"

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

#endif // DOOMBOX_REAPER_MANAGER_H
