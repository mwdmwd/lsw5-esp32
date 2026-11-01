// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/priority_modbus_controller/priority_modbus_controller.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::deye_event_monitor
{

struct EventSnapshot
{
	uint32_t timestamp_ms{ 0 };
	uint16_t change_mask{ 0 };
	uint16_t operating_state{ 0 };
	uint16_t power_state{ 0 };
	uint16_t relay_bitmap{ 0 };
	std::array<uint16_t, 2> warnings{};
	std::array<uint16_t, 4> faults{};
	uint16_t trip_data{ 0 };
	uint16_t battery_voltage{ 0 };
	uint16_t battery_soc{ 0 };
	int16_t battery_power{ 0 };
	int16_t battery_current{ 0 };
	std::array<uint16_t, 3> grid_voltage{};
	uint16_t grid_frequency{ 0 };
	std::array<uint16_t, 3> inverter_voltage{};
	int16_t inverter_power{ 0 };
	std::array<uint16_t, 3> load_voltage{};
	int16_t load_power{ 0 };
	uint16_t load_frequency{ 0 };
	bool telemetry_valid{ false };
};

class DeyeEventMonitor final : public PollingComponent
{
public:
	void update() override;
	void dump_config() override;

	void set_modbus_controller(priority_modbus_controller::PriorityModbusController *controller)
	{
		this->controller_ = controller;
	}

	SUB_SENSOR(event_sequence)
	SUB_SENSOR(trip_data)
	SUB_SENSOR(relay_bitmap)
	SUB_TEXT_SENSOR(operating_state)
	SUB_TEXT_SENSOR(current_warnings)
	SUB_TEXT_SENSOR(current_faults)
	SUB_TEXT_SENSOR(latched_warnings)
	SUB_TEXT_SENSOR(latched_faults)
	SUB_TEXT_SENSOR(last_event)
	SUB_BINARY_SENSOR(any_fault)
	SUB_BINARY_SENSOR(inverter_relay)
	SUB_BINARY_SENSOR(grid_relay)
	SUB_BINARY_SENSOR(generator_relay)

	void clear_latches();

protected:
	// Poll only run state and events. Reading the whole 500..603 page at 1 Hz
	// would occupy more bus time; the HMI refreshes it only about every 2 s.
	static constexpr uint16_t RUN_STATE_START = 500;
	static constexpr uint16_t RUN_STATE_COUNT = 1;
	static constexpr uint16_t EVENT_STATE_START = 551;
	static constexpr uint16_t EVENT_STATE_COUNT = 9;

	// One context read per transition. Reading the gap costs less than a second
	// request/turnaround and avoids rebuilding the HMI register map twice.
	static constexpr uint16_t SNAPSHOT_START = 587;
	static constexpr uint16_t SNAPSHOT_COUNT = 69; // battery/grid/inverter/load through r655
	static constexpr uint32_t PENDING_EVENT_TIMEOUT_MS = 3000;

	enum ChangeMask : uint16_t
	{
		CHANGE_OPERATING_STATE = 1U << 0,
		CHANGE_POWER_STATE = 1U << 1,
		CHANGE_RELAY = 1U << 2,
		CHANGE_WARNING = 1U << 3,
		CHANGE_FAULT = 1U << 4,
		CHANGE_TRIP_DATA = 1U << 5,
		CHANGE_ABNORMAL_AT_BOOT = 1U << 6,
	};

	void handle_run_state_(std::vector<uint8_t> const &data);
	void handle_event_state_(std::vector<uint8_t> const &data);
	void handle_snapshot_(uint32_t sequence, std::vector<uint8_t> const &data);
	void begin_event_(uint16_t change_mask);
	void queue_event_snapshot_();
	void publish_pending_event_();
	void publish_live_state_();
	void publish_latches_();

	static uint16_t word_at_(std::vector<uint8_t> const &data, uint16_t start_address,
	                         uint16_t address);
	static std::string operating_state_name_(uint16_t state);
	static std::string format_bitmap_(uint16_t const *words, size_t word_count, char prefix);
	static std::string format_event_(uint32_t sequence, EventSnapshot const &snapshot);

	priority_modbus_controller::PriorityModbusController *controller_{ nullptr };

	EventSnapshot current_{};
	EventSnapshot pending_event_{};
	bool have_run_state_{ false };
	bool have_event_state_{ false };
	bool event_pending_{ false };
	uint32_t event_sequence_{ 0 };
	std::array<uint16_t, 2> latched_warnings_{};
	std::array<uint16_t, 4> latched_faults_{};
};

} // namespace esphome::deye_event_monitor
