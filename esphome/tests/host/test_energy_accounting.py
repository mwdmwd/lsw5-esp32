#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and test the energy accounting callbacks from the example YAML."""

import argparse
from decimal import Decimal, ROUND_HALF_UP
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

import yaml


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def mapping(node):
    return {key.value: value for key, value in node.value}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def generate(config):
    # compose() accepts ESPHome tags without loading secrets or importing ESPHome.
    document = mapping(yaml.compose(config.read_text()))
    sensors = {
        fields["id"].value: fields
        for node in document["text_sensor"].value
        if "id" in (fields := mapping(node))
    }
    pv = sensors["total_pv_production"]
    for key, value in {
        "platform": "template",
        "update_interval": "never",
        "discovery": "false",
        "retain": "true",
        "state_topic": "${name}/generated_energy",
    }.items():
        require(pv[key].value == value, f"PV: unexpected {key}")
    generator = next(
        fields
        for node in document["sensor"].value
        if (fields := mapping(node)).get("id") is not None
        and fields["id"].value == "total_generator_production"
    )
    require("state_topic" not in generator, "GEN must keep its default MQTT topic")
    for key, value in {
        "platform": "template",
        "unit_of_measurement": "kWh",
        "accuracy_decimals": "4",
        "discovery": "true",
    }.items():
        require(generator[key].value == value, f"GEN: unexpected {key}")
    source = sensors["efer"]
    require(source["internal"].value == "true", "reader must be internal")
    require("state_topic" not in source, "reader must not publish raw data over MQTT")
    require("skip_updates" not in source, "energy must use the normal poll interval")
    require("custom_command" not in source, "energy must use the controller's normal FC03 read")
    for key, value in {
        "register_type": "holding",
        "address": "534",
        "register_count": "7",
        "response_size": "14",
        "raw_encode": "HEXBYTES",
    }.items():
        require(source[key].value == value, f"unexpected {key}")
    temperature = next(
        fields
        for node in document["sensor"].value
        if (fields := mapping(node)).get("name") is not None
        and fields["name"].value == "Temperature"
    )
    for key, value in {
        "register_type": "holding",
        "address": "541",
        "register_count": "7",
        "value_type": "S_WORD",
    }.items():
        require(temperature[key].value == value, f"Temperature: unexpected {key}")
    require("skip_updates" not in temperature, "Temperature must not slow the shared energy read")
    require("force_new_range" not in temperature, "Temperature must extend the energy range")
    require("response_size" not in temperature, "Temperature must span seven normal registers")

    code = source["lambda"].value
    history = mapping(document["substitutions"])["solar_production_offset_kwh"].value
    history_raw = int((Decimal(history) * 10000).to_integral_value(rounding=ROUND_HALF_UP))
    require(0 <= history_raw < (1 << 64) - (1 << 32) * 1000, "history outside safe range")
    replacement = "${solar_production_offset_kwh}"
    require(code.count(replacement) == 1, "expected one history substitution")
    require(
        set(re.findall(r"\bid\((\w+)\)", code))
        == {"total_pv_production", "total_generator_production", "total_pv_production_filter"},
        "unexpected decoder dependency",
    )

    # Keep both full callbacks verbatim except for ESPHome's numeric substitution.
    # The zero-offset variant also tests a fresh installation with no history.
    generated = [f"constexpr uint64_t configured_history = {history_raw}ULL;\n"]
    for name, offset in (("configured", history), ("zero", "0.0")):
        generated.append(
            f"std::string decode_{name}(std::vector<uint8_t> const &data, Item const *item, "
            "Sensors &sensors)\n{\n" + code.replace(replacement, offset) + "\n}\n"
        )

    # The exhaustive loop omits allocation/logging/formatting. Extract the actual
    # arithmetic statements and rejection predicate; never maintain a second
    # implementation of the decoder. Fail if its structure changes unexpectedly.
    def declaration(name):
        found = re.findall(rf"uint(?:16|64)_t const {name}\s*=\s*[^;]+;", code)
        require(len(found) == 1, f"cannot extract declaration: {name}")
        return found[0]

    names = (
        "efer",
        "combined_low",
        "generator_low",
        "combined_base",
        "generator_base",
        "combined_delta",
        "generator_delta",
    )
    rejection = re.findall(r"if\((combined_delta[^\n]+)\)\s*\{", code)
    require(len(rejection) == 1, "cannot extract paired rejection predicate")
    generated.append(
        "Totals arithmetic(std::array<uint16_t, 14> const &words)\n{\n"
        "auto word = [&](size_t index) { return words.at(index); };\n"
        + "\n".join(declaration(name) for name in names)
        + f"\nif({rejection[0]}) return {{}};\n"
        + declaration("combined")
        + "\n"
        + declaration("generator")
        + "\nreturn {true, combined, generator};\n}\n"
    )
    mqtt = mapping(document["mqtt"])
    subscription = mapping(mqtt["on_message"].value[0])
    require(subscription["topic"].value == pv["state_topic"].value, "floor topic must match PV")
    seed = mapping(subscription["then"].value[0])["lambda"].value
    generated.append(
        "void seed_floor(std::string const &x, Sensors &sensors)\n{\n" + seed + "\n}\n"
    )
    return "\n".join(generated)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exhaustive", action="store_true", help="also run the billion-case sweeps"
    )
    parser.add_argument("--config", type=Path, default=ROOT / "esphome/lsw5-esp32.yaml")
    args = parser.parse_args()
    generated = generate(args.config)
    with tempfile.TemporaryDirectory(prefix="energy-accounting-tests-") as directory:
        build = Path(directory)
        (build / "energy_accounting_lambda.h").write_text(generated)
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-O3", "-g"]
        if os.environ.get("SANITIZE") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        binary = build / "energy-accounting-tests"
        subprocess.run(
            shlex.split(os.environ.get("CXX", "g++"))
            + flags
            + [
                f"-I{build}",
                f"-I{ROOT / 'esphome'}",
                str(HERE / "energy_accounting_main.cpp"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)] + (["--exhaustive"] if args.exhaustive else []), check=True)


if __name__ == "__main__":
    main()
