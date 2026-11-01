// SPDX-License-Identifier: GPL-3.0-or-later
#include "priority_modbus_controller.h"

namespace esphome::priority_modbus_controller
{

void PriorityModbusController::queue_priority_command(
    modbus_controller::ModbusCommandItem const &command)
{
	// Readiness describes the RTU transport, not the lifetime of the command.
	// After a timeout it can become true while the front still owns retries.
	// should_retry(0) is true only before the first send; never reorder or
	// replace a sent front until the controller removes it.
	auto priority_position = this->command_queue_.begin();
	if(priority_position != this->command_queue_.end() &&
	   (!this->ready_for_immediate_send() || !(*priority_position)->should_retry(0)))
		++priority_position;

	// Search only commands that are safe to reorder. This also keeps at most one
	// queued successor when the equivalent command at the front is in flight.
	for(auto item = priority_position; item != this->command_queue_.end(); ++item)
	{
		if(!(*item)->is_equal(command))
			continue;

		(*item)->payload = command.payload;
		(*item)->on_data_func = command.on_data_func;
		if(item != priority_position)
			this->command_queue_.splice(priority_position, this->command_queue_, item);
		return;
	}

	this->command_queue_.insert(priority_position,
	                            make_unique<modbus_controller::ModbusCommandItem>(command));
}

} // namespace esphome::priority_modbus_controller
