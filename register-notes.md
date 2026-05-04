# Register notes
- Total PV Production (534): subtract Total Generator Production (537) to get just the Deye MPPTs' production, at least if the **GEN** port is configured as a microinverter input

## Useless or redundant registers
- Battery BMS SOC (214): duplicate of Battery SOC (588), at least in lithium mode
- Battery BMS Current (216): no decimal places
- DC Temperature (540): always reports 25 °C
- Battery Corrected Capacity (592): always(?) reports 200 Ah
- External CT (all registers), if you're using meter emulation: they just repeat what is fed into the RS485-Meter port
