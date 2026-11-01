// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::deye_firmware_upgrade
{

static constexpr size_t DEYE_TRAILER_SIZE = 11;
static constexpr size_t XMODEM_BLOCK_SIZE = 1024;
static constexpr size_t XMODEM_PACKET_SIZE = 3 + XMODEM_BLOCK_SIZE + 2;

struct DeyeImageInfo
{
	std::array<uint8_t, 3> selectors{};
	uint32_t payload_size{ 0 };
	uint32_t crc32{ 0 };
};

uint16_t crc16_modbus(uint8_t const *data, size_t length);
uint16_t crc16_xmodem(uint8_t const *data, size_t length);
uint32_t crc32_ieee(uint8_t const *data, size_t length);

// Require a complete 1-KiB block stream with a valid trailer at its exact end.
bool inspect_deye_image(uint8_t const *data, size_t length, DeyeImageInfo &info);

// Build a packet from one complete block of an already validated image.
void build_xmodem_packet(uint8_t const *block, size_t block_index,
                         std::array<uint8_t, XMODEM_PACKET_SIZE> &packet);

} // namespace esphome::deye_firmware_upgrade
