# Deye inverter firmware upgrade protocol

LSW3/LSW5 loggers and the ESPHome sender upload to the HMI, which stages the image in external SPI flash. It then updates itself or transfers the image to the DSP's resident updater. The new application overwrites the old one. There is no rollback or reply confirming a successful boot.

## Activation and packets

At 9600 8N1, write target 1 (HMI) or 2 (DSP) and `0x0060` to registers 1080/1081 with FC16:

```text
MCU1: 01 10 04 38 00 02 04 00 01 00 60 92 35
MCU2: 01 10 04 38 00 02 04 00 02 00 60 62 35
ACK:  01 10 04 38 00 02 c1 35
```

C059 suspends Modbus parsing and sends `C` about 300 ms later. Send XMODEM-1K packets:

| Bytes | Field |
| --- | --- |
| 0 | `STX` (`0x02`) |
| 1–2 | Block number starting at 1, modulo 256; one's complement |
| 3–1026 | 1024 data bytes |
| 1027–1028 | CRC-16/XMODEM, big-endian |

CRC-16/XMODEM: polynomial `0x1021`, initial value zero, no reflection or final XOR. The receiver uses ACK, NAK, and CAN. C059 writes blocks to SPI flash from `0x80000` and checks readback before ACKing.

Sender timing: five activation attempts at two-second intervals; up to 76 s for `C`; 20 s for the first block reply, 15 s thereafter; ten retries with 6 s gaps. Wait 1 s before EOT and 10 s afterward.

## Image format and limits

Files must be multiples of 1024 bytes, ending with an 11-byte trailer:

```text
selector[3] | payload length (BE32) | IEEE CRC32 of payload (BE32)
```

HMI selectors are `01 ff ff`; DSP selectors are `ff ff 05`. Padding after the trailer causes rejection.

| Target | C059 payload range | Aligned file range |
| --- | ---: | ---: |
| MCU1/HMI | 30,721–511,999 bytes | 31,744–512,000 bytes |
| MCU2/DSP | 10,241–511,999 bytes | 11,264–512,000 bytes |

The DSP 1172/1175 resident updater programs word addresses `0x84000..0xBFFFF`: **480 KiB**, less than C059's 500 KiB staging limit. The ESPHome uploader checks alignment, trailer, and CRC32, and calculates SHA-256 for comparison. `max_firmware_size` limits PSRAM allocation; it does not check target capacity or model compatibility.

## Completion

After EOT, C059 checks the image's length and CRC32:

* MCU1 success: schedules its own update and resets; silence is expected.
* MCU2 success: ACK (`0x06`), then a second internal XMODEM transfer to the DSP.
* Rejection: NAK (`0x15`).

**External transfer complete** means delivery to the HMI. DSP programming takes additional minutes. The sender cannot confirm that the target application booted.

## Emulator and original logger

The fake inverter checks and saves incoming blocks, handles duplicates, and keeps partial transfers. It does not simulate flash programming or HMI-to-DSP transfer. Modbus resumes afterward; version registers stay unchanged.

LSW5 also checks filename metadata; use a short name such as `MCU2.bin`. Cloud progress does not confirm delivery to the UART.

The `Update-LCD` / 115200-baud path is not implemented.
