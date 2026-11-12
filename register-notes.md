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

## DSP 1175 memory readback

Raw `AA 10` frames configure the DSP's indirect-read window; ordinary unit-1 FC03 reads return the HMI's cached result.

* FC16 must write exactly two registers, encoding a 32-bit C28x **word address**, high register first.
* Starts 560, 562, …, 582 select twelve pointers. Every write clears pointer 0, then **adds** the supplied value to the selected pointer. Thus 560 is an absolute assignment; the others must not be blindly retried.
* At 584, the first register selects the mode; the second is ignored. This also clears pointer 0. Zero restores normal diagnostics; nonzero enables indirect reads.
* Registers 560–583 contain two reads of each pointed-to **same 16-bit word**, not low/high halves. All twelve pointers are dereferenced; their startup values are zero.
* Pointer 0 is stored at `0xdf9c`/`0xdf9d` in 1175. Flash is readable. The application spans `0x84000`–`0xa95ff` (306,176 bytes), including the update trailer.

Raw-console examples, with automatic CRC:

| Operation | Frame |
| --- | --- |
| Enable indirect reads | `AA 10 02 48 00 02 04 00 01 00 00` |
| Set pointer 0 to `0x84000` | `AA 10 02 30 00 02 04 00 08 40 00` |
| Restore normal diagnostics | `AA 10 02 48 00 02 04 00 00 00 00` |

Set the pointer **after** changing mode. The echo confirms HMI reception, not DSP execution. The HMI has one pending raw frame and refreshes its cache asynchronously; neither a fixed delay nor a repeated data value proves that a new address has taken effect.

SCI-B/Meter-485 has a separate 50-word FC05 reader: `01 05 ADDRESS_BE32 CRC16`. The 102-byte reply contains the unit, function, and 25 big-endian 32-bit values, with no CRC or echoed address. It requires diagnostic-slave mode rather than external-CT polling. It is absent from SCI-A and is not used by this memory console.

The console points slots 1 and 2 at pointer 0's low/high words, so each sample also contains its source address. It checks the firmware version, entry word, address, and duplicate data words. It initializes zero slots and reuses those already pointing at the expected addresses. Normal diagnostics are restored before and after each capture, including cancellation.

User reads are limited to word addresses `0x80000..0xa95ff`, with even byte lengths. The DSP can read RAM, but arbitrary RAM reads are not safe: 1175 assigns LS2–LS5 to CLA program memory, where CPU reads violate access protection. Uninitialized RAM can also raise parity/ECC errors. See the [TI reference manual](https://www.ti.com/lit/ug/spruhx5/spruhx5.pdf), sections 3.11.1.6.3 and 3.11.1.9. RAM, peripherals, and unverified flash are therefore blocked here.

Other revisions need pointer addresses, an entry-word check, and flash bounds; this is especially tricky, as a revision number may cover more than one build (viz. [the `1184` binaries rebadged as multiple older versions including `1150` and `1172`](https://github.com/Mights001/DeyeFirmware/pull/34)).

Meter-485 capture is not implemented.
