# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact kWh formatting and a retained, monotonic PV filter."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

energy_accounting_ns = cg.esphome_ns.namespace("energy_accounting")
PvProduction = energy_accounting_ns.class_("PvProduction")

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(PvProduction)})


async def to_code(config):
    # The filter is called by YAML callbacks; it has no setup or loop work.
    cg.new_Pvariable(config[CONF_ID])
