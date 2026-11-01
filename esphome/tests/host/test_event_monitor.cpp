// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
// White-box access is confined to the test translation unit.
#define protected public
#include "esphome/components/deye_event_monitor/deye_event_monitor.h"
#undef protected
#include "esphome/components/deye_event_monitor/deye_event_monitor.cpp"
#include "esphome/components/priority_modbus_controller/priority_modbus_controller.cpp"
using namespace esphome;
using esphome::deye_event_monitor::DeyeEventMonitor;
using esphome::priority_modbus_controller::PriorityModbusController;

TEST(priority_deduplicates_without_replacing_inflight_or_retrying_work)
{
	for(bool retry : { false, true })
	{
		PriorityModbusController c;
		int old_calls = 0, new_calls = 0;
		auto make = [&](uint16_t address, int &calls)
		{
			return modbus_controller::ModbusCommandItem::create_read_command(
			    &c, modbus::ModbusRegisterType::HOLDING, address, 1,
			    [&calls](auto, auto, auto const &) { ++calls; });
		};
		c.queue_command(make(100, old_calls));
		c.queue_command(make(200, old_calls));
		if(retry)
			CHECK(c.command_queue_.front()->send());
		c.ready =
		    retry; // After a timeout the transport is ready, but the sent front still owns retries.
		c.queue_priority_command(make(500, new_calls));
		c.queue_priority_command(make(500, new_calls));
		CHECK(c.command_queue_.size() == 3);
		CHECK(c.command_queue_.front()->register_address == 100);
		CHECK((*std::next(c.command_queue_.begin()))->register_address == 500);
		c.queue_priority_command(make(100, new_calls));
		c.queue_priority_command(make(100, new_calls));
		CHECK(c.command_queue_.size() == 4);
		c.command_queue_.front()->on_data_func(modbus::ModbusRegisterType::HOLDING, 100, {});
		CHECK(old_calls == 1 && new_calls == 0);
	}
}

TEST(recovery_does_not_overwrite_pending_fault)
{
	PriorityModbusController c;
	DeyeEventMonitor m;
	text_sensor::TextSensor events;
	m.set_modbus_controller(&c);
	m.set_last_event_text_sensor(&events);
	m.handle_run_state_({ 0, 2 });
	std::vector<uint8_t> normal(18), fault(18);
	fault[12] = 0x20; // r557, F46
	m.handle_event_state_(normal);
	test_now = 100;
	m.handle_event_state_(fault);
	CHECK(m.event_sequence_ == 1 && m.pending_event_.faults[2] == 0x2000);
	std::vector<uint8_t> data(138);
	data[0] = 0x14;
	data[1] = 0x50;
	test_now = 200;
	m.handle_event_state_(normal);
	CHECK(events.history.size() == 1);
	CHECK(events.history[0].find("f=0000:2000:0000:0000") != std::string::npos);
	CHECK(events.history[0].find("t=100ms") != std::string::npos);
	CHECK(events.history[0].find("v=0") != std::string::npos);
	CHECK(m.event_sequence_ == 2 && m.pending_event_.timestamp_ms == 200);
	m.handle_snapshot_(1, data);
	CHECK(events.history.size() == 1); // late old response
	m.handle_snapshot_(2, data);
	CHECK(events.history.size() == 2);
	CHECK(events.history[1].find("f=0000:0000:0000:0000") != std::string::npos);
	CHECK(events.history[1].find("v=1") != std::string::npos);
	CHECK(m.latched_faults_[2] == 0x2000);
}
TEST(event_context_is_one_read_and_never_reuses_old_telemetry)
{
	PriorityModbusController c;
	DeyeEventMonitor m;
	text_sensor::TextSensor events;
	m.set_modbus_controller(&c);
	m.set_last_event_text_sensor(&events);
	m.handle_run_state_({ 0, 2 });
	m.handle_event_state_(std::vector<uint8_t>(18));
	m.handle_run_state_({ 0, 3 });
	CHECK(c.command_queue_.size() == 1);
	CHECK(c.command_queue_.front()->register_address == 587);
	CHECK(c.command_queue_.front()->register_count == 69);
	std::vector<uint8_t> data(138);
	for(auto [offset, word] : std::vector<std::pair<size_t, uint16_t>>{ { 0, 5200 },
	                                                                    { 1, 60 },
	                                                                    { 3, 0xfd44 },
	                                                                    { 4, 0xff85 },
	                                                                    { 11, 2301 },
	                                                                    { 12, 2302 },
	                                                                    { 13, 2303 },
	                                                                    { 22, 5000 },
	                                                                    { 40, 2291 },
	                                                                    { 41, 2292 },
	                                                                    { 42, 2293 },
	                                                                    { 49, 1500 },
	                                                                    { 57, 2281 },
	                                                                    { 58, 2282 },
	                                                                    { 59, 2283 },
	                                                                    { 66, 1400 },
	                                                                    { 68, 4999 } })
	{
		data[2 * offset] = word >> 8;
		data[2 * offset + 1] = word;
	}
	m.handle_snapshot_(1, std::vector<uint8_t>(136));
	CHECK(events.history.empty());
	c.command_queue_.front()->on_data_func(modbus::ModbusRegisterType::HOLDING, 587, data);
	CHECK(events.history.size() == 1);
	CHECK(events.state.find("b=5200,60,-700,-123 g=2301,2302,2303,5000 "
	                        "i=2291,2292,2293,1500 l=2281,2282,2283,1400,4999 v=1") !=
	      std::string::npos);
	m.handle_run_state_({ 0, 2 });
	test_now = 3000;
	m.update();
	CHECK(events.history.size() == 2);
	CHECK(events.state.find("b=0,0,0,0") != std::string::npos);
	CHECK(events.state.find("v=0") != std::string::npos);
	m.handle_snapshot_(2, data);
	CHECK(events.history.size() == 2); // A late reply cannot replace the timed-out snapshot.
}
