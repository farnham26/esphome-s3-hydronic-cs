# Hydronic S3 fault codes (DTCs)

Summarised from the Eberspächer repair instructions *Hydronic S3 CS* 25.2696.95.2682.0E (09.2020, German) and
the public *Repair Instructions Hydronic S3 Economy 12V CS / Commercial 24V CS*
(https://files.chartindustries.com/Hydronic_S3_Service.pdf). Wording is ours and shortened; follow the OEM document
for repairs. "Seen" = observed on this installation (see [`research.md`](research.md)). Protocol: [`protocol.md`](protocol.md).

## Reading them

- **Read Fault Codes** (button) or automatically when the `2C4` fault flags rise: UDS `19 02 08` on `0x7A0`
  (ReadDTCInformation, status mask "confirmed"), reply `59 02 <mask> {DTC hi mid lo, status}*`.
  Shown in "Heater Fault Codes" and in red on the LCD.
- The control unit stores up to 10 faults. Clearing needs SecurityAccess (see the README,
  [Controlling the heater](../README.md#controlling-the-heater)).
- Status byte (ISO 14229): bit 0 failing now, 1 failed this cycle, 2 pending, 3 confirmed, 4 not tested since
  clear, 5 failed since clear, 6 not tested this cycle, 7 warning lamp. Typical: `2F` active, `28` stored (not
  failing now), `68`/`6C` stored, not yet tested after wake.
- Class = what the EasyStart shows: 0 none, 1 service, 3 voltage, 4 fuel, 5 water circuit/pump, 6 overheat
  lock, 7 limp mode.

## Lockout

The control unit locks (no starts until unlocked with EasyScan / EasyStart workshop menu) after repeated failed
starts (P00012B) or repeated overheating (P00011A). The OEM documents disagree on the counts (3 or 5 starts,
3 or 10 overheats).

## Codes

| Code | Fault | Check / remedy | Class | Seen |
|---|---|---|---|---|
| P000100 / 101 / 102 | Water outlet sensor open / short / short to +Ub | XB4 pins 9–10 (RD), NTC 10 kΩ @25 °C; else heater harness | 1 | |
| P00010A | Cool-down purge timeout: chamber not cool enough for a restart | Hot intake air? Flame sensor | 1 | |
| P000110 / 111 / 112 | Water inlet sensor open / short / short to +Ub | XB4 pins 5–6 (BU), NTC | 1 | |
| P000114 | Possible overheating: inlet/outlet difference implausible (only after outlet ≥ 80 °C) | As P000115, inlet sensor | 1 | |
| P000115 | Overheat, software threshold (outlet > 125 °C) | Leaks, flow direction, flow rate, bleed circuit, outlet sensor, pump | 5 | |
| P000116 | Overheat, hardware threshold (outlet > 130 °C) | As P000115 | 5 | |
| P00011A | Lockout: too many overheats | Fix cause, unlock | 6 | |
| P000120 / 121 / 122 | Flame sensor open / short / short to +Ub | XB4 pins 7–8 (BN), PT1000; else heater harness, then control unit | 1 | |
| P000125 | Flame out during start | Exhaust / combustion air path | 1 | |
| P000126–129 | Flame out in operation (power band 0–25 / 25–50 / 50–75 / 75–100 %) | Fuel quantity and supply, flame sensor. Up to 5 restarts; a successful one clears the display | 1 | |
| P00012A | Start failed | Exhaust / air path, fuel quantity and supply, fuel strainer | 4 | |
| P00012B | Lockout: too many failed starts | Unlock, fuel supply | 1 | |
| P000143 | Air pressure sensor implausible (< 598 or > 1106 hPa), limp mode | 24 V: clear; persists → control unit | 7 | |
| P000150 / 151 / 152 | PCB temperature sensor high / low / overtemperature | Control unit | 1 | |
| P000200 / 201 | Metering pump open / short | Harness, pump | 4 | |
| P000202 | Metering pump short to +Ub or transistor fault | Unplug pump: P000200 → pump defective | 4 | |
| P0002a1 | Water pump control/diagnosis pin open | Harness XB3 – XB8/2, pump | 5 | |
| P000210 / 211 / 212 | Glow plug open / short / short to +Ub | XB4 pins 3–4 (WH); 9.5 V → 9.5 A (+1/−1.5) after 25 s; **never above 9.5 V** | 1 | |
| P000213 | Glow plug energy too low | As P000210 | 1 | |
| P000220 / 221 / 222 | Blower motor open / short / short to +Ub or transistor | Contacts, dirt, corrosion, blocked wheel | 1 | P000222 (`28`, stored) |
| P000223 / 224 | Blower blocked / overcurrent | Free the wheel; else blower | 1 | |
| P000250 / 251 | Water pump open / short | Harness XB3 – XB8/2, pump | 5 | P000250 (pump unplugged, or bleeder on the wrong side) |
| P000252 | Water pump short to +Ub or transistor fault | Unplug pump: P000250 → pump defective | 5 | P000252 (start holds with the non-stock pump, see [README](../README.md#non-stock-water-pump-start-holds-p000252--p000250)) |
| P000253 | Water pump blocked | Kinked hoses | 5 | |
| P000254 | Water pump overcurrent | Dirty pump / circuit | 5 | |
| P000255 | Water pump below minimum speed | Dirty pump / circuit | 5 | |
| P000256 | Water pump dry running | Coolant level, bleed | 5 | |
| P000257 | Water pump overheating | Move away from hot parts | 5 | |
| P000258 | Water pump (ADR) under/overvoltage | Harness, pump | 5 | |
| P000259 | Water pump / vehicle blower (ADR) short | Wiring, blower relay | 5 | |
| P000260 / 262 | Universal output open / short to +Ub | Wiring, coding | 1 | |
| P000261 | Vehicle blower short | Motor cover; blower relay K1 | 1 | |
| P000300 | Overheat detection hardware or metering pump cut-off path | Outlet sensor, harness; unlock | 1 | |
| P000301 / 302 | Watchdog reset / too many | Clear; repeated → control unit | 1 | |
| P000303 | Lockout: too many output stage faults | Control unit | 1 | |
| P000304 | Too many resets (loose contact) | Control unit | 1 | |
| P000305 / 306 | Not calibrated / second cut-off path defective | Control unit | 1 | |
| P000307 | CAN communication error, control unit | Clear; repeated → CAN wiring | 1 | Controller silent mid-cycle (OTA, our stop) |
| P00030A | CAN communication error | As P000307 | 1 | |
| P000310 / 311 | Overvoltage ≥ 20 s: control unit / heater shut down | Supply at XB1, alternator regulator, battery | 3 | |
| P000312 / 313 | Undervoltage ≥ 20 s: control unit / heater shut down | Supply at XB1, fuses, ground, corrosion | 3 | |
| P000315 | Air pressure information implausible | Link to control unit; check it with EasyScan | 1 | |
| P000316 | Too little heat taken by the coolant (too many short runs) | Coolant circuit | 5 | |
| P000330 / 331 / 332 | Control unit defective | Control unit | 1 | |
| P000342 | Invalid configuration: too many CAN units (24 V ADR: one control unit only) | Configuration, CAN link | 1 | |
| P000394 | ADR switch short | Wiring, switch | 1 | |
| P000440 | *Not in the OEM table.* Seen with P000307 when the CAN control unit was lost while the heater was awake | Stays active (`2F`) until the heater sleeps; clear after that | | Yes (10-12) |
| P000500 | Error state entry; heating / ventilation continues | Withdraw the request, clear | 0 | |
| P000A00 | Heater ends communication: EasyFan not answering | Withdraw the request, clear | 0 | |
| P000E01 | Coded run-time limit reached | | 1 | |
