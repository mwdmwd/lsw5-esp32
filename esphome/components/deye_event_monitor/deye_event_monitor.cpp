// SPDX-License-Identifier: GPL-3.0-or-later
#include "deye_event_monitor.h"

#include "esphome/core/log.h"

#include <cstdio>

namespace esphome::deye_event_monitor
{

static char const *const TAG = "deye_event_monitor";

template<typename Sensor, typename Value>
static void publish_if_changed(Sensor *output, Value const &value)
{
	if(output != nullptr && (!output->has_state() || output->state != value))
		output->publish_state(value);
}

void DeyeEventMonitor::dump_config()
{
	ESP_LOGCONFIG(TAG,
	              "Inverter event monitor:\n"
	              "  Poll interval: %u ms\n"
	              "  Continuous reads: holding register %u and registers %u..%u\n"
	              "  Event-only snapshot: registers %u..%u\n"
	              "  Persistence: RAM only",
	              this->get_update_interval(), RUN_STATE_START, EVENT_STATE_START,
	              EVENT_STATE_START + EVENT_STATE_COUNT - 1, SNAPSHOT_START,
	              SNAPSHOT_START + SNAPSHOT_COUNT - 1);
}

void DeyeEventMonitor::update()
{
	if(!this->controller_->is_in_loop_state())
		return;

	if(this->event_pending_ &&
	   millis() - this->pending_event_.timestamp_ms >= PENDING_EVENT_TIMEOUT_MS)
		this->publish_pending_event_();

	// Queue in reverse order because each priority insertion goes to the front.
	auto event_state = modbus_controller::ModbusCommandItem::create_read_command(
	    this->controller_, modbus::ModbusRegisterType::HOLDING, EVENT_STATE_START,
	    EVENT_STATE_COUNT,
	    [this](modbus::ModbusRegisterType, uint16_t, std::vector<uint8_t> const &data)
	    { this->handle_event_state_(data); });
	this->controller_->queue_priority_command(event_state);

	auto run_state = modbus_controller::ModbusCommandItem::create_read_command(
	    this->controller_, modbus::ModbusRegisterType::HOLDING, RUN_STATE_START, RUN_STATE_COUNT,
	    [this](modbus::ModbusRegisterType, uint16_t, std::vector<uint8_t> const &data)
	    { this->handle_run_state_(data); });
	this->controller_->queue_priority_command(run_state);
}

uint16_t DeyeEventMonitor::word_at_(std::vector<uint8_t> const &data, uint16_t start_address,
                                    uint16_t address)
{
	size_t const offset = static_cast<size_t>(address - start_address) * 2U;
	return (static_cast<uint16_t>(data[offset]) << 8U) | data[offset + 1];
}

void DeyeEventMonitor::handle_run_state_(std::vector<uint8_t> const &data)
{
	if(data.size() != RUN_STATE_COUNT * 2U)
	{
		ESP_LOGW(TAG, "Ignoring register 500 response with %zu bytes; expected %u", data.size(),
		         RUN_STATE_COUNT * 2U);
		return;
	}

	uint16_t changes = 0;
	uint16_t const operating_state = word_at_(data, RUN_STATE_START, RUN_STATE_START);
	if(this->have_run_state_)
	{
		if(operating_state != this->current_.operating_state)
			changes |= CHANGE_OPERATING_STATE;
	}
	else if(operating_state == 3 || operating_state == 4)
	{
		// Standby/self-test/activating are legitimate startup states; alarm and
		// fault are worth preserving even on the first successful poll.
		changes |= CHANGE_ABNORMAL_AT_BOOT;
	}

	this->have_run_state_ = true;
	this->current_.operating_state = operating_state;
	publish_if_changed(this->operating_state_text_sensor_, operating_state_name_(operating_state));
	if(changes != 0)
		this->begin_event_(changes);
}

void DeyeEventMonitor::handle_event_state_(std::vector<uint8_t> const &data)
{
	if(data.size() != EVENT_STATE_COUNT * 2U)
	{
		ESP_LOGW(TAG, "Ignoring register 551 response with %zu bytes; expected %u", data.size(),
		         EVENT_STATE_COUNT * 2U);
		return;
	}

	EventSnapshot next = this->current_;
	next.power_state = word_at_(data, EVENT_STATE_START, 551);
	next.relay_bitmap = word_at_(data, EVENT_STATE_START, 552);
	next.warnings[0] = word_at_(data, EVENT_STATE_START, 553);
	next.warnings[1] = word_at_(data, EVENT_STATE_START, 554);
	for(size_t index = 0; index < next.faults.size(); index++)
		next.faults[index] = word_at_(data, EVENT_STATE_START, 555 + index);
	next.trip_data = word_at_(data, EVENT_STATE_START, 559);

	uint16_t changes = 0;
	bool const first_event_state = !this->have_event_state_;
	if(this->have_event_state_)
	{
		if(next.power_state != this->current_.power_state)
			changes |= CHANGE_POWER_STATE;
		if(next.relay_bitmap != this->current_.relay_bitmap)
			changes |= CHANGE_RELAY;
		if(next.warnings != this->current_.warnings)
			changes |= CHANGE_WARNING;
		if(next.faults != this->current_.faults)
			changes |= CHANGE_FAULT;
		if(next.trip_data != this->current_.trip_data)
			changes |= CHANGE_TRIP_DATA;
	}
	else
	{
		bool const has_warning = next.warnings[0] != 0 || next.warnings[1] != 0;
		bool has_fault = false;
		for(uint16_t word : next.faults)
			has_fault = has_fault || word != 0;
		if(has_warning || has_fault || next.trip_data != 0)
			changes |= CHANGE_ABNORMAL_AT_BOOT;
	}
	this->have_event_state_ = true;
	this->current_ = next;

	bool latch_changed = false;
	for(size_t index = 0; index < this->latched_warnings_.size(); index++)
	{
		uint16_t const combined = this->latched_warnings_[index] | next.warnings[index];
		latch_changed = latch_changed || combined != this->latched_warnings_[index];
		this->latched_warnings_[index] = combined;
	}
	for(size_t index = 0; index < this->latched_faults_.size(); index++)
	{
		uint16_t const combined = this->latched_faults_[index] | next.faults[index];
		latch_changed = latch_changed || combined != this->latched_faults_[index];
		this->latched_faults_[index] = combined;
	}

	if(first_event_state || changes != 0)
		this->publish_live_state_();
	if(first_event_state || latch_changed)
		this->publish_latches_();
	if(changes != 0)
		this->begin_event_(changes);
}

void DeyeEventMonitor::handle_snapshot_(uint32_t sequence, std::vector<uint8_t> const &data)
{
	if(data.size() != SNAPSHOT_COUNT * 2U)
	{
		ESP_LOGW(TAG, "Ignoring event snapshot with %zu bytes; expected %u", data.size(),
		         SNAPSHOT_COUNT * 2U);
		return;
	}
	if(!this->event_pending_ || sequence != this->event_sequence_)
		return;

	auto &event = this->pending_event_;
	event.battery_voltage = word_at_(data, SNAPSHOT_START, 587);
	event.battery_soc = word_at_(data, SNAPSHOT_START, 588);
	event.battery_power = static_cast<int16_t>(word_at_(data, SNAPSHOT_START, 590));
	event.battery_current = static_cast<int16_t>(word_at_(data, SNAPSHOT_START, 591));
	for(size_t phase = 0; phase < 3; phase++)
	{
		event.grid_voltage[phase] = word_at_(data, SNAPSHOT_START, 598 + phase);
		event.inverter_voltage[phase] = word_at_(data, SNAPSHOT_START, 627 + phase);
		event.load_voltage[phase] = word_at_(data, SNAPSHOT_START, 644 + phase);
	}
	event.grid_frequency = word_at_(data, SNAPSHOT_START, 609);
	event.inverter_power = static_cast<int16_t>(word_at_(data, SNAPSHOT_START, 636));
	event.load_power = static_cast<int16_t>(word_at_(data, SNAPSHOT_START, 653));
	event.load_frequency = word_at_(data, SNAPSHOT_START, 655);
	event.telemetry_valid = true;
	this->publish_pending_event_();
}

void DeyeEventMonitor::begin_event_(uint16_t change_mask)
{
	// Do not replace a pending fault with a later recovery, or combine telemetry
	// responses from different transitions. Publish the older event as partial,
	// then give this transition its own sequence and context read.
	if(this->event_pending_)
		this->publish_pending_event_();

	this->event_sequence_++;
	this->pending_event_ = this->current_;
	this->pending_event_.timestamp_ms = millis();
	this->pending_event_.change_mask = change_mask;
	this->event_pending_ = true;
	publish_if_changed(this->event_sequence_sensor_, static_cast<float>(this->event_sequence_));
	this->queue_event_snapshot_();
}

void DeyeEventMonitor::queue_event_snapshot_()
{
	uint32_t const sequence = this->event_sequence_;
	auto snapshot = modbus_controller::ModbusCommandItem::create_read_command(
	    this->controller_, modbus::ModbusRegisterType::HOLDING, SNAPSHOT_START, SNAPSHOT_COUNT,
	    [this, sequence](modbus::ModbusRegisterType, uint16_t, std::vector<uint8_t> const &data)
	    { this->handle_snapshot_(sequence, data); });
	this->controller_->queue_priority_command(snapshot);
}

void DeyeEventMonitor::publish_pending_event_()
{
	std::string const event = format_event_(this->event_sequence_, this->pending_event_);
	ESP_LOGW(TAG, "%s", event.c_str());
	publish_if_changed(this->last_event_text_sensor_, event);
	this->event_pending_ = false;
}

void DeyeEventMonitor::publish_live_state_()
{
	publish_if_changed(this->trip_data_sensor_, this->current_.trip_data);
	publish_if_changed(this->relay_bitmap_sensor_, this->current_.relay_bitmap);
	publish_if_changed(
	    this->current_warnings_text_sensor_,
	    format_bitmap_(this->current_.warnings.data(), this->current_.warnings.size(), 'W'));
	publish_if_changed(
	    this->current_faults_text_sensor_,
	    format_bitmap_(this->current_.faults.data(), this->current_.faults.size(), 'F'));

	bool any_fault = false;
	for(uint16_t word : this->current_.faults)
		any_fault = any_fault || word != 0;
	publish_if_changed(this->any_fault_binary_sensor_, any_fault);
	publish_if_changed(this->inverter_relay_binary_sensor_,
	                   (this->current_.relay_bitmap & 0x0001U) != 0);
	publish_if_changed(this->grid_relay_binary_sensor_,
	                   (this->current_.relay_bitmap & 0x0004U) != 0);
	publish_if_changed(this->generator_relay_binary_sensor_,
	                   (this->current_.relay_bitmap & 0x0008U) != 0);
}

void DeyeEventMonitor::publish_latches_()
{
	publish_if_changed(
	    this->latched_warnings_text_sensor_,
	    format_bitmap_(this->latched_warnings_.data(), this->latched_warnings_.size(), 'W'));
	publish_if_changed(
	    this->latched_faults_text_sensor_,
	    format_bitmap_(this->latched_faults_.data(), this->latched_faults_.size(), 'F'));
}

void DeyeEventMonitor::clear_latches()
{
	this->latched_warnings_.fill(0);
	this->latched_faults_.fill(0);
	this->publish_latches_();
	ESP_LOGI(TAG, "Cleared RAM-only warning and fault latches");
}

std::string DeyeEventMonitor::operating_state_name_(uint16_t state)
{
	char const *name = "Unknown";
	switch(state)
	{
		case 0:
			name = "Standby";
			break;
		case 1:
			name = "Self-test";
			break;
		case 2:
			name = "Normal";
			break;
		case 3:
			name = "Alarm";
			break;
		case 4:
			name = "Fault";
			break;
		case 5:
			name = "Activating";
			break;
	}
	char output[40];
	snprintf(output, sizeof(output), "%s (%u)", name, state);
	return output;
}

std::string DeyeEventMonitor::format_bitmap_(uint16_t const *words, size_t word_count, char prefix)
{
	std::string output;
	size_t code_count = 0;
	for(size_t word_index = 0; word_index < word_count; word_index++)
	{
		for(size_t bit = 0; bit < 16; bit++)
		{
			if((words[word_index] & (1U << bit)) == 0)
				continue;
			if(code_count < 12)
			{
				char code[8];
				snprintf(code, sizeof(code), "%c%02u", prefix,
				         static_cast<unsigned>(word_index * 16 + bit + 1));
				if(!output.empty())
					output += ',';
				output += code;
			}
			code_count++;
		}
	}
	if(code_count == 0)
		output = "OK";
	else if(code_count > 12)
	{
		char remainder[16];
		snprintf(remainder, sizeof(remainder), ",+%u", static_cast<unsigned>(code_count - 12));
		output += remainder;
	}

	output += " raw=";
	for(size_t index = word_count; index-- > 0;)
	{
		char word[8];
		snprintf(word, sizeof(word), "%04X", words[index]);
		output += word;
		if(index != 0)
			output += ':';
	}
	return output;
}

std::string DeyeEventMonitor::format_event_(uint32_t sequence, EventSnapshot const &snapshot)
{
	char output[320];
	snprintf(output, sizeof(output),
	         "#%u t=%ums chg=%03X s=%u p=%04X r=%04X w=%04X:%04X f=%04X:%04X:%04X:%04X d=%04X "
	         "b=%u,%u,%d,%d g=%u,%u,%u,%u i=%u,%u,%u,%d l=%u,%u,%u,%d,%u v=%u",
	         static_cast<unsigned>(sequence), static_cast<unsigned>(snapshot.timestamp_ms),
	         snapshot.change_mask, snapshot.operating_state, snapshot.power_state,
	         snapshot.relay_bitmap, snapshot.warnings[1], snapshot.warnings[0], snapshot.faults[3],
	         snapshot.faults[2], snapshot.faults[1], snapshot.faults[0], snapshot.trip_data,
	         snapshot.battery_voltage, snapshot.battery_soc, snapshot.battery_power,
	         snapshot.battery_current, snapshot.grid_voltage[0], snapshot.grid_voltage[1],
	         snapshot.grid_voltage[2], snapshot.grid_frequency, snapshot.inverter_voltage[0],
	         snapshot.inverter_voltage[1], snapshot.inverter_voltage[2], snapshot.inverter_power,
	         snapshot.load_voltage[0], snapshot.load_voltage[1], snapshot.load_voltage[2],
	         snapshot.load_power, snapshot.load_frequency, snapshot.telemetry_valid ? 1U : 0U);
	return output;
}

} // namespace esphome::deye_event_monitor
