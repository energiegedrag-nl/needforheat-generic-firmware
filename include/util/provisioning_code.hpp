#pragma once

/*
 * Provisioning QR code, version 2: "NFH:" + 24 Base32 characters (RFC 4648 alphabet, no padding).
 *
 * Everything is upper case A-Z, 2-7 and ':', so the QR code uses alphanumeric mode (5.5 bits per character)
 * instead of byte mode for JSON: version 2 (25x25 modules) instead of version 5 (37x37), i.e. 6 instead of
 * 4 screen pixels per module on the 200x200 e-ink screen. Base32 rather than BASE45: BASE45 also uses space,
 * '%', '+', '/' and ':', which break links and are hard to type; Base32 is URL-safe and case-insensitive.
 *
 * Payload, 15 bytes:
 *   [0]      0x21: format version 2 (high nibble), flags (low nibble; bit 0 = BLE transport)
 *   [1..2]   device type hash (CRC-16, big-endian), as in the BLE name
 *   [3..5]   last three bytes of the station MAC address, as in the BLE name
 *   [6..13]  proof of possession / activation secret, unsigned 64-bit big-endian; as text: decimal
 *   [14]     CRC-8 (polynomial 0x07) of bytes 0..13, catches typing errors when the code is entered by hand
 * The BLE name follows from bytes 1..5: "%04X-%02X%02X%02X".
 *
 * Header-only and free of ESP-IDF, so it can be tested on the host (test/host/provisioning_code_test.cpp).
 */

#include <cstdint>
#include <string>

namespace ProvisioningCode
{
    constexpr const char *PREFIX = "NFH:";
    constexpr uint8_t FORMAT_V2_BLE = 0x21;

    inline uint8_t Crc8(const uint8_t *data, size_t length)
    {
        uint8_t crc = 0;
        for (size_t i = 0; i < length; i++)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; bit++)
                crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
        }
        return crc;
    }

    /** RFC 4648 Base32, upper case, without padding. */
    inline std::string Base32(const uint8_t *data, size_t length)
    {
        static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        std::string out;
        uint32_t buffer = 0;
        int bits = 0;
        for (size_t i = 0; i < length; i++)
        {
            buffer = (buffer << 8) | data[i];
            bits += 8;
            while (bits >= 5)
            {
                out += alphabet[(buffer >> (bits - 5)) & 0x1F];
                bits -= 5;
            }
        }
        if (bits > 0)
            out += alphabet[(buffer << (5 - bits)) & 0x1F];
        return out;
    }

    inline std::string Make(uint16_t deviceTypeHash, const uint8_t macTail[3], uint64_t pop)
    {
        uint8_t payload[15];
        payload[0] = FORMAT_V2_BLE;
        payload[1] = static_cast<uint8_t>(deviceTypeHash >> 8);
        payload[2] = static_cast<uint8_t>(deviceTypeHash);
        for (int i = 0; i < 3; i++)
            payload[3 + i] = macTail[i];
        for (int i = 0; i < 8; i++)
            payload[6 + i] = static_cast<uint8_t>(pop >> (8 * (7 - i)));
        payload[14] = Crc8(payload, 14);
        return std::string(PREFIX) + Base32(payload, sizeof(payload));
    }
} // namespace ProvisioningCode
