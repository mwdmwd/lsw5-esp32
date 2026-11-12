// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace esphome::energy_accounting
{
// MQTT kWh payloads, represented exactly in 0.1 Wh. Accept legacy one-decimal
// states as well as the four-decimal states published by the high-resolution
// decoder.
inline std::optional<uint64_t> parse_kwh(std::string const &text)
{
	uint64_t raw = 0;
	unsigned decimals = 0;
	bool point = false, digit = false;
	for(char c : text)
	{
		if(c == '.' && !point && digit)
		{
			point = true;
			digit = false;
			continue;
		}
		if(c < '0' || c > '9' || (point && ++decimals > 4))
			return {};
		unsigned const value = c - '0';
		if(raw > (std::numeric_limits<uint64_t>::max() - value) / 10)
			return {};
		raw = raw * 10 + value;
		digit = true;
	}
	if(!digit)
		return {};
	for(; decimals < 4; ++decimals)
	{
		if(raw > std::numeric_limits<uint64_t>::max() / 10)
			return {};
		raw *= 10;
	}
	return raw;
}

inline std::string kwh(uint64_t raw)
{
	std::string fraction = std::to_string(raw % 10000);
	return std::to_string(raw / 10000) + "." + std::string(4 - fraction.size(), '0') + fraction;
}

class PvProduction
{
public:
	bool seed(std::string const &state)
	{
		auto const raw = parse_kwh(state);
		if(!raw)
			return false;
		floor_ = std::max(floor_, *raw);
		seeded_ = true;
		return true;
	}

	std::optional<uint64_t> update(uint64_t raw)
	{
		window_[next_] = raw;
		next_ = (next_ + 1) % window_.size();
		count_ = std::min(count_ + 1, window_.size());
		// Fill the window before the first publication, including after reboot.
		if(!seeded_ || count_ < window_.size())
			return {};
		uint64_t const candidate = *std::min_element(window_.begin(), window_.end());
		if(candidate < floor_)
			return {};
		floor_ = candidate;
		return candidate;
	}

private:
	uint64_t floor_ = 0;
	bool seeded_ = false;
	std::array<uint64_t, 3> window_{};
	size_t next_ = 0, count_ = 0;
};
}
