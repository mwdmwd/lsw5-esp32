// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esphome/components/modbus_controller/modbus_controller.h"

namespace esphome::priority_modbus_controller
{

class PriorityModbusController : public modbus_controller::ModbusController
{
public:
	// Put urgent work immediately behind an active transaction. If an equivalent
	// unsent command already exists, refresh and promote it instead of warning.
	void queue_priority_command(modbus_controller::ModbusCommandItem const &command);
};

} // namespace esphome::priority_modbus_controller
