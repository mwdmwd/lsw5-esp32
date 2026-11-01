# SPDX-License-Identifier: GPL-3.0-or-later
from modbus import FunctionCode, RequestFrame


class FakePort:
    def __init__(self) -> None:
        self.writes: list[bytes] = []

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)


def build_request(function: FunctionCode, address: int, content: dict, slave: int = 1) -> bytes:
    return RequestFrame.build(
        {
            "data": {
                "value": {
                    "slave_addr": slave,
                    "function": function,
                    "address": address,
                    "content": content,
                }
            }
        }
    )
