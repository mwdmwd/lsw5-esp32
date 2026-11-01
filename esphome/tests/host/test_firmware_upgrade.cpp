// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
// White-box access is confined to the test translation unit.
#define protected public
#include "esphome/components/deye_firmware_upgrade/deye_firmware_upgrade.h"
#undef protected
#include "esphome/components/deye_firmware_upgrade/deye_firmware_upgrade.cpp"
#include "esphome/components/deye_firmware_upgrade/protocol.cpp"
using namespace esphome;
using namespace esphome::deye_firmware_upgrade;
using Phase = DeyeFirmwareUpgrade::TransportPhase;

std::vector<uint8_t> image(size_t blocks = 1)
{
	std::vector<uint8_t> b(blocks * 1024, 0x5a);
	size_t const n = b.size() - 11;
	b[n] = 1;
	b[n + 1] = 255;
	b[n + 2] = 255;
	uint32_t crc = crc32_ieee(b.data(), n);
	for(unsigned i = 0; i < 4; i++)
	{
		b[n + 3 + i] = uint32_t(n) >> (24 - 8 * i);
		b[n + 7 + i] = crc >> (24 - 8 * i);
	}
	return b;
}
void stage(DeyeFirmwareUpgrade &u, uint8_t target = 1, size_t blocks = 1)
{
	auto bytes = image(blocks);
	CHECK(u.begin_upload(&u, "image.bin", target, bytes.size() + 512));
	CHECK(u.append_upload(&u, 0, bytes.data(), bytes.size()));
	CHECK(u.finish_upload(&u));
}
bool start(DeyeFirmwareUpgrade &u, UpgradeSnapshot const &s)
{
	return u.request_start(s.sha256, s.confirmation_token);
}
struct Rig
{
	modbus::Modbus bus;
	modbus_controller::ModbusController controller;
	PollingComponent monitor;
	DeyeFirmwareUpgrade u;
	Rig()
	{
		u.set_modbus(&bus);
		u.set_modbus_controller(&controller);
		u.set_event_monitor(&monitor);
	}
	void begin(uint8_t target = 1, size_t blocks = 1)
	{
		stage(u, target, blocks);
		CHECK(start(u, u.snapshot()));
		u.loop();
	}
	void ready()
	{
		CHECK(u.transport_phase_ == Phase::WAIT_ACTIVATION_ACK);
		u.rx = { 1, 0x10, 4, 0x38, 0, 2, 0xc1, 0x35, 0x43 };
		u.loop();
		CHECK(u.transport_phase_ == Phase::SEND_BLOCK);
	}
	void packet()
	{
		unsigned n = 0;
		while(u.transport_phase_ == Phase::SEND_BLOCK && n++ < 40)
			u.loop();
		CHECK(u.transport_phase_ == Phase::WAIT_BLOCK_REPLY);
	}
	void eot()
	{
		u.rx.push_back(6);
		u.loop();
		CHECK(u.transport_phase_ == Phase::EOT_DELAY);
		test_now += 1000;
		u.loop();
		CHECK(u.transport_phase_ == Phase::POST_TRANSFER);
	}
};

TEST(crc_golden_vectors)
{
	auto const *v = reinterpret_cast<uint8_t const *>("123456789");
	CHECK(crc16_modbus(v, 9) == 0x4b37);
	CHECK(crc16_xmodem(v, 9) == 0x31c3);
	CHECK(crc32_ieee(v, 9) == 0xcbf43926);
}
TEST(image_validation_and_packet_wrap)
{
	auto bytes = image(257);
	DeyeImageInfo info{};
	CHECK(inspect_deye_image(bytes.data(), bytes.size(), info));
	CHECK(info.payload_size == bytes.size() - 11);
	std::array<uint8_t, XMODEM_PACKET_SIZE> packet{};
	build_xmodem_packet(bytes.data() + 255 * XMODEM_BLOCK_SIZE, 255, packet);
	CHECK(packet[0] == 2 && packet[1] == 0 && packet[2] == 255);
	build_xmodem_packet(bytes.data() + 256 * XMODEM_BLOCK_SIZE, 256, packet);
	CHECK(packet[1] == 1 && packet[2] == 254);
	bytes[10] ^= 1;
	CHECK(!inspect_deye_image(bytes.data(), bytes.size(), info));
	CHECK(!inspect_deye_image(bytes.data(), bytes.size() - 1, info));
}
TEST(upload_bounds_allocation_and_timeout)
{
	auto bytes = image(2);
	for(auto [content_length, offset, length] :
	    { std::array<size_t, 3>{ 512, 0, 1024 }, { 4096, 0, 1025 }, { 1024, 1, 1024 } })
	{
		DeyeFirmwareUpgrade u;
		u.set_max_firmware_size(1024);
		CHECK(u.begin_upload(&u, "x", 1, content_length));
		CHECK(!u.append_upload(&u, offset, bytes.data(), length));
		CHECK(!u.finish_upload(&u));
		CHECK(u.snapshot().state == UpgradeState::ERROR && u.firmware_ == nullptr && u.tx.empty());
	}
	DeyeFirmwareUpgrade u;
	CHECK(!u.begin_upload(&u, "x", 1, 0));
	RAMAllocator<uint8_t>::fail = true;
	bool const accepted = u.begin_upload(&u, "x", 1, 1024);
	RAMAllocator<uint8_t>::fail = false;
	CHECK(!accepted && u.tx.empty());
	CHECK(u.begin_upload(&u, "x", 1, 1024));
	test_now += 120001;
	u.loop();
	CHECK(u.snapshot().state == UpgradeState::ERROR && u.firmware_ == nullptr);
}

TEST(quiesce_timeout_does_not_activate)
{
	Rig r;
	r.bus.blocked = true;
	r.begin();
	CHECK(r.u.tx.empty());
	CHECK(!r.controller.polling && !r.monitor.polling);
	test_now += 60000;
	r.u.loop();
	CHECK(r.u.snapshot().state == UpgradeState::ERROR && r.u.tx.empty());
	CHECK(r.controller.polling && r.monitor.polling && r.bus.enabled);
}
TEST(http_upload_hmi_transfer_and_replay_rejection)
{
	Rig r;
	FirmwareUpgradeRequestHandler handler(&r.u);
	AsyncWebServerRequest req;
	auto bytes = image();
	handler.handleUpload(&req, "<image>.bin", 0, nullptr, 0, false);
	handler.handleUpload(&req, "<image>.bin", 0, bytes.data(), bytes.size(), true);
	handler.handleRequest(&req);
	auto const staged = r.u.snapshot();
	CHECK(req.response->status == 200 && staged.state == UpgradeState::STAGED);
	CHECK(r.u.tx.empty()); // Uploading alone must not contact the inverter.
	CHECK(req.response->body.find("&lt;image&gt;.bin") != std::string::npos);
	CHECK(req.response->body.find(staged.confirmation_token) != std::string::npos);
	req.url = "/firmware/start";
	req.args = { { "sha256", staged.sha256 }, { "confirmation_token", staged.confirmation_token } };
	handler.handleRequest(&req);
	CHECK(req.response->status == 200 && r.u.snapshot().state == UpgradeState::QUEUED);
	r.u.loop();
	CHECK(!r.bus.enabled && !r.controller.polling && !r.monitor.polling);
	CHECK(r.u.tx.size() == 13 && r.u.tx[8] == 1);
	CHECK(crc16_modbus(r.u.tx.data(), 11) == (r.u.tx[11] | uint16_t(r.u.tx[12]) << 8));
	r.ready();
	r.packet();
	CHECK(r.u.tx.size() == 13 + XMODEM_PACKET_SIZE);
	r.eot();
	CHECK(r.u.tx.back() == 4 && !r.bus.enabled);
	test_now += 10000; // HMI may reset without acknowledging EOT.
	r.u.loop();
	CHECK(r.u.snapshot().state == UpgradeState::COMPLETE);
	CHECK(r.bus.enabled && r.controller.enabled && r.controller.polling && r.monitor.polling);
	CHECK(!start(r.u, staged));
}

TEST(dsp_staging_ack_and_post_eot_rejection)
{
	for(uint8_t reply : { uint8_t(6), uint8_t(0x15), uint8_t(0x18) })
	{
		Rig r;
		r.begin(2);
		r.ready();
		r.packet();
		r.eot();
		r.u.rx.push_back(reply);
		r.u.loop();
		test_now += 10000;
		r.u.loop();
		CHECK(r.u.snapshot().state == (reply == 6 ? UpgradeState::COMPLETE : UpgradeState::ERROR));
		CHECK(r.bus.enabled && r.controller.enabled);
	}
}
TEST(retry_delay_discards_stale_ack_before_resend)
{
	Rig r;
	r.begin();
	r.ready();
	r.packet();
	r.u.rx.push_back(0x15);
	r.u.loop();
	CHECK(r.u.transport_phase_ == Phase::RETRY_DELAY);
	r.u.rx.push_back(6);
	test_now += 6000;
	r.u.loop();
	r.packet();
	CHECK(r.u.snapshot().acknowledged_blocks == 0 && r.u.rx.empty());
	r.u.loop();
	CHECK(r.u.transport_phase_ == Phase::WAIT_BLOCK_REPLY);
	r.u.rx.push_back(6);
	r.u.loop();
	CHECK(r.u.transport_phase_ == Phase::EOT_DELAY);
}
TEST(receiver_cancellation_restores_bus_and_requires_fresh_confirmation)
{
	test_now = UINT32_MAX - 500;
	Rig r;
	stage(r.u);
	auto const old = r.u.snapshot();
	CHECK(start(r.u, old));
	r.u.loop();
	r.u.rx = { 1, 0x10, 4, 0x38, 0, 2, 0xc1, 0x35 };
	r.u.loop();
	test_now += 1000; // Cross millis() wrap while awaiting the receiver.
	r.u.loop();
	CHECK(r.u.transport_phase_ == Phase::WAIT_RECEIVER);
	r.u.rx.push_back(0x18);
	r.u.loop();
	CHECK(r.u.snapshot().state == UpgradeState::ERROR);
	CHECK(r.bus.enabled && r.controller.polling && r.monitor.polling);
	CHECK(!start(r.u, old));
	CHECK(start(r.u, r.u.snapshot()));
}

TEST(stale_browser_actions_cannot_mutate_replacement)
{
	for(uint8_t target : { 1, 2 })
	{
		DeyeFirmwareUpgrade u;
		stage(u);
		auto const old = u.snapshot();
		stage(u, target); // Same bytes, either the same or a different target.
		auto const fresh = u.snapshot();
		CHECK(old.sha256 == fresh.sha256);
		CHECK(!start(u, old));
		CHECK(!u.clear_staged(old.sha256, old.confirmation_token));
		CHECK(u.snapshot().state == UpgradeState::STAGED && u.tx.empty());
		CHECK(start(u, fresh));
		CHECK(!start(u, fresh)); // The accepted attempt consumes its confirmation.
	}
}

TEST(incomplete_or_multiple_file_uploads_are_discarded)
{
	enum BadUpload
	{
		MISSING_FINAL,
		SECOND_FILE,
		DATA_AFTER_FINAL
	};
	for(auto problem : { MISSING_FINAL, SECOND_FILE, DATA_AFTER_FINAL })
	{
		DeyeFirmwareUpgrade u;
		FirmwareUpgradeRequestHandler handler(&u);
		AsyncWebServerRequest req;
		auto bytes = image();
		handler.handleUpload(&req, "image", 0, nullptr, 0, false);
		handler.handleUpload(&req, "image", 0, bytes.data(), bytes.size(),
		                     problem != MISSING_FINAL);
		if(problem == SECOND_FILE)
			handler.handleUpload(&req, "second", 0, nullptr, 0, false);
		if(problem == DATA_AFTER_FINAL)
			handler.handleUpload(&req, "image", bytes.size(), bytes.data(), 1, false);
		handler.handleRequest(&req);
		CHECK(req.response->status == 409 && u.snapshot().state == UpgradeState::ERROR);
		CHECK(!u.snapshot().has_verified_image && u.firmware_ == nullptr && u.tx.empty());
	}
}

TEST(overlapping_http_request_cannot_clear_active_owner)
{
	DeyeFirmwareUpgrade u;
	FirmwareUpgradeRequestHandler h(&u);
	AsyncWebServerRequest a, b;
	auto bytes = image();
	b.args["target"] = "2";
	h.handleUpload(&a, "first", 0, nullptr, 0, false);
	h.handleUpload(&a, "first", 0, bytes.data(), 512, false);
	h.handleUpload(&b, "second", 0, nullptr, 0, false);
	h.handleUpload(&b, "second", 0, bytes.data(), bytes.size(), true);
	h.handleRequest(&b);
	CHECK(b.response->status == 409);
	h.handleUpload(&a, "first", 512, bytes.data() + 512, 512, true);
	h.handleRequest(&a);
	CHECK(a.response->status == 200);
	CHECK(u.snapshot().target == 1 && u.snapshot().filename == "first");
}
TEST(expired_request_pointer_can_be_reused)
{
	DeyeFirmwareUpgrade u;
	FirmwareUpgradeRequestHandler h(&u);
	AsyncWebServerRequest req;
	auto bytes = image();
	h.handleUpload(&req, "old", 0, nullptr, 0, false);
	h.handleUpload(&req, "old", 0, bytes.data(), 512, false);
	test_now += 120001;
	u.loop();
	CHECK(u.snapshot().state == UpgradeState::ERROR);
	h.handleUpload(&req, "new", 0, nullptr, 0, false);
	h.handleUpload(&req, "new", 0, bytes.data(), bytes.size(), true);
	h.handleRequest(&req);
	CHECK(req.response->status == 200 && u.snapshot().filename == "new");
}
TEST(activation_timeout_restores_polling_without_sending_image)
{
	Rig r;
	r.begin();
	for(unsigned attempt = 0; attempt < 5; ++attempt)
	{
		test_now += 2000;
		r.u.loop();
	}
	CHECK(r.u.snapshot().state == UpgradeState::ERROR);
	CHECK(r.u.tx.size() == 5 * 13);
	CHECK(r.bus.enabled && r.controller.enabled && r.controller.polling && r.monitor.polling);
}

TEST(block_retry_exhaustion_restores_polling_without_eot)
{
	Rig r;
	r.begin();
	r.ready();
	for(unsigned attempt = 0; attempt <= 10; ++attempt)
	{
		r.packet();
		r.u.rx.push_back(0x15);
		r.u.loop();
		if(attempt < 10)
		{
			test_now += 6000;
			r.u.loop();
		}
	}
	CHECK(r.u.snapshot().state == UpgradeState::ERROR);
	CHECK(r.u.snapshot().acknowledged_blocks == 0);
	CHECK(r.u.tx.size() == 13 + 11 * XMODEM_PACKET_SIZE);
	CHECK(r.bus.enabled && r.controller.enabled && r.controller.polling && r.monitor.polling);
}
