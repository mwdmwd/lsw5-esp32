# SPDX-License-Identifier: GPL-3.0-or-later
import json
import tempfile
import unittest
from pathlib import Path

from main import Inverter
from state import PersistentState


class PersistentStateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.path = self.directory / "state.json"

    def test_round_trip_keeps_identity_settings_and_counters_only(self) -> None:
        PersistentState(serial_number="ABCDEFGHIJ").save(self.path)
        inverter = Inverter(state_path=self.path)
        inverter.write_holding_registers(142, [1])
        inverter.write_mcu2_holding_registers(35, [1, 100])
        inverter.registers.update({534: 12345, 535: 2, 500: 4, 557: 0x2000, 1080: 2, 1081: 0x60})
        inverter.save_state()
        restored = Inverter(state_path=self.path)
        self.assertEqual(restored.serial_number, "ABCDEFGHIJ")
        self.assertEqual(
            restored.read_holding_registers(3, 5), [0x4142, 0x4344, 0x4546, 0x4748, 0x494A]
        )
        self.assertEqual(restored.read_holding_registers(534, 2), [12345, 2])
        self.assertEqual(restored.registers[142], 1)
        self.assertEqual(restored.registers[35], 1)
        self.assertEqual(restored.registers[500], 2)
        self.assertEqual(restored.registers[557], 0)
        self.assertEqual(restored.read_holding_registers(1080, 2), [0, 0])
        self.assertFalse(restored.firmware_upgrade.active)

    def test_invalid_state_is_not_overwritten(self) -> None:
        for changes in (
            {"serial_number": "short"},
            {"registers": {"557": 1}},
        ):
            with self.subTest(changes=changes):
                value = {
                    "schema_version": 1,
                    "serial_number": "1234567890",
                    "registers": {},
                    "mcu2_registers": {},
                }
                self.path.write_text(json.dumps(value | changes))
                original = self.path.read_bytes()
                with self.assertRaises(ValueError):
                    Inverter(state_path=self.path)
                self.assertEqual(self.path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
