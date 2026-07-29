#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
import argparse
import logging
import random
import sys
import time
from collections.abc import Iterable
from dataclasses import dataclass, field

import serial
from construct import ConstructError

from modbus import FunctionCode, RequestFrame, ResponseFrame

LOGGER = logging.getLogger(__name__)
LOGGER_FORMAT = "%(asctime)s - %(levelname)s - %(message)s"

DEFAULT_BAUDRATE = 9600
SERIAL_TIMEOUT_SECONDS = 0.1
STATE_UPDATE_INTERVAL_SECONDS = 1.0
IDLE_SLEEP_SECONDS = 0.01
MAX_MODBUS_BUFFER_SIZE = 256
LOGGER_PREFIX = b"\xc2"
PROFILE_RANDOM = "random"
PROFILE_GENERATOR_FIRST = "generator-first"
ENERGY_TOTAL_ADDRESS = 534
ENERGY_GENERATOR_ADDRESS = 537


def clamp(value: int, minimum: int, maximum: int) -> int:
    return max(minimum, min(maximum, value))


def to_u16(value: int) -> int:
    return value & 0xFFFF


def from_s16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value >= 0x8000 else value


def words_to_bytes(registers: Iterable[int]) -> list[int]:
    data: list[int] = []
    for register in registers:
        value = to_u16(register)
        data.extend((value >> 8, value & 0xFF))
    return data


def set_u32(registers: dict[int, int], low_word_address: int, value: int) -> None:
    registers[low_word_address] = value & 0xFFFF
    registers[low_word_address + 1] = (value >> 16) & 0xFFFF


def get_u32(registers: dict[int, int], low_word_address: int) -> int:
    return (registers.get(low_word_address + 1, 0) << 16) | registers.get(low_word_address, 0)


@dataclass
class Inverter:
    profile: str = PROFILE_RANDOM
    event_interval_seconds: float = 120.0
    catchup_delay_seconds: float = 600.0
    log_energy_reads: bool = False
    registers: dict[int, int] = field(default_factory=dict)
    next_event_at: float | None = None
    pending_total_catchups: list[float] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.reset_registers()

    def reset_registers(self) -> None:
        self.registers = {
            0: 0x500,  # 3-phase hybrid
            1: 1,  # Modbus address
            2: 0x0102,  # Protocol version
            3: 0x3132,  # Serial number
            4: 0x3334,
            5: 0x3536,
            6: 0x3738,
            7: 0x3930,
            8: 12_000,  # Rated power
            10: 0x12,  # MCU board version
            11: 0x1234,  # Bootloader and assistant version
            12: 0x2345,  # AFCI version
            13: 0x3456,  # Slave MCU version
            14: 0x4567,  # HMI version-2
            15: 0x5678,  # HMI version
            598: 2300,  # Grid L1 voltage, 0.1 V
            599: 2300,  # Grid L2 voltage, 0.1 V
            600: 2300,  # Grid L3 voltage, 0.1 V
            609: 5000,  # Grid frequency, 0.01 Hz
            610: 200,  # Internal CT L1 current, 0.01 A
            611: 200,
            612: 200,
            672: 1500,  # PV1 power, W
            673: 1500,  # PV2 power, W
            676: 3000,  # PV1 voltage, 0.1 V
            678: 3000,  # PV2 voltage, 0.1 V
            677: 50,  # PV1 current, 0.1 A
            679: 50,  # PV2 current, 0.1 A
            540: 40 * 10 + 1000,  # DC temperature
            541: 35 * 10 + 1000,  # AC temperature
            586: 25 * 10 + 1000,  # Battery temperature
            587: 5120,  # Battery voltage, 0.01 V
            588: 80,  # Battery SoC, %
            590: to_u16(-500),  # Battery power, W
            591: to_u16(-1000),  # Battery current, 0.01 A
            604: 200,  # Internal CT power L1, W
            605: 200,
            606: 200,
            607: 600,
            640: 500,  # UPS L1, W
            641: 500,
            642: 500,
            643: 1500,
            650: 500,  # Load L1, W
            651: 500,
            652: 500,
            653: 1500,
        }
        set_u32(self.registers, ENERGY_TOTAL_ADDRESS, 10_000)  # Total PV production, 0.1 kWh
        set_u32(
            self.registers, ENERGY_GENERATOR_ADDRESS, 10_000
        )  # Total generator production, 0.1 kWh

    def update(self, now: float | None = None) -> None:
        # Grid voltages
        for register in (598, 599, 600):
            self.registers[register] = clamp(
                self.registers[register] + random.randint(-5, 5), 2200, 2400
            )

        # Grid frequency
        self.registers[609] = clamp(self.registers[609] + random.randint(-2, 2), 4980, 5020)

        # PV power, voltage and current
        for power_register, voltage_register, current_register in (
            (672, 676, 677),
            (673, 678, 679),
        ):
            self.registers[power_register] = max(
                0, self.registers[power_register] + random.randint(-10, 10)
            )
            self.registers[voltage_register] = max(
                0, self.registers[voltage_register] + random.randint(-5, 5)
            )

            voltage = self.registers[voltage_register]
            if voltage > 0:
                self.registers[current_register] = int(
                    100 * self.registers[power_register] / voltage
                )

        # Battery power
        battery_power = clamp(from_s16(self.registers[590]) + random.randint(-20, 20), -2000, 2000)
        self.registers[590] = to_u16(battery_power)

        battery_voltage = self.registers[587] / 100.0
        if battery_voltage > 0:
            # Battery current
            self.registers[591] = to_u16(int(battery_power * 100 / battery_voltage))

        if self.profile == PROFILE_RANDOM:
            self.update_random_energy()
        elif self.profile == PROFILE_GENERATOR_FIRST:
            self.update_generator_first_energy(time.monotonic() if now is None else now)
        else:
            raise ValueError(f"Unsupported inverter profile: {self.profile}")

    def update_random_energy(self) -> None:
        # Total PV production
        if random.random() > 0.9:
            set_u32(
                self.registers,
                ENERGY_TOTAL_ADDRESS,
                get_u32(self.registers, ENERGY_TOTAL_ADDRESS) + 1,
            )

        # Total generator production
        if random.random() > 0.95:
            set_u32(
                self.registers,
                ENERGY_GENERATOR_ADDRESS,
                get_u32(self.registers, ENERGY_GENERATOR_ADDRESS) + 1,
            )

    def update_generator_first_energy(self, now: float) -> None:
        if self.next_event_at is None:
            self.next_event_at = now + self.event_interval_seconds

        while now >= self.next_event_at:
            generator_raw = get_u32(self.registers, ENERGY_GENERATOR_ADDRESS) + 1
            set_u32(self.registers, ENERGY_GENERATOR_ADDRESS, generator_raw)
            self.pending_total_catchups.append(self.next_event_at + self.catchup_delay_seconds)
            LOGGER.info(
                "Profile event: generator_raw=%d total_raw=%d catchup_due_in=%.1fs",
                generator_raw,
                get_u32(self.registers, ENERGY_TOTAL_ADDRESS),
                self.catchup_delay_seconds,
            )
            self.next_event_at += self.event_interval_seconds

        due_count = sum(1 for due_at in self.pending_total_catchups if due_at <= now)
        if due_count:
            total_raw = get_u32(self.registers, ENERGY_TOTAL_ADDRESS) + due_count
            set_u32(self.registers, ENERGY_TOTAL_ADDRESS, total_raw)
            self.pending_total_catchups = [
                due_at for due_at in self.pending_total_catchups if due_at > now
            ]
            LOGGER.info(
                "Profile catchup: total_raw=%d generator_raw=%d corrected_raw=%d",
                total_raw,
                get_u32(self.registers, ENERGY_GENERATOR_ADDRESS),
                total_raw - get_u32(self.registers, ENERGY_GENERATOR_ADDRESS),
            )

    def read_holding_registers(self, start_address: int, count: int) -> list[int]:
        if (
            self.log_energy_reads
            and start_address < ENERGY_GENERATOR_ADDRESS + 2
            and start_address + count > ENERGY_TOTAL_ADDRESS
        ):
            total_raw = get_u32(self.registers, ENERGY_TOTAL_ADDRESS)
            generator_raw = get_u32(self.registers, ENERGY_GENERATOR_ADDRESS)
            LOGGER.info(
                "Energy read: addr=%d count=%d total_raw=%d generator_raw=%d corrected_raw=%d",
                start_address,
                count,
                total_raw,
                generator_raw,
                total_raw - generator_raw,
            )
        return [self.registers.get(start_address + offset, 0) for offset in range(count)]

    def write_holding_registers(self, start_address: int, values: Iterable[int]) -> None:
        for offset, value in enumerate(values):
            self.registers[start_address + offset] = to_u16(value)


def build_response(inverter: Inverter, request) -> bytes | None:
    function_code = request.function
    response_data = {
        "function": function_code,
        "slave_addr": request.slave_addr,
    }

    if int(function_code) == FunctionCode.ReadHoldingRegisters:
        registers = inverter.read_holding_registers(request.address, request.content.nr_registers)
        response_data["content"] = words_to_bytes(registers)
    elif int(function_code) == FunctionCode.PresetMultipleRegisters:
        inverter.write_holding_registers(request.address, request.content.data)
        response_data["content"] = {
            "address": request.address,
            "nr_registers": request.content.nr_registers,
        }
    elif int(function_code) == FunctionCode.PresetSingleRegister:
        inverter.write_holding_registers(request.address, [request.content.data])
        response_data["content"] = {
            "address": request.address,
            "data": request.content.data,
        }
    else:
        LOGGER.warning("Function %s is not implemented: %s", function_code, request)
        return None

    return ResponseFrame.build({"data": {"value": response_data}})


def parse_request(buffer: bytes):
    parsed = RequestFrame.parse(buffer)
    assert parsed is not None, "Parsing should have raised ConstructError if it failed"
    frame_size = parsed.data.length + 2
    return parsed.data.value, frame_size


def find_modbus_frame_offset(buffer: bytes) -> int | None:
    """Find a CRC-valid request after UART noise without discarding it bytewise."""
    for offset in range(1, len(buffer)):
        if buffer[offset] not in (0x01, 0xA2):
            continue
        if offset + 1 >= len(buffer) or buffer[offset + 1] not in (
            FunctionCode.ReadHoldingRegisters,
            FunctionCode.PresetSingleRegister,
            FunctionCode.PresetMultipleRegisters,
        ):
            continue
        try:
            parse_request(buffer[offset:])
        except ConstructError:
            continue
        return offset
    return None


def process_buffer(inverter: Inverter, port: serial.Serial, buffer: bytes) -> bytes:
    while buffer:
        if len(buffer) < 4:
            return buffer

        try:
            request, frame_size = parse_request(buffer)
        except ConstructError:
            if len(buffer) > MAX_MODBUS_BUFFER_SIZE:
                frame_offset = find_modbus_frame_offset(buffer)
                discard_size = (
                    frame_offset
                    if frame_offset is not None
                    else len(buffer) - MAX_MODBUS_BUFFER_SIZE
                )
                LOGGER.warning("Discarding %d bytes from oversized garbage buffer", discard_size)
                buffer = buffer[discard_size:]
                continue
            return buffer

        LOGGER.info("Request: slave=%s function=%s", request.slave_addr, request.function)
        response = build_response(inverter, request)
        if response is not None:
            port.write(response)
            LOGGER.info("Response: %s", response.hex())

        buffer = buffer[frame_size:]

    return buffer


def run_emulator(
    port_name: str,
    baudrate: int = DEFAULT_BAUDRATE,
    profile: str = PROFILE_RANDOM,
    event_interval_seconds: float = 120.0,
    catchup_delay_seconds: float = 600.0,
    log_energy_reads: bool = False,
) -> None:
    LOGGER.info("Starting fake inverter on %s at %d bps", port_name, baudrate)

    inverter = Inverter(
        profile=profile,
        event_interval_seconds=event_interval_seconds,
        catchup_delay_seconds=catchup_delay_seconds,
        log_energy_reads=log_energy_reads,
    )

    try:
        with serial.Serial(port_name, baudrate, timeout=SERIAL_TIMEOUT_SECONDS) as port:
            buffer = b""
            last_update = time.monotonic()

            while True:
                now = time.monotonic()
                if now - last_update >= STATE_UPDATE_INTERVAL_SECONDS:
                    inverter.update(now)
                    last_update = now

                if not port.in_waiting:
                    time.sleep(IDLE_SLEEP_SECONDS)
                    continue

                buffer += port.read(port.in_waiting)
                buffer = process_buffer(inverter, port, buffer)
    except KeyboardInterrupt:
        LOGGER.info("Stopping fake inverter")
    except serial.SerialException as err:
        LOGGER.error("Failed to use serial port %s: %s", port_name, err)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Serial port, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUDRATE, help="Serial baud rate")
    parser.add_argument(
        "--profile",
        choices=(PROFILE_RANDOM, PROFILE_GENERATOR_FIRST),
        default=PROFILE_RANDOM,
        help=(
            "Energy register behavior. 'generator-first' increments register 537 before "
            "register 534 catches up."
        ),
    )
    parser.add_argument(
        "--event-interval",
        type=float,
        default=120.0,
        help="Seconds between generator-first profile events",
    )
    parser.add_argument(
        "--catchup-delay",
        type=float,
        default=600.0,
        help="Seconds before total production catches up after a generator-first event",
    )
    parser.add_argument(
        "--log-energy-reads",
        action="store_true",
        help="Log returned raw energy counters whenever registers 534/537 are read",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(
        level=logging.INFO,
        format=LOGGER_FORMAT,
        handlers=[logging.StreamHandler(sys.stdout)],
    )
    args = parse_args(sys.argv[1:] if argv is None else argv)
    run_emulator(
        args.port,
        args.baud,
        args.profile,
        args.event_interval,
        args.catchup_delay,
        args.log_energy_reads,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
