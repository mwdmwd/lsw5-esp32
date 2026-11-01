// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esphome/components/modbus/modbus.h"
#include "esphome/components/modbus_controller/modbus_controller.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome::deye_firmware_upgrade
{

enum class UpgradeState : uint8_t
{
	EMPTY,
	UPLOADING,
	STAGED,
	QUEUED,
	QUIESCING,
	ACTIVATING,
	WAITING_FOR_RECEIVER,
	SENDING,
	FINISHING,
	COMPLETE,
	ERROR,
};

struct UpgradeSnapshot
{
	UpgradeState state{ UpgradeState::EMPTY };
	std::string state_name;
	std::string message;
	std::string filename;
	std::string sha256;
	std::string confirmation_token;
	uint8_t target{ 0 };
	size_t image_size{ 0 };
	uint32_t payload_size{ 0 };
	std::array<uint8_t, 3> selectors{};
	uint32_t crc32{ 0 };
	size_t acknowledged_blocks{ 0 };
	size_t total_blocks{ 0 };
	size_t max_firmware_size{ 0 };
	bool has_verified_image{ false };
	bool active{ false };
};

class DeyeFirmwareUpgrade final : public Component, public uart::UARTDevice
{
public:
	~DeyeFirmwareUpgrade();

	void setup() override;
	void loop() override;
	void dump_config() override;
	float get_setup_priority() const override
	{
		return setup_priority::AFTER_WIFI;
	}

	void set_modbus(modbus::Modbus *modbus)
	{
		this->modbus_ = modbus;
	}
	void set_modbus_controller(modbus_controller::ModbusController *controller)
	{
		this->modbus_controller_ = controller;
	}
	void set_max_firmware_size(size_t max_firmware_size)
	{
		this->max_firmware_size_ = max_firmware_size;
	}

	bool begin_upload(void const *owner, std::string const &filename, uint8_t target,
	                  size_t content_length);
	bool append_upload(void const *owner, size_t index, uint8_t const *data, size_t length);
	bool finish_upload(void const *owner);
	void abort_upload(void const *owner);
	bool request_start(std::string const &sha256, std::string const &confirmation_token);
	bool clear_staged(std::string const &sha256, std::string const &confirmation_token);
	UpgradeSnapshot snapshot();

protected:
	static constexpr uint8_t MAX_ACTIVATION_ATTEMPTS = 5;
	static constexpr uint8_t MAX_BLOCK_RETRIES = 10;
	static constexpr size_t UART_WRITE_CHUNK_SIZE = 32;
	static constexpr uint32_t UPLOAD_TIMEOUT_MS = 120000;
	static constexpr uint32_t QUIESCE_TIMEOUT_MS = 60000;
	static constexpr uint32_t ACTIVATION_ACK_TIMEOUT_MS = 2000;
	static constexpr uint32_t RECEIVER_READY_TIMEOUT_MS = 76000;
	static constexpr uint32_t FIRST_BLOCK_ACK_TIMEOUT_MS = 20000;
	static constexpr uint32_t BLOCK_ACK_TIMEOUT_MS = 15000;
	static constexpr uint32_t BLOCK_RETRY_DELAY_MS = 6000;
	static constexpr uint32_t EOT_DELAY_MS = 1000;
	static constexpr uint32_t POST_TRANSFER_DELAY_MS = 10000;

	enum class TransportPhase : uint8_t
	{
		IDLE,
		QUIESCING,
		WAIT_ACTIVATION_ACK,
		WAIT_RECEIVER,
		SEND_BLOCK,
		WAIT_BLOCK_REPLY,
		RETRY_DELAY,
		EOT_DELAY,
		POST_TRANSFER,
	};

	void renew_confirmation_token_locked_();
	bool validate_uploaded_image_locked_();
	bool reject_image_locked_(std::string const &message);
	void free_firmware_locked_();
	void expire_interrupted_upload_(uint32_t now);

	void begin_transport_(uint32_t now);
	void service_quiescing_(uint32_t now);
	void take_bus_();
	void restore_bus_();
	void drain_uart_();

	void send_activation_(uint32_t now);
	void service_activation_ack_(uint32_t now);
	void service_receiver_wait_(uint32_t now);
	void begin_block_(size_t block_index);
	void service_block_send_(uint32_t now);
	void service_block_reply_(uint32_t now);
	void retry_block_(uint32_t now, char const *reason);
	void accept_block_(uint32_t now);
	void send_eot_(uint32_t now);
	void service_post_transfer_(uint32_t now);
	void complete_transport_();
	void fail_transport_(std::string const &message);

	void set_state_(UpgradeState state, std::string const &message);
	static char const *state_name_(UpgradeState state);
	static bool state_is_active_(UpgradeState state);
	static bool valid_target_(uint8_t target)
	{
		return target == 1 || target == 2;
	}

	Mutex mutex_;
	UpgradeState state_{ UpgradeState::EMPTY };
	std::string message_{ "No firmware image is staged." };

	uint8_t *firmware_{ nullptr };
	size_t firmware_capacity_{ 0 };
	size_t firmware_size_{ 0 };
	size_t max_firmware_size_{ 2 * 1024 * 1024 };
	std::string filename_;
	std::array<char, 65> sha256_{};
	std::array<char, 33> confirmation_token_{};
	DeyeImageInfo image_info_{};
	uint8_t target_{ 0 };
	void const *upload_owner_{ nullptr };
	uint32_t upload_last_activity_{ 0 };

	size_t acknowledged_blocks_{ 0 };
	size_t total_blocks_{ 0 };

	modbus::Modbus *modbus_{ nullptr };
	modbus_controller::ModbusController *modbus_controller_{ nullptr };
	TransportPhase transport_phase_{ TransportPhase::IDLE };
	uint32_t phase_deadline_{ 0 };
	bool bus_owned_{ false };
	bool modbus_loop_was_enabled_{ true };
	bool controller_loop_was_enabled_{ true };

	uint8_t activation_attempt_{ 0 };
	size_t activation_ack_match_{ 0 };
	bool activation_ack_received_{ false };
	bool early_crc_request_{ false };

	size_t block_index_{ 0 };
	size_t packet_offset_{ 0 };
	uint8_t block_retries_{ 0 };
	bool eot_ack_received_{ false };
	std::array<uint8_t, XMODEM_PACKET_SIZE> packet_{};
};

} // namespace esphome::deye_firmware_upgrade
