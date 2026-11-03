# Fake inverter and logger tools

## Emulator

Emulates a Deye three-phase LV hybrid inverter for a logger. From this directory:

```sh
python3 -m pip install -r requirements.txt
python3 main.py /dev/ttyUSB0
python3 main.py /dev/ttyUSB0 --debug --eps-event-interval 30
```

UART: 9600 8N1, opened exclusively. Writes are logged; `--debug` also prints reads and raw frames. Stop with Ctrl-C.

* Identity, settings, and counters persist in `state.json`; edit it while stopped. See `state.example.json`. Override with `--state PATH` or disable with `--no-state`.
* Firmware uploads go to `captures/` (`--capture-dir PATH` to override). Interrupted files end in `.partial.bin`; EOT produces `.bin` or `.rejected.bin` according to trailer validation. Nothing is flashed. See [the upgrade protocol](FIRMWARE-UPGRADE.md).
* `--eps-event-interval 30 --eps-event-duration 2` simulates a two-second dropout every 30 seconds: state 2, relays open, F46/detail `0x000A`, then recovery.
* Register 0 reports type `0x0005`. The emulator answers unit 1 and DSP passthrough `0xAA`; other addresses are ignored. Low Noise Mode requires `AA` FC16 with `[r35, r36] = [mode, 100]`.

Cloud telemetry may take several minutes to appear, depending on the logger's upload interval.

## Original logger console

`logger_shell.py` connects to the logger's TCP server on port 8899. It needs only Python's standard library and the included CRC helper.

```sh
python3 logger_shell.py 10.77.0.115
python3 logger_shell.py 10.77.0.115 -c ':read 0 8'
python3 logger_shell.py 10.77.0.115 --serial 1234567890 --mode framed
```

Use your logger's IP and serial. Set **Internal server parameters** to TCP-Server, port 8899; the [hotspot](hotspot/README.md) needs `output-allow tcp <logger-ip> 8899`.

| Command | Action |
| --- | --- |
| `:read START [COUNT [SLAVE]]` | Read holding registers; count and slave default to 1, maximum count 60 |
| `:rtu HEX` | Send a Modbus frame with automatic CRC |
| `:mode plain` / `:mode framed` | Use `AT+INVDATA` or serial-number framing |
| `:help` / `:quit` | Help / exit |

Each command opens a connection; there are no retries. `--timeout` defaults to 6 s and `--verbose` prints wire bytes. Use repeated `-c` options or stdin for scripts; the first error stops execution.

Stock `LSW5_32_5406_SS_04_00.00.00.11` supports Modbus and `AT+YWFPW` on this socket. Full AT administration requires modded logger firmware; changing TCP mode does not enable it.

`AT+YWFPW=0,SSID` selects an open network; `AT+YWFPW=1,SSID,PASSWORD` selects WPA2-PSK/AES and reboots. **There is no query form:** `?` and `=?` also invoke the setter. Spaces, commas, and equals signs are not supported within credentials.

With the full AT dispatcher enabled, use `:help flash`, `:help hiflying`, or `:help ram`. `FLASH` addresses user partitions, `HIFLYING` physical flash, and `RAM` CPU memory. Flash dumps and XMODEM writes require the AT UART. **Do not send `AT+FLASH=partition` over TCP:** it can overrun the response buffer.

RAM writes and flash erases execute immediately. The firmware can reply `+ok` even when an operation fails.
