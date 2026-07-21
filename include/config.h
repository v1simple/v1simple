/**
 * Configuration file for V1 Gen2 Simple Display
 * Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, 640x172)
 *
 * This file contains:
 * - Firmware version and board definitions
 * - BLE UUIDs for V1 Gen2 protocol
 * - ESP packet framing constants
 * - Display layout and timing parameters
 */

#pragma once

// Include display driver abstraction
#include "display_driver.h"

// Firmware Version
#define FIRMWARE_VERSION "1.0.6"

// BLE Configuration
// V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#ble-gatt-proxy-surface
#define V1_SERVICE_UUID "92A0AFF4-9E05-11E2-AA59-F23C91AEC05E"
#define V1_DISPLAY_DATA_UUID "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E" // V1 out SHORT (notify) - display data
#define V1_DISPLAY_DATA_LONG_UUID                                                                                      \
    "92A0B4E0-9E05-11E2-AA59-F23C91AEC05E" // V1 out LONG (notify) - alert data, voltage responses
#define V1_NOTIFY_ALT_UUID                                                                                             \
    "92A0BCE0-9E05-11E2-AA59-F23C91AEC05E" // Additional notify characteristic - compatibility stub; proxy never
                                           // notifies on it but companion apps (V1Driver, JBV1, etc.) expect the UUID
                                           // present in the V1 attribute table
#define V1_COMMAND_WRITE_UUID "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E"      // Client out, V1 in
#define V1_COMMAND_WRITE_ALT_UUID "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E"  // Alternate writable characteristic
#define V1_COMMAND_WRITE_LONG_UUID "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E" // Long writable characteristic (optional)
// 16-bit short UUIDs extracted from the V1 128-bit UUIDs above (bytes 4-5 of the string form)
#define V1_SHORT_UUID_DISPLAY_LONG ((uint16_t)0xB4E0) // Short UUID for V1_DISPLAY_DATA_LONG_UUID
#define V1_SHORT_UUID_COMMAND_LONG ((uint16_t)0xB8D2) // Short UUID for V1_COMMAND_WRITE_LONG_UUID
#define V1_CCCD_DESCRIPTOR_UUID ((uint16_t)0x2902)    // Client Characteristic Configuration Descriptor UUID
#define SCAN_DURATION 10000                           // 10-second scan in milliseconds (stops early when V1 found)
#define RECONNECT_DELAY 100                           // 100ms delay between scan attempts

// ESP Packet Constants
#define ESP_PACKET_START 0xAA
#define ESP_PACKET_END 0xAB
#define ESP_PACKET_ORIGIN_V1 0x0A // V1 with checksum
#define ESP_PACKET_DEST_V1 0x0A
#define ESP_PACKET_REMOTE 0x06 // V1connection type (remote app) - was 0x04

// Packet IDs
// V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#packet-ids-and-volume-responses
#define PACKET_ID_DISPLAY_DATA 0x31    // infDisplayData
#define PACKET_ID_ALERT_DATA 0x43      // respAlertData
#define PACKET_ID_REQ_START_ALERT 0x41 // reqStartAlertData (matches T4S3 logic)
#define PACKET_ID_VERSION 0x01         // reqVersion (request we send TO the V1)
#define PACKET_ID_RESP_VERSION                                                                                         \
    0x02                                // respVersion (reply V1 sends back). Per
                                        // Valentine's PacketId.java: REQVERSION=0x01,
                                        // RESPVERSION=0x02. Incoming version must
                                        // match 0x02; 0x01 is only the outbound id.
#define PACKET_ID_TURN_OFF_DISPLAY 0x32 // reqTurnOffMainDisplay (dark mode)
#define PACKET_ID_TURN_ON_DISPLAY 0x33  // reqTurnOnMainDisplay
#define PACKET_ID_MUTE_ON 0x34          // reqMuteOn
#define PACKET_ID_MUTE_OFF 0x35         // reqMuteOff
#define PACKET_ID_REQ_WRITE_VOLUME 0x39 // reqWriteVolume (mainVolume, mutedVolume, aux0)
// Dedicated all-volume request/response per the Valentine protocol summary:
// docs/V1_PROTOCOL_REFERENCES.md#packet-ids-and-volume-responses.
// RESPALLVOLUME payload is exactly 4 bytes: [main, muted, savedMain, savedMuted].
// We still parse the aux2-nibble in display packets as a fallback for older
// firmware / before the first 0x3D arrives.
#define PACKET_ID_REQ_ALL_VOLUME 0x3C   // reqAllVolume (no payload)
#define PACKET_ID_RESP_ALL_VOLUME 0x3D  // respAllVolume (4-byte payload)
#define PACKET_ID_REQ_USER_BYTES 0x11   // reqUserBytes
#define PACKET_ID_RESP_USER_BYTES 0x12  // respUserBytes
#define PACKET_ID_WRITE_USER_BYTES 0x13 // reqWriteUserBytes

// Timing
#define DISPLAY_UPDATE_MS 50 // Update display every 50ms (snappier response)
