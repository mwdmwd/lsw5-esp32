#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Interactive AT / Modbus console for the LSW5 local TCP socket."""

import argparse
import math
import re
import socket
import struct
import sys
import time

from crc import crc16_modbus

try:
    import readline  # noqa: F401 -- in-memory editing/history; no history file
except ImportError:
    pass


HELP = """AT+...                    Send an AT line in the current mode
:read START [COUNT [SLAVE]] Read holding registers (defaults: count=1, slave=1)
:rtu HEX                   Send Modbus bytes; CRC is added
:flash ...                 User-flash info, MD5, UART dump, erase; :help flash
:hiflying ...              Raw-flash MD5, UART dump, erase; :help hiflying
:ram ...                   CPU memory read/write; :help ram
:mode plain|framed         Plain AT+INVDATA or binary logger framing
:serial NUMBER             Set the logger serial required by framed mode
:help [flash|hiflying|ram]  Local help, including all firmware command forms
:quit                      Exit (also Ctrl-D)

Numbers are decimal or 0x-prefixed hex. Modbus reads allow up to 60 registers.
Plain mode needs no logger serial. Stock firmware ignores ordinary AT queries
such as AT+VER; modded logger firmware enables them in framed mode.
Writes and erases take effect immediately, including through :rtu or AT commands.
Each request opens a fresh connection; timed-out commands are never retried.
"""


FLASH_FORMS = """AT+{name}=?                     Firmware's brief help (read-only)
AT+{name}=R,OFFSET,LENGTH       Hex dump to the AT UART (not TCP)
AT+{name}=C,OFFSET,LENGTH       Return a 32-character MD5 digest (read-only)
AT+{name}=E,OFFSET,LENGTH       Erase flash; LENGTH is bytes, divided by 4096
AT+{name}=W,OFFSET              Write flash via UART XMODEM-CRC

Helpers (numbers are decimal or 0x hex):
:{helper} md5 OFFSET LENGTH
:{helper} uart-read OFFSET LENGTH
:{helper} erase OFFSET LENGTH

Operation letters accept either case. OFFSET and LENGTH are bytes.
MD5 and UART dump helpers allow 1..4096 bytes per command. The MD5 code
loops LENGTH times, including redundant zero-byte reads; large ranges are slow.
Erase helpers require a 4096-aligned offset and a positive multiple of 4096;
the firmware otherwise truncates LENGTH/4096 and can report +ok on failure.
W waits up to 30 seconds for UART XMODEM, even over TCP; it takes no inline data.
The write callbacks erase from offset zero, ignoring the supplied base.
There is no TCP upload helper for this path.
Use :help locally; AT+{name}? is not a query form for this SDK command.
"""

COMMAND_HELP = {
    "flash": """FLASH uses logical offsets into uflash followed by uflash1.
These are not physical flash offsets or CPU addresses.

AT+FLASH                       Identify flash size/type (read-only)
:flash info                    Same query
AT+FLASH=partition             List partitions (overflows the TCP reply buffer)

The partition handler writes all 13 rows without checking output capacity,
overflowing this transport's 256-byte reply buffer. It has no shell helper.
FLASH erase supports only 1..8 sectors (4096..32768 bytes) per call.

"""
    + FLASH_FORMS.format(name="FLASH", helper="flash"),
    "hiflying": """HIFLYING uses raw physical flash offsets, including bootloader and firmware.
E/W enable the SDK's raw-flash write override for the operation.
There is no bare/status form; AT+FLASH identifies the chip.

"""
    + FLASH_FORMS.format(name="HIFLYING", helper="hiflying"),
    "ram": """RAM accesses CPU addresses directly, including mapped firmware.

AT+RAM=?                       Firmware's brief help (read-only)
AT+RAM=R,ADDRESS,SIZE           Read memory; SIZE is a byte count
AT+RAM=W,ADDRESS,WIDTH,VALUE    Write one 1-, 2-, or 4-byte value immediately
:ram read ADDRESS [SIZE]       Read 1..50 bytes; default SIZE=4
:ram write ADDRESS WIDTH VALUE Write a value that fits WIDTH (1, 2, or 4)

SIZE=1/2/4 returns one little-endian integer; other positive sizes return
a list of bytes. The read helper displays the original bytes and ASCII too.
Helpers require alignment for 2-/4-byte access and reject address wraparound.
The firmware itself checks neither addresses nor alignment and truncates
oversized write values. Unmapped addresses can fault; MMIO reads may have
side effects. Use known memory addresses. Reads above 50 bytes can overflow
the 256-byte AT reply buffer; the helper enforces that limit.

Numbers are decimal or 0x hex. R/W accept either case. There is no bare/status
form; use AT+RAM=? for firmware help, not AT+RAM?.
""",
}


def help_topic(value: str) -> str:
    return value.lower().removeprefix(":").removeprefix("at+")


def number(value: str) -> int:
    return int(value, 16 if value.lower().startswith("0x") else 10)


def memory_command(topic: str, args: list[str]) -> str:
    """Translate one helper command to one AT request."""
    usage = f"use :help {topic} for syntax and firmware limitations"
    if topic == "flash" and args == ["info"]:
        return "AT+FLASH"
    if topic == "ram":
        if args[0] == "read" and len(args) in (2, 3):
            address = number(args[1])
            size = number(args[2]) if len(args) == 3 else 4
            if not 1 <= size <= 50:
                raise ValueError("RAM reads must be 1..50 bytes to fit the AT reply")
            line = f"AT+RAM=R,0x{address:X},{size}"
        elif args[0] == "write" and len(args) == 4:
            address, size, value = map(number, args[1:])
            if size not in (1, 2, 4) or not 0 <= value < 1 << (8 * size):
                raise ValueError("RAM write width must be 1, 2 or 4 and value must fit")
            line = f"AT+RAM=W,0x{address:X},{size},0x{value:X}"
        else:
            raise ValueError(usage)
        if size in (2, 4) and address % size:
            raise ValueError("RAM address must be aligned to the 2-/4-byte width")
    else:
        operations = {"md5": "C", "uart-read": "R", "erase": "E"}
        if len(args) != 3 or args[0] not in operations:
            raise ValueError(usage)
        address, size = map(number, args[1:])
        if args[0] == "erase":
            if address % 4096 or size <= 0 or size % 4096:
                raise ValueError("erase offset and positive length must be multiples of 4096")
            if topic == "flash" and size > 32768:
                raise ValueError("FLASH erase supports at most 8 sectors (32768 bytes)")
        elif not 1 <= size <= 4096:
            raise ValueError("MD5/UART dump helpers accept 1..4096 bytes per command")
        line = f"AT+{topic.upper()}={operations[args[0]]},0x{address:X},{size}"
    if not 0 <= address <= 0xFFFFFFFF or address + size > 0x100000000 or size > 0x7FFFFFFF:
        raise ValueError("address and length must fit the firmware's 32-bit range")
    return line


def show_memory(payload: bytes, address: int, size: int) -> None:
    if not payload.startswith(b"+ok="):
        raise ValueError("expected a memory read reply")
    try:
        words = payload[4:].split()
        if size in (1, 2, 4) and len(words) == 1:
            data = int(words[0], 16).to_bytes(size, "little")
        elif size not in (1, 2, 4) and len(words) == size:
            data = bytes(int(word, 16) for word in words)
        else:
            raise ValueError("unexpected value count")
    except (ValueError, OverflowError) as exc:
        raise ValueError(f"invalid RAM read reply: {payload!r}") from exc
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        ascii_text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  {address + offset:08X}  {chunk.hex(' '):47s}  |{ascii_text}|")


def with_crc(body: bytes) -> bytes:
    return body + crc16_modbus(body).to_bytes(2, "little")


def logger_frame(data: bytes, kind: int, serial: int, sequence: int) -> bytes:
    # Request data starts at byte 26; replies have one fewer metadata byte.
    payload = bytes([kind]) + bytes(14) + data
    header = b"\xa5" + struct.pack("<HHHI", len(payload), 0x4510, sequence, serial)
    frame = header + payload
    return frame + bytes([sum(frame[1:]) & 0xFF, 0x15])


def reply_length(data: bytes, invdata: bool = False) -> int | None:
    """None: incomplete header; -1: unknown text needs an idle delimiter."""
    if not data:
        return None
    if data[0] == 0xA5:
        if len(data) < 3:
            return None
        size = int.from_bytes(data[1:3], "little") + 13
        if not 27 <= size <= 4096:
            raise ValueError(f"invalid logger reply length: {size}")
        return size
    if not invdata:
        return -1
    if b"+ok=".startswith(data):
        return None
    if data.startswith(b"+ok="):
        tail = data[4:]
        if b"no data".startswith(tail) or tail.startswith(b"no data"):
            return 11
        if not re.fullmatch(rb"[0-9a-fA-F]*", tail):
            return -1
        if len(tail) < 4:
            return None
        function = int(tail[2:4], 16)
        if function & 0x80:
            return 4 + 2 * 5
        if function in (1, 2, 3, 4):
            if len(tail) < 6:
                return None
            return 4 + 2 * (5 + int(tail[4:6], 16))
        if function in (5, 6, 15, 16):
            return 4 + 2 * 8
    return -1


def receive(sock: socket.socket, timeout: float, invdata: bool = False) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while True:
        size = reply_length(bytes(data), invdata)
        if size is not None and size >= 0 and len(data) >= size:
            if len(data) != size:
                raise ValueError(f"unexpected trailing reply bytes: {bytes(data)!r}")
            return bytes(data)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"incomplete reply: {bytes(data)!r}")
        sock.settimeout(min(remaining, 0.3) if size == -1 else remaining)
        try:
            chunk = sock.recv(4096 - len(data))
        except TimeoutError:
            if data and size == -1:
                return bytes(data)
            detail = f"partial reply: {bytes(data)!r}" if data else "no reply"
            raise TimeoutError(f"{detail} after {timeout:g}s; command was not retried") from None
        if not chunk:
            if data and size == -1:
                return bytes(data)
            raise ConnectionError(f"logger closed the connection; partial reply: {bytes(data)!r}")
        data.extend(chunk)
        if len(data) >= 4096:
            raise ValueError("reply exceeds 4096 bytes")


def check_rtu_request(rtu: bytes) -> None:
    if len(rtu) < 4:
        raise ValueError("RTU request needs an address, function and CRC")
    if with_crc(rtu[:-2]) != rtu:
        raise ValueError("request has an invalid Modbus CRC")
    if rtu[1] in (3, 4):
        if len(rtu) != 8:
            raise ValueError("FC03/FC04 request must be 8 bytes including CRC")
        start, count = struct.unpack(">HH", rtu[2:6])
        # Plain firmware reply buffer: 4 + 2*(5 + 2*count) bytes plus NUL.
        if not 1 <= count <= 60 or start + count > 65536:
            raise ValueError("read must address 1..60 registers within 0..65535")


def show_rtu(reply: bytes, request: bytes | None) -> None:
    if len(reply) < 5 or with_crc(reply[:-2]) != reply:
        raise ValueError(f"invalid Modbus reply/CRC: {reply.hex(' ')}")
    slave, function = reply[:2]
    print(f"  RTU slave={slave}, FC{function & 0x7F:02d}, CRC OK: {reply.hex(' ')}")
    if request and (slave != request[0] or function & 0x7F != request[1]):
        raise ValueError("Modbus reply does not match the request address/function")
    if function & 0x80:
        raise ValueError(f"Modbus exception {reply[2]}")
    if function in (3, 4):
        if reply[2] % 2 or len(reply) != reply[2] + 5:
            raise ValueError("invalid register reply length")
        if request and reply[2] != 2 * int.from_bytes(request[4:6], "big"):
            raise ValueError("reply register count does not match the request")
        start = int.from_bytes(request[2:4], "big") if request else None
        for index, (value,) in enumerate(struct.iter_unpack(">H", reply[3:-2])):
            label = f"r{start + index}" if start is not None else f"word[{index}]"
            print(f"  {label:>8} = {value:5d}  0x{value:04X}")
        print("  ASCII: " + "".join(chr(b) if 32 <= b < 127 else "." for b in reply[3:-2]))


class Shell:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.sequence = 0

    def execute(self, line: str) -> bool:
        line = line.strip()
        if not line:
            return True
        if any(ord(char) < 32 or ord(char) == 127 for char in line):
            raise ValueError("enter one command per line, without control characters")
        command, *args = line.split()
        rtu = None
        memory_read = None
        if command == ":quit":
            return False
        if command == ":help":
            if not args:
                print(HELP)
            elif len(args) == 1 and help_topic(args[0]) in COMMAND_HELP:
                print(COMMAND_HELP[help_topic(args[0])])
            else:
                raise ValueError("usage: :help [flash|hiflying|ram]")
            return True
        if command == ":mode":
            if args not in (["plain"], ["framed"]):
                raise ValueError("usage: :mode plain|framed")
            self.args.mode = args[0]
            print(f"Mode: {self.args.mode}")
            return True
        if command == ":serial":
            if len(args) != 1 or not 0 <= number(args[0]) <= 0xFFFFFFFF:
                raise ValueError("usage: :serial LOGGER_SERIAL (0..4294967295)")
            self.args.serial = number(args[0])
            print(f"Logger serial: {self.args.serial}")
            return True
        if command == ":read":
            if not 1 <= len(args) <= 3:
                raise ValueError("usage: :read START [COUNT [SLAVE]]")
            start = number(args[0])
            count = number(args[1]) if len(args) > 1 else 1
            slave = number(args[2]) if len(args) > 2 else 1
            if not 0 <= start <= 65535 or not 1 <= count <= 60 or not 1 <= slave <= 247:
                raise ValueError("start must be 0..65535, count 1..60, slave 1..247")
            rtu = with_crc(struct.pack(">BBHH", slave, 3, start, count))
        elif command == ":rtu":
            if not args:
                raise ValueError("usage: :rtu HEX_BYTES_WITHOUT_CRC")
            rtu = with_crc(bytes.fromhex(" ".join(args)))
        elif command[1:] in COMMAND_HELP and command.startswith(":"):
            topic = command[1:]
            if not args or args == ["help"]:
                print(COMMAND_HELP[topic])
                return True
            if self.args.mode != "framed":
                raise ValueError("memory/flash helpers require :mode framed and the local-AT patch")
            line = memory_command(topic, args)
            if topic == "ram" and args[0] == "read":
                memory_read = (
                    number(args[1]),
                    number(args[2]) if len(args) == 3 else 4,
                )
            elif args[0] == "uart-read":
                print("  Dump goes to the logger's AT UART; the TCP reply only reports status.")
        elif command.startswith(":"):
            raise ValueError("unknown shell command; use :help")
        elif not line.startswith("AT"):
            raise ValueError("enter an AT command or use :help")

        if rtu is not None:
            line = f"AT+INVDATA={len(rtu)},{rtu.hex().upper()}"
        elif "AT+INVDATA" in line:
            match = re.fullmatch(r"AT\+INVDATA=(\d+),([0-9A-Fa-f]+)", line)
            if not match:
                raise ValueError("usage: AT+INVDATA=BYTE_COUNT,HEX_RTU_INCLUDING_CRC")
            rtu = bytes.fromhex(match[2])
            if len(rtu) != int(match[1]):
                raise ValueError("AT+INVDATA byte count does not match hex data")
        if rtu is not None:
            check_rtu_request(rtu)

        # Literal AT lines stay literal in framed mode; only helpers use type 2.
        kind = 2 if command in (":read", ":rtu") else 1
        if self.args.mode == "framed":
            if self.args.serial is None:
                raise ValueError("framed mode requires --serial or :serial LOGGER_SERIAL")
            self.sequence = (self.sequence + 1) & 0xFF
            data = logger_frame(
                # dispatch_wifi_at_command appends LF to a trailing CR.
                rtu if kind == 2 else (line + "\r").encode("ascii"),
                kind,
                self.args.serial,
                self.sequence,
            )
        else:
            data = (line + "\r\n").encode("ascii")
        # Callback copies into a 256-byte buffer; leave room for a terminator.
        if len(data) > 255:
            raise ValueError("request exceeds the logger's 255-byte command limit")
        print(
            f"> {line}" if self.args.mode == "plain" else f"> framed type={kind}: {line}",
            flush=True,
        )
        if self.args.verbose:
            print("  TX " + data.hex(" "))
        with socket.create_connection((self.args.host, self.args.port), self.args.timeout) as sock:
            sock.sendall(data)
            reply = receive(sock, self.args.timeout, self.args.mode == "plain" and rtu is not None)
        if self.args.verbose:
            print("  RX " + reply.hex(" "))
        if self.args.mode == "framed":
            payload = self.show_frame(reply, kind, rtu)
            if memory_read is not None:
                show_memory(payload, *memory_read)
        else:
            print("< " + repr(reply.decode("ascii", errors="backslashreplace")))
            if reply == b"+ok=no data":
                raise ValueError("logger received no UART response")
            if rtu is not None and reply.startswith(b"+ok="):
                show_rtu(bytes.fromhex(reply[4:].decode("ascii")), rtu)
        return True

    def show_frame(self, reply: bytes, kind: int, rtu: bytes | None) -> bytes:
        if len(reply) < 27 or reply[0] != 0xA5 or reply[-1] != 0x15:
            raise ValueError(f"invalid logger frame: {reply.hex(' ')}")
        if reply[-2] != sum(reply[1:-2]) & 0xFF:
            raise ValueError("logger frame checksum mismatch")
        if (
            int.from_bytes(reply[7:11], "little") != self.args.serial
            or reply[5] != self.sequence
            or reply[11] != kind
        ):
            raise ValueError("logger reply serial/sequence/type does not match request")
        if reply[3:5] != b"\x10\x15":
            raise ValueError(f"unexpected logger control code: {reply[3:5].hex()}")
        payload = reply[25:-2]
        print(f"< framed type={reply[11]}, checksum OK: {payload.hex(' ')}")
        if len(payload) == 2 and payload[1] == 0:
            errors = {
                1: "checksum error",
                2: "frame format error",
                3: "sequence error",
                5: "no UART response",
                6: "logger serial mismatch",
            }
            raise ValueError("logger: " + errors.get(payload[0], f"error {payload[0]}"))
        if kind == 2:
            show_rtu(payload, rtu)
        else:
            print("  " + repr(payload.decode("ascii", errors="backslashreplace")))
            if payload.startswith(b"+ERR="):
                raise ValueError(
                    "AT command failed: " + payload.decode("ascii", errors="backslashreplace")
                )
        return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", nargs="?", default="10.77.0.115")
    parser.add_argument("--port", type=number, default=8899)
    parser.add_argument("--serial", type=number, help="logger serial (needed for framed mode)")
    parser.add_argument("--mode", choices=("plain", "framed"), default="plain")
    parser.add_argument(
        "--timeout", type=float, default=6, help="reply timeout in seconds (default: 6)"
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="also print wire hex")
    parser.add_argument(
        "-c", "--command", action="append", help="run a command and exit; repeatable"
    )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be 1..65535")
    if args.serial is not None and not 0 <= args.serial <= 0xFFFFFFFF:
        parser.error("serial must be 0..4294967295")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("timeout must be positive and finite")
    shell = Shell(args)
    interactive = not args.command and sys.stdin.isatty()
    if interactive:
        print(f"LSW5 console: {args.host}:{args.port}. Type :help for commands.")
    commands = iter(args.command) if args.command else None
    while True:
        try:
            line = (
                next(commands)
                if commands is not None
                else input(f"logger[{args.mode}]> " if interactive else "")
            )
            if not shell.execute(line):
                return 0
        except (StopIteration, EOFError):
            return 0
        except KeyboardInterrupt:
            if not interactive:
                return 130
            print("\nInterrupted; command was not retried.")
        except (OSError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            if not interactive:
                return 1


if __name__ == "__main__":
    sys.exit(main())
