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

## Session state (2026-10-10 22:20, live at the heater)
- Fire on power bank, WiFi weak (RTT 125–230 ms → API **log lines get dropped**; entity updates are reliable).
- Captures (in `work/`, gitignored): `run2.log` (API logs, auto-reconnect loop), `events.log` (web `/events`
  SSE: Heater*/Outlet/Overflow/Diag entities, auto-reconnect). Earlier: `run1.log`, `stress*.log`.
- **Poll DIDs switch ON**: sweeps FEFF + FD00..FD44 every 5 s (normal mode while on). Turn off when done.
- Heater running via tempcontrol S+ relay (ON); tempcontrol PID climate OFF.
- Diagnostic tools on device (HA/web): Read Fault Codes, Send UDS Request (+ "UDS Request" text), Scan DID Range
  (+ from/to numbers), Scan Identification DIDs, Scan Local IDs, Poll DIDs; results → "Diag Response".
- Readable DIDs (default session, no security): F18B F193 F195 F1F0–F1F5 FD00–FD44 (54) FEFF; raw in
  `docs/research.md`. 0000–0FFF, F100–F17F negative (NRC 31); service 21 not supported (NRC 11).
- Last RX overflow 22:14:20 during polling (reader wake 119 µs after ISR). Suspected floating GPIO36
  pulse_meter → removed; watch whether overflows recur.
-
  Readable: factory-setting XMLs (coding names/values), seed/key DLL names, Java DTC job (uses 19 06, 19 10).

## Next steps
1. Correlate polled DIDs across phases (regulation, pause, restart, start hold vs. good start) → identify live
   values (inlet temp, power, pump rate, flame, glow, voltage) and the start-hold cause. Keep commentary factual.
2. Startup fault: S+ start hold = `2C4` B2 `0x40` 1.7 s after request; P000252 (water pump) active during hold.
   Minimize ignitions (soot), esp. cold. React-to-hold logic for tempcontrol is a component feature candidate.
3. RX overflows: confirm gone after pulse_meter removal; else analyse the frozen timeline.

## Safety
Combustion heater: never send frames until the passive decode is solid; keep independent power cut;
emulating EasyStart identity alongside the real one can trigger P000342 / lock the control box.
