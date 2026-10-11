# Hydronic S3 CAN integration

## Goal
ESPHome **`hydronic_s3` external component** for an **Eberspächer Hydronic S3 D6L Commercial (24 V)**
over CAN, to be integrated later into the user's tempcontrol heating regulator (van, water heating).
- All obtainable heater data on the display and as HA sensors (loop temps in/out, pump rate → power, state, faults).
- Later: control (on/off, power) replacing/coexisting with EasyStart Pro.
- **Not** a universal sniffer tool. Loop: flash/OTA → read the component's logs → update decoder → OTA.

## Working style (user)
- Short, technically precise output. No filler.
- Don't busy-wait on long jobs; user may comment in between.
- Honest assessments, push back on bad ideas.

## Layout
| Path | Purpose |
|---|---|
| `components/hydronic_s3/` | The component: broadcast + UDS DID decoding, polling state machine (heater may sleep), start/stop in place of the EasyStart (wake, command set, config burst, retry, watchdog), TX queue, research tools (scans, ID table, wake trace). |
| `esphome/hydronic-s3.yaml` | Device config: Fire + COMMU, component, HA entities (web UI groups: Heater Control, Heater, Components, Counters, CAN, Debug). |
| `esphome/research.yaml` | Research package (ID table via `research: true`, scans, UDS console, raw DIDs); drop for production. |
| `esphome/example-twai.yaml` | Minimal integration on TWAI (`esp32_can`), no listen-only hook, bus-off recovery — template for tempcontrol. |
| `esphome/ui/heater-ui.yaml` | LVGL UI (fonts, styles, pages), shared by the device and the SDL preview. |
| `esphome/hydronic-s3-sdl.yaml` | SDL preview of the UI on the host (demo values); screenshots via `import -window` on XWayland. |
| `README.md`, `docs/img/` | Human-facing README with screenshots (web UI: headless Chrome `--timeout=35000`; LCD: SDL). |
| `esphome/boards/m5stack-fire.yaml` | Board package (Fire v2.7: PSRAM, SD CS held high). |
| `tools/m5.sh` | upload/logs by stable by-id path. `tools/bootlog.py`: reset + boot log over USB. |
| `docs/research.md` | Protocol research (Airtronic S3 map, J1939 HTR, heater facts). |

## Hardware
- M5Stack **Fire v2.7** (CH9102 → by-id `usb-1a86_USB_Single_Serial_5864000646-if00`, WiFi 10.22.10.73) + **COMMU**.
  - **The Core2 `usb-1a86_..._5761016107` on this host is off-limits.** Never use `/dev/tty*` names.
  - COMMU: MCP2515, 8 MHz, CS GPIO12 (strapping, boots fine), INT GPIO15 (M5Stack COMMU examples).
    SPI 18/23/19, shared with LCD (CS 14, DC 27) and SD (CS 4).
  - **No SD card in the slot** (not needed; HA logs). An unparked card in native SD mode answers SPI traffic
    on MOSI/MISO → ~10% corrupted MCP2515 bytes. GPIO4 (SD CS) is driven high regardless.
  - SPI bus on **SPI3 (IOMUX)**: on SPI2 the GPIO matrix delay + MCP2515 tV (45 ns) corrupts reads at 8 MHz
    (deterministic, 1-bits lost). IOMUX: clean up to 10 MHz. MCP2515 runs 8 MHz.
  - PSRAM unused: ESP32 SPI DMA can't read PSRAM (bounce copies), and it shares the flash cache.
  - SPI DMA on ESP32: buffers 4-byte aligned, RX lengths multiples of 4 (else IDF mallocs per transaction /
    DMA may overrun). `mcp2515_irq` does all transfers on one aligned buffer (see BSP `mem.rs`, `spi2.rs`).
  - LCD SPI 40 MHz, flash XIP 80 MHz, upload 921600 — as in the emobotics m5stack-core BSP
    (Fire27 display/SD/LVGL experience, bus sharing).
  - Rejected: CoreS3 (LCD DC = SPI MISO = GPIO35); Basic unit on hand (dead backlight).
  - Onboard 120 Ω on COMMU must be **off** for a mid-bus tap (verify).
  - Supply: 24 V vehicle → buck ≥36 V input + TVS → 5 V.
- EasyStart Pro stays on the bus. Bitrate 500 kbit/s **assumed** (Airtronic S3 RE); no frames → try 250k.
- Tap: EasyStart Pro 4-pin connector (BU/RD, BU/BK) or diagnostic XB6/1 (has terminator cap).
- CAN RX: `components/mcp2515_irq` (fork of core mcp2515): INT ISR → reader task → queue; counts MCP2515
  RX overflows and queue drops. Rollover ordering per Linux mcp251x. Budget: read within ~460 µs of INT.
- Display: LVGL (partial redraw) + `components/mipi_spi` (fork): pixel writes split into ≤1 KB bus holds,
  so the MCP2515 reader gets the shared bus within ~200 µs.
- Stress test: `esphome/hydronic-s3-loopback.yaml` (MCP2515 loopback, 3 back-to-back frames per loop,
  full-screen redraw every 200 ms). Pass = lost/reord/corrupt/ovf/drop all 0.
- Flash **via OTA only** (user's rule; the Fire runs on the M5GO battery). After an OTA, never USB-reset within
  ~1 min: the new image is not yet marked valid and the bootloader rolls back.
- Reader priority 23 (above lwIP tcpip 18, esp_timer 22); it holds the SPI bus per drain and drives CS itself.
- Reader SPI path in IRAM: own IDF device (not the ESPHome delegate), `SPI_MASTER_IN_IRAM`, gpio_ll for CS/INT;
  needs FreeRTOS/heap functions in IRAM (component reverts ESPHome's flash placement; IRAM 79 %).
  Frame read ~57 µs, stress 1620 fps lossless.
  Never add GPIO one_wire (interrupt-locking bit-bang on the loop core) — it overflowed the MCP2515 every poll.

## Secrets
Root `secrets.yaml` (user-maintained, gitignored, **never read**); `esphome/secrets.yaml` symlinks to it.
Keys: `wifi_ssid`, `wifi_password`, `api_key` (`head -c 32 /dev/urandom | base64`), `ota_password`.

## Build
```
cd esphome && ../work/venv/bin/esphome compile hydronic-s3.yaml
tools/m5.sh upload         # USB; then OTA: esphome upload hydronic-s3.yaml --device 10.22.10.73
```
`work/venv`: `uv venv work/venv && uv pip install -p work/venv/bin/python esphome pyserial`.

## Session state (2026-10-11 ~18:40)
- EasyStart **disconnected** (to be retired for the final wiring). Device firmware = HEAD
  (flashed ~18:20). WiFi reachable, log level INFO (runtime "Log Level" select for DEBUG).
- Own control without EasyStart solid: Heat, Fan, Residual heat; CAN wake + start from sleep (0.3 s).
- Pump line fix (Schottky MBR3060PT + 1.2 kΩ bleeder heater side): **no start hold since** (flag `0x40`).
- DTC clear in firmware (`clear_dtc()`, SecurityAccess key via `set_security_provider()`, key not in repo).
- **Clock `10A` decoded** (weekday/h/min/s, set flags, counter); optional `time_id` (SNTP) → sent in the config
  burst (also when our run wakes the heater, fixed 10-11). **The heater runs no timers** (EasyStart does) →
  firmware timers: optional package `esphome/timers.yaml` (3 slots, HA/web settable, verified).
- Combustion names (FD0F B1: No fuel / Ignition / Burning); phase `0D` = pre-start purge.
- **S+ vs CAN** (tested 18:02–18:15): S+ wins (CAN on ignored during S+, S+ off stops). No S+ bit found
  (broadcasts, FD00–FD44, FEFF) → source inferred from FD10 B0; "Request Source" / "Heater Request" entities;
  **Heater Run "on" refused while S+**, Heater Mode shows read-only "S+" (verified 18:31). S+ held on at power-up
  = misuse (blocked `C8`, CAN wake fails) — not pursued.

## Next steps
1. Done 10-11: bus wake (EasyStart = CAN, `60D` retries), EasyStart start (no hold, phase `0A`), 10 min run
   (timer in the EasyStart), polling state machine (heater sleeps), EasyStart back-off. See `docs/research.md`.
2. Target: **replace the EasyStart completely**. Still open with it disconnected: **run time** (> 30 min: indefinite
   or heater cut-off?), **controller loss mid-run** (ESP reboot/OTA: timeout, P000307 lock?), `60D` keep-alive
   required/checked?, `055`/`056` (`10 27`) = power/setpoint? Done: DTC clear, S+ coexistence, start hold.
3. Start/stop by us ("Heater" switch, REST `/switch/Heater%20Run/turn_on|turn_off` (watchdog "Heater Run Watchdog", 0 = indefinite), API `heater_start|stop`) needs the
   EasyStart **off the bus** (it wakes on our frames and sends off). Test that: start, regulation, stop, and
   whether the heater has its own max run time (config burst `05C`/`05F` = 30 = the EasyStart's duration preset?).
4. **P000307** (CAN communication error control unit): raised when a controller goes silent mid-cycle (fixed:
   "off" until idle + 15 s; OTA during a CAN run accepted to raise it). Error memory cleared 10-11 13:48 via the
   EasyStart workshop menu (that capture is now the basis of `clear_dtc()`, see session state).
5. Done 10-11 afternoon: pump fix verified (6/6 S+ starts, memory empty); EasyStart modes heat/fan/residual
   (`054` B1 01/02/03, state 05/21/81, phase 14/D2/DC) → "Heater Mode" select; vehicle blower relay = FEFF B17
   bit 3 ("Vehicle Fan"); config frames `063`, `064`, timers `067`–`069`, clock `10A` decoded (docs/research.md).
   Open: `05D`–`062`/`06D`, `066`, timer B0 — not reachable from the menu; burst pairs likely heater 1 / heater 2.
   S+ bit: not in broadcasts/FD00–FD44/FEFF; unscanned: other DID ranges, local IDs (`21`).
6. Open DIDs: water pump power (FD08 is the blower), FD0F B2–B5, FD13/14/16/1B/27, stored counters, DTC 19 06 layout.
7. Low prio: cooperate with the EasyStart instead of replacing it. Lead: OEM documents EasyStart Pro + Remote+
   combinations → a second control unit identity may be allowed (P000342 above a limit?); needs a capture of a
   real second unit. EasyStart self-wakes once 19 s after a foreign-woken bus sleeps (065 temperature update?).
8. Published: `origin` = github.com/farnham26/esphome-s3-hydronic-cs (GPLv3, SSH key `id_ed25519_shasta8086`,
   author `farnham26`). History scrubbed of dealer-tool material, heater serial, internal URLs — keep it that way.
   Web screenshots: redact the "Heater Serial Number" row before committing.
9. Clean-up done 10-11: listen-only hook optional, `research:` switch, research package + `hydronic_s3_research.cpp`,
   TWAI example compiles. tempcontrol will include the component on TWAI (S+/running lines become CAN). Open:
   zero-loss MCP2515 read in an IRAM ISR (Fire only).

## Uncertain decodes (verify when they show up)
- `2C4` B0 state names: `00` Booting, `03` Ready, `05` Running are solid; `41`/`C1` "Stopping" seen once (10-10);
  `0B` "Fault" is an Airtronic assumption, never seen here. Run-down shows under `03` (B1 bit 3).
- FD10 phases `0A` cold / `0C` warm start: told apart by temperature only; `0F` pre-start check seen once; `C8`
  Blocked also after P000307 (not pump-specific).
- FD35 control unit temperature, FD08 blower command, FD0F B2–B5, water flow estimate (coolant cp assumed).
- Config frames: `063` B0/B1/B3 and `064` B1/B6–B7 confirmed; `05C`/`05F` (`1E` = 30) not a menu setting.

## Safety
- Start trials: each aborted start (even at 4 s) costs one fuel pump stroke → after at most 5 aborted starts do one
  run until warm (≥ 5 min after flame) to burn off the fuel. Minimize ignitions on a cold heater (soot).
Combustion heater: never send frames until the passive decode is solid; keep independent power cut;
emulating EasyStart identity alongside the real one can trigger P000342 / lock the control box.
