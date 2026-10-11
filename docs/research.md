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
| `625` B1 | `10` while awake (also with S+ off after power-up), `00` before sleep | revised 10-11 |

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

## DID correlation, regulation → burner stop → run-down → pause (2026-10-10 22:19–22:26)
Poll FEFF + FD00..FD44 every 5 s, `2C4` state 05 throughout; burner stopped ~22:21:45 (outlet peak 83 °C).
Decodes (C = confirmed against CAN outlet / phase timing, ? = candidate):

| DID | Decode | Observed |
|---|---|---|
| FD01 | 3× u16 BE, °C = v/100 − 50: [flame/exhaust sensor?, inlet (C), outlet (C)] | outlet = `2C4` ±0.1; inlet ~3 K below while burning, = outlet in pause; w1 133 °C burning → 71 °C |
| FD10 B1 | phase (C): 0x14 burn, 0x1F run-down (~90 s), 0x28 pause | B5 sub-phase, B6–7 timer 0.01 s (0x1B59 when idle) |
| FD09 | blower rpm? | ~2000 burn, 2900 run-down, 0 pause |
| FD04 | supply V ×0.1 (C: 26.0 vs. 26.14 V multimeter) | 26.0/26.1 V, 25.1 V in run-down |
| FD0A, FD0C | glow plug? | non-zero only in run-down (0x0351, 0x34) |
| FD0D | metering pump ×0.1 Hz? | 0x0E burn → 0; earlier 0x11–0x17 |
| FD11 | B1 0x29 → 00 at stop; B6–9 counter, counts only while burning | |
| FD17 | 7× u32 BE counters; d1 total minutes (C, +1/min, 108856); d2 burn-only; d6 pause-only | |
| FD08 | ? | ~23100 burn, ~24650 run-down, 20000 pause |
| FD0F | ? | burn `01 03 7531 753F 81E1 8194`, else `00 01 75xx 753F 7FFC 7FFC` |

Raw (every 3rd cycle):
```
t        out   FD01 FD04 FD08 FD09 FD0A FD0C FD0D FD0F FD10 FD11 FD17
22:19:54 77.3 4826304831BA 0105 5A16 07CC 0000 00 0E 01037534753F81E18194 01140001F5001B59 00290000012F0C8916D6 0001A9320000A70F00002FE900000AB900000F4B00003FFC0000593A
22:20:09 78.2 46BE30DE3214 0104 5A15 07CC 0000 00 0E 01037531753F81FB8194 01140001F5001B59 00290000012F0C9285FE 0001A9320000A70F00002FE900000AB900000F4B00003FFC0000593A
22:20:24 79 468231423264 0104 5A37 07DB 0000 00 0E 01037531753F82088194 01140001F5001B59 00290000012F0C9BEEBE 0001A9330000A70F00002FE900000AB900000F4B00003FFC0000593A
22:20:39 79.7 4682319C32B4 0104 5A38 07DB 0000 00 0E 01037531753F81F48194 01140001F5001B59 00290000012F0CA55116 0001A9330000A71000002FE900000AB900000F4B00003FFC0000593A
22:20:54 80.5 46F031EC3304 0105 5A36 07DB 0000 00 0E 01037531753F82118194 01140001F5001B59 00290000012F0CAEBD0A 0001A9330000A71000002FE900000AB900000F4B00003FFC0000593A
22:21:09 81.2 472232323354 0104 5A3B 07DB 0000 00 0E 01037531753F820E8194 01140001F5001B59 00290000012F0CB81F62 0001A9330000A71000002FE900000AB900000F4B00003FFC0000593A
22:21:24 82 47903278339A 0104 5A3A 07DB 0000 00 0E 01037531753F821F8194 01140001F5001B59 00290000012F0CC18688 0001A9340000A71000002FE900000AB900000F4B00003FFC0000593A
22:21:39 82.7 486232C833F4 00FA 5A6A 0864 034E 30 00 0001751E753F7FFC7FFC 011F0001F50100AE 00000000012F0CCCEAFA 0001A9340000A71100002FE900000AB900000F4B00003FFC0000593A
22:21:59 82.5 4510330433C2 00FB 5EF6 0B0C 0344 35 00 00017525753F7FFC7FFC 011F0001F501068E 00000000012F0CCCEAFA 0001A9340000A71100002FE900000AB900000F4B00003FFC0000593A
22:22:14 82 4150332C3390 0104 604B 0B54 0000 00 00 00017529753F7FFC7FFC 011F0001F5020499 00000000012F0CCCEAFA 0001A9350000A71100002FE900000AB900000F4B00003FFC0000593A
22:22:29 81.5 3E8033183354 0104 6050 0B54 0000 00 00 0001752B753F7FFC7FFC 011F0001F5020A79 00000000012F0CCCEAFA 0001A9350000A71100002FE900000AB900000F4B00003FFC0000593A
22:22:44 81 3B7E32F03322 0104 6053 0B54 0000 00 00 0001752C753F7FFC7FFC 011F0001F5021057 00000000012F0CCCEAFA 0001A9350000A71100002FE900000AB900000F4B00003FFC0000593A
22:22:59 80.4 391232C832E6 0104 6056 0B54 0000 00 00 0001752D753F7FFC7FFC 011F0001F5021633 00000000012F0CCCEAFA 0001A9350000A71100002FE900000AB900000F4B00003FFC0000593A
22:23:14 79.7 374632A032AA 0104 4E20 0000 0000 00 00 0001752E753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9360000A71100002FE900000AB900000F4B00003FFC0000593A
22:23:29 79 34E432643264 0104 4E20 0000 0000 00 00 00017530753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9360000A71100002FE900000AB900000F4B00003FFC0000593A
22:23:44 78.5 335432323232 0104 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9360000A71100002FE900000AB900000F4B00003FFC0000593A
22:23:59 78 325A32003200 0104 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9360000A71100002FE900000AB900000F4B00003FFC0000593A
22:24:14 77.5 316A31CE31CE 0104 4E20 0000 0000 00 00 00017530753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9370000A71100002FE900000AB900000F4B00003FFD0000593A
22:24:29 77 30D4319C3192 0105 4E20 0000 0000 00 00 00017530753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9370000A71100002FE900000AB900000F4B00003FFD0000593A
22:24:44 76.5 303E31603160 0104 4E20 0000 0000 00 00 00017530753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9370000A71100002FE900000AB900000F4B00003FFD0000593A
22:24:59 76 303E31383138 0105 4E20 0000 0000 00 00 00017530753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9370000A71100002FE900000AB900000F4B00003FFD0000593A
22:25:14 75.5 2FA830FC3106 0105 4E20 0000 0000 00 00 00017530753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9380000A71100002FE900000AB900000F4B00003FFE0000593A
22:25:29 75 2F8030DE30DE 0105 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9380000A71100002FE900000AB900000F4B00003FFE0000593A
22:25:44 74.7 2F8030B630AC 0104 4E20 0000 0000 00 00 00017530753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9380000A71100002FE900000AB900000F4B00003FFE0000593A
22:25:59 74.2 2F4E308E3084 0105 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9380000A71100002FE900000AB900000F4B00003FFE0000593A
22:26:19 73.7 -- 0104 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9390000A71100002FE900000AB900000F4B00003FFF0000593A
22:26:34 73.6 2F1C3034302A 0105 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9390000A71100002FE900000AB900000F4B00003FFF0000593A
22:26:49 73 2E86300C300C 0104 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9390000A71100002FE900000AB900000F4B00003FFF0000593A
22:27:04 72.7 2E862FE42FE4 0104 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA 0001A9390000A71100002FE900000AB900000F4B00003FFF0000593A
```

### Restart from pause (22:28:19 → 22:29:55, good start, no hold)
- 22:28:19 FD10 B1 0x0C (start), B2 sub-step 01…12, B3–4 timer. FD0A ~0x03B2 + FD0C ramp 0x27→0x3E (glow plug, 40 s
  preheat); FD04 0x0104→0x00F6 at glow-on.
- 22:29:00 FD09 1400 rising (blower), FD0D 0x19 (metering pump), FD11 counter starts, FD11 B1 0x49, FD0F B1 01→02.
- 22:29:15 FD01 w1 rises 66→141 °C, FD0F B0 00→01 (flame).
- 22:29:55 FD10 0x14; FD0A/FD0C → 0; FD0D 0x16→0x0E; FD11 B1 0x40→0x29 (FD11 B1 / FD0D ≈ 2.9 throughout).
Now confirmed: FD0A/FD0C glow plug, FD04 supply V, FD0D metering pump ×0.1 Hz, FD01 w1 flame sensor, FD0F B0 flame.
Water pump: no DID identified yet.
```
t        out   FD01 FD04 FD08 FD09 FD0A FD0C FD0D FD0F FD10 FD11
22:28:14 None 2DBE2F582F58 0104 4E20 0000 0000 00 00 00017531753F7FFC7FFC 01280001F5001B59 00000000012F0CCCEAFA
22:28:19 None 2D8C2F4E2F4E 00F6 4E20 0000 03B2 27 00 00017531753F810181CE 010C010060001B59 00000000012F0CCCEAFA
22:28:24 71 2DF02F442F44 0104 4E20 0000 03B3 31 00 00017531753F80F481CE 010C03005F001B59 00000000012F0CCCEAFA
22:28:29 71 2D8C2F302F26 0104 4E20 0000 03B5 3A 00 00017531753F80FD81CE 010C0401F0001B59 00000000012F0CCCEAFA
22:28:40 70.7 2DBE2F262F26 0105 4E20 0000 03A6 3B 00 00017531753F80F481CE 010C0403E5001B59 00000000012F0CCCEAFA
22:28:45 70.7 2D8C2F1C2F12 00FB 4E20 0000 03A9 3D 00 00017531753F80F481CC 010C0407CE001B59 00000000012F0CCCEAFA
22:28:55 70.5 2D8C2F122F12 0104 4E20 0000 03B5 3D 00 00017531753F810881CC 010C0409C2001B59 --
22:29:00 70.5 2DBE2F082F12 00FB 55FF 0578 03B4 3E 19 00027554753F80EB81CC 010C070020001B59 00490000012F0CCDE824
22:29:05 70.3 2E222EFE2F12 00FB 5634 0581 03B4 3E 18 00027551753F812581CC 010C09014D001B59 00460000012F0CD35A66
22:29:10 70.3 2EB82EFE2F12 00FC 5793 069E 03A8 3E 18 0002754F753F816C81CC 010C0A014F001B59 00460000012F0CD8B9CA
22:29:15 70.7 303E2EF42F30 00FB 5A5B 0847 03B3 3E 18 0102754975458191811A 010C0B00EB001B59 00460000012F0CDE13B6
22:29:20 71.5 33542EEA2F8A 0104 -- 096E 03B3 3E 18 0103753E7545823C811A 010C0E00E9001B59 00460000012F0CE36DA2
22:29:25 72.5 37782EEA2FE4 0104 5E40 0A86 03B3 3F 18 0103753575458299811A 010C0F00E7001B59 00460000012F0CE8C216
22:29:30 73.5 3C782EEA303E 00FB 606E 0B76 0381 3D 18 01037531754582D1811A 010C100020001B59 00460000012F0CEE1EBE
22:29:35 74.2 40882EF43098 0104 60B2 0B7E 0381 3C 18 0103752D754582F5811A 010C100215001B59 00460000012F0CF378AA
22:29:40 75 46BE2F3A3106 00FB 60A1 0BA4 0314 34 16 01037531754482A9811A 010C110020001B59 00400000012F0CFDC58A
22:29:50 76 48622F6C3138 00FB 60A6 0B8D 01F0 21 16 01037532754482A4811A 010C12001A001B59 00400000012F0D029B0A
22:29:55 76.5 49DE2F9E316A 0104 60AE 0BEB 0000 00 15 01037531754482878194 01140001F5001B59 003D0000012F0D0782F0
22:30:00 76.7 4A7E2FDA319C 0104 5B80 0862 0000 00 0E 0103752B753F82578194 01140001F5001B59 00290000012F0D0B73B0
22:30:05 77.1 4A1A300C31A6 0104 5A2A 07D0 0000 00 0E 01037532753F81D88194 01140001F5001B59 00290000012F0D0E9612
22:30:10 77.1 48D0304831C4 0104 5A26 07D0 0000 00 0E 01037533753F81E48194 01140001F5001B59 00290000012F0D11B3A6
22:30:15 77.7 47F4308431E2 0104 5A28 07D0 0000 00 0E 01037533753F81FC8194 01140001F5001B59 00290000012F0D14D608
22:30:20 78 475E30B63200 0104 5A26 07C8 0000 00 0E 01037531753F81FD8194 01140001F5001B59 00290000012F0D17FA04
```

### Fuel: FD11 decoded (22:19–22:52, 4 burn periods)
- B6–9 counter rate / FD0D Hz = 29280 ±0.5 % at 1.4 and 2.4 Hz → stroke-proportional. B1 = 29.3 × Hz (41 @ 1.4, 70 @ 2.4).
- Units: B1 µl/s, B6–9 nl → B1 41 = 0.148 l/h at the regulated minimum; D6L manual minimum load: 1300 W / 0.15 l/h (C).
- **Metering pump stroke ≈ 29.3 µl.** Max 5600 W / 0.65 l/h → ~6.2 Hz (activation range 0.1–10 Hz).
- B2–5 = 303 = total fuel litres; B6–9 the nl fraction (rollover at 1e9 assumed).
- Heating power sensor = fuel × 8615 W per l/h (D6L datasheet 5600 W/0.65 l/h, 1300 W/0.15 l/h; datasheet heat
  flow, not measured). Web: no published pump frequency/stroke volume; manual: technical description Hydronic S3
  Commercial 25.2696.90.0001 (butlertechnik.com copy).
- Cross-check (user-supplied repair-manual table, Hydronic S3 Economy D4E/D5E, first start at 12–13 V): 7.0 cm³
  (6.65–7.35) in 86 s; EasyScan fuel test 8.2 cm³ (7.4–9.0) in 80 s. Our D6L warm restart 22:28 (24.6–26 V): FD11
  counter +4.30e6 in the first 65 s of pumping → ≈5.2 cm³ per 86 s. Confirms nl as counter unit (not stroke-exact:
  other type, start type, voltage).
- **Confirmed** by the repair manual covering D6L 24 V CS (Reparaturanleitung Hydronic S3 CS 25.2696.95.2682.0E,
  09.2020, §3.5.11): EasyScan pump test 7 Hz × 60 s = 12.4 cm³ (11.2–13.6) for D4E/D5E/D6L → **29.5 µl/stroke**
  (26.7–32.4); FD11-derived 29.3 µl. Tech data D6L: 5600/1300 W, 0.65/0.15 l/h, 37/5 W, 20.4–32 V.

### Water pump (Flowtronic 1400, 25.2435.99.2701; 24 V, max 35 W, 0.15 bar, 1400 l/h)
- Repair-manual DTCs: P000250/251 open/short, P000252 short to +Ub or transistor, P000253 blockage, P000254
  overcurrent shutdown, P000255 below minimum speed, P000256 dry run, P000257 overheating → pump reports status/speed
  (smart pump). Pump power DID not yet found
  (FD08 candidate: 20000 pause, ~23100 burn, ~24650 run-down).

### Regulation observed (6 burns 22:19–23:15, loop throttled, no high load)
Start fuel 70–73 µl/s (~39 % ≈ 2.2 kW) until outlet ≈75 °C; steps 67/64/61 at 75–77 °C; minimum 41 µl/s (1300 W)
from ≈77 °C; pause at ≈83 °C; restart at ≈70.5 °C. Never above 40 % → upper regulation unseen. FD33
(`02 46 64 47 5F 48 5A 49 50 4A 46 4B 3C 4C 00` = pairs 70/100 … 76/0?) does not match this as outlet→% curve; open.

### Identification (text sensors)
F193 HW 03.04.02, F195 SW 07.04.04, F1F0 BCD part 25.2745.01.5001 (D6L order no. 25.2745.05.0000), F1F5 serial
<serial>. F1F3 `19 04 09 …` (date 2019-04-09?), F1F4 "04.02.00".

### DTC extended data (19 06 <DTC> FF), 23:23, raw
```
P000252: 59 06 00 02 52 2E 01 69 00 01 A9 03 1D 3C 00 15 00 00 59 3A 26 D4 01 06 00 00 00 00 00 00 00 00 23 78 23 82 22 9C 1A 01 05
P000222: 59 06 00 02 22 28 01 06 00 01 9B 78 1C A1 00 14 00 00 56 54 1F B8 00 F4 00 00 00 01 F5 00 00 01 2D 14 2D 3C 32 F0 1A 06 01
```
Candidate layout after status: record 01, occurrence count (0x69 = 105 / 6), operating minutes u32 (FD17 d1 format:
108803 = 110 min before now / 105336), u16 (FD06-like), u16, u32 (FD17 d7-like), u16 (FD36-like), u16, 8 bytes
(FD10-like for P000222), 3× temp (FD01 format: 40.8/40.9/38.6 °C resp. 65.4/65.8/80.4 °C), 0x1A (26 V?), 2 bytes.

### High-power run (23:29–23:38, tempcontrol HEAT, valve 100 %, 15 °C water in; then climate OFF, S+ off)
- Start from pause 23:29:43, inlet fell to 24 °C. Burn 23:31:24; ramp 76→179 µl/s in 20 s. **179 µl/s = 0.644 l/h
  ≈ 5550 W** = datasheet max (0.65 l/h, 5600 W) → fuel units and 29.3–29.5 µl stroke hold at full load too.
- Full load: FD09 0x180A = 6154 (blower rpm, C: range 500–11000; ~2000 at min), FD08 ≈ 29950 (open), FD35 rising
  11300→11730 (falls in pause: temperature-like, candidate control unit / heat exchanger), FD0F w3 0x83F5.
- ΔT outlet−inlet ≈ 20 K at 5.55 kW → water flow ≈ 240–280 l/h (below manual minimum 300 l/h).
- **Regulation (C, FD33 = outlet °C → % pairs 70/100 71/95 72/90 73/80 74/70 75/60 76/0, min 1300 W):** full power
  below 70 °C; 159 @70.4, 141 @71.3, 123 @72.0, 106 @74.4, 76 @75.2, 47 @75.7, 41 (min) @76.2 °C. Intermediate
  values below the table (controller dynamics). Control pause at ≈83 °C, restart ≈70.5 °C.
- Phases FD10 B1: 00 off, 0C start, 14 burn, 1E run-down after switch-off, 1F run-down to control pause, 28 pause.
  FD10 B0 01→00 at switch-off (request flag?).
- Glow plug after every flame-off (run-down 0x1E/0x1F): ~89→74 W (FD0C 0x36→0x2D) for ~45 s; 0 while burning.
- Counters: FD1E w2 = starts (+1 per start; 3416), FD06 = +1 at every start and every burner stop.

## Power-up wake (2026-10-11 09:30, heater supply on, S+ relay off, EasyStart idle)
Wake trace (first frames after silence, Fire listen-only until `2C4`):
- +0 ms `625` `25 00 D8 CD C7 BB EC 01`, +6 ms `2C4` boot (`00`, `FE FF`), +34–64 ms `2C5` `2C6` `60D`,
  +76–121 ms EasyStart `054` (`00 00` off) `055` `056` `057`, +143 ms `065`. EasyStart is powered with the heater.
- +213 ms `2C4` state `03`; **+357 ms `625` B1 `00 → 10`, B2 `D8 → D9` with S+ off** → B1 `10` is not the
  S+ request (earlier hypothesis wrong); likely "awake/active" of the `625` sender. +2 s init burst `05C`–`10A`.
- UDS answers from +209 ms (polling during boot works). No start, no flags, phase 0.

## Bus sleep and wake-up (2026-10-11)
- **Our UDS polling keeps the heater awake**: `625` B1 `10` while requests come; 0.7 s after polling stops
  B1 → `00`, ~2 s later the bus is silent (heater and EasyStart asleep). → poll only while the heater is active
  (`2C4` state ≠ 00/03 or B1 bit 3 run-down), plus 2 sweeps after each state change; listen-only otherwise.
- **EasyStart wake-up = CAN**: it sends `60D` (its 100 ms keep-alive) unacked; the MCP2515 sees the retries
  every ~11 ms for ~250 ms until the heater's transceiver wakes and acks (heater `625`/`2C4` boot follows).
- **The EasyStart wakes on bus activity too**: our own `60D`/`054` (research command, bus asleep) woke it within
  30 ms; it sent `054 00 00` (off). → a third-party controller must replace the EasyStart (disconnect it or
  drive it through its own interface), never join it. Component backs off on any `054`–`057`, `065`, `60D`.

## EasyStart start (09:32, from sleep, S+ off) and 10 min run
- `054` `00 00 → 01 01` 0.4 s after the heater boot, `625` B1 `10` at +1.4 s, `2C4` `03 → 05` at +1.6 s.
  **No start hold** (S+ starts 10-10: held at +1.7 s, `2C4` B2 `40`).
- Phases: **`0A` start (EasyStart)** with glow ~94 W, blower 0 → flame at +64 s (glow on, blower 2643 rpm,
  pump 2.4 Hz, 0.25 l/h) → `14` burn at +2 min (glow off). S+ starts used `0C`.
- **Run duration is not on the bus**: `054`–`057` constant `01 01 …` for 10 min, then `054 00 00` exactly at
  10:00.0. Timer is in the EasyStart. Config burst `05C`–`10A` (4× at power-up only, unchanged at the start):
  `05C`/`05F` `1E 00 1E 00` = 30 (EasyStart default heating time 30 min?), no `0A`. Open: heater-side max run
  time (hold `054` on > 30 min with the EasyStart disconnected).
- Switch-off: `054 → 00 00`, `2C4` `05 → 03` with B1 `08` (run-down) at once, flame off, afterglow ~90 W,
  phase `1E`; run-down ends after 95 s (`2C4` B1 → `00`, phase `00`).

## Without the EasyStart (2026-10-11 ~10:00, EasyStart disconnected, bus unterminated: REC/TEC 0)
- Heater alone sends `625`, `2C4`, `2C5`, `2C6` only → `60D`, `054`–`057`, `065`, `05C`–`10A` are the EasyStart's.
- **Someone must ack**: with the Fire listen-only the heater retried its first `625` forever (~3800 fps), never
  reached `2C4`. Component now acks (normal mode) whenever no EasyStart was seen for 60 s.
- Our `60D`/`054` wake it; DTC read: no P000342 → it accepts us as the only controller.

## Start hold = P000252 detection (2026-10-11)
| Time | Start | Water | Result |
|---|---|---|---|
| 09:32 | EasyStart (off until state 03, then `054 01 01`) | 13 °C | start |
| 10:00 | S+ alone (no `054` on the bus) | ~56 °C | start (phase `0C`) |
| 10:08, 10:11 | ours: `054 01 01` from the first frame (heater still booting, state 00), no `065` | 70–72 °C | **hold**, `2C4` B2 `40`, phase `C8`, P000252 (2F) |
| 10:12 | S+ alone | 70 °C | start, flame +60 s |
- `19 06 00 02 52 FF`: occurrence count 0x69 (105, 10-10) → **0x6B (107)**: +2 = exactly our two holds; good starts do
  not count. Freeze-frame temps 70.3/70.8/64.7 °C (10-10: 40.8/40.9/38.6 °C) → not temperature.
  Start/stop counter 0x1D3C → 0x1D57. `19 04` (snapshot): NRC 12.
- Common to all holds (incl. 10-10 S+ holds with the EasyStart sending `054 00 00`): an unexpected `054` while the
  heater starts. Next: our start with the EasyStart sequence (off until state 03, `065` every 3 s).
- Hold clears with the request (S+ off / `054` off); phase `C8` = hold.

## Start hold: alternating S+ / CAN series (2026-10-11 10:44–10:51)
EasyStart disconnected, Fire acks, loop ~50–52 °C. Each trial from sleep, stopped at the hold or 4 s after `2C4`
state 05 (glow only, no fuel). CAN = our command (EasyStart sequence, config burst replayed once per boot).

| # | Source | Result | # | Source | Result |
|---|---|---|---|---|---|
| 1 | S+ | **hold** | 2 | CAN | start |
| 3 | S+ | start | 4 | CAN | start |
| 5 | S+ | start | 6 | CAN | **hold** |
| 7 | S+ | start | 8 | CAN | **hold** |
| 9 | S+ | start | 10 | CAN | start |
| 11 | S+ | start | 12 | CAN | start |

- **S+ 1/6, CAN 2/6 holds**: no significant difference. P000252 count 112 → 115 (+3, one per hold).
- With the earlier starts (holds at 67.9 °C, start at 67.5 °C): **neither start source nor loop temperature
  decides**. Two hold patterns: (A) state 05 → pause 0x28 → hold, (B) state stays 03, FD10 B0 = 0 (on request not
  yet registered) → hold ~2 s after wake: the pump output check runs at wake.
- Cause (user): **DC/DC converter in front of the (non-stock) pump** — its input capacitance holds the line up
  when the heater's PWM/output is off → "short to +Ub". Planned fix: Schottky diode in series (heater → converter),
  optional bleeder on the heater side. Firmware: automatic retry (3×, 5 s off), verified 10:34.
- P000307/P000440 counts unchanged (1) after 6 CAN stops → continued "off" until idle + 15 s fixes P000307.

## Start sequence and DID decodes (offline, 10:12 S+ and 10:23 CAN warm starts)
- **FD10**: B0 on request, B1 phase, **B2 start sub-step**, **B3–B4 timer in 10 ms** within the sub-step
  (step 04: 401 → 902 → 1403 per 5 s). Run-down: B5 sub-step, B6–B7 timer. Same sequence for S+ and CAN:

| Step | from state 05 | What happens |
|---|---|---|
| 01, 03, 04 (05) | 0–43 s | glow ~94 W, blower 0, no fuel (pre-glow) |
| 06 / 08 | +45–48 s | fuel on (0.09 → 0.25 l/h, 2.5 Hz), blower ~1450 rpm |
| 09, 0A | +50–58 s | blower ramps |
| 0B | +60–63 s | flame (FD0F B0 = 1, flame sensor rising) |
| 0E, 0F, 10 | +65–80 s | blower → ~3000 rpm, glow 94 → 89 W |
| 11, 12 | +90–97 s | glow 79 → 49 W |
| phase 14 | +100 s | burn, glow off, regulation |

- **FD0F B1**: combustion state 01 pre-glow, 02 fuel on, 03 flame. B2–B5: two u16 around 30000 (±50), change at
  fuel-on and flame, meaning open.
- **FD08 ≈ 20000 + 1.55 × blower rpm** (20000 during pre-glow while the water pump runs; 29950 at 6154 rpm):
  blower command/PWM, **not** the water pump. Water pump power still not found.
- **FD35 = control unit temperature (?)**, FD01 format v/100 − 50: 40 °C idle, +6 K within 5 s of glow plug on
  (driver losses), 47 °C at minimum burn, 63–67 °C at full load (10-10).
- Regulation: EasyStart run 09:3x (cold water): 0.644 l/h up to 69 °C, 72.5 °C → 0.443, 74.5 → 0.230, ≥ 75 °C
  minimum 0.148, no pause even at 76.9 °C → dynamic (lag/trend), not the static FD33 curve. CAN start at 70.6 °C
  rising → minimum: consistent, no sign of a CAN power cap (`055`/`056` `10 27` identical with the EasyStart).

## Pump line fix, step 1 (2026-10-11 ~11:40): Schottky diode, bleeder on the pump side
MBR3060PT in series (heater output → converter), 2.2 kΩ bleeder on the **converter** side, no series resistor/NTC.
EasyStart reconnected. EasyStart start: no hold. S+ series (EasyStart present): hold, hold, start → stopped.
- **P000252 count unchanged (115)**: the diode stops the converter's capacitance from back-feeding (+Ub fault gone).
- **New: P000250 "open circuit" (count 2 = the two holds)**: with the bleeder behind the diode the heater's output
  sees no load in its off-state check. Next: bleeder on the heater side (diode anode → GND), ~1 kΩ / 2 W.

## Pump line fix, step 2 (2026-10-11 ~12:00): bleeder moved to the heater side
1.2 kΩ from the diode anode (heater pump output) to GND; diode MBR3060PT; no series resistor/NTC; EasyStart present.
- Power-up clean; S+ start (aborted at 4 s): no hold; S+ start + 5 min burn (flame +51 s, 3.2 kW): no hold.
- DTC memory after: P000252 still 115, P000250 still 2 (status 68, not active), **no overcurrent / 25x codes**.
- Too few starts for a rate yet; next: S+ trials (≤ 5 aborts, then a warm-up run) and a CAN series without the
  EasyStart.

## EasyStart workshop menu, sniffed (2026-10-11 ~13:48–13:51, Fire listen-only, our polling off)
| Menu item | On the bus |
|---|---|
| Read error memory heater (H1) | `19 02 01`, then `19 02 08` (multi-frame, flow control `30 08 14`) — same as ours |
| Delete error memory heater | session change + authentication step, then `14 FF FF FF` → `54`. Left to the EasyStart menu |
| Read / delete control unit (EasyStart) | no CAN traffic: the EasyStart's own memory |
| Read hour counter | `22 FD 17` → 1817 h on the display = FD17 dword 1 (109062 min) → **FD17 confirmed** |
- Error memory cleared 13:48:55 → all DTC occurrence counters restart at 0 (baseline for the pump-line fix).
- The other FD17 dwords (power bands, pause, altitude) match our sensors; FD17 counts in state 05 incl. pause.

## Config burst decode, step 1: maintenance interval in `064` (2026-10-11 13:51–13:52)
The EasyStart re-sends config frames **live on every settings change** (not only at power-up). Maintenance
interval set to 1000 h, then back to "off" (no UDS traffic at all; status computed from FD17):
- **`064` B6–B7 = maintenance interval, u16 LE in 10 h units, `03FF` = off** (`0064` = 1000 h).
- `064` B1 bits 2–3: change sequence counter (04 → 08 → 0C → 04; 00 after power-up).
- Open in `064`: B0 `3C` (60), B2 `05`, B4 `C8` (200). Method: change one EasyStart setting, watch the frame.

## Config burst decode, step 2: `063` (2026-10-11 13:54)
Sensor selection → "control unit", temperature offset → +3 °C, sent together on confirm/menu exit:
`063` `32 00 01 00 …` (morning) → `50 01 01 01 …`.
- **B0 = temperature offset, (raw − 50) / 10 °C** (`32` = 0.0, `50` = +3.0) — confirmed (set back to 0 → `32`).
- 13:55:26 `50 00 01 02 …`: B1 01 → 00, B3 01 → 02 → **B3 = change counter** (0, 1, 2), **B1 = temperature
  sensor selection, 0 = none, 1 = control unit** (confirmed: set back to "none"). B2 `01`: open.
- Frames are sent on confirming a setting (`064` within seconds of each change), not only on menu exit.
- Confirmed sequence (13:54–13:56): CU on + offset +3 → `50 01 01 01`; sensor none → `50 00 01 02`;
  CU on + offset 0 → `32 01 01 03`; CU off → `32 00 01 01`. **B3 = 2-bit change counter 1→2→3→1** (0 after power-up).

## Pump line fix verified (2026-10-11 13:59–14:08): diode + 1.2 kΩ bleeder (heater side), EasyStart present
Error memory cleared 13:48:55 (baseline 0). S+ series: 5 starts aborted at 4 s + 1 warm-up run.
- **6 of 6 S+ starts without hold** (before the fix, same setup: 3 of 4 held). Error memory afterwards: **none**
  (`19 02 FF`).
- Warm-up run: flame 14:04:15; at minimum power with the mixer closed the heater went into control pause at
  **outlet 83.1 °C** (phase 14 → 1F, 14:07:36) — regular, not a fault.

## Residual heat (2026-10-11 14:13, EasyStart menu)
- **`054` `01 03`** (B1 03 = residual heat; `01 01` = heat) → **`2C4` state `81`**, **FD10 `04 DC`** (B0 04, phase
  `DC`), FD0F B1 01. Blower 0, glow 0, fuel 0, no flame, inlet ≈ outlet: **water pump only**.

## Fan mode and the vehicle blower relay (2026-10-11 14:16–14:19, EasyStart menu)
- EasyStart ring colours: blue = fan, yellow = residual heat, red = heat.
- **Fan mode: `054` `01 02`** → `2C4` state **`21`**, FD10 **`03 D2`** (phase D2); vehicle fan on at once.
- Mode scheme: `054` B1 01 heat / 02 fan / 03 residual heat; FD10 B0 01 / 03 / 04; `2C4` state 05 / 21 / 81.
- FD00–FD44 (all 49 answering DIDs) fan on vs. off: only FD01 and FD10 differ → relay not there.
- **FEFF byte 17 bit 3 (0x08) = vehicle blower relay**: `00` fan off (14:18:40), `08` fan mode on (14:19:44/46),
  `08` during heating (10-10). FEFF polled with the sensor DIDs → "Vehicle Fan" binary sensor.
