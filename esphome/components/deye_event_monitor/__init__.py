# SPDX-License-Identifier: GPL-3.0-or-later

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from esphome.components import (
    binary_sensor,
    priority_modbus_controller,
    sensor,
    text_sensor,
)

CODEOWNERS = []
DEPENDENCIES = ["priority_modbus_controller"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor"]
MULTI_CONF = False

CONF_MODBUS_CONTROLLER_ID = "modbus_controller_id"
CONF_EVENT_SEQUENCE = "event_sequence"
CONF_TRIP_DATA = "trip_data"
CONF_RELAY_BITMAP = "relay_bitmap"
CONF_OPERATING_STATE = "operating_state"
CONF_CURRENT_WARNINGS = "current_warnings"
CONF_CURRENT_FAULTS = "current_faults"
CONF_LATCHED_WARNINGS = "latched_warnings"
CONF_LATCHED_FAULTS = "latched_faults"
CONF_LAST_EVENT = "last_event"
CONF_ANY_FAULT = "any_fault"
CONF_INVERTER_RELAY = "inverter_relay"
CONF_GRID_RELAY = "grid_relay"
CONF_GENERATOR_RELAY = "generator_relay"

deye_event_monitor_ns = cg.esphome_ns.namespace("deye_event_monitor")
DeyeEventMonitor = deye_event_monitor_ns.class_("DeyeEventMonitor", cg.PollingComponent)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DeyeEventMonitor),
        cv.Required(CONF_MODBUS_CONTROLLER_ID): cv.use_id(
            priority_modbus_controller.PriorityModbusController
        ),
        cv.Optional(CONF_EVENT_SEQUENCE): sensor.sensor_schema(
            icon="mdi:counter",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_TRIP_DATA): sensor.sensor_schema(
            icon="mdi:flash-alert",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_RELAY_BITMAP): sensor.sensor_schema(
            icon="mdi:electric-switch",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_OPERATING_STATE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CURRENT_WARNINGS): text_sensor.text_sensor_schema(
            icon="mdi:alert",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CURRENT_FAULTS): text_sensor.text_sensor_schema(
            icon="mdi:alert",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LATCHED_WARNINGS): text_sensor.text_sensor_schema(
            icon="mdi:alert-decagram-outline",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LATCHED_FAULTS): text_sensor.text_sensor_schema(
            icon="mdi:alert-decagram",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LAST_EVENT): text_sensor.text_sensor_schema(
            icon="mdi:timeline-alert",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_ANY_FAULT): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_INVERTER_RELAY): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_GRID_RELAY): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_GENERATOR_RELAY): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
).extend(cv.polling_component_schema("1s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    controller = await cg.get_variable(config[CONF_MODBUS_CONTROLLER_ID])
    cg.add(var.set_modbus_controller(controller))

    for key in (CONF_EVENT_SEQUENCE, CONF_TRIP_DATA, CONF_RELAY_BITMAP):
        if sensor_config := config.get(key):
            output = await sensor.new_sensor(sensor_config)
            cg.add(getattr(var, f"set_{key}_sensor")(output))

    for key in (
        CONF_OPERATING_STATE,
        CONF_CURRENT_WARNINGS,
        CONF_CURRENT_FAULTS,
        CONF_LATCHED_WARNINGS,
        CONF_LATCHED_FAULTS,
        CONF_LAST_EVENT,
    ):
        if text_sensor_config := config.get(key):
            output = await text_sensor.new_text_sensor(text_sensor_config)
            cg.add(getattr(var, f"set_{key}_text_sensor")(output))

    for key in (
        CONF_ANY_FAULT,
        CONF_INVERTER_RELAY,
        CONF_GRID_RELAY,
        CONF_GENERATOR_RELAY,
    ):
        if binary_sensor_config := config.get(key):
            output = await binary_sensor.new_binary_sensor(binary_sensor_config)
            cg.add(getattr(var, f"set_{key}_binary_sensor")(output))
