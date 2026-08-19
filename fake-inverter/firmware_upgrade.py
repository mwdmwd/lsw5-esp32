# SPDX-License-Identifier: AGPL-3.0-or-later
import enum
import logging
import time
import zlib
from dataclasses import dataclass, field

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
    expected_crc32: int
    actual_crc32: int
    padding_size: int

    @property
    def valid(self) -> bool:
        return self.expected_crc32 == self.actual_crc32


def inspect_firmware_image(block_data: bytes) -> FirmwareImageInfo | None:
    """Find and validate a Deye image inside the padded XMODEM block stream."""
    minimum_size = max(DEYE_TRAILER_SIZE, len(block_data) - XMODEM_BLOCK_SIZE + 1)

    for image_size in range(len(block_data), minimum_size - 1, -1):
        padding = block_data[image_size:]
        if padding and (padding[0] not in (0x1A, 0xFF) or any(b != padding[0] for b in padding)):
            continue

        trailer_start = image_size - DEYE_TRAILER_SIZE
        trailer = block_data[trailer_start:image_size]
        payload_size = int.from_bytes(trailer[3:7], "big")
        if payload_size + DEYE_TRAILER_SIZE != image_size:
            continue

        expected_crc32 = int.from_bytes(trailer[7:11], "big")
        actual_crc32 = zlib.crc32(block_data[:payload_size]) & 0xFFFFFFFF
        if expected_crc32 != actual_crc32:
            continue

        return FirmwareImageInfo(
            image_size=image_size,
            payload_size=payload_size,
            selectors=trailer[:3],
            expected_crc32=expected_crc32,
            actual_crc32=actual_crc32,
            padding_size=len(block_data) - image_size,
        )

    return None


@dataclass
class FirmwareUpgrade:
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
        self.phase = UpgradePhase.READY_PENDING
        self.target_id = target_id
        self.expected_block = 1
        self.block_data.clear()
        self.image = None
        self.image_info = None
        self.blocks_received = 0
        self.ready_at = (time.monotonic() if now is None else now) + BOOTLOADER_READY_DELAY_SECONDS
        LOGGER.info("Firmware upgrade requested: target=%s command=0x60", self.target_name)

    def poll_ready(self, now: float | None = None) -> bytes | None:
        if self.phase != UpgradePhase.READY_PENDING or self.ready_at is None:
            return None
        if (time.monotonic() if now is None else now) < self.ready_at:
            return None

        self.phase = UpgradePhase.RECEIVING
        self.ready_at = None
        LOGGER.info("Firmware bootloader ready: sending XMODEM CRC request 'C'")
        return b"C"

    def consume(self, buffer: bytes) -> tuple[bytes, list[bytes]]:
        """Consume upgrade bytes and return (unconsumed bytes, raw replies)."""
        replies: list[bytes] = []

        while self.active and buffer:
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
                self.phase = UpgradePhase.IDLE
                return buffer, replies

            if buffer[0] == XMODEM_EOT:
                buffer = buffer[1:]
                self._finish()
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
            elif block_number == ((self.expected_block - 1) & 0xFF):
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

    def _finish(self) -> None:
        self.phase = UpgradePhase.COMPLETE
        self.ready_at = None
        self.image_info = inspect_firmware_image(bytes(self.block_data))

        if self.image_info is None:
            self.image = bytes(self.block_data)
            LOGGER.warning(
                "Firmware transfer complete but Deye trailer validation failed: "
                "target=%s blocks=%d padded_bytes=%d",
                self.target_name,
                self.blocks_received,
                len(self.block_data),
            )
            return

        self.image = bytes(self.block_data[: self.image_info.image_size])
        LOGGER.info(
            "Firmware upgrade complete: target=%s blocks=%d image_bytes=%d "
            "payload_bytes=%d selectors=%s crc32=%08x padding=%d",
            self.target_name,
            self.blocks_received,
            self.image_info.image_size,
            self.image_info.payload_size,
            self.image_info.selectors.hex(),
            self.image_info.actual_crc32,
            self.image_info.padding_size,
        )
