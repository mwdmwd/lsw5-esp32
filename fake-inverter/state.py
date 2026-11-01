# SPDX-License-Identifier: GPL-3.0-or-later
"""Persist identity, settings, and counters; never resume a firmware update."""

import json
import os
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path

DEFAULT_SERIAL_NUMBER = "1234567890"
ENERGY_REGISTERS = {534, 535, 537, 538}
TRANSIENT_REGISTERS = {35, 36, 60, 61, 62, 63, 64, 1080, 1081}


def persistent_register(address: int) -> bool:
    return (
        35 <= address <= 65535
        and address not in TRANSIENT_REGISTERS
        and (address < 500 or address >= 700 or address in ENERGY_REGISTERS)
    )


def parse_registers(value: object, *, mcu2: bool = False) -> dict[int, int]:
    if not isinstance(value, dict):
        raise ValueError("register maps must be JSON objects")  # noqa: TRY004 - invalid JSON content
    result = {}
    for key, word in value.items():
        address = int(key)
        if str(address) != key or not 0 <= address <= 65535:
            raise ValueError(f"invalid register address: {key!r}")
        if type(word) is not int or not 0 <= word <= 65535:
            raise ValueError(f"r{address} must be an unsigned 16-bit integer")
        if (mcu2 and address != 35) or (not mcu2 and not persistent_register(address)):
            raise ValueError(f"r{address} is not persistent in this register map")
        if address == 35 and word not in (0, 1):
            raise ValueError("r35 must be 0 or 1")
        result[address] = word
    return result


@dataclass
class PersistentState:
    serial_number: str = DEFAULT_SERIAL_NUMBER
    registers: dict[int, int] = field(default_factory=dict)
    mcu2_registers: dict[int, int] = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> "PersistentState":
        if not path.exists():
            return cls()
        value = json.loads(path.read_text())
        fields = {"schema_version", "serial_number", "registers", "mcu2_registers"}
        if not isinstance(value, dict) or set(value) != fields:
            raise ValueError(f"state must contain exactly: {', '.join(sorted(fields))}")
        if type(value["schema_version"]) is not int or value["schema_version"] != 1:
            raise ValueError("unsupported state schema_version (expected 1)")
        serial_number = value["serial_number"]
        if (
            not isinstance(serial_number, str)
            or len(serial_number) != 10
            or not all(0x21 <= ord(character) <= 0x7E for character in serial_number)
        ):
            raise ValueError("serial_number must contain exactly 10 printable ASCII characters")
        return cls(
            serial_number,
            parse_registers(value["registers"]),
            parse_registers(value["mcu2_registers"], mcu2=True),
        )

    def save(self, path: Path) -> None:
        """Replace atomically so a stopped process cannot leave half a JSON file."""
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
        try:
            with os.fdopen(descriptor, "w") as stream:
                json.dump({"schema_version": 1, **asdict(self)}, stream, indent=2, sort_keys=True)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            Path(temporary).unlink(missing_ok=True)
