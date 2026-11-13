# Host tests

Run `make check` for formatting, component, emulator, and hotspot tests. Requires clang-format, Ruff, Python 3 with PyYAML, Bash, a C++17 compiler, Node.js, OpenSSL development files, and `fake-inverter/requirements.txt`.

* `./esphome/tests/host/run.sh`: component and browser-script tests only.
* `CXX=clang++ SANITIZE=1 make check`: AddressSanitizer/UndefinedBehaviorSanitizer. If LeakSanitizer cannot run under a debugger or sandbox, add `ASAN_OPTIONS=detect_leaks=0`.
* Compile `esphome/lsw5-esp32.yaml` with ESPHome and your `secrets.yaml` to check the firmware build.

The C++ tests use ESPHome stubs to check transfers, retries, cancellation, UART sharing, memory reads, raw Modbus, and events. Browser tests run the embedded JavaScript. These tests run without an inverter.

## Energy accounting

The tests compile the high-resolution decoder and MQTT callback from the YAML. They cover EFER, stock/extended transitions, counter boundaries, PV formatting and subtraction, startup/reboot, old retained values, and mixed-cache spikes. GEN stays numeric with its default MQTT topic.

`make check-energy-accounting-exhaustive` adds:

| Check | Cases |
| --- | ---: |
| Single-counter reconstruction at both ends of the quotient range | 2,147,483,648 |
| Coarse high-word boundaries | 6,553,600 |
| Combined/GEN low-word pairs at one quotient pair | 4,294,967,296 |
| Complete callbacks, including two fractional subtraction matrices | 2,862,293 |

The large sweeps extract the arithmetic from the YAML; full callbacks also check publication and formatting. See [the register notes](../../../register-notes.md#decoder-validation) for the arithmetic and limits. `CXX` and `SANITIZE` apply to this target too.
