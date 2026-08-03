#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
import argparse
import logging
import random
import re
import sys
import time
from collections.abc import Iterable
from dataclasses import dataclass, field

import serial
from construct import ConstructError

from firmware_upgrade import FirmwareUpgrade
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
LOW_NOISE_MODE_ADDRESS = 35
LOW_NOISE_COMMAND_ADDRESS = 36
LOW_NOISE_COMMAND_MAGIC = 100
MCU2_PASSTHROUGH_ADDRESS = 0xAA
MAX_LOGGED_REGISTER_VALUES = 16


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


def bytes_to_words(data: Iterable[int]) -> list[int]:
    data = list(data)
    return [(data[index] << 8) | data[index + 1] for index in range(0, len(data) - 1, 2)]


def format_function(function) -> str:
    function_number = int(function)
    try:
        function_code = FunctionCode(function_number)
    except ValueError:
        return f"FC{function_number:02d} Unknown"
    function_name = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", function_code.name)
    return f"FC{function_number:02d} {function_name}"


def format_register_values(start_address: int, values: Iterable[int]) -> str:
    values = list(values)
    displayed_values = values[:MAX_LOGGED_REGISTER_VALUES]
    formatted = ", ".join(
        f"r{start_address + offset}={to_u16(value)} (0x{to_u16(value):04X})"
        for offset, value in enumerate(displayed_values)
    )
    if len(values) > len(displayed_values):
        formatted += f", ... (+{len(values) - len(displayed_values)} registers)"
    return f"[{formatted}]"


def format_modbus_request(request) -> str:
    function_number = int(request.function)
    prefix = f"Modbus request: slave={request.slave_addr}, {format_function(request.function)}"

    if function_number in (
        FunctionCode.ReadHoldingRegisters,
        FunctionCode.ReadInputRegisters,
    ):
        return f"{prefix}, start={request.address}, count={request.content.nr_registers}"
    if function_number == FunctionCode.PresetSingleRegister:
        return (
            f"{prefix}, register={request.address}, "
            f"value={request.content.data} (0x{request.content.data:04X})"
        )
    if function_number == FunctionCode.PresetMultipleRegisters:
        return (
            f"{prefix}, start={request.address}, count={request.content.nr_registers}, "
            f"values={format_register_values(request.address, request.content.data)}"
        )
    return f"{prefix}, address={request.address}"


def format_modbus_response(request, response) -> str:
    function_number = int(response.function)
    prefix = f"Modbus response: slave={response.slave_addr}, {format_function(response.function)}"

    if function_number in (
        FunctionCode.ReadHoldingRegisters,
        FunctionCode.ReadInputRegisters,
    ):
        registers = bytes_to_words(response.content)
        return f"{prefix}, values={format_register_values(request.address, registers)}"
    if function_number == FunctionCode.PresetSingleRegister:
        return (
            f"{prefix}, register={response.content.address}, "
            f"value={response.content.data} (0x{response.content.data:04X}), acknowledged"
        )
    if function_number == FunctionCode.PresetMultipleRegisters:
        return (
            f"{prefix}, start={response.content.address}, "
            f"count={response.content.nr_registers}, acknowledged"
        )
    return prefix


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
    firmware_upgrade: FirmwareUpgrade = field(default_factory=FirmwareUpgrade)

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
            35: 0,  # Inverter bridge FSW: 0 = 15 kHz, 1 = 20 kHz
            36: 0,  # Relay self-check status
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
        values = list(values)

        # MCU1 has no write handlers for r35/r36. It still ACKs the logger's
        # FC16 request, but its later reconstructed MCU2 write uses the
        # unchanged cache values rather than the received command payload.
        for offset, value in enumerate(values):
            address = start_address + offset
            if address in (LOW_NOISE_MODE_ADDRESS, LOW_NOISE_COMMAND_ADDRESS):
                continue
            self.registers[address] = to_u16(value)
        if start_address == 1080 and len(values) == 2 and values[1] == 0x60:
            self.firmware_upgrade.begin(to_u16(values[0]))

    def write_mcu2_holding_registers(self, start_address: int, values: Iterable[int]) -> None:
        values = list(values)
        if (
            start_address == LOW_NOISE_MODE_ADDRESS
            and len(values) == 2
            and values[1] == LOW_NOISE_COMMAND_MAGIC
            and values[0] in (0, 1)
        ):
            old_value = self.registers[LOW_NOISE_MODE_ADDRESS]
            self.registers[LOW_NOISE_MODE_ADDRESS] = values[0]
            LOGGER.info(
                "MCU2 applied Low Noise command: r35 %d -> %d",
                old_value,
                values[0],
            )
            return

        LOGGER.info(
            "MCU2 ignored unexpected raw write: start=%d, values=%s",
            start_address,
            format_register_values(start_address, values),
        )

    def service_firmware_upgrade(self, port: serial.Serial, now: float | None = None) -> None:
        ready = self.firmware_upgrade.poll_ready(now)
        if ready is not None:
            port.write(ready)
            LOGGER.info("Response: %s", ready.hex())


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


def find_modbus_frame_offset(buffer: bytes, inverter_addr: int) -> int | None:
    """Find a CRC-valid request after UART noise without discarding it bytewise."""
    for offset in range(1, len(buffer)):
        if buffer[offset] not in (inverter_addr, 0xA2, MCU2_PASSTHROUGH_ADDRESS):
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
        if inverter.firmware_upgrade.active:
            buffer, replies = inverter.firmware_upgrade.consume(buffer)
            for reply in replies:
                port.write(reply)
            if inverter.firmware_upgrade.active or not buffer:
                return buffer

        buffer = buffer.lstrip(LOGGER_PREFIX)
        if not buffer:
            return buffer

        if len(buffer) < 4:
            return buffer

        try:
            request, frame_size = parse_request(buffer)
        except ConstructError:
            if len(buffer) > MAX_MODBUS_BUFFER_SIZE:
                frame_offset = find_modbus_frame_offset(buffer, inverter.registers.get(1, 1))
                discard_size = (
                    frame_offset
                    if frame_offset is not None
                    else len(buffer) - MAX_MODBUS_BUFFER_SIZE
                )
                LOGGER.warning("Discarding %d bytes from oversized garbage buffer", discard_size)
                buffer = buffer[discard_size:]
                continue
            return buffer

        request_frame = buffer[:frame_size]
        if request.slave_addr == MCU2_PASSTHROUGH_ADDRESS:
            LOGGER.info("HMI to MCU2 passthrough request: %s", format_modbus_request(request))
            LOGGER.debug("HMI to MCU2 passthrough request raw: %s", request_frame.hex(" "))
            # echo
            port.write(request_frame)
            if int(request.function) == FunctionCode.PresetMultipleRegisters:
                inverter.write_mcu2_holding_registers(request.address, request.content.data)
            buffer = buffer[frame_size:]
            continue

        LOGGER.info(format_modbus_request(request))
        LOGGER.debug("Modbus request raw: %s", request_frame.hex(" "))
        response = build_response(inverter, request)
        if response is not None:
            port.write(response)
            parsed_response = ResponseFrame.parse(response).data.value
            LOGGER.info(format_modbus_response(request, parsed_response))
            LOGGER.debug("Modbus response raw: %s", response.hex(" "))

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

                inverter.service_firmware_upgrade(port, now)

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
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Include raw Modbus request and response frames in the logs",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format=LOGGER_FORMAT,
        handlers=[logging.StreamHandler(sys.stdout)],
    )
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
