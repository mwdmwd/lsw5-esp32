# Host tests

Run `make check` for formatting, component, emulator, and hotspot tests. Requires clang-format, Ruff, Python 3 with PyYAML, Bash, a C++17 compiler, OpenSSL development files, and `fake-inverter/requirements.txt`.

* `./esphome/tests/host/run.sh`: component tests only.
* `CXX=clang++ SANITIZE=1 make check`: AddressSanitizer/UndefinedBehaviorSanitizer. If LeakSanitizer cannot run under a debugger or sandbox, add `ASAN_OPTIONS=detect_leaks=0`.
* Compile `esphome/lsw5-esp32.yaml` with ESPHome and your `secrets.yaml` to check the firmware build.

The C++ tests use ESPHome stubs to check transfers, retries, cancellation, UART sharing, and events. These tests run without an inverter.
