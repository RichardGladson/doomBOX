/*
    doomBOX by Richard Gladson
    https://github.com/RichardGladson/doomBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/

/**
 * DOOMBOX Protocol implementation (Core side)
 */

#include "../include/reaper_protocol.h"
#include <string.h>

namespace Protocol {

bool isValidHeader(const ProtocolHeader& hdr) {
    if (hdr.magic != PROTOCOL_MAGIC) return false;
    if (hdr.version == 0 || hdr.version > REAPER_PROTOCOL_VERSION + 2) return false;
    if (hdr.payload_len > PROTOCOL_MAX_PAYLOAD) return false;
    if (hdr.checksum != computeChecksum(hdr)) return false;
    return true;
}

uint8_t computeChecksum(const ProtocolHeader& hdr) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&hdr);
    uint8_t cs = 0;
    for (size_t i = 0; i < offsetof(ProtocolHeader, checksum); ++i) {
        cs ^= p[i];
    }
    return cs;
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
