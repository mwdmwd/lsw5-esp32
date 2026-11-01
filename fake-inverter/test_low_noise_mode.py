# SPDX-License-Identifier: GPL-3.0-or-later
import unittest

from main import Inverter, process_buffer
from modbus import FunctionCode, ResponseFrame
from test_support import FakePort, build_request


class LowNoiseModeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.inverter = Inverter()
        self.port = FakePort()

    def request(self, function: FunctionCode, address: int, content: dict, slave: int = 1):
        request = build_request(function, address, content, slave)
        self.assertEqual(process_buffer(self.inverter, self.port, request), b"")
        return request

    def test_logger_fc16_is_acknowledged_but_does_not_reach_mcu2_intact(self) -> None:
        self.request(FunctionCode.PresetMultipleRegisters, 35, {"data": [1, 100]})
        response = ResponseFrame.parse(self.port.writes[-1]).data.value

        self.assertEqual(int(response.function), FunctionCode.PresetMultipleRegisters)
        self.assertEqual(response.content.address, 35)
        self.assertEqual(response.content.nr_registers, 2)
        self.assertEqual(self.inverter.read_holding_registers(35, 2), [0, 0])

    def test_raw_aa_fc16_changes_mcu2_and_echoes_without_updating_hmi_cache(self) -> None:
        for value, wire in ((1, "aa10002300020400010064c6af"), (0, "aa10002300020400000064976f")):
            with self.subTest(value=value):
                request = self.request(
                    FunctionCode.PresetMultipleRegisters, 35, {"data": [value, 100]}, slave=0xAA
                )
                self.assertEqual(request.hex(), wire)
                self.assertEqual(self.port.writes[-1], request)
                self.assertEqual(self.inverter.mcu2_registers[35], value)
                self.assertEqual(self.inverter.read_holding_registers(35, 2), [0, 0])

    def test_raw_aa_fc16_requires_guard_value_and_exact_range(self) -> None:
        self.request(FunctionCode.PresetMultipleRegisters, 35, {"data": [1, 99]}, slave=0xAA)
        self.request(FunctionCode.PresetMultipleRegisters, 34, {"data": [0, 1, 100]}, slave=0xAA)

        self.assertEqual(self.inverter.mcu2_registers[35], 0)
        self.assertEqual(self.inverter.read_holding_registers(35, 2), [0, 0])

    def test_a2_probe_is_ignored_before_inverter_request(self) -> None:
        probe = build_request(FunctionCode.ReadHoldingRegisters, 0, {"nr_registers": 65}, 0xA2)
        request = build_request(FunctionCode.ReadHoldingRegisters, 0, {"nr_registers": 1})
        self.assertEqual(process_buffer(self.inverter, self.port, probe + request), b"")

        self.assertEqual(len(self.port.writes), 1)
        response = ResponseFrame.parse(self.port.writes[0]).data.value
        self.assertEqual(response.slave_addr, 1)
        self.assertEqual(list(response.content), [0x00, 0x05])


if __name__ == "__main__":
    unittest.main()
