# Hydronic S3 CAN protocol (reference)

Current state of the decode, for a Hydronic S3 D6L Commercial 24 V with an EasyStart Pro. The evidence (dated
experiments, raw captures) is in [`research.md`](research.md), how to capture in [`can-sniffing.md`](can-sniffing.md),
fault codes in [`dtc.md`](dtc.md). (?) = plausible, not confirmed.

## Bus

500 kbit/s, 11-bit IDs, no 29-bit / J1939 traffic. Same frame set as the Airtronic S3
([TrooperDuper](https://github.com/TrooperDuper/espar-airtronic-esphome)), with Hydronic-specific content.
Byte numbers B0…B7; broadcast values little-endian, UDS values big-endian.

| ID | From | Period | Content |
|---|---|---|---|
| `2C4` | heater | 100 ms | status, see below |
| `2C5`, `2C6` | heater | 100 ms | `2C5` zeros, `2C6` static |
| `625` | heater | 100 ms | B1 `10` awake / `00` just before sleep |
| `054` | controller | 200 ms | command, see below |
| `055`, `056` | controller | 200 ms | static `10 27 …` (meaning open; identical from EasyStart and us) |
| `057` | controller | 200 ms | static `00 00 FE FF FE FF 00 00` |
| `60D` | controller | 100 ms | keep-alive `0D 10 82 B6 09 20 70 00`; also the wake-up frame |
| `065` | controller | ~3 s | bursts, B3 cycling `01`/`02`/`03` |
| `05C`–`06D`, `10A` | controller | at power-up and on each settings change | configuration burst, see below |
| `7A0` → `73C` | tester → heater | on request | UDS, see below |

`FE FF` = "not available" filler (e.g. temperatures in the first frame after boot).

### `2C4` status

| Byte | Meaning |
|---|---|
| B0 | state: `00` booting, `03` ready (also run-down, start hold), `05` running (start, heating, pause), `21` fan, `81` residual heat, `41`/`C1` residual heat refused/ended. `0B` fault is the Airtronic value, never seen here |
| B1 bit 3 | burner run-down (~65–95 s, into a control pause or after switch-off) |
| B2 | fault flags: `40` start hold (see [README](../README.md#non-stock-water-pump-start-holds-p000252--p000250)), `04` control unit lost (P000307) |
| B4–B5 | outlet coolant temperature, (raw − 500) / 10 °C |

### `054` command

B0 `01` on / `00` off; B1 mode `01` heat, `02` fan (vehicle fan only), `03` residual heat (water pump + vehicle
fan, needs warm coolant). The heater does not keep a run time: the controller sends "off" when its timer ends.

### Configuration burst

Sent by the EasyStart at power-up and again on each confirmed settings change. Pairs of frames are the copies for
heater 1 / heater 2 (only the heater-1 copy changed in our tests).

| Frame | Content |
|---|---|
| `063` | B0 temperature offset (raw − 50) / 10 °C; B1 temperature sensor (0 none, 1 control unit); B3 change counter |
| `064` | B1 bits 2–3 change counter; B6–B7 maintenance interval, u16 LE, 10 h units, `03FF` = off |
| `067`–`069` | timers 1–3: B1 weekdays (bit 1 Mo … bit 7 Su), B2 bits 0–4 hour, B3 minute, B4 mode (as `054` B1), B5 bit 7 active + bits 0–6 minutes, B6 change counter. `06A`–`06C`: heater 2 |
| `05C`/`05F` | `1E 00 1E 00` = 30, likely the EasyStart's preselected run time (?) |
| `10A` | clock: B0 bits 0–2 weekday (Mo = 0); B1 bits 2–7 minute, bit 0/1 weekday/time set; B2 bits 0–5 second, bits 6–7 change counter; B3 bits 0–4 hour |

The heater stores but does not execute timers; the EasyStart runs them (this firmware: `esphome/timers.yaml`).

## Controller duties, wake and sleep

- **Wake:** the controller sends `60D` until the heater acknowledges (~250 ms of retries); the heater boots and
  `2C4` reaches `03` ~0.2 s later. Any frame on a sleeping bus wakes the heater and the EasyStart.
- **Sleep:** ~0.7 s after the last diagnostic request `625` B1 drops to `00`, ~2 s later the bus is silent. UDS
  polling keeps the heater awake, so poll only while it is active.
- **Presence:** once it has met a control unit, the heater expects one at every wake. Without `060D`/`054` it raises
  P000307 (flag `04`) after ~15 s; losing the controller mid-run also adds P000440, which stays active until the
  heater sleeps. Run-down follows, no lock. A replacement controller therefore answers every wake with the "off"
  set until idle + 15 s, and keeps sending through the run-down after a stop.
- **One controller only.** The EasyStart reacts to foreign controller frames (it sends "off"); P000342 = too many
  CAN units. Replace it, do not join it. Testers on `7A0` are not controllers: several may share the bus, one
  request at a time (the heater drops a request sent before it answered the pending one).
- **Acknowledge:** without the EasyStart, someone must ack, or the heater repeats its first `625` forever.
- **S+ (wired switch-on) wins over CAN:** a CAN "on" during an S+ run is ignored, S+ off ends the run. No S+ status
  bit was found; the request source is inferred from FD10 B0. With S+ held on at power-up the CAN wake fails.
- **No run-time limit** in the heater (≥ 2 h tested); the controller keeps the time.

## Phases (FD10 B1)

| Code | Phase | Notes |
|---|---|---|
| `00` | off | |
| `0F` | pre-start check | ~2 s |
| `0A` / `0C` | start, cold / warm | told apart by temperature only |
| `0D` | pre-start purge | ~35 s blower only, before a warm start after a cut-short burn |
| `14` | heating | |
| `1F` / `28` | run-down to pause / control pause | water pump keeps running; `1F` ≈ 90 s (`2C4` run-down bit), `28` until the coolant is ≈ 70.5 °C, then a warm restart if still requested (no time limit seen); a hot start enters `28` directly; state `05` throughout (busy) |
| `1E` | run-down after switch-off | glow plug afterglow ~45 s |
| `C8` | blocked | start hold, also after P000307 |
| `D2` | fan | |
| `DC` / `DD` / `DE` | residual heat / checking coolant / refused (too cold) | |

Start sub-steps (FD10 B2, timer B3–B4 in 10 ms): `01`–`05` pre-glow (~94 W, no fuel), `06`/`08` fuel on, `09`/`0A`
blower ramp, `0B` flame, `0E`–`12` glow off ramp, then phase `14` (~100 s after the request, warm start).

## UDS (ISO 14229 over ISO-TP)

Tester `7A0`, heater `73C`, padding `AA`, flow control `30 08 14` (block size 8, STmin 20 ms). Used here:
`22` ReadDataByIdentifier, `19 02`/`19 06` fault memory ([`dtc.md`](dtc.md)), `14` clear (needs SecurityAccess, key
not in this repo, see [README](../README.md#controlling-the-heater)). KWP `21` is not supported. Requests must not
overlap: the heater drops a request sent before the previous answer.

### Live and stored data (`22`)

| DID | Content |
|---|---|
| FD01 | 3× u16, °C = v/100 − 50: flame sensor, inlet, outlet |
| FD04 | supply voltage, 0.1 V |
| FD06 | counter, +1 at every start and every burner stop |
| FD08 | blower command, ≈ 20000 + 1.55 × rpm |
| FD09 | blower speed, rpm (~2000 minimum, ~6150 full load) |
| FD0A / FD0C | glow plug power 0.1 W / raw drive |
| FD0D | metering pump, 0.1 Hz |
| FD0F | B0 flame (1 = flame); B1 combustion state `01` no fuel, `02` ignition, `03` burning; B2–B5 open |
| FD10 | B0 request (`01` heat, `03` fan, `04` residual, `00` none); B1 phase; B2 start sub-step; B3–B4 timer 10 ms; B5–B7 run-down sub-step and timer |
| FD11 | B1 fuel rate µl/s; B2–B5 total fuel, litres; B6–B9 fraction, nl (stroke ≈ 29.3 µl) |
| FD17 | 7× u32 minutes: total, power bands 0–25 / 25–50 / 50–75 / 75–100 %, control pause, altitude mode |
| FD1A, FD1E | counters (FD1E w2 = starts; FD1A not incremented by a fan start) |
| FD35 | control unit temperature (?), FD01 format |
| FEFF | B17 bit 3 vehicle blower relay; B19 fuel pump current, B21 water pump current (10 mA); B24/B25 follow the glow plug |
| F193 / F195 | hardware / software version (ASCII) |
| F1F0 | part number, BCD (`25 27 45 01 50 01` = 25.2745.01.5001) |
| F1F5 | serial number (ASCII) |

The other answering DIDs (FD00–FD44, F1F1–F1F4) are constant or unexplained; raw values in
[`research.md`](research.md#uds-did-scan-2026-10-10-220x-raw-captures-default-session-no-security-access).
Derived values in the component: heating power = fuel rate × D6L datasheet heat per litre, water flow = power /
(coolant heat capacity × ΔT) while burning.

## Open

- `055`/`056` `10 27`, `063` B2, `064` B0/B2/B4, `05D`–`062`, `066`, `06D`, timer B0, `10A` B4–B5.
- FD0F B2–B5, FD1A, the `19 06` extended-data layout, a water pump power setpoint (3-wire pump only?).
- Whether the heater uses our clock (`10A`); no DID shows the time.
