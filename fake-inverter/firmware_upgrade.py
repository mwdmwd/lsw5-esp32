# SPDX-License-Identifier: GPL-3.0-or-later
import enum
import logging
import tempfile
import time
import zlib
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path

from crc import crc16_xmodem

LOGGER = logging.getLogger(__name__)

XMODEM_STX = 0x02
XMODEM_EOT = 0x04
XMODEM_ACK = 0x06
XMODEM_NAK = 0x15
XMODEM_BLOCK_SIZE = 1024
XMODEM_PACKET_SIZE = 3 + XMODEM_BLOCK_SIZE + 2
DEYE_TRAILER_SIZE = 11
BOOTLOADER_READY_DELAY_SECONDS = 0.3

TARGET_NAMES = {
    0: "MCU0",
    1: "MCU1 (HMI)",
    2: "MCU2 (DSP)",
    3: "MCU3",
    4: "MCU4",
    5: "MCU5",
    6: "MCU6",
    9: "MCU9",
}


class UpgradePhase(enum.Enum):
    IDLE = "idle"
    READY_PENDING = "ready-pending"
    RECEIVING = "receiving"
    COMPLETE = "complete"


@dataclass(frozen=True)
class FirmwareImageInfo:
    image_size: int
    payload_size: int
    selectors: bytes
    crc32: int


def inspect_firmware_image(block_data: bytes) -> FirmwareImageInfo | None:
    """Validate an aligned Deye image whose trailer is at the exact end."""
    if len(block_data) < DEYE_TRAILER_SIZE or len(block_data) % XMODEM_BLOCK_SIZE != 0:
        return None

    trailer = block_data[-DEYE_TRAILER_SIZE:]
    payload_size = int.from_bytes(trailer[3:7], "big")
    if payload_size + DEYE_TRAILER_SIZE != len(block_data):
        return None

    expected_crc32 = int.from_bytes(trailer[7:11], "big")
    actual_crc32 = zlib.crc32(block_data[:payload_size]) & 0xFFFFFFFF
    if expected_crc32 != actual_crc32:
        return None

    return FirmwareImageInfo(
        image_size=len(block_data),
        payload_size=payload_size,
        selectors=trailer[:3],
        crc32=actual_crc32,
    )


@dataclass
class FirmwareUpgrade:
    capture_directory: Path | None = None
    capture_path: Path | None = field(default=None, init=False)
    phase: UpgradePhase = UpgradePhase.IDLE
    target_id: int | None = None
    expected_block: int = 1
    block_data: bytearray = field(default_factory=bytearray)
    image: bytes | None = None
    image_info: FirmwareImageInfo | None = None
    ready_at: float | None = None
    blocks_received: int = 0

    @property
    def active(self) -> bool:
        return self.phase in (UpgradePhase.READY_PENDING, UpgradePhase.RECEIVING)

    @property
    def target_name(self) -> str:
        if self.target_id is None:
            return "unknown"
        return TARGET_NAMES.get(self.target_id, f"MCU{self.target_id}")

    def begin(self, target_id: int, now: float | None = None) -> None:
        self.abort("superseded")
        if self.capture_directory is not None:
            self.capture_directory.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                prefix=f"{datetime.now(UTC):%Y%m%dT%H%M%SZ}-mcu{target_id}-",
                suffix=".partial.bin",
                dir=self.capture_directory,
                delete=False,
            ) as stream:
                self.capture_path = Path(stream.name)
        self.phase = UpgradePhase.READY_PENDING
        self.target_id = target_id
        self.expected_block = 1
        self.block_data.clear()
        self.image = None
        self.image_info = None
        self.blocks_received = 0
        self.ready_at = (time.monotonic() if now is None else now) + BOOTLOADER_READY_DELAY_SECONDS
        LOGGER.info("Firmware upgrade requested: target=%s command=0x60", self.target_name)

    def abort(self, reason: str = "aborted") -> None:
        if self.capture_path is not None:
            LOGGER.info(
                "Firmware transfer ended (%s); partial image: %s", reason, self.capture_path
            )
            self.capture_path = None
        self.phase = UpgradePhase.IDLE
        self.ready_at = None

    def poll_ready(self, now: float | None = None) -> bytes | None:
        if self.phase != UpgradePhase.READY_PENDING or self.ready_at is None:
            return None
        if (time.monotonic() if now is None else now) < self.ready_at:
            return None

        self.phase = UpgradePhase.RECEIVING
        self.ready_at = None
        LOGGER.info("Firmware receiver ready: sending XMODEM CRC request 'C'")
        return b"C"

    def consume(self, buffer: bytes) -> tuple[bytes, list[bytes]]:
        """Consume upgrade bytes and return (unconsumed bytes, raw replies)."""
        replies: list[bytes] = []

        while self.active and buffer:
            # A request can be split after its slave byte by the USB UART.
            if len(buffer) == 1 and buffer[0] in (0x01, 0xA2):
                break
            # A Modbus request means the logger left or never entered XMODEM.
            # Leave it for the normal parser, another activation can restart the upgrade.
            if (
                len(buffer) >= 2
                and buffer[0] in (0x01, 0xA2)
                and buffer[1]
                in (
                    0x03,
                    0x06,
                    0x10,
                )
            ):
                self.abort("modbus-resumed")
                return buffer, replies

            if buffer[0] == 0x18:  # XMODEM CAN
                self.abort("cancelled")
                return buffer[1:].lstrip(b"\x18"), replies

            if buffer[0] == XMODEM_EOT:
                buffer = buffer[1:]
                reply = self._finish()
                if reply is not None:
                    replies.append(bytes((reply,)))
                continue

            if buffer[0] != XMODEM_STX:
                LOGGER.warning("Discarding unexpected upgrade byte 0x%02x", buffer[0])
                buffer = buffer[1:]
                continue

            if len(buffer) < XMODEM_PACKET_SIZE:
                break

            packet = buffer[:XMODEM_PACKET_SIZE]
            buffer = buffer[XMODEM_PACKET_SIZE:]
            block_number = packet[1]
            block_inverse = packet[2]
            payload = packet[3 : 3 + XMODEM_BLOCK_SIZE]
            expected_crc = int.from_bytes(packet[-2:], "big")
            actual_crc = crc16_xmodem(payload)

            if block_inverse != (block_number ^ 0xFF) or expected_crc != actual_crc:
                LOGGER.warning(
                    "Rejecting XMODEM block %d: complement=%02x crc=%04x/%04x",
                    block_number,
                    block_inverse,
                    expected_crc,
                    actual_crc,
                )
                replies.append(bytes((XMODEM_NAK,)))
                continue

            if block_number == self.expected_block:
                if self.capture_path is not None:
                    # Close each accepted block before ACK; retries are not appended.
                    with self.capture_path.open("ab") as stream:
                        stream.write(payload)
                self.block_data.extend(payload)
                self.blocks_received += 1
                self.expected_block = (self.expected_block + 1) & 0xFF
                if self.blocks_received == 1 or self.blocks_received % 64 == 0:
                    LOGGER.info(
                        "Firmware data: target=%s blocks=%d bytes=%d",
                        self.target_name,
                        self.blocks_received,
                        len(self.block_data),
                    )
                replies.append(bytes((XMODEM_ACK,)))
            elif self.blocks_received and block_number == ((self.expected_block - 1) & 0xFF):
                # The logger missed our ACK and retransmitted the previous block.
                LOGGER.info("ACKing duplicate XMODEM block %d", block_number)
                replies.append(bytes((XMODEM_ACK,)))
            else:
                LOGGER.warning(
                    "Rejecting out-of-sequence XMODEM block %d (expected %d)",
                    block_number,
                    self.expected_block,
                )
                replies.append(bytes((XMODEM_NAK,)))

        return buffer, replies

    def _finish(self) -> int | None:
        self.phase = UpgradePhase.COMPLETE
        self.ready_at = None
        self.image_info = inspect_firmware_image(bytes(self.block_data))
        if self.capture_path is not None:
            suffix = ".bin" if self.image_info is not None else ".rejected.bin"
            destination = self.capture_path.with_name(
                self.capture_path.name.removesuffix(".partial.bin") + suffix
            )
            self.capture_path.rename(destination)
            LOGGER.info("Firmware saved to %s", destination)
            self.capture_path = None

        if self.image_info is None:
            self.image = bytes(self.block_data)
            LOGGER.warning(
                "Firmware image rejected after EOT: Deye trailer validation failed: "
                "target=%s blocks=%d image_bytes=%d",
                self.target_name,
                self.blocks_received,
                len(self.block_data),
            )
            return XMODEM_NAK

        self.image = bytes(self.block_data)
        LOGGER.info(
            "External firmware transfer complete: target=%s blocks=%d image_bytes=%d "
            "payload_bytes=%d selectors=%s crc32=%08x",
            self.target_name,
            self.blocks_received,
            self.image_info.image_size,
            self.image_info.payload_size,
            self.image_info.selectors.hex(),
            self.image_info.crc32,
        )
        return XMODEM_ACK if self.target_id == 2 else None
