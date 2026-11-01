# SPDX-License-Identifier: GPL-3.0-or-later
import unittest

from main import (
    EPS_EVENT_F46_MASK,
    EPS_EVENT_TRIP_DETAIL,
    FAULT_WORD_33_48_ADDRESS,
    RELAY_BITMAP_ADDRESS,
    RUN_STATE_ADDRESS,
    TRIP_DETAIL_ADDRESS,
    Inverter,
)


class EpsEventProfileTest(unittest.TestCase):
    def setUp(self) -> None:
        self.inverter = Inverter(
            eps_event_interval_seconds=10.0,
            eps_event_duration_seconds=2.0,
        )

    def test_transient_event_and_recovery(self) -> None:
        self.inverter.update(now=0.0)
        self.assertEqual(self.inverter.registers[RUN_STATE_ADDRESS], 2)
        self.assertEqual(self.inverter.registers[RELAY_BITMAP_ADDRESS], 0x0005)

        self.inverter.update(now=10.0)
        self.assertEqual(self.inverter.registers[RUN_STATE_ADDRESS], 2)
        self.assertEqual(self.inverter.registers[RELAY_BITMAP_ADDRESS], 0)
        self.assertEqual(self.inverter.registers[FAULT_WORD_33_48_ADDRESS], EPS_EVENT_F46_MASK)
        self.assertEqual(self.inverter.registers[TRIP_DETAIL_ADDRESS], EPS_EVENT_TRIP_DETAIL)
        self.assertEqual(self.inverter.registers[598], 1200)
        self.assertEqual(self.inverter.registers[644], 0)

        event_words = self.inverter.read_holding_registers(551, 9)
        self.assertEqual(event_words[1], 0)
        self.assertEqual(event_words[6], EPS_EVENT_F46_MASK)
        self.assertEqual(event_words[8], EPS_EVENT_TRIP_DETAIL)

        self.inverter.update(now=12.0)
        self.assertEqual(self.inverter.registers[RELAY_BITMAP_ADDRESS], 0x0005)
        self.assertEqual(self.inverter.registers[FAULT_WORD_33_48_ADDRESS], 0)
        self.assertEqual(self.inverter.registers[TRIP_DETAIL_ADDRESS], 0)
        self.assertGreaterEqual(self.inverter.registers[598], 2200)
        self.assertEqual(self.inverter.registers[644], 2300)


if __name__ == "__main__":
    unittest.main()
