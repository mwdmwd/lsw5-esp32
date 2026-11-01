# SPDX-License-Identifier: GPL-3.0-or-later

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

from esphome.components import modbus, modbus_controller

CODEOWNERS = []
DEPENDENCIES = ["modbus"]
AUTO_LOAD = ["modbus_controller"]
MULTI_CONF = True

priority_modbus_controller_ns = cg.esphome_ns.namespace("priority_modbus_controller")
PriorityModbusController = priority_modbus_controller_ns.class_(
    "PriorityModbusController", modbus_controller.ModbusController
)


def _use_priority_controller(config):
    config[CONF_ID].type = PriorityModbusController
    return config


CONFIG_SCHEMA = cv.All(modbus_controller.CONFIG_SCHEMA, _use_priority_controller)

FINAL_VALIDATE_SCHEMA = modbus.final_validate_modbus_device(
    "priority_modbus_controller", role="client"
)


async def to_code(config):
    # The declared ID supplies the subclass; configuration is otherwise stock.
    await modbus_controller.to_code(config)
