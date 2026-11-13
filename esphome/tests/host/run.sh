#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../.." && pwd)
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -g -O1)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
# All framework doubles live in support.h; generate the include-only shims here.
for header in core/{component,helpers,log}.h \
    components/{binary_sensor/binary_sensor,modbus/modbus,modbus_controller/modbus_controller}.h \
    components/{sensor/sensor,sha256/sha256,text_sensor/text_sensor,uart/uart}.h \
    components/web_server_base/web_server_base.h; do
    path="$build/stubs/esphome/$header"
    mkdir -p -- "$(dirname -- "$path")"
    printf '#include "support.h"\n' > "$path"
done
"${CXX:-g++}" "${flags[@]}" -I"$build/stubs" -I"$here" -I"$root" \
    "$here"/test_*.cpp -lcrypto -o "$build/host-tests"
"$build/host-tests"
node "$here/test_modbus_console.cjs"
node "$here/test_memory_console.cjs"
python3 "$here/test_energy_accounting.py"
