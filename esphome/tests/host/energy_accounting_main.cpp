// SPDX-License-Identifier: GPL-3.0-or-later
// The implementation under test is generated from YAML by test_energy_accounting.py.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "components/energy_accounting/energy_accounting.h"

namespace energy_accounting = esphome::energy_accounting;

#define CHECK(expr) do { if(!(expr)) throw std::runtime_error(std::string(__FILE__)+":"+std::to_string(__LINE__)+": " #expr); } while(false)

constexpr uint64_t limit = (uint64_t(1) << 32) * 1000;
constexpr uint64_t absent = std::numeric_limits<uint64_t>::max();

struct Sink
{
	std::string state;
	unsigned count = 0;
	void publish_state(std::string const &value)
	{
		state = value;
		++count;
	}
};
struct Sensors
{
	struct NumericSink
	{
		float state = 0;
		unsigned count = 0;
		void publish_state(float value)
		{
			state = value;
			++count;
		}
	} total_generator_production;
	Sink total_pv_production;
	energy_accounting::PvProduction total_pv_production_filter;
	unsigned warnings = 0;
};
struct Item
{
	size_t offset = 0;
};
struct Totals
{
	bool valid = false;
	uint64_t combined = 0, generator = 0;
};

#define id(name) sensors.name
#define ESP_LOGW(...) (++sensors.warnings)
#include "energy_accounting_lambda.h"
#undef ESP_LOGW
#undef id

using Decoder = std::string (*)(std::vector<uint8_t> const &, Item const *, Sensors &);
using Words = std::array<uint16_t, 14>;
uint64_t callback_cases = 0;

// Independent oracle: enumerate the low-word congruence class in the coarse
// interval. No uint16 subtraction and no copied decoder rejection expression.
std::optional<uint64_t> reference(uint32_t quotient, uint16_t low)
{
	uint64_t const begin = uint64_t(quotient) * 1000;
	uint64_t candidate = begin / 65536 * 65536 + low;
	if(candidate < begin)
		candidate += 65536;
	if(candidate / 1000 == quotient)
		return candidate;
	return {};
}

// Format independently by inserting a decimal point into the raw decimal string.
std::string decimal(uint64_t raw)
{
	std::string digits = std::to_string(raw);
	if(digits.size() < 5)
		digits.insert(0, 5 - digits.size(), '0');
	digits.insert(digits.size() - 4, 1, '.');
	return digits;
}

Words packet(uint32_t combined_q, uint16_t combined_low, uint32_t generator_q,
             uint16_t generator_low, uint16_t features = 1)
{
	// Unrelated registers deliberately contain nonzero asymmetric sentinel values.
	return { uint16_t(combined_q),
	         uint16_t(combined_q >> 16),
	         0x1234,
	         uint16_t(generator_q),
	         uint16_t(generator_q >> 16),
	         0xabcd,
	         0xffff,
	         0x8000,
	         combined_low,
	         generator_low,
	         0x1357,
	         0x2468,
	         0xdead,
	         features };
}

Words encode(uint64_t combined, uint64_t generator)
{
	// Truncation of the legacy quotient is intentional for the overflow fixture.
	return packet(uint32_t(combined / 1000), uint16_t(combined), uint32_t(generator / 1000),
	              uint16_t(generator));
}

std::vector<uint8_t> wire(Words const &words, size_t offset = 0, size_t suffix = 0)
{
	std::vector<uint8_t> bytes(offset, 0xa5);
	for(auto word : words)
	{
		bytes.push_back(word >> 8);
		bytes.push_back(word & 255);
	}
	bytes.insert(bytes.end(), suffix, 0x5a);
	return bytes;
}

void check_sink(Sink const &sink, std::optional<uint64_t> expected)
{
	CHECK(sink.count == (expected ? 1U : 0U));
	CHECK(sink.state == (expected ? decimal(*expected) : ""));
}

Sensors check_callback(Words const &words, Decoder decode = decode_configured,
                       uint64_t history = configured_history, size_t offset = 0, size_t suffix = 0)
{
	Sensors sensors;
	Item item{ offset };
	seed_floor("0", sensors);
	for(unsigned sample = 0; sample < 3; ++sample)
		CHECK(decode(wire(words, offset, suffix), &item, sensors) == std::to_string(words[13]));

	auto combined = reference(uint32_t(words[0]) | (uint32_t(words[1]) << 16), words[8]);
	auto generator = reference(uint32_t(words[3]) | (uint32_t(words[4]) << 16), words[9]);
	if(!(words[13] & 1))
	{
		combined = (uint64_t(words[0]) | (uint64_t(words[1]) << 16)) * 1000;
		generator = (uint64_t(words[3]) | (uint64_t(words[4]) << 16)) * 1000;
	}
	CHECK(sensors.warnings == 3 * unsigned(!combined || !generator));
	if(!combined || !generator)
	{
		combined.reset();
		generator.reset();
	}
	CHECK(sensors.total_generator_production.count == (generator ? 3U : 0U));
	if(generator)
	{
		float const actual = sensors.total_generator_production.state;
		double const ulp = double(std::nextafter(actual, INFINITY)) - actual;
		CHECK(std::abs(double(actual) - double(*generator) / 10000.0) <= ulp);
	}
	std::optional<uint64_t> pv;
	if(combined && *combined + history >= *generator)
		pv = *combined + history - *generator;
	check_sink(sensors.total_pv_production, pv);
	++callback_cases;
	return sensors;
}

void live_and_protocol()
{
	auto words = packet(90452, 12403, 35819, 36476);
	auto live = check_callback(words);
	CHECK(std::abs(live.total_generator_production.state - 3581.9132) < 0.0002);
	CHECK(live.total_pv_production.state == decimal(54632951 + configured_history));
	if(configured_history == 241619000)
		CHECK(live.total_pv_production.state == "29625.1951");
	for(size_t offset : { 0, 1, 3, 28, 65535 })
	{
		check_callback(words, decode_configured, configured_history, offset, 5);
		Item item{ offset };
		for(size_t length = 0; length < 28; ++length)
		{
			auto bytes = wire(words, offset);
			bytes.resize(offset + length);
			Sensors sensors;
			CHECK(decode_configured(bytes, &item, sensors) == "invalid reply");
			CHECK(sensors.total_generator_production.count == 0);
			CHECK(sensors.total_pv_production.count == 0);
			CHECK(sensors.warnings == 0);
			++callback_cases;
		}
		Sensors sensors;
		CHECK(decode_configured({}, &item, sensors) == "invalid reply");
	}
	for(uint32_t features = 0; features <= 65535; ++features)
	{
		words[13] = features;
		check_callback(words);
	}
}

void boundaries()
{
	std::set<uint64_t> values{
		0,         1,        998,     999,          1000,         1001,
		9999,      10000,    10001,   65535,        65536,        65537,
		8191999,   8192000,  8192001, limit - 1001, limit - 1000, limit - 999,
		limit - 2, limit - 1
	};
	for(unsigned bit = 0; bit < 42; ++bit)
		for(uint64_t value :
		    { (uint64_t(1) << bit) - 1, uint64_t(1) << bit, (uint64_t(1) << bit) + 1 })
			if(value < limit)
				values.insert(value);
	for(unsigned bit = 0; bit < 32; ++bit)
		for(uint64_t q : { (uint64_t(1) << bit) - 1, uint64_t(1) << bit, (uint64_t(1) << bit) + 1 })
			for(uint64_t remainder : { 0, 1, 998, 999 })
				values.insert(q * 1000 + remainder);
	for(auto combined : values)
		for(auto generator : values)
		{
			check_callback(encode(combined, generator));
			check_callback(encode(combined, generator), decode_zero, 0);
		}

	// Every decimal suffix, including float32 precision-loss boundaries and the
	// largest representable total, goes through the full formatting/publication path.
	for(uint64_t block :
	    { uint64_t(0), uint64_t(1) << 24, uint64_t(1) << 32, uint64_t(296250000), limit - 10000 })
		for(uint64_t remainder = 0; remainder < 10000; ++remainder)
			check_callback(encode(block + remainder, remainder));

	// Full callback at acceptance and low-word wrap edges in every residue class,
	// both near zero and near the maximum quotient, for each counter separately.
	for(uint64_t layer : { uint64_t(0), (uint64_t(1) << 32) - 8192 })
		for(uint64_t q = layer; q < layer + 8192; ++q)
			for(uint16_t low :
			    { uint16_t(0), uint16_t(65535), uint16_t(q * 1000 - 1), uint16_t(q * 1000),
			      uint16_t(q * 1000 + 999), uint16_t(q * 1000 + 1000) })
			{
				check_callback(packet(q, low, 0, 0));
				check_callback(packet(0, 0, q, low));
			}
}

void sequences_and_limits()
{
	Item item;
	Sensors sensors;
	auto publish = [&](Words const &words)
	{ return decode_configured(wire(words), &item, sensors); };
	auto repeat = [&](uint64_t combined, uint64_t generator)
	{
		for(unsigned i = 0; i < 3; ++i)
			CHECK(publish(encode(combined, generator)) == "1");
	};
	constexpr uint64_t combined = 1000100, generator = 200100;
	uint64_t const initial = combined + configured_history - generator;
	// No value may escape before the retained MQTT checkpoint arrives.
	repeat(combined, generator);
	CHECK(sensors.total_pv_production.count == 0);
	seed_floor(decimal(initial), sensors);
	CHECK(publish(encode(combined, generator)) == "1");
	CHECK(sensors.total_pv_production.state == decimal(initial));

	// A torn HMI copy can pass both remainder checks. Do not pin its positive
	// spike in the floor when the following coherent pair is lower.
	CHECK(publish(encode(combined + 500, generator)) == "1");
	CHECK(sensors.total_pv_production.state == decimal(initial));
	repeat(combined + 500, generator + 300);
	CHECK(sensors.total_pv_production.state == decimal(initial + 200));
	std::string const last = sensors.total_pv_production.state;
	unsigned const count = sensors.total_pv_production.count;
	CHECK(publish(packet(0, 1000, 0, 0)) == "1");
	CHECK(publish(packet(0, 0, 0, 1000)) == "1");
	CHECK(publish(packet(0, 0, 0, 0, 0)) == "0");
	CHECK(decode_configured({}, &item, sensors) == "invalid reply");
	CHECK(sensors.total_pv_production.state == last);
	CHECK(sensors.total_pv_production.count == count);
	repeat(0, 0); // coherent reset/stale sample
	CHECK(sensors.total_pv_production.state == last);
	CHECK(sensors.total_pv_production.count == count);
	repeat(combined + 501, generator + 300);
	CHECK(sensors.total_pv_production.state == decimal(initial + 201));

	// A reboot must restore all four decimals, fill the minimum window, and
	// reject both older samples and a one-sample startup spike.
	std::string checkpoint = sensors.total_pv_production.state;
	sensors = {};
	seed_floor(checkpoint, sensors);
	repeat(combined, generator);
	CHECK(sensors.total_pv_production.count == 0);
	repeat(combined + 501, generator + 300);
	CHECK(sensors.total_pv_production.state == checkpoint);
	sensors = {};
	seed_floor(checkpoint, sensors);
	CHECK(publish(encode(combined + 5000, generator)) == "1");
	CHECK(sensors.total_pv_production.count == 0);
	CHECK(publish(encode(combined + 502, generator + 300)) == "1");
	CHECK(sensors.total_pv_production.count == 0);
	CHECK(publish(encode(combined + 502, generator + 300)) == "1");
	CHECK(sensors.total_pv_production.state == decimal(initial + 202));

	// One-decimal legacy states may be above the precise value. Wait for the
	// physical counter to catch up, without rounding the new samples.
	sensors = {};
	seed_floor("29625.2", sensors);
	for(unsigned i = 0; i < 3; ++i)
		decode_zero(wire(encode(296251951, 0)), &item, sensors);
	CHECK(sensors.total_pv_production.count == 0);
	for(unsigned i = 0; i < 3; ++i)
		decode_zero(wire(encode(296252001, 0)), &item, sensors);
	CHECK(sensors.total_pv_production.state == "29625.2001");
	// An older broker echo must not lower the floor.
	seed_floor("0", sensors);
	for(unsigned i = 0; i < 3; ++i)
		decode_zero(wire(encode(296252000, 0)), &item, sensors);
	CHECK(sensors.total_pv_production.state == "29625.2001");

	sensors = {};
	seed_floor("0", sensors);
	for(uint64_t delta : { 0, 1, 999, 1000, 1001, 65535, 65536, 65537, 8191999, 8192000 })
	{
		repeat(90452083 + delta, 35819132 + delta);
		CHECK(sensors.total_pv_production.state == decimal(54632951 + configured_history));
	}
	// The wire format aliases at the coarse quotient's 32-bit rollover.
	CHECK(encode(limit, 0) == encode(0, 0));
	CHECK(encode(limit + 65536, 0) == encode(65536, 0));
	check_callback(encode(limit, 0));
	check_callback(encode(limit - 1, 0));
	check_callback(packet(0, uint16_t(65536), 0, 0));
}

void stock_and_feature_transitions()
{
	Item item;
	Sensors sensors;
	seed_floor("0", sensors);
	auto publish = [&](uint64_t combined, uint64_t generator, uint16_t features)
	{
		auto words = encode(combined, generator);
		words[13] = features;
		if(!(features & 1))
		{
			// Reserved low words have no meaning without the enable bit.
			words[8] = 0xffff;
			words[9] = 0x4321;
		}
		for(unsigned i = 0; i < 3; ++i)
			CHECK(decode_zero(wire(words), &item, sensors) == std::to_string(features));
	};
	publish(100100, 20900, 0x8000);
	CHECK(sensors.total_pv_production.state == "8.0000");
	publish(100300, 21100, 2); // stock quantization dip, actual net PV unchanged
	CHECK(sensors.total_pv_production.state == "8.0000");
	publish(100300, 21100, 0x8001); // precise 7.9200 still below the old floor
	CHECK(sensors.total_pv_production.state == "8.0000");
	publish(101101, 21100, 3);
	CHECK(sensors.total_pv_production.state == "8.0001");
	publish(101101, 21100, 0); // precision removed; must not lower the floor
	CHECK(sensors.total_pv_production.state == "8.0001");
	publish(102100, 21100, 0);
	CHECK(sensors.total_pv_production.state == "8.1000");
	// Low-word changes below float resolution still reach the PV MQTT payload.
	sensors = {};
	seed_floor("29625.1951", sensors);
	publish(296251952, 0, 0xffff);
	CHECK(sensors.total_pv_production.state == "29625.1952");
}

void retained_payloads()
{
	for(uint64_t value : { uint64_t(0), uint64_t(1), uint64_t(999), uint64_t(1000),
	                       uint64_t(296251951), limit - 1, std::numeric_limits<uint64_t>::max() })
	{
		CHECK(energy_accounting::kwh(value) == decimal(value));
		CHECK(energy_accounting::parse_kwh(decimal(value)) == value);
	}
	CHECK(energy_accounting::parse_kwh("29625.1") == 296251000);
	CHECK(energy_accounting::parse_kwh("0001.02") == 10200);
	CHECK(energy_accounting::parse_kwh("1") == 10000);
	CHECK(energy_accounting::parse_kwh("0") == 0);
	CHECK(energy_accounting::parse_kwh("1.234") == 12340);
	for(auto text :
	    { "", "NaN", "nan", "inf", "-1", "+1", "1e4", " 1", "1 ", ".1", "1.", "1.2.3", "1.00000",
	      "1844674407370955.1616", "1844674407370956", "18446744073709551616" })
	{
		Sensors sensors;
		seed_floor(text, sensors);
		CHECK(sensors.warnings == 1);
		CHECK(!sensors.total_pv_production_filter.update(10000));
		CHECK(!sensors.total_pv_production_filter.update(10000));
		CHECK(!sensors.total_pv_production_filter.update(10000));
	}
}

void random_cases()
{
	std::mt19937_64 rng(542543547);
	for(unsigned i = 0; i < 25000; ++i)
	{
		check_callback(encode(rng() % limit, rng() % limit));
		check_callback(packet(rng(), rng(), rng(), rng()));
	}
}

void verify_arithmetic(Words const &words, std::optional<uint64_t> combined,
                       std::optional<uint64_t> generator)
{
	auto const result = arithmetic(words);
	bool const valid = combined.has_value() && generator.has_value();
	if(result.valid != valid ||
	   (valid && (result.combined != *combined || result.generator != *generator)))
		throw std::runtime_error("arithmetic mismatch: Cq=" +
		                         std::to_string(uint32_t(words[0]) | (uint32_t(words[1]) << 16)) +
		                         " Cl=" +
		                         std::to_string(words[8]) +
		                         " Gq=" +
		                         std::to_string(uint32_t(words[3]) | (uint32_t(words[4]) << 16)) +
		                         " Gl=" +
		                         std::to_string(words[9]));
}

void exhaustive_counters()
{
	uint64_t cases = 0;
	std::array<uint64_t, 65536> expected;
	constexpr uint64_t partner = limit - 12345;
	for(uint64_t layer : { uint64_t(0), (uint64_t(1) << 32) - 8192 })
	{
		for(uint64_t q = layer; q < layer + 8192; ++q)
		{
			// Independent encoder oracle: fill exactly the 1000 possible raw values
			// for this quotient; all other 64536 low words must be rejected.
			expected.fill(absent);
			for(uint64_t raw = q * 1000; raw < (q + 1) * 1000; ++raw)
				expected[raw % 65536] = raw;
			for(uint32_t low = 0; low <= 65535; ++low)
			{
				std::optional<uint64_t> raw;
				if(expected[low] != absent)
					raw = expected[low];
				verify_arithmetic(packet(q, low, partner / 1000, uint16_t(partner)), raw, partner);
				verify_arithmetic(packet(partner / 1000, uint16_t(partner), q, low), partner, raw);
				cases += 2;
			}
		}
		std::cout << "PASS exhaustive quotient layer " << layer << ".." << layer + 8191
		          << " (both counters, every low word)" << std::endl;
	}
	CHECK(cases == 2147483648ULL);
	std::cout << "PASS " << cases << " counter arithmetic checks" << std::endl;
}

void coarse_high_words()
{
	uint64_t cases = 0;
	for(uint64_t high = 0; high <= 65535; ++high)
		for(uint64_t low : { 0, 1, 65, 66, 8191, 8192, 32767, 32768, 65534, 65535 })
		{
			uint32_t const q = (high << 16) | low;
			for(uint64_t delta : { 0, 1, 999, 1000, 65535 })
			{
				uint16_t const raw_low = uint16_t(uint64_t(q) * 1000 + delta);
				auto const expected = reference(q, raw_low);
				verify_arithmetic(packet(q, raw_low, 0, 0), expected, 0);
				verify_arithmetic(packet(0, 0, q, raw_low), 0, expected);
				cases += 2;
			}
		}
	std::cout << "PASS " << cases << " high-word boundary checks" << std::endl;
}

void exhaustive_pairs()
{
	constexpr uint32_t combined_q = 0xffffe041; // residue 65: interval crosses low16 zero
	constexpr uint32_t generator_q = 131;       // another wrap, with different endpoints
	std::array<std::optional<uint64_t>, 65536> combined, generator;
	for(uint32_t low = 0; low <= 65535; ++low)
	{
		combined[low] = reference(combined_q, low);
		generator[low] = reference(generator_q, low);
	}
	uint64_t accepted = 0;
	for(uint32_t c = 0; c <= 65535; ++c)
	{
		for(uint32_t g = 0; g <= 65535; ++g)
		{
			auto const words = packet(combined_q, c, generator_q, g);
			verify_arithmetic(words, combined[c], generator[g]);
			accepted += arithmetic(words).valid;
		}
		if(c % 16384 == 16383)
			std::cout << "Paired low-word sweep: " << (c + 1) / 16384 << "/4" << std::endl;
	}
	CHECK(accepted == 1000000);
	std::cout << "PASS 4294967296 low-word pairs (1000000 accepted)" << std::endl;
}

void exhaustive_subtraction()
{
	// All fractional pairs on both sides of equality, with and without history.
	// The full callback must publish zero at equality and suppress negative PV
	// while still publishing the valid combined and generator counters.
	for(uint64_t c = 0; c < 1000; ++c)
		for(uint64_t g = 0; g < 1000; ++g)
		{
			check_callback(encode(65000 + c, 65000 + g), decode_zero, 0);
			if(configured_history < limit - 66000)
				check_callback(encode(65000 + c, 65000 + configured_history + g));
		}
	std::cout << "PASS full-callback fractional subtraction matrices" << std::endl;
}

int main(int argc, char **argv)
{
	try
	{
		auto const start = std::chrono::steady_clock::now();
		CHECK(argc == 1 || (argc == 2 && std::string(argv[1]) == "--exhaustive"));
		live_and_protocol();
		boundaries();
		sequences_and_limits();
		retained_payloads();
		stock_and_feature_transitions();
		random_cases();
		std::cout << "PASS " << callback_cases << " full YAML callback cases" << std::endl;
		if(argc == 2)
		{
			exhaustive_counters();
			coarse_high_words();
			exhaustive_pairs();
			exhaustive_subtraction();
			std::cout << "PASS " << callback_cases << " total full YAML callback cases"
			          << std::endl;
		}
		auto const seconds =
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::cout << "Energy accounting tests passed in " << seconds << " s" << std::endl;
		return 0;
	}
	catch(std::exception const &error)
	{
		std::cerr << "FAIL " << error.what() << std::endl;
		return 1;
	}
}
