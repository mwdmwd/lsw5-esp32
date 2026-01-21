# SPDX-License-Identifier: AGPL-3.0-or-later
import enum

from construct import (
    Checksum,
    Enum,
    GreedyRange,
    Int16ub,
    Int16ul,
    Int8ub,
    len_,
    Prefixed,
    RawCopy,
    Rebuild,
    Struct,
    Switch,
    this,
)

from crc import crc16_modbus


# pyright: reportOperatorIssue=false, reportIndexIssue=false


class FunctionCode(enum.IntEnum):
    ReadCoilStatus = 0x01
    ReadInputStatus = 0x02
    ReadHoldingRegisters = 0x03
    ReadInputRegisters = 0x04

    PresetSingleRegister = 0x06
    PresetMultipleRegisters = 0x10


RequestFrame = Struct(
    "data"
    / RawCopy(
        Struct(
            "slave_addr" / Int8ub,
            "function" / Enum(Int8ub, FunctionCode),
            "address" / Int16ub,
            "content"
            / Switch(
                lambda this: int(this.function),
                {
                    FunctionCode.ReadInputRegisters: Struct(
                        "nr_registers" / Int16ub,
                    ),
                    FunctionCode.ReadHoldingRegisters: Struct(
                        "nr_registers" / Int16ub,
                    ),
                    FunctionCode.PresetSingleRegister: Struct(
                        "data" / Int16ub,
                    ),
                    FunctionCode.PresetMultipleRegisters: Struct(
                        "nr_registers" / Rebuild(Int16ub, len_(this.data)),
                        "data" / Prefixed(Int8ub, Int16ub[this.nr_registers]),
                    ),
                },
                default=Struct(),
            ),
        ),
    ),
    "crc" / Checksum(Int16ul, crc16_modbus, this.data.data),
)

ResponseFrame = Struct(
    "data"
    / RawCopy(
        Struct(
            "slave_addr" / Int8ub,
            "function" / Enum(Int8ub, FunctionCode),
            "content"
            / Switch(
                lambda this: int(this.function),
                {
                    FunctionCode.PresetSingleRegister: Struct(
                        "address" / Int16ub,
                        "data" / Int16ub,
                    ),
                    FunctionCode.PresetMultipleRegisters: Struct(
                        "address" / Int16ub,
                        "nr_registers" / Int16ub,
                    ),
                },
                default=Prefixed(
                    Int8ub,
                    "data" / GreedyRange(Int8ub),
                ),
            ),
        )
    ),
    "crc" / Checksum(Int16ul, crc16_modbus, this.data.data),
)
