// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.h"

#include <cstring>

namespace esphome::deye_firmware_upgrade
{

uint16_t crc16_modbus(uint8_t const *data, size_t length)
{
	uint16_t crc = 0xFFFF;
	for(size_t i = 0; i < length; i++)
	{
		crc ^= data[i];
		for(uint8_t bit = 0; bit < 8; bit++)
			crc = (crc & 1U) != 0 ? static_cast<uint16_t>((crc >> 1U) ^ 0xA001U)
			                      : static_cast<uint16_t>(crc >> 1U);
	}
	return crc;
}

uint16_t crc16_xmodem(uint8_t const *data, size_t length)
{
	uint16_t crc = 0;
	for(size_t i = 0; i < length; i++)
	{
		crc ^= static_cast<uint16_t>(data[i]) << 8U;
		for(uint8_t bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000U) != 0 ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
			                           : static_cast<uint16_t>(crc << 1U);
	}
	return crc;
}

uint32_t crc32_ieee(uint8_t const *data, size_t length)
{
	uint32_t crc = 0xFFFFFFFFU;
	for(size_t i = 0; i < length; i++)
	{
		crc ^= data[i];
		for(uint8_t bit = 0; bit < 8; bit++)
			crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
	}
	return crc ^ 0xFFFFFFFFU;
}

static uint32_t read_be32(uint8_t const *data)
{
	return (static_cast<uint32_t>(data[0]) << 24U) |
	       (static_cast<uint32_t>(data[1]) << 16U) |
	       (static_cast<uint32_t>(data[2]) << 8U) |
	       static_cast<uint32_t>(data[3]);
}

bool inspect_deye_image(uint8_t const *data, size_t length, DeyeImageInfo &info)
{
	if(length < DEYE_TRAILER_SIZE || length % XMODEM_BLOCK_SIZE != 0)
		return false;

	uint8_t const *trailer = data + length - DEYE_TRAILER_SIZE;
	DeyeImageInfo candidate{};
	candidate.selectors = { trailer[0], trailer[1], trailer[2] };
	candidate.payload_size = read_be32(trailer + 3);
	candidate.crc32 = read_be32(trailer + 7);

	if(candidate.payload_size != length - DEYE_TRAILER_SIZE)
		return false;

	if(crc32_ieee(data, candidate.payload_size) != candidate.crc32)
		return false;

	info = candidate;
	return true;
}

void build_xmodem_packet(uint8_t const *block, size_t block_index,
                         std::array<uint8_t, XMODEM_PACKET_SIZE> &packet)
{
	uint8_t const block_number = static_cast<uint8_t>(block_index + 1U);
	packet[0] = 0x02; // STX: XMODEM-1K
	packet[1] = block_number;
	packet[2] = static_cast<uint8_t>(block_number ^ 0xFFU);

	uint8_t *payload = packet.data() + 3;
	std::memcpy(payload, block, XMODEM_BLOCK_SIZE);

	uint16_t const crc = crc16_xmodem(payload, XMODEM_BLOCK_SIZE);
	packet[XMODEM_PACKET_SIZE - 2] = static_cast<uint8_t>(crc >> 8U);
	packet[XMODEM_PACKET_SIZE - 1] = static_cast<uint8_t>(crc);
}

} // namespace esphome::deye_firmware_upgrade
