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
| `components/hydronic_s3/` | The component. Listen-only discovery now: logs new IDs, per-byte changes (muted after 16), 10 s ID table with period/gaps. |
| `esphome/hydronic-s3.yaml` | Device config: Fire + COMMU, component, display, HA sensors, DS18B20/pump reference sensors. |
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

## Session state (2026-10-10 23:45, end of live session)
- Heater OFF for the night: tempcontrol S+ relay OFF (`switch/TempControl S3 Heater relay (real)`), PID climate
  OFF (`climate/TempControl PID Climate Controller`, valve readback `sensor/TempControl Ventilposition`).
- Fire on power bank; switch "Poll All DIDs" ON (FD00..FD44 every 5 s, research). WiFi weak: API log lines get dropped, entity updates reliable.
- Firmware = d0428fb: DID sensors (temps, voltage, blower, glow, pump, phase, fuel l/h, heating power, fuel total,
  hour counters, flame), ident text sensors, CAN start after boot-good flash writes (no overflows since).
- Decoded so far: `docs/research.md` (DID correlation, fuel, regulation curve FD33, phases, counters, DTC 19 06).

## Next steps
1. Live test (heater asleep, S+ off, Fire listen-only until the heater's 2C4 appears):
   a. Bus wake-up by the EasyStart: "wake" event + first 40 frames after ≥2 s silence. EasyStart IDs before
      the heater's 2C4 boot frame → CAN wake; 2C4 first → separate wake line. Note REC/EFLG (unacked retries).
   b. Start via EasyStart Pro (not S+): does the start hold (2C4 B2 0x40, P000252) occur too? Compare 054/625.
   c. Cold start data with "Poll All DIDs": start counters, start fuel. Minimize ignitions (soot).
   d. Disconnect/reconnect: heater supply off/on → "Heater Connected", values "--" after 15 s and back.
2. Open DIDs: water pump power (FD08?), FD35 (temperature?), FD0F w2–5, FD13/14/16/1B/27, stored counters
   (ignitions, glow plug switch-ons, flame-outs, overheats, run durations, start types), DTC 19 06 layout.
3. Display: show the new values on the LVGL UI. Then clean-up for the S3 component (drop research tools,
   raw sensors). Zero-loss: MCP2515 read in IRAM ISR before any control TX.

## Safety
Combustion heater: never send frames until the passive decode is solid; keep independent power cut;
emulating EasyStart identity alongside the real one can trigger P000342 / lock the control box.
