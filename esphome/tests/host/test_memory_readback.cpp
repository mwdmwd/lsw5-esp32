// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#define protected public
#include "esphome/components/deye_firmware_upgrade/deye_firmware_upgrade.h"
#undef protected
#include "esphome/components/deye_firmware_upgrade/hmi_memory_readback.cpp"
using namespace esphome;
using namespace esphome::deye_firmware_upgrade;

namespace
{
std::vector<uint8_t> memory_reply(uint32_t word, uint8_t unit = 1)
{
	std::vector<uint8_t> data = {
		unit, 5, uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word), 0, 0
	};
	auto const crc = crc16_modbus(data.data(), 6);
	data[6] = crc;
	data[7] = crc >> 8;
	return data;
}
struct MemoryRig
{
	modbus::Modbus bus;
	modbus_controller::ModbusController controller;
	PollingComponent monitor;
	DeyeFirmwareUpgrade reader;
	MemoryRig()
	{
		reader.set_modbus(&bus);
		reader.set_modbus_controller(&controller);
		reader.set_event_monitor(&monitor);
	}
	void begin(uint32_t address, size_t length)
	{
		CHECK(reader.request_memory_read(address, length) == nullptr);
		reader.loop();
	}
	void tick(uint32_t ms = 100)
	{
		test_now += ms;
		reader.loop();
	}
	void reply(uint32_t value)
	{
		auto const bytes = memory_reply(value);
		reader.rx.insert(reader.rx.end(), bytes.begin(), bytes.end());
		reader.loop();
	}
	void word(uint32_t value)
	{
		if(reader.readbackPhase_ == DeyeFirmwareUpgrade::ReadbackPhase::SETTLING)
			tick(20);
		if(reader.readbackPhase_ == DeyeFirmwareUpgrade::ReadbackPhase::YIELDING)
			tick(1);
		CHECK(reader.readbackPhase_ == DeyeFirmwareUpgrade::ReadbackPhase::WAITING);
		reply(value);
	}
};
} // namespace

void stage(DeyeFirmwareUpgrade &upgrade, uint8_t target, size_t blocks);
TEST(memory_full_flash_single_pass_preserves_staged_firmware)
{
	MemoryRig r;
	stage(r.reader, 1, 1);
	auto const staged = r.reader.snapshot();
	r.begin(0x08000000, 512 * 1024);
	CHECK(!r.bus.enabled && r.controller.polling && r.monitor.polling);
	CHECK(!r.reader.begin_upload(&r, "x", 1, 1024));
	CHECK(!r.reader.request_start(staged.sha256, staged.confirmation_token));
	CHECK(r.reader.request_memory_read(0x20000000, 4) != nullptr);
	for(uint32_t i = 0; i < 512 * 1024; i += 4)
		r.word(0x08000000 + i);
	r.tick();
	auto const s = r.reader.memory_snapshot();
	CHECK(s.complete && !s.active && s.received == 512 * 1024);
	CHECK(r.reader.tx.size() == s.length * 2); // One eight-byte request per four bytes.
	CHECK(s.data.get()[0] == 0 && s.data.get()[3] == 8);
	CHECK(s.data.get()[s.received - 4] == 0xFC && s.data.get()[s.received - 2] == 7);
	CHECK(r.bus.enabled && r.controller.polling && r.monitor.polling);
	CHECK(r.reader.snapshot().sha256 == staged.sha256);
	// A download snapshot pins its bytes even after clearing the capture.
	CHECK(r.reader.clear_memory_read(s.jobId));
	CHECK(s.data.get()[s.received - 4] == 0xFC);
}

TEST(memory_arbitrary_unaligned_reads_and_allocation_failure)
{
	MemoryRig r;
	CHECK(r.reader.request_memory_read(0x08000000, 0) != nullptr);
	CHECK(r.reader.request_memory_read(UINT32_MAX - 3, 8) != nullptr);
	r.begin(0x40020001,
	        5); // No region whitelist; include partial leading and trailing words.
	r.word(0x44332211);
	r.word(0x88776655);
	r.tick();
	auto s = r.reader.memory_snapshot();
	CHECK(s.complete && s.received == 5 && r.reader.tx.size() == 16);
	CHECK(std::equal(s.data.get(), s.data.get() + 5, std::string("\x22\x33\x44\x55\x66").begin()));
	RAMAllocator<uint8_t>::fail = true;
	CHECK(r.reader.request_memory_read(0x60000000, 65536) != nullptr);
	RAMAllocator<uint8_t>::fail = false;
	CHECK(r.reader.memory_snapshot().jobId == s.jobId && s.data.get()[0] == 0x22);
	r.begin(UINT32_MAX, 1);
	r.word(0x12345678);
	r.tick();
	CHECK(r.reader.memory_snapshot().complete && r.reader.memory_snapshot().data.get()[0] == 0x12);
}

TEST(memory_yields_to_normal_polling_between_words)
{
	MemoryRig r;
	r.bus.blocked = true;
	r.begin(0x0800C000, 8);
	CHECK(r.reader.tx.empty() && r.controller.polling && r.monitor.polling && r.bus.enabled);
	r.bus.blocked = false;
	r.reader.loop();
	// A poll that becomes due while readback owns the UART must keep priority.
	r.controller.command_queue_.push_back(std::make_unique<modbus_controller::ModbusCommandItem>());
	r.reply(0x2001E538);
	r.tick(20);
	CHECK(r.bus.enabled && r.controller.enabled && r.controller.polling && r.monitor.polling);
	r.tick(1);
	CHECK(r.reader.tx.size() == 8 && r.bus.enabled);
	r.tick(5000); // Pending normal work keeps priority regardless of elapsed time.
	CHECK(r.reader.tx.size() == 8 && r.bus.enabled && r.reader.memory_snapshot().active);
	r.controller.command_queue_.clear();
	r.bus.blocked = true; // Queue drained, but its last transaction is still in flight.
	r.reader.loop();
	CHECK(r.reader.tx.size() == 8 && r.bus.enabled);
	r.bus.blocked = false;
	r.reader.loop();
	CHECK(r.reader.tx.size() == 16 && r.controller.polling && r.monitor.polling);
	r.reply(0x0800C2A1);
	r.tick(20);
	CHECK(r.reader.memory_snapshot().complete && r.bus.enabled);
}

TEST(memory_idle_bus_resumes_next_loop_after_reply_guard)
{
	test_now = UINT32_MAX - 10; // The reply guard must also survive millis() wrapping.
	MemoryRig r;
	r.begin(0x0800C000, 8);
	r.reply(0x2001E538);
	r.tick(19);
	CHECK(r.reader.tx.size() == 8 && !r.bus.enabled && !r.controller.enabled);
	r.tick(1);
	// Handoff returns to the scheduler without sending the next request immediately.
	CHECK(r.reader.tx.size() == 8 && r.bus.enabled && r.controller.enabled);
	r.tick(1);
	CHECK(r.reader.tx.size() == 16 && !r.bus.enabled);
	CHECK(r.controller.polling && r.monitor.polling);
	r.reply(0x0800C2A1);
	r.tick(20);
	CHECK(r.reader.memory_snapshot().complete && r.bus.enabled);
}

TEST(memory_extra_reply_during_guard_stops_before_next_word)
{
	MemoryRig r;
	r.begin(0x0800C000, 8);
	r.reply(0x2001E538);
	r.tick(19);
	r.reply(0x2001E538); // A duplicate cannot become the next word's response.
	auto const s = r.reader.memory_snapshot();
	CHECK(!s.active && !s.complete && s.received == 4);
	CHECK(r.reader.tx.size() == 8 && r.bus.enabled && r.controller.enabled);
	CHECK(s.message.find("Unexpected UART data") != std::string::npos);
}

TEST(memory_cancel_and_timeout_restore_polling_and_retain_partial_bytes)
{
	for(bool cancel : { false, true })
	{
		test_now = UINT32_MAX - 500;
		MemoryRig r;
		r.begin(0x0800C000, 1024);
		r.word(0x2001E538);
		r.tick(20);
		r.tick(100);
		auto const job = r.reader.memory_snapshot().jobId;
		CHECK(!r.reader.cancel_memory_read(job + 1));
		if(cancel)
		{
			CHECK(r.reader.cancel_memory_read(job));
			r.reader.loop();
			r.reply(0x0800C2A1); // Pending response is discarded while cancelling.
			CHECK(!r.bus.enabled);
		}
		r.tick(1000);
		auto const s = r.reader.memory_snapshot();
		CHECK(!s.active && !s.complete && s.received == 4 && s.data.get()[0] == 0x38);
		CHECK(
		    r.reader.tx.size() == 16 && r.bus.enabled && r.controller.polling && r.monitor.polling);
	}
}

TEST(memory_crc_and_fragmented_reply_use_native_wire_format)
{
	MemoryRig r;
	r.begin(0x0800C000, 4);
	r.tick();
	CHECK(r.reader.tx == std::vector<uint8_t>({ 2, 5, 8, 0, 0xC0, 0, 0x9F, 0x99 }));
	auto corrupt = memory_reply(0xDEADBEEF);
	corrupt[6] ^= 1;
	r.reader.rx.insert(r.reader.rx.end(), corrupt.begin(), corrupt.end());
	auto const good = memory_reply(0x2001E538);
	r.reader.rx.insert(r.reader.rx.end(), good.begin(), good.begin() + 4);
	r.reader.loop();
	CHECK(r.reader.memory_snapshot().received == 0);
	r.reader.rx.insert(r.reader.rx.end(), good.begin() + 4, good.end());
	r.reader.loop();
	r.tick();
	CHECK(r.reader.memory_snapshot().complete && r.reader.tx.size() == 8);
}

TEST(memory_http_reads_pages_and_downloads_with_supported_status_codes)
{
	MemoryRig r;
	MemoryReadRequestHandler handler(&r.reader);
	AsyncWebServerRequest req;
	req.verb = HTTP_GET;
	req.url = "/memory";
	handler.handleRequest(&req);
	CHECK(req.response->status == 200 && !r.reader.memory_snapshot().active && r.reader.tx.empty());
	req.verb = HTTP_POST;
	req.url = "/memory/read";
	req.args = { { "address", "0x0800C000" }, { "length", "8oops" } };
	handler.handleRequest(&req);
	CHECK(req.response->status == 409);
	req.args["length"] = "8";
	handler.handleRequest(&req);
	CHECK(req.response->status == 200); // 202 became 500 on IDF.
	r.reader.loop();
	r.word(0x2001E538);
	req.verb = HTTP_GET;
	req.url = "/memory/download";
	req.args = { { "job", "1" } };
	handler.handleRequest(&req);
	CHECK(req.response->status == 200 && req.response->body == std::string("\x38\xe5\x01\x20", 4));
	CHECK(req.response->headers["Content-Disposition"].find(".partial.bin") != std::string::npos);
	r.word(0x0800C2A1);
	r.tick();
	handler.handleRequest(&req);
	CHECK(req.response->body.size() == 8 &&
	      req.response->headers["Content-Disposition"].find(".partial") == std::string::npos);
	req.url = "/memory/data";
	req.args = { { "job", "1" }, { "offset", "4" }, { "length", "4" } };
	handler.handleRequest(&req);
	CHECK(req.response->body == std::string("\xa1\xc2\x00\x08", 4));
	req.args["length"] = "8";
	handler.handleRequest(&req);
	CHECK(req.response->status == 409);
	req.args["length"] = "4";
	req.args["job"] = "0";
	handler.handleRequest(&req);
	CHECK(req.response->status == 409);
	uint32_t number;
	for(char const *text : { "", "-1", "8x", "4294967296", "0x100000000" })
		CHECK(!readback_number(text, number));
}
