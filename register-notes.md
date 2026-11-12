# Register notes
- Total PV Production (534): subtract Total Generator Production (537) to get just the Deye MPPTs' production, at least if the **GEN** port is configured as a microinverter input

## High-resolution PV/GEN counters

The modded DSP 1175 firmware supplies PV/GEN counters in 0.1 Wh units. The example YAML uses them when enabled, otherwise the stock 0.1 kWh counters.

| Register | Meaning |
| --- | --- |
| 534/535 | Combined production, low word first, in 0.1 kWh |
| 537/538 | GEN production, low word first, in 0.1 kWh |
| 542 | Combined raw counter low 16 bits |
| 543 | GEN raw counter low 16 bits |
| 547 | EFER (Extended Feature Enable Register): bit 0 enables extended PV/GEN precision; other bits ignored here |

Read 534–547 in **one response**. For each quotient `Q`, calculate `base = uint64_t(Q) * 1000`. With EFER bit 0 set, use low word `L` to calculate `delta = uint16_t(L - uint16_t(base))` and accept only `delta < 1000`. Otherwise ignore both low words and use `delta = 0`. `base + delta` is the counter in 0.1 Wh. Stock and PV-only firmware leave bit 0 clear. Zero counters and low-word rollover are valid. The DSP commits these counters every ten minutes; C05E needs no HMI modification.

The YAML merges `534..540` with Temperature's `541..547` into one FC03 read, using the controller's address and five-second interval. The explicit counts put both counters, temperature, and EFER in the same reply. Do not split the range or set `skip_updates` on either item.

PV is combined + historical offset − GEN; this assumes the GEN port is a microinverter input. Arithmetic, the three-sample minimum, and the monotonic floor use integer 0.1 Wh units. The HMI cache copy is not atomic, so even a single reply can mix updates that pass the remainder checks.

Total PV Production publishes retained kWh with four decimal places on `${name}/generated_energy`. It is an ESPHome text sensor to avoid float precision loss. Keep the existing numeric MQTT sensor in HA:

```yaml
- name: inverter_generated_energy
  state_topic: inverter/generated_energy
  state_class: total_increasing
  device_class: energy
  unit_of_measurement: kWh
```

PV publication waits for a retained MQTT value and three valid samples. Existing one-decimal values are accepted; a lower counter must catch up before publishing. On a new installation, seed the retained topic with `0` (or the required starting total). Nothing is written to NVS. Bad replies and backward values leave the previous state intact, so check freshness as well as EFER.

Total Generator Production remains a numeric ESPHome sensor with `kWh` and its default MQTT topic. Its float state is approximate and does not feed PV accounting.

## Useless or redundant registers
- Battery BMS SOC (214): duplicate of Battery SOC (588), at least in lithium mode
- Battery BMS Current (216): no decimal places
- DC Temperature (540): always reports 25 °C
- Battery Corrected Capacity (592): always(?) reports 200 Ah
- External CT (all registers), if you're using meter emulation: they just repeat what is fed into the RS485-Meter port
