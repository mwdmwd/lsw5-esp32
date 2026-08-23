# SPDX-License-Identifier: GPL-3.0-or-later
def crc16(
    data: bytes, poly: int, init: int = 0xFFFF, lsb: bool = True, xor_out: int = 0x0000
) -> int:
    """Generic CRC-16 implementation"""
    crc = init
    if lsb:
        for b in data:
            crc ^= b
            for _ in range(8):
                crc = (crc >> 1) ^ poly if (crc & 1) else (crc >> 1)
    else:
        for b in data:
            crc ^= b << 8
            for _ in range(8):
                crc = ((crc << 1) ^ poly if (crc & 0x8000) else (crc << 1)) & 0xFFFF

    return (crc ^ xor_out) & 0xFFFF


def crc16_modbus(data: bytes) -> int:
    """CRC-16/MODBUS (Poly: 0xA001, init: 0xFFFF, LSB-first)"""
    return crc16(data, poly=0xA001, init=0xFFFF, lsb=True)


def crc16_xmodem(data: bytes) -> int:
    """CRC-16/XMODEM (Poly: 0x1021, init: 0x0000, MSB-first)"""
    return crc16(data, poly=0x1021, init=0x0000, lsb=False)
