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
