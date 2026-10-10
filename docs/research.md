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

### Root-cause lead: water pump fault P000252 (21:27–21:29, own UDS reads via "Read Fault Codes")
- Read 0.1 s after wake (before self-test): `P000252 (6C), P000222 (68)` — 0x40 "test not completed this cycle".
- Read during the start hold (`0x40` flag set): **`P000252 (2F)`** = testFailed + testFailedThisOperationCycle +
  pending + confirmed; `P000222 (28)` old, not failing. → The hold coincides with an active water-pump fault.
- OEM remedy (Chart repair instructions): unplug XB8/2 at the pump; if the code changes to P000250 (open) the
  pump is defective → renew; else check harness / heater output. Also check for external +Ub back-feed onto the
  pump line (P000252 = "short circuit downstream of +Ub or transistor error").
- In a hold the heater stays on the bus as long as S+ is on (> 3 min seen) and answers UDS: safe diagnostic
  window without ignition.

## UDS DID scan (2026-10-10 22:0x, raw captures, default session, no security access)
Range scans via the component's `Scan DID Range` (22 <DID> on 0x7A0, answer on 0x73C). All negative
answers were NRC 0x31 (requestOutOfRange). Service 0x21 (local identifiers): NRC 0x11 for all 256.
Scanned: 0000–0FFF (none), F100–F17F (none), F180–F19F (F18B F193 F195), F1A0–F1FF, FD00–FEFF.

Positive answers (DID: data bytes, ASCII):
```
F1F0: 25 27 45 01 50 01 30 41 '%'E.P.0A'
F1F1: 1D E3 E6 EC '....'
F1F2: 00 05 01 00 00 01 '......'
F1F3: 19 04 09 01 00 54 96 00 00 '.....T...'
F1F4: 30 34 2E 30 32 2E 30 30 '04.02.00'
F1F5: xx xx xx xx xx xx xx xx xx xx xx xx xx xx '<serial>'
FD00: 00 00 '..'
FD01: 30 3E 31 06 30 FC '0>1.0.'
FD02: 00 '.'
FD04: 01 04 '..'
FD05: 00 '.'
FD06: 1D 42 '.B'
FD07: 00 '.'
FD08: 4E 20 'N '
FD09: 00 00 '..'
FD0A: 00 00 '..'
FD0B: 00 00 '..'
FD0C: 00 '.'
FD0D: 00 '.'
FD0E: 00 00 '..'
FD0F: 00 01 75 30 75 3F 7F FC 7F FC '..u0u?....'
FD10: 01 28 00 01 F5 00 1B 59 '.(.....Y'
FD11: 00 00 00 00 01 2F 0B B0 E9 1E '...../....'
FD12: 0A '.'
FD13: 07 D0 07 D0 09 C4 09 C4 01 40 01 40 00 CC 00 CC '.........@.@....'
FD14: 0B B8 00 14 '....'
FD15: 01 '.'
FD16: 02 DA 64 14 '..d.'
FD17: 00 01 A9 24 00 00 A7 0D 00 00 2F E9 00 00 0A B9 00 00 0F 4B 00 00 3F F4 00 00 59 3A '...$....../........K..?...Y:'
FD18: 05 '.'
FD19: ''
FD1A: 0F 63 0D EF '.c..'
FD1B: 00 00 05 11 00 13 '......'
FD1C: 00 00 00 00 '....'
FD1D: 01 '.'
FD1E: 01 FE 0D 50 00 15 '...P..'
FD1F: 03 E8 03 E8 '....'
FD21: 27 10 ''.'
FD22: 02 03 '..'
FD23: ''
FD24: 00 00 '..'
FD25: 00 F0 '..'
FD26: 00 F0 03 '...'
FD27: 00 00 00 00 00 05 00 09 '........'
FD29: 1E 14 '..'
FD30: 01 '.'
FD33: 02 46 64 47 5F 48 5A 49 50 4A 46 4B 3C 4C 00 '.FdG_HZIPJFK<L.'
FD34: ''
FD35: 27 D8 ''.'
FD36: 26 D4 '&.'
FD37: 01 00 00 00 '....'
FD38: 01 '.'
FD39: 00 '.'
FD3A: 00 '.'
FD3D: 00 00 00 00 '....'
FD3E: 01 '.'
FD3F: 25 '%'
FD41: ''
FD44: 23 32 '#2'
FEFF: 00 00 00 00 00 00 00 49 30 C0 30 CA 2F A8 FF FF 00 08 00 00 00 4C 00 00 00 00 00 00 65 F8 00 00 27 C4 64 BC 00 00 '.......I0.0./........L......e...'.d...'
```

Repeated `22 FE FF` reads with the CAN outlet temperature (0x2C4 B4–B5) last published before each read:
```
22:05:53.191  outlet(CAN) 75.0 °C  FEFF: 00 00 00 00 00 00 00 49 30 C0 30 CA 2F A8 FF FF 00 08 00 00 00 4C 00 00 00 00 00 00 65 F8 00 00 27 C4 64 BC 00 00 '.......I0.0./........L......e...'.d...'
22:08:26.587  outlet(CAN) 70.5 °C  FEFF: 00 00 00 00 00 00 00 30 2F 12 2F 12 2D F0 FF FF 00 08 00 00 00 4E 00 00 60 E0 02 6C 62 75 00 00 27 A6 64 1C 00 00 '.......0/./.-........N..`..lbu..'.d...'
22:08:33.751  outlet(CAN) 70.4 °C  FEFF: 00 00 00 00 00 00 00 30 2E FE 2E FE 2D BE FF FF 00 08 00 00 00 4D 00 00 60 E0 02 62 65 BF 00 00 27 BA 63 FC 00 00 '.......0....-........M..`..be...'.c...'
22:08:40.824  outlet(CAN) 70.2 °C  FEFF: 00 00 00 00 00 00 00 30 2E F4 2E EA 2D 8C FF FF 00 08 00 00 00 50 00 00 60 E0 02 62 62 6D 00 00 29 CC 62 9C 00 00 '.......0....-........P..`..bbm..).b...'
22:08:47.896  outlet(CAN) 70.2 °C  FEFF: 00 00 00 00 00 00 00 30 2E E0 2E F4 2E 54 FF FF 00 08 00 35 00 4B 00 00 61 44 02 62 62 5D 00 00 29 72 62 BC 00 00 '.......0.....T.....5.K..aD.bb]..)rb...'
22:08:54.869  outlet(CAN) 70.2 °C  FEFF: 00 00 00 00 00 00 00 30 2E D6 2F 08 2F A8 FF FF 00 08 00 28 00 45 00 00 60 E0 02 58 62 95 00 00 29 0E 62 BC 00 00 '.......0.././......(.E..`..Xb...).b...'
22:09:02.022  outlet(CAN) 71.2 °C  FEFF: 00 00 00 00 00 00 00 30 2E C2 2F 6C 33 86 FF FF 00 08 00 23 00 4E 00 00 60 E0 02 58 62 6D 00 00 29 0E 63 7C 00 00 '.......0../l3......#.N..`..Xbm..).c|..'
```

DTC extended data (19 06 00 02 52 FF, 22:00:49):
```
59 06 00 02 52 2E 01 69 00 01 A9 03 1D 3C 00 15 00 00 59 3A 26 D4 01 06 00 00 00 00 00 00 00 00 23 78 23 82 22 9C 06 DF 05
```
