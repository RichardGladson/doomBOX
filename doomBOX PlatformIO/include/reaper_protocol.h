/**
 * DOOMBOX Protocol definitions (Core side)
 * Must stay binary-compatible with ESP32-Reaper protocol.h
 */

#ifndef DOOMBOX_REAPER_PROTOCOL_H
#define DOOMBOX_REAPER_PROTOCOL_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

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
    uint8_t  magic;
    uint8_t  version;
    PacketType type;
    DeviceRole sender_role;
    uint8_t  sender_id[DEVICE_ID_LEN];
    uint16_t sequence;
    uint8_t  payload_len;
    uint8_t  checksum;
};

#define PROTOCOL_HEADER_SIZE  sizeof(ProtocolHeader)

struct ProtocolPacket {
    ProtocolHeader header;
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
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

bool isValidHeader(const ProtocolHeader& hdr);
uint8_t computeChecksum(const ProtocolHeader& hdr);
void fillHeader(ProtocolHeader& hdr, PacketType type, DeviceRole role,
                const uint8_t* sender_id, uint16_t seq, uint8_t payload_len);

size_t packPacket(uint8_t* out, size_t out_len,
                  PacketType type, DeviceRole role,
                  const uint8_t* sender_id, uint16_t seq,
                  const void* payload, uint8_t payload_len);

bool unpackPacket(const uint8_t* data, size_t len, ProtocolPacket& pkt);

const char* packetTypeToString(PacketType t);
const char* modeToString(OperatingMode m);

} // namespace Protocol

#endif // DOOMBOX_REAPER_PROTOCOL_H
