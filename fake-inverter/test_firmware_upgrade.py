# SPDX-License-Identifier: GPL-3.0-or-later
import tempfile
import time
import unittest
import zlib
from pathlib import Path

from crc import crc16_modbus, crc16_xmodem
from firmware_upgrade import (
    XMODEM_ACK,
    XMODEM_BLOCK_SIZE,
    XMODEM_EOT,
    XMODEM_NAK,
    XMODEM_STX,
    FirmwareUpgrade,
    UpgradePhase,
    inspect_firmware_image,
)
from main import Inverter, process_buffer
from modbus import FunctionCode
from test_support import FakePort, build_request


def with_modbus_crc(data: bytes) -> bytes:
    return data + crc16_modbus(data).to_bytes(2, "little")


def xmodem_packet(block_number: int, payload: bytes) -> bytes:
    padded = payload.ljust(XMODEM_BLOCK_SIZE, b"\x1a")
    return (
        bytes((XMODEM_STX, block_number, block_number ^ 0xFF))
        + padded
        + crc16_xmodem(padded).to_bytes(2, "big")
    )


def deye_image(payload: bytes, selectors: bytes = b"\x01\xff\xff") -> bytes:
    return (
        payload
        + selectors
        + len(payload).to_bytes(4, "big")
        + (zlib.crc32(payload) & 0xFFFFFFFF).to_bytes(4, "big")
    )


class FirmwareUpgradeTest(unittest.TestCase):
    def test_image_validation(self) -> None:
        self.assertEqual(crc16_xmodem(b"123456789"), 0x31C3)
        payload = b"A" * (XMODEM_BLOCK_SIZE - 11)
        info = inspect_firmware_image(deye_image(payload))
        self.assertIsNotNone(info)
        self.assertEqual(info.payload_size, len(payload))
        unaligned = deye_image(b"firmware payload")
        for invalid in (unaligned, unaligned.ljust(1024, b"\x1a"), unaligned.ljust(1024, b"\xff")):
            with self.subTest(length=len(invalid)):
                self.assertIsNone(inspect_firmware_image(invalid))

    def test_activation_ack_precedes_delayed_crc_request(self) -> None:
        inverter = Inverter()
        port = FakePort()
        request = with_modbus_crc(b"\x01\x10\x04\x38\x00\x02\x04\x00\x01\x00\x60")

        self.assertEqual(process_buffer(inverter, port, request), b"")
        self.assertEqual(port.writes, [with_modbus_crc(b"\x01\x10\x04\x38\x00\x02")])
        self.assertEqual(inverter.firmware_upgrade.phase, UpgradePhase.READY_PENDING)

        inverter.service_firmware_upgrade(port, time.monotonic() + 1)
        self.assertEqual(port.writes[-1], b"C")
        self.assertEqual(inverter.firmware_upgrade.phase, UpgradePhase.RECEIVING)

    def test_resynchronizes_after_uart_garbage(self) -> None:
        for prefix, function in ((b"\x00" * 300, 3), (b"\xff", 4)):
            with self.subTest(function=function):
                inverter, port = Inverter(), FakePort()
                request = with_modbus_crc(bytes((1, function, 0, 0, 0, 1)))
                self.assertEqual(process_buffer(inverter, port, prefix + request), b"")
                self.assertEqual(port.writes, [with_modbus_crc(bytes((1, function, 2, 0, 5)))])

    def test_partial_write_with_embedded_read_is_not_resynchronized(self) -> None:
        read = build_request(FunctionCode.ReadHoldingRegisters, 0, {"nr_registers": 1})
        values = [int.from_bytes(read[i : i + 2], "big") for i in range(0, len(read), 2)]
        write = build_request(FunctionCode.PresetMultipleRegisters, 100, {"data": values})
        inverter, port = Inverter(), FakePort()
        remaining = process_buffer(inverter, port, write[:-2])
        self.assertEqual(remaining, write[:-2])
        self.assertEqual(port.writes, [])
        self.assertEqual(process_buffer(inverter, port, remaining + write[-2:]), b"")
        self.assertEqual(inverter.read_holding_registers(100, len(values)), values)
        self.assertEqual(len(port.writes), 1)

    def test_interrupted_capture_keeps_only_accepted_blocks(self) -> None:
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        session = FirmwareUpgrade(capture_directory=directory)
        session.begin(1, now=0)
        session.poll_ready(now=1)
        packet = xmodem_packet(1, b"A" * XMODEM_BLOCK_SIZE)
        session.consume(packet + packet + b"\x18\x18")
        partial = next(directory.glob("*.partial.bin"))
        self.assertEqual(partial.read_bytes(), b"A" * XMODEM_BLOCK_SIZE)
        self.assertFalse(session.active)
        session.begin(2, now=2)
        self.assertNotEqual(session.capture_path, partial)
        self.assertEqual(partial.read_bytes(), b"A" * XMODEM_BLOCK_SIZE)

    def test_valid_duplicate_corrupt_and_eot(self) -> None:
        image = deye_image(b"A" * (XMODEM_BLOCK_SIZE - 11))
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        session = FirmwareUpgrade(capture_directory=directory)
        session.begin(1, now=0)
        self.assertEqual(session.poll_ready(now=1), b"C")
        packet = xmodem_packet(1, image)

        remaining, replies = session.consume(packet)
        self.assertEqual(remaining, b"")
        self.assertEqual(replies, [bytes((XMODEM_ACK,))])
        self.assertEqual(session.blocks_received, 1)
        self.assertEqual(next(directory.glob("*.partial.bin")).read_bytes(), image)

        _, replies = session.consume(packet)
        self.assertEqual(replies, [bytes((XMODEM_ACK,))])
        self.assertEqual(session.blocks_received, 1)

        corrupt = bytearray(xmodem_packet(2, b"B"))
        corrupt[100] ^= 1
        _, replies = session.consume(bytes(corrupt))
        self.assertEqual(replies, [bytes((XMODEM_NAK,))])

        remaining, replies = session.consume(bytes((XMODEM_EOT,)))
        self.assertEqual(remaining, b"")
        self.assertEqual(replies, [])
        self.assertEqual(session.phase, UpgradePhase.COMPLETE)
        self.assertEqual(session.image, image)
        self.assertIsNotNone(session.image_info)
        self.assertFalse(list(directory.glob("*.partial.bin")))
        self.assertEqual(next(directory.iterdir()).read_bytes(), image)

    def test_dsp_eot_reports_staging_result(self) -> None:
        for valid in (True, False):
            with self.subTest(valid=valid):
                image = bytearray(deye_image(b"A" * (XMODEM_BLOCK_SIZE - 11)))
                if not valid:
                    image[-1] ^= 1
                session = FirmwareUpgrade()
                session.begin(2, now=0)
                session.poll_ready(now=1)
                _, replies = session.consume(xmodem_packet(1, image) + bytes((XMODEM_EOT,)))
                self.assertEqual(
                    replies, [bytes((XMODEM_ACK,)), bytes((XMODEM_ACK if valid else XMODEM_NAK,))]
                )
                self.assertEqual(session.image_info is not None, valid)

    def test_block_number_wrap_and_modbus_fallback(self) -> None:
        session = FirmwareUpgrade()
        session.begin(2, now=0)
        session.poll_ready(now=1)
        session.expected_block = 0xFF

        for block_number in (0xFF, 0x00):
            _, replies = session.consume(xmodem_packet(block_number, b"data"))
            self.assertEqual(replies, [bytes((XMODEM_ACK,))])

        modbus_request = with_modbus_crc(b"\x01\x03\x00\x00\x00\x01")
        remaining, replies = session.consume(modbus_request)
        self.assertEqual(remaining, modbus_request)
        self.assertEqual(replies, [])
        self.assertEqual(session.phase, UpgradePhase.IDLE)


if __name__ == "__main__":
    unittest.main()
