# Hydronic S3 — protocol research (2026-10-10)

## Summary
- No public Eberspächer CAN spec / DBC / leaked OEM doc found (searched EN/DE/RU, GitHub, Scribd, van forums).
- Heater interfaces: **CAN + S+ (switch-on)**. Compatible with EasyStart CAN control units (Pro, Remote+, Web).
- Two candidate protocols on the bus:
  1. **Proprietary EasyStart CAN**, 11-bit IDs, 500 kbit/s — decoded for Airtronic S3 (air heater) by TrooperDuper.
  2. **SAE J1939** — standard heater PGNs exist; Commercial line is used in trucks (Navistar lists SA 69 "Auxiliary Heater").
  First capture decides which (or both).
- Closed-source proof that temps are on the bus: UndermountAC "Espar CANbus Interface Module to WiFi"
  (HS3 etc.) reads temperatures, hour meter, faults; does on/off, schedule, altitude adjustment.

## J1939 (if present)
- **PGN 65133 (0xFE6D) Heater Information (HTR)**: SPN 1687 Aux Heater Output Coolant Temp, 1688 Input Air Temp,
  1689 Output Power Percent, 1677 Mode, 1676 Water Pump Status, 1678 Cab Ventilation, 1679 Engine Heating Zone,
  1680 Cab Heating Zone, 6821 ADR installation. Expected ID `0x18FE6D45` (prio 6, SA 69).
- Requests (likely in CM1, PGN 57344 — unverified): SPN 1683 Mode Request, 1684 Coolant Pump Request,
  1685/1686 Engine/Cab Zone Heating Request, 1690 Max Output Power.
- Bit layouts are in SAE J1939DA (paid). Verify against capture, don't trust memory.

## Airtronic S3 EasyStart CAN map (TrooperDuper, Airtronic S3 B2L 12 V only)
500 kbit/s, 11-bit, little-endian.

| ID | Dir | Rate | Content |
|---|---|---|---|
| 0x2C4 | heater→ctl | 100 ms | D1 state (02 startup, 03 idle, 08 init, 09 heating, 0B fault, 21 fan); D2 bit5 flame; D3 fault code (0x20 flame loss, 0x40 overtemp); D5–6 counter echo |
| 0x2C5 | heater→ctl | 100 ms | always 0 |
| 0x2C6 | heater→ctl | 100 ms | static `32 00 7C 01 …` (HW id) |
| 0x625 | heater→ctl | 100 ms | heartbeat `25 <toggle 00/10> <session> 86 01 54 5C 06` |
| 0x54 | ctl→heater | 200 ms | command: `00 00 FE FF ctr ctr` idle, `01 02 FE FF …` fan, `01 05 <setpoint u16le 0.1 °C> ctr ctr` heat |
| 0x55/0x56 | ctl→heater | 200 ms | static `10 27 …` (= 10000 → **hypothesis: 1000.0 hPa barometric pressure**, altitude comp.) |
| 0x57 | ctl→heater | 200 ms | `00 00 FE FF FE FF 00 00` |
| 0x60D | ctl→heater | 100 ms | controller heartbeat `0D 10 82 B6 09 20 70 00` |
| 0x65 | ctl→heater | 10 s | burst of 3, D3 cycles 01/02/03 |
| 0x5C–0x10A | ctl→heater | once | init burst until 0x625 seen |

- `FE FF` = J1939-style "not available" filler.
- Heater requires controller restart in sync with heater power for handshake.
- Diagnostic (EasyScan) request/response protocol not decoded.
- Expect differences for Hydronic: no fan mode, coolant temps, power level, water pump.

## Heater facts (service / repair manuals, cover D5L/D6L Commercial)
- Sensors (connector XB4): water inlet NTC (BU, pins 5–6), water outlet NTC (RD, 9–10), flame sensor (BN, 7–8),
  glow plug (WH, 3–4), PCB temp sensor, air-pressure sensor (altitude, valid 598–1106 hPa).
- Water NTC: 32.55 kΩ @0 °C, 10 kΩ @25, 5.33 kΩ @40, 2.49 kΩ @60, 1.26 kΩ @80, 677 Ω @100, 389 Ω @120, 183 Ω @150.
- Flame sensor (PTC-like): 1000 Ω @0 °C, 1097 @25, 1385 @100, 1758 @200.
- Overheat: outlet >125 °C SW (P000115), >130 °C HW (P000116); lockout after 10 overheats (P00011A).
- Control: start MAX when cold, modulate MAX↔MIN holding **75 °C outlet**, pause at **85 °C**, restart MIN at **70 °C**.
- Metering pump: EasyScan test 7 Hz × 60 s = 12.4 cm³ (D4E/D5E/D6L) → **~29.5 µl/stroke**.
  D6L 0.15–0.65 l/h → ~1.4–6.1 Hz (estimate).
- D6L: 1.3–5.6 kW heat, 5–37 W electrical, start ~135 W, 20.4–32 V.
- CAN harness parts: 120 Ω terminator, 9.2 kΩ "stub line" resistor, mating connector with terminator (XS6/1),
  EasyScan connector XB6/1. CAN pair colours BU/RD and BU/BK (H/L not labelled — verify).
- CAN faults: P000307/P00030A comm error, P000342 too many CAN components (ADR: only one control unit), P000A00 EasyFan comm.

## Sources
- https://github.com/TrooperDuper/espar-airtronic-esphome (protocol-reference.md)
- https://files.chartindustries.com/Hydronic_S3_Service.pdf
- https://eberspacher-russia.ru/repair-manual-hydronic-s3-25.2696.95.2682.0E-0920-ru.pdf
- https://mygenerator.com.au/assets/files/Hydronic_S3_tech_manual.pdf
- https://mygenerator.com.au/assets/files/Eberspacher_Hydronic_Product_Brochure.pdf
- https://static.nhtsa.gov/odi/tsbs/2026/MC-11031193-0001.pdf (Navistar Hydronic S3 Gen 3 diag guide)
- https://static.nhtsa.gov/odi/tsbs/2018/MC-10141869-9999.pdf (SA 69, aux heater SPNs)
- https://beta.isobus.net/isobus/pGNAndSPN/15778?type=PGN (PGN 65133 HTR)
- https://undermountac.com/products/eberspacher-espar-canbus-interface-module-to-wifi
- Unread (fetch blocked): fordtransitusaforum threads 101989 ("Spoofing a second EasyStart Pro") and 93875 ("Espar S3 CL Canbus control thermostat")
- https://docs.m5stack.com/en/module/commu, https://esphome.io/components/canbus/esp32_can/

## Hydronic S3 capture (2026-10-10, EasyStart Pro + tempcontrol S+)
EasyStart protocol, 11-bit, 500 kbit/s; **no 29-bit / J1939 traffic seen**. IDs as for the Airtronic S3
(054–057, 2C4–2C6, 60D, 625, init burst 05C–10A, 065).

| Field | Meaning | Confidence |
|---|---|---|
| `2C4` B4–B5 u16le | outlet coolant temp, (raw − 500) / 10 °C | confirmed: holds ~75 °C in regulation, drops with mixer open |
| `2C4` B0 | `03` idle (also seen during residual heat), `05` active (heating, control pause, after-run), `41`→`C1`→`03` end-of-run, `00` with `FE FF` fillers = one-cycle re-init | partly |
| `2C4` B1 bit 3 | burner run-down (~65–90 s), into control pause or at switch-off | 3 observations |
| `054` B0 / B1 | B0 `01` on / `00` off; B1 mode: `01` heat, `03` residual heat | consistent |
| `625` B1 | `10` while heater active, `00` idle | consistent |

Not in cyclic traffic: power level, pump rate, inlet temp, glow plug, flame (glow/ignition/restart from
pause change no byte). S+ (switch input) status bit: not identified yet (test was confounded).

**Diagnostics: UDS (ISO 14229) over ISO-TP (ISO 15765-2)**, tester `0x7A0` → heater `0x73C`, padding `AA`,
flow control `30 08 14` (BS 8, STmin 20 ms). Example: `19 02 08` (ReadDTCInformation by status mask) →
`59 02 08 | 00 02 52 2C | 00 02 22 28` = P000252 (status 2C), P000222 (status 28). Live values should be
readable via `22` ReadDataByIdentifier — DIDs unknown, needs TX (safety review first).

## DTC meanings (OEM sources, web research 2026-10-10)
From Eberspächer "Repair Instructions Hydronic S3 Economy 12V CS / Commercial 24V CS"
(https://files.chartindustries.com/Hydronic_S3_Service.pdf, covers D6L 24 V CS 25.2745.05.0000) and Navistar TSB
IK1900301 (https://static.nhtsa.gov/odi/tsbs/2026/MC-11031193-0001.pdf):
- **P000222** = combustion-air blower motor: short circuit to +Ub or transistor error.
- **P000252** = water pump: short circuit to +Ub or transistor error.
- Scheme P000 + group + index: 20x metering pump, 21x glow plug, 22x blower (0 open, 1 short, 2 short +Ub,
  3 blocked, 4 overcurrent), 25x water pump (… 5 below min speed, 6 dry running, 7 overheating), 12x flame/start,
  3xx control box / voltage / CAN (P000342 "too many CAN components"). Control box stores up to 10 faults.
- No public source for 0x7A0/0x73C or UDS DIDs. No evidence the S3 emits J1939 HTR (PGN 65133).
- P000342 implies a third-party controller must replace the EasyStart, not join as an extra node.

## S+ start behaviour (2026-10-10 evening, EasyStart idle/off, tempcontrol relay on S+)
- S+ on wakes the bus: heater boot frame (`2C4` state `00`, B4–B5 `FE FF`), ~0.25 s later state `03`,
  **`625` B1 `00 → 10`** (switch-on request seen). `625` B1 drops to `00` within ~0.6 s of S+ off.
- Failed starts (3 of 4 attempts, warm and cold): **`2C4` B2 = `0x40` exactly 1.7 s after the request**, heater
  stays idle (`03`), no ignition, for minutes. Cleared together with the request. EasyStart `054` = `00 00`
  (off) throughout. One OFF/ON toggle once led to a start, once not — matches tempcontrol's retry habit.
- On the Hydronic `0x40` is therefore "start held/refused", not the Airtronic overtemperature code.
- Open: cause of the hold. User reports a DTC in the error memory after failed starts → read it (passive decode
  on `0x73C` is in place: "Heater Fault Codes"). Hypothesis to test: conflict between S+ and the EasyStart's
  active "off" command.
