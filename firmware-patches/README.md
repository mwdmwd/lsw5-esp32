# DSP 1175 energy counters

[`dsp-1175-energy-counters.bps`](dsp-1175-energy-counters.bps) adds 0.1 Wh PV/GEN counters. The existing 0.1 kWh counters keep their meanings. See [register notes](../register-notes.md#high-resolution-pvgen-counters).

Download the stock [`MCU2_SG12KLP3-V200x-1175-1127.bin`](https://github.com/Mights001/DeyeFirmware/raw/6cb86ac083e382c5180c39c8fab059f7a8e099e9/Controller/3PH/LV/MCU2_SG12KLP3-V200x-1175-1127.bin) from DeyeFirmware into this directory.

With [Flips](https://github.com/Sir-Walrus/Flips) installed, run from this directory:

```sh
flips --apply --exact dsp-1175-energy-counters.bps \
  MCU2_SG12KLP3-V200x-1175-1127.bin \
  MCU2_SG12KLP3-V200x-1175-1127-energy.bin
sha256sum --check SHA256SUMS
```

Or use any patcher of your choosing that supports the BPS format.

All three hashes must match: the patch, stock image, and patched image. Keep checksum checks enabled in the patcher.

The output includes the corrected Deye trailer CRC and is ready for an [MCU2/DSP firmware upload](../README.md#inverter-firmware-upgrades).
