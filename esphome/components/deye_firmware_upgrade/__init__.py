# SPDX-License-Identifier: GPL-3.0-or-later

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import modbus, modbus_controller, uart
from esphome.components.esp32 import add_idf_component
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = [
    "esp32",
    "psram",
    "uart",
    "modbus",
    "modbus_controller",
    "web_server_base",
]
AUTO_LOAD = ["sha256"]
MULTI_CONF = False

CONF_READBACK_FLASH_SIZE = "readback_flash_size"
CONF_READBACK_UNIT_ADDRESS = "readback_unit_address"
CONF_MAX_FIRMWARE_SIZE = "max_firmware_size"
CONF_EVENT_MONITOR_ID = "event_monitor_id"
CONF_MODBUS_ID = "modbus_id"
CONF_MODBUS_CONTROLLER_ID = "modbus_controller_id"

deye_firmware_upgrade_ns = cg.esphome_ns.namespace("deye_firmware_upgrade")
DeyeFirmwareUpgrade = deye_firmware_upgrade_ns.class_(
    "DeyeFirmwareUpgrade", cg.Component, uart.UARTDevice
)


CONFIG_SCHEMA = cv.All(
    cv.only_with_framework("esp-idf"),
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DeyeFirmwareUpgrade),
            cv.Required(CONF_MODBUS_ID): cv.use_id(modbus.Modbus),
            cv.Required(CONF_MODBUS_CONTROLLER_ID): cv.use_id(modbus_controller.ModbusController),
            cv.Optional(CONF_READBACK_FLASH_SIZE, default="524288 B"): cv.All(
                cv.validate_bytes, cv.Range(min=65536, max=3145728)
            ),
            cv.Optional(CONF_READBACK_UNIT_ADDRESS, default=1): cv.int_range(min=1, max=247),
            cv.Optional(CONF_EVENT_MONITOR_ID): cv.use_id(cg.PollingComponent),
            cv.Optional(CONF_MAX_FIRMWARE_SIZE, default="2097152 B"): cv.All(
                cv.validate_bytes, cv.Range(min=1024)
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
)


FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "deye_firmware_upgrade",
    baud_rate=9600,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    modbus_var = await cg.get_variable(config[CONF_MODBUS_ID])
    controller_var = await cg.get_variable(config[CONF_MODBUS_CONTROLLER_ID])
    cg.add(var.set_modbus(modbus_var))
    cg.add(var.set_modbus_controller(controller_var))
    if event_monitor_config := config.get(CONF_EVENT_MONITOR_ID):
        event_monitor = await cg.get_variable(event_monitor_config)
        cg.add(var.set_event_monitor(event_monitor))
    cg.add(var.set_readback_unit_address(config[CONF_READBACK_UNIT_ADDRESS]))
    cg.add(var.set_readback_flash_size(config[CONF_READBACK_FLASH_SIZE]))
    cg.add(var.set_max_firmware_size(config[CONF_MAX_FIRMWARE_SIZE]))

    # Required for multipart uploads in ESPHome's ESP-IDF web server.
    # Reuse the OTA parser; uploaded bytes stay in PSRAM.
    cg.add_define("USE_WEBSERVER_OTA")
    add_idf_component(name="zorxx/multipart-parser", ref="1.0.1")
