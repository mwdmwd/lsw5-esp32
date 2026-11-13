// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#define protected public
#include "esphome/components/deye_firmware_upgrade/deye_firmware_upgrade.h"
#undef protected
#include "esphome/components/deye_firmware_upgrade/raw_modbus.cpp"
using namespace esphome;
using namespace esphome::deye_firmware_upgrade;

namespace
{
std::vector<uint8_t> rtu(std::vector<uint8_t> body)
{
	auto const crc = crc16_modbus(body.data(), body.size());
	body.push_back(crc);
	body.push_back(crc >> 8);
	return body;
}
struct ModbusRig
{
	modbus::Modbus bus;
	modbus_controller::ModbusController controller;
	PollingComponent monitor;
	DeyeFirmwareUpgrade console;
	ModbusRig()
	{
		console.set_modbus(&bus);
		console.set_modbus_controller(&controller);
		console.set_event_monitor(&monitor);
	}
	void begin()
	{
		CHECK(console.request_raw_modbus("01 03 01 f4 00 01", true, 1000) == nullptr);
		console.loop();
	}
	void tick(uint32_t ms)
	{
		test_now += ms;
		console.loop();
	}
	void reply(std::vector<uint8_t> const &data)
	{
		console.rx.insert(console.rx.end(), data.begin(), data.end());
		console.loop();
	}
	void restored()
	{
		CHECK(!console.raw_modbus_snapshot().active);
		CHECK(bus.enabled && controller.enabled && controller.polling && monitor.polling);
	}
};
}

TEST(raw_modbus_validates_hex_bounds_and_timeout_without_sending)
{
	ModbusRig r;
	for(auto const *hex : { "", "01", "01 0", "0 1 03", "01 0x03", "01 GG", "01:03", "<script>" })
		CHECK(r.console.request_raw_modbus(hex, true, 1000) != nullptr);
	CHECK(r.console.request_raw_modbus(std::string(510, '0'), true, 1000) != nullptr);
	CHECK(r.console.request_raw_modbus(std::string(514, '0'), false, 1000) != nullptr);
	CHECK(r.console.request_raw_modbus(std::string(1025, ' '), true, 1000) != nullptr);
	CHECK(r.console.request_raw_modbus("01 03", false, 1000) != nullptr);
	CHECK(r.console.request_raw_modbus("01 03", true, 99) != nullptr);
	CHECK(r.console.request_raw_modbus("01 03", true, 10001) != nullptr);
	CHECK(r.console.raw_modbus_snapshot().request_id == 0 && r.console.tx.empty());
	CHECK(r.console.request_raw_modbus("  010301F4\n0001\t", true, 1000) == nullptr);
	r.console.loop();
	CHECK(r.console.tx == rtu({ 1, 3, 1, 0xf4, 0, 1 }));
}

TEST(raw_modbus_exact_frame_and_maximum_frame)
{
	for(bool append : { false, true })
	{
		ModbusRig r;
		CHECK(r.console.request_raw_modbus(std::string(append ? 508 : 512, 'a'), append, 1000) ==
		      nullptr);
		r.console.loop();
		CHECK(r.console.tx.size() == 256 && r.console.tx.front() == 0xaa);
		if(append)
			CHECK(crc16_modbus(r.console.tx.data(), 256) == 0);
		else
			CHECK(r.console.tx ==
			      std::vector<uint8_t>(256, 0xaa)); // Do not repair caller-supplied CRC.
	}
}

TEST(raw_modbus_waits_for_polling_and_preserves_polling_timers)
{
	ModbusRig r;
	r.controller.queue_command({});
	r.begin();
	CHECK(r.console.tx.empty() && r.bus.enabled && r.controller.polling && r.monitor.polling);
	r.controller.command_queue_.clear();
	r.bus.blocked = true;
	r.tick(100);
	CHECK(r.console.tx.empty());
	r.bus.blocked = false;
	r.bus.empty = false;
	r.tick(100);
	CHECK(r.console.tx.empty());
	r.bus.empty = true;
	r.console.rx.push_back(0xff);
	r.tick(100);
	CHECK(r.console.tx.empty() && r.console.rx.size() == 1);
	r.console.rx.clear();
	r.tick(100);
	CHECK(r.console.tx.size() == 8 && !r.bus.enabled && !r.controller.enabled);
	CHECK(r.controller.polling && r.monitor.polling);
	r.reply(rtu({ 1, 3, 2, 0, 2 }));
	r.tick(20);
	r.restored();
}

TEST(raw_modbus_fragmented_reply_guard_and_clock_wrap)
{
	test_now = UINT32_MAX - 10;
	ModbusRig r;
	r.begin();
	auto const reply = rtu({ 1, 3, 2, 0xab, 0xcd });
	r.reply({ reply.begin(), reply.begin() + 3 });
	r.tick(19);
	CHECK(r.console.raw_modbus_snapshot().active);
	r.reply({ reply.begin() + 3, reply.end() });
	r.tick(19);
	CHECK(r.console.raw_modbus_snapshot().active && !r.bus.enabled);
	r.tick(1);
	r.restored();
	auto const s = r.console.raw_modbus_snapshot();
	CHECK(s.response == reply && !s.timed_out && !s.overflow && s.elapsed_ms == 39);
	CHECK(s.message == "Reply received.");
	r.tick(10000);
	CHECK(r.console.tx.size() == 8); // Requests, including writes, are never retried.
}

TEST(raw_modbus_retains_exception_mismatched_and_invalid_crc_replies)
{
	for(auto reply :
	    { rtu({ 1, 0x83, 2 }), rtu({ 2, 0x41, 4, 5 }), std::vector<uint8_t>{ 1, 3, 2, 0xff } })
	{
		ModbusRig r;
		r.begin();
		r.reply(reply);
		r.tick(20);
		r.restored();
		CHECK(r.console.raw_modbus_snapshot().response == reply);
	}
}

TEST(raw_modbus_timeout_waits_for_transmission_and_restores_bus)
{
	test_now = UINT32_MAX - 500;
	ModbusRig r;
	r.begin();
	r.tick(1000);
	CHECK(!r.console.raw_modbus_snapshot().timed_out && !r.bus.enabled);
	r.tick(9); // Eight bytes at 9600 8N1, rounded up.
	CHECK(r.console.raw_modbus_snapshot().timed_out && !r.bus.enabled);
	r.tick(20);
	r.restored();
	CHECK(r.console.raw_modbus_snapshot().response.empty() && r.console.tx.size() == 8);
	// A rejected request must leave the last transaction intact.
	CHECK(r.console.request_raw_modbus("01 0", true, 1000) != nullptr);
	CHECK(r.console.raw_modbus_snapshot().request_id == 1);
}

TEST(raw_modbus_overflow_and_continuous_noise_are_bounded)
{
	ModbusRig r;
	r.begin();
	r.reply(std::vector<uint8_t>(700, 0xff));
	CHECK(r.console.raw_modbus_snapshot().response.size() == 256);
	CHECK(r.console.raw_modbus_snapshot().overflow && r.console.rx.size() == 188);
	r.console.loop();
	r.tick(20);
	r.restored();
	r.begin();
	for(unsigned i = 0; i < 110; ++i)
	{
		r.console.rx.push_back(0xff);
		r.tick(20);
	}
	r.restored();
	CHECK(r.console.raw_modbus_snapshot().timed_out);
}

TEST(raw_modbus_cancel_drains_sent_request_and_rejects_stale_ids)
{
	ModbusRig r;
	r.bus.blocked = true;
	r.begin();
	CHECK(!r.console.cancel_raw_modbus(2));
	CHECK(r.console.cancel_raw_modbus(1));
	r.console.loop();
	r.restored();
	CHECK(r.console.tx.empty() && r.console.raw_modbus_snapshot().cancelled);
	r.bus.blocked = false;
	r.begin();
	CHECK(!r.console.cancel_raw_modbus(1));
	CHECK(r.console.cancel_raw_modbus(2));
	r.console.loop();
	CHECK(!r.bus.enabled); // Keep the UART until the pending reply or timeout has settled.
	r.reply(rtu({ 1, 3, 2, 0, 2 }));
	r.tick(20);
	r.restored();
	CHECK(r.console.raw_modbus_snapshot().cancelled && r.console.tx.size() == 8);
}

TEST(raw_modbus_busy_bus_deadline_does_not_send)
{
	ModbusRig r;
	r.bus.enabled = false;
	r.begin();
	r.tick(60000);
	CHECK(!r.console.raw_modbus_snapshot().active && r.console.raw_modbus_snapshot().timed_out);
	CHECK(r.console.tx.empty() && !r.bus.enabled && r.controller.polling && r.monitor.polling);
}

void stage(DeyeFirmwareUpgrade &upgrade, uint8_t target, size_t blocks);
TEST(raw_modbus_excludes_memory_uploads_and_upgrades_both_ways)
{
	ModbusRig r;
	stage(r.console, 1, 1);
	auto const staged = r.console.snapshot();
	r.begin();
	CHECK(r.console.request_memory_read(0x08000000, 4) != nullptr);
	CHECK(r.console.request_raw_modbus("0103", true, 1000) != nullptr);
	CHECK(!r.console.begin_upload(&r, "x", 1, 1024));
	CHECK(!r.console.request_start(staged.sha256, staged.confirmation_token));
	r.reply(rtu({ 1, 3, 2, 0, 2 }));
	r.tick(20);
	CHECK(r.console.snapshot().sha256 == staged.sha256);
	CHECK(r.console.request_memory_read(0x08000000, 4) == nullptr);
	CHECK(r.console.request_raw_modbus("0103", true, 1000) != nullptr);
	CHECK(r.console.cancel_memory_read(r.console.memory_snapshot().jobId));
	r.console.loop();
	CHECK(r.console.request_start(staged.sha256, staged.confirmation_token));
	CHECK(r.console.request_raw_modbus("0103", true, 1000) != nullptr);
	ModbusRig uploading;
	CHECK(uploading.console.begin_upload(&r, "x", 1, 1024));
	CHECK(uploading.console.request_raw_modbus("0103", true, 1000) != nullptr);
}

TEST(raw_modbus_http_admission_status_and_method_routing)
{
	ModbusRig r;
	RawModbusRequestHandler handler(&r.console);
	AsyncWebServerRequest req;
	req.verb = HTTP_GET;
	req.url = "/modbus";
	CHECK(handler.canHandle(&req));
	handler.handleRequest(&req);
	CHECK(req.response->status == 200 && r.console.tx.empty());
	CHECK(req.response->body.find("Register view") != std::string::npos);
	CHECK(req.response->headers["Cache-Control"] == "no-store");
	req.url = "/modbus/send";
	CHECK(!handler.canHandle(&req));
	req.verb = HTTP_POST;
	CHECK(handler.canHandle(&req));
	req.args = { { "hex", "01 03 01 f4 00 01" }, { "append_crc", "1" }, { "timeout_ms", "1000x" } };
	handler.handleRequest(&req);
	CHECK(req.response->status == 409 && !r.console.raw_modbus_snapshot().active);
	req.args["timeout_ms"] = "1000";
	req.args["append_crc"] = "yes";
	handler.handleRequest(&req);
	CHECK(req.response->status == 409);
	req.args["append_crc"] = "1";
	handler.handleRequest(&req);
	CHECK(req.response->status == 200 &&
	      r.console.raw_modbus_snapshot().active &&
	      r.console.tx.empty());
	r.console.loop();
	r.reply(rtu({ 1, 0x83, 2 }));
	r.tick(20);
	req.verb = HTTP_GET;
	req.url = "/modbus/status";
	CHECK(handler.canHandle(&req));
	handler.handleRequest(&req);
	CHECK(req.response->status == 200 &&
	      req.response->body.find("\"crc_valid\":true") != std::string::npos);
	CHECK(req.response->body.find("\"response\":\"01 83 02 c0 f1\"") != std::string::npos);
	req.url = "/modbus/cancel";
	CHECK(!handler.canHandle(&req));
	req.verb = HTTP_POST;
	req.args = { { "request_id", "0" } };
	handler.handleRequest(&req);
	CHECK(req.response->status == 409);
}
