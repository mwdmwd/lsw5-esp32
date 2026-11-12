// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#define protected public
#include "esphome/components/deye_firmware_upgrade/deye_firmware_upgrade.h"
#undef protected
#include "esphome/components/deye_firmware_upgrade/dsp_memory_readback.cpp"
using namespace esphome;
using namespace esphome::deye_firmware_upgrade;

namespace
{
struct DspRig
{
	modbus::Modbus bus;
	modbus_controller::ModbusController controller;
	PollingComponent monitor;
	DeyeFirmwareUpgrade reader;
	std::array<uint32_t, 3> pointers{};
	std::array<uint16_t, 6> cache{};
	uint16_t selector = 0, cachedSelector = 0, version = 0x1175;
	uint32_t applyAt = 0, cacheAt = 0;
	std::vector<uint8_t> pending;
	std::vector<std::vector<uint8_t>> requests;
	size_t txOffset = 0;
	bool freeze = false, dropInitialization = false, silence = false;
	unsigned missingDataReplies = 0;
	bool dropInitializationEcho = false;
	DspRig()
	{
		reader.set_modbus(&bus);
		reader.set_modbus_controller(&controller);
		reader.set_event_monitor(&monitor);
		cacheAt = test_now + 2000;
	}
	uint16_t memory(uint32_t address)
	{
		if(address >= 0xDF9C && address < 0xDFA2)
			return pointers[(address - 0xDF9C) / 2] >> ((address & 1) ? 16 : 0);
		if(address == 0x84000)
			return 0x49;
		if(address == 0x84001)
			return 0xCAA5;
		if(address >= 0x80000 && address < 0xA9600)
			return 0; // Long zero runs must still require a new address tag.
		CHECK(address == 0);
		return 0;
	}
	void tick(uint32_t ms = 50)
	{
		test_now += ms;
		if(!pending.empty() && static_cast<int32_t>(test_now - applyAt) >= 0)
		{
			auto const &p = pending;
			unsigned const reg = uint16_t(p[2]) << 8 | p[3];
			uint32_t const value =
			    uint32_t(p[7]) << 24 | uint32_t(p[8]) << 16 | uint32_t(p[9]) << 8 | p[10];
			if(!(dropInitialization && reg == 562))
			{
				pointers[0] = 0;
				if(reg == 584)
					selector = value >> 16;
				else
					pointers[(reg - 560) / 2] += value;
			}
			pending.clear();
		}
		if(!freeze && static_cast<int32_t>(test_now - cacheAt) >= 0)
		{
			cacheAt = test_now + 2000;
			cachedSelector = selector;
			for(size_t i = 0; i < pointers.size(); ++i)
				cache[2 * i] = cache[2 * i + 1] = selector ? memory(pointers[i]) : 0;
		}
		reader.loop();
		if(reader.tx.size() == txOffset)
			return;
		std::vector<uint8_t> request(reader.tx.begin() + txOffset, reader.tx.end());
		txOffset = reader.tx.size();
		requests.push_back(request);
		CHECK(crc16_modbus(request.data(), request.size()) == 0);
		std::vector<uint8_t> response;
		unsigned const reg = uint16_t(request[2]) << 8 | request[3];
		if(request[1] == 0x10)
		{
			// Model the single HMI queue: an early next write would replace it.
			pending = request;
			applyAt = test_now + 1000;
			response = request; // Echo precedes DSP execution.
		}
		else
		{
			CHECK(request[1] == 3);
			response = { 1, 3, uint8_t(request[5] * 2) };
			for(unsigned i = 0; i < request[5]; ++i)
			{
				uint16_t const word = reg == 15 ? version : reg == 584 ? cachedSelector : cache[i];
				response.push_back(word >> 8);
				response.push_back(word);
			}
			auto const crc = crc16_modbus(response.data(), response.size());
			response.push_back(crc);
			response.push_back(crc >> 8);
		}
		bool drop = silence;
		// Lose replies during capture, after the sentinel/bootstrap reads.
		if(request[1] == 3 &&
		   reg == 560 &&
		   request[5] == 6 &&
		   missingDataReplies != 0 &&
		   reader.memory_snapshot().received != 0)
		{
			--missingDataReplies;
			drop = true;
		}
		if(request[1] == 0x10 && reg == 562 && dropInitializationEcho)
		{
			dropInitializationEcho = false;
			drop = true;
		}
		if(!drop)
			reader.rx.insert(reader.rx.end(), response.begin(), response.end());
	}
	void begin(uint32_t address = 0x84000, size_t length = 16)
	{
		CHECK(reader.request_memory_read(address, length, true) == nullptr);
	}
	void until(std::function<bool()> const &condition, unsigned limit = 5000)
	{
		for(unsigned i = 0; i < limit && !condition(); ++i)
			tick();
		CHECK(condition());
	}
	unsigned writes(unsigned reg)
	{
		return std::count_if(requests.begin(), requests.end(), [reg](auto const &r)
		                     { return r[1] == 0x10 && (unsigned(r[2]) << 8 | r[3]) == reg; });
	}
};
} // namespace

TEST(dsp_memory_captures_word_addresses_little_endian_and_restores_diagnostics)
{
	DspRig r;
	r.begin();
	r.until([&] { return !r.reader.memory_snapshot().active; });
	auto const s = r.reader.memory_snapshot();
	CHECK(s.complete && s.dsp && s.received == 16 && s.diagnostics_restored);
	CHECK(s.data.get()[0] == 0x49 && s.data.get()[1] == 0);
	CHECK(s.data.get()[2] == 0xA5 && s.data.get()[3] == 0xCA);
	CHECK(std::all_of(s.data.get() + 4, s.data.get() + 16, [](uint8_t x) { return x == 0; }));
	CHECK(r.selector == 0 && r.writes(562) == 1 && r.writes(564) == 1);
	CHECK(r.bus.enabled && r.controller.enabled && r.controller.polling && r.monitor.polling);
	// A new ESP32 session can reuse the two valid tags without adding again.
	r.begin(0x84001, 2);
	r.until([&] { return !r.reader.memory_snapshot().active; });
	CHECK(r.reader.memory_snapshot().complete && r.writes(562) == 1 && r.writes(564) == 1);
}

TEST(dsp_memory_rejects_stale_cache_even_when_flash_words_repeat)
{
	DspRig r;
	r.begin(0x84002, 8);
	r.until([&] { return r.reader.memory_snapshot().received == 2; });
	r.freeze = true;
	for(unsigned i = 0; i < 160; ++i)
		r.tick();
	CHECK(r.reader.memory_snapshot().received == 2 && r.reader.memory_snapshot().active);
	r.freeze = false;
	r.until([&] { return !r.reader.memory_snapshot().active; });
	CHECK(r.reader.memory_snapshot().complete && r.selector == 0);
}

TEST(dsp_memory_reuses_live_address_tags_without_reprobing_the_pointer_table)
{
	DspRig r;
	r.pointers = { 0, 0xDF9C, 0xDF9D };
	r.begin(0x84000, 8);
	r.until([&] { return !r.reader.memory_snapshot().active; });
	auto const s = r.reader.memory_snapshot();
	CHECK(s.complete && s.received == 8 && s.diagnostics_restored);
	CHECK(s.data.get()[0] == 0x49 && s.data.get()[1] == 0);
	CHECK(s.data.get()[2] == 0xA5 && s.data.get()[3] == 0xCA);
	// Exactly one absolute assignment per captured word, including the initial
	// sentinel. No pointer-table reads or additive writes are needed on this path.
	CHECK(r.writes(560) == 4 && r.writes(562) == 0 && r.writes(564) == 0);
}

TEST(dsp_memory_lost_additive_write_is_not_retried)
{
	DspRig r;
	r.dropInitialization = true;
	r.begin();
	r.until([&] { return !r.reader.memory_snapshot().active; });
	auto const s = r.reader.memory_snapshot();
	CHECK(!s.complete && s.received == 0 && s.diagnostics_restored);
	CHECK(r.writes(562) == 1 && r.writes(564) == 0 && r.selector == 0);
}

TEST(dsp_memory_cancel_waits_for_cleanup_and_preserves_partial_capture)
{
	DspRig r;
	r.begin();
	r.until([&] { return r.reader.memory_snapshot().received == 4; });
	auto const s = r.reader.memory_snapshot();
	CHECK(r.reader.cancel_memory_read(s.jobId));
	r.tick();
	CHECK(r.reader.memory_snapshot().active);
	CHECK(r.reader.request_raw_modbus("01 03 00 50 00 01", true, 1000) != nullptr);
	r.until([&] { return !r.reader.memory_snapshot().active; });
	auto const done = r.reader.memory_snapshot();
	CHECK(!done.complete && done.received == 4 && done.diagnostics_restored);
	CHECK(done.data == s.data && r.selector == 0 && r.bus.enabled);
}

TEST(dsp_memory_admission_checks_profile_and_allowed_flash_extent)
{
	DspRig r;
	CHECK(r.reader.request_memory_read(0x84000, 3, true) != nullptr);
	CHECK(r.reader.request_memory_read(0xA9600, 2, true) != nullptr);
	CHECK(r.reader.request_memory_read(0x7FFFF, 2, true) != nullptr);
	CHECK(r.reader.request_memory_read(0xA95FF, 4, true) != nullptr);
	CHECK(r.reader.request_memory_read(0xDF9C, 2, true) !=
	      nullptr); // Pointer table: internal use only.
	CHECK(r.reader.request_memory_read(0x9000, 2, true) != nullptr);  // CLA program RAM.
	CHECK(r.reader.request_memory_read(0x5D000, 2, true) != nullptr); // Peripheral registers.
	CHECK(r.reader.tx.empty() && !r.reader.memory_snapshot().active);
	r.version = 0x1172;
	r.begin();
	r.until([&] { return !r.reader.memory_snapshot().active; });
	CHECK(!r.reader.memory_snapshot().complete && r.reader.memory_snapshot().diagnostics_restored);
	CHECK(r.requests.size() == 1 && r.requests[0][1] == 3);
}

TEST(dsp_memory_cleanup_failure_is_reported_and_bounded)
{
	DspRig r;
	r.begin();
	r.until([&] { return r.reader.memory_snapshot().received == 2; });
	r.silence = true;
	CHECK(r.reader.cancel_memory_read(r.reader.memory_snapshot().jobId));
	r.until([&] { return !r.reader.memory_snapshot().active; });
	auto const s = r.reader.memory_snapshot();
	CHECK(!s.complete && !s.diagnostics_restored && s.received == 2);
	CHECK(s.message.find("restoration was not confirmed") != std::string::npos);
	CHECK(r.bus.enabled && r.controller.enabled);
}

TEST(dsp_memory_recovers_interrupted_setup_without_readding_existing_tag)
{
	DspRig r;
	r.selector = 1;
	r.pointers = { 0x84007, 0xDF9C, 0 };
	r.begin(0x84001, 2);
	r.until([&] { return !r.reader.memory_snapshot().active; });
	CHECK(r.reader.memory_snapshot().complete && r.selector == 0);
	CHECK(r.writes(562) == 0 && r.writes(564) == 1);
}

TEST(dsp_memory_unknown_pointer_configuration_is_rejected_without_addition)
{
	DspRig r;
	r.pointers[1] = 0x84009;
	r.begin();
	r.until([&] { return !r.reader.memory_snapshot().active; });
	CHECK(!r.reader.memory_snapshot().complete && r.reader.memory_snapshot().received == 0);
	CHECK(r.reader.memory_snapshot().diagnostics_restored && r.selector == 0);
	CHECK(r.writes(562) == 0 && r.writes(564) == 0);
}

TEST(dsp_memory_address_and_timer_wraps_preserve_freshness)
{
	test_now = UINT32_MAX - 5000;
	DspRig r;
	r.begin(0x8FFFF, 4);
	r.until([&] { return !r.reader.memory_snapshot().active; });
	CHECK(r.reader.memory_snapshot().complete && r.reader.memory_snapshot().received == 4);
	CHECK(r.selector == 0 && r.reader.memory_snapshot().diagnostics_restored);
}

TEST(dsp_memory_retries_tagged_reads_and_observes_lost_additive_echo_without_repeating_write)
{
	DspRig r;
	r.missingDataReplies = 2;
	r.dropInitializationEcho = true;
	r.begin();
	r.until([&] { return !r.reader.memory_snapshot().active; });
	CHECK(r.reader.memory_snapshot().complete && r.reader.memory_snapshot().received == 16);
	CHECK(r.reader.memory_snapshot().diagnostics_restored && r.selector == 0);
	CHECK(r.writes(562) == 1 && r.writes(564) == 1 && r.missingDataReplies == 0);
}
