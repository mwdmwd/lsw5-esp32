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
#include <memory>
#include <string>
#include <vector>

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

// Snapshots keep the PSRAM buffer alive during HTTP sends.
struct MemoryReadSnapshot
{
	bool dsp{ false };
	bool diagnostics_restored{ true };
	uint32_t jobId{ 0 };
	uint32_t address{ 0 };
	size_t length{ 0 };
	size_t received{ 0 };
	uint8_t unit{ 1 };
	bool active{ false };
	bool complete{ false };
	uint32_t elapsedMs{ 0 };
	size_t flashSize{ 512 * 1024 };
	std::string message{ "Ready." };
	std::shared_ptr<uint8_t> data;
};

struct RawModbusSnapshot
{
	uint32_t request_id{ 0 };
	bool active{ false };
	bool sent{ false };
	bool timed_out{ false };
	bool cancelled{ false };
	bool overflow{ false };
	uint32_t elapsed_ms{ 0 };
	uint32_t timeout_ms{ 1000 };
	std::string message{ "Ready." };
	std::vector<uint8_t> request;
	std::vector<uint8_t> response;
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
	void set_event_monitor(PollingComponent *monitor)
	{
		this->event_monitor_ = monitor;
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

	// Memory captures and upgrades cannot run at the same time.
	void set_readback_unit_address(uint8_t unit)
	{
		this->readbackUnit_ = unit;
		this->memoryRead_.unit = unit;
	}
	void set_readback_flash_size(size_t size)
	{
		this->readbackFlashSize_ = size;
	}
	// Returns nullptr on success, otherwise a fixed error string.
	char const *request_memory_read(uint32_t address, size_t length, bool dsp = false);
	bool cancel_memory_read(uint32_t jobId);
	bool clear_memory_read(uint32_t jobId);
	MemoryReadSnapshot memory_snapshot();

	// Raw requests share the upgrade/readback UART owner and are never retried.
	char const *request_raw_modbus(std::string const &hex, bool append_crc, uint32_t timeout_ms);
	bool cancel_raw_modbus(uint32_t request_id);
	RawModbusSnapshot raw_modbus_snapshot();

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
	void restore_bus_(bool restartPollers = true);
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

	enum class ReadbackPhase : uint8_t
	{
		IDLE,
		YIELDING,
		WAITING,
		SETTLING,
		STOPPING
	};
	void setup_memory_readback_();
	void service_memory_readback_(uint32_t now);
	void finish_memory_readback_(bool success, char const *message);
	MemoryReadSnapshot memoryRead_;
	ReadbackPhase readbackPhase_{ ReadbackPhase::IDLE };
	uint8_t readbackUnit_{ 1 };
	uint32_t readbackDeadline_{ 0 };
	uint32_t readbackQuietUntil_{ 0 };
	uint32_t readbackStarted_{ 0 };
	size_t readbackFlashSize_{ 512 * 1024 };
	bool readbackCancel_{ false };
	std::array<uint8_t, 8> readbackFrame_{};
	size_t readbackFrameLength_{ 0 };

	// The stock DSP has a cached indirect window, not the HMI's FC05 reader.
	// Only source 0 is assigned absolutely; sources 1/2 provide address tags.
	enum class DspReadStage : uint8_t
	{
		IDENTITY,
		DISABLE,
		WAIT_DISABLED,
		ENABLE,
		WAIT_ENABLED,
		SENTINEL,
		WAIT_SENTINEL,
		PROBE,
		WAIT_PROBE,
		CONFIG_SENTINEL,
		WAIT_CONFIG_SENTINEL,
		CONFIGURE,
		WAIT_CLEARED,
		ADDRESS,
		DATA,
		RESTORE,
		WAIT_RESTORED,
	};
	void start_dsp_readback_(uint32_t now);
	void service_dsp_readback_(uint32_t now);
	void dsp_read_stage_(DspReadStage stage, uint32_t now);
	void dsp_read_request_();
	void dsp_read_reply_(uint32_t now);
	void stop_dsp_readback_(bool success, char const *message, uint32_t now);
	DspReadStage dspReadStage_{ DspReadStage::IDENTITY };
	uint32_t dspStageDeadline_{ 0 };
	uint32_t dspNextRequest_{ 0 };
	uint32_t dspRestoreAfter_{ 0 };
	uint8_t dspProbeIndex_{ 0 };
	uint8_t dspConfigureIndex_{ 0 };
	uint8_t dspRestoreAttempts_{ 0 };
	std::array<uint32_t, 2> dspPointers_{};
	std::vector<uint8_t> dspRequest_;
	std::vector<uint8_t> dspReply_;
	bool dspTouched_{ false };
	bool dspStopping_{ false };
	bool dspSucceeded_{ false };
	std::string dspResult_;

	void setup_raw_modbus_();
	void service_raw_modbus_(uint32_t now);
	void finish_raw_modbus_(char const *message);
	RawModbusSnapshot raw_modbus_;
	uint32_t raw_modbus_started_{ 0 };
	uint32_t raw_modbus_deadline_{ 0 };
	uint32_t raw_modbus_quiet_until_{ 0 };
	bool raw_modbus_cancel_{ false };

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
	PollingComponent *event_monitor_{ nullptr };
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
