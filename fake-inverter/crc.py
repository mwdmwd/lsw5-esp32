# SPDX-License-Identifier: AGPL-3.0-or-later
def crc16_modbus(data: bytes, poly=0xA001):
    """
    CRC-16-Modbus Algorithm
    """
    crc = 0xFFFF
    for b in data:
        cur_byte = 0xFF & b
        for _ in range(0, 8):
            if (crc & 0x0001) ^ (cur_byte & 0x0001):
                crc = (crc >> 1) ^ poly
            else:
                crc >>= 1
            cur_byte >>= 1

    return crc & 0xFFFF
