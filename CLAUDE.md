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
  - PSRAM 40 MHz (80 MHz fails its memory test with 80 MHz flash).
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
- After an OTA, do **not** USB-reset within ~1 min: the new image is not yet marked valid and the bootloader rolls back.

## Secrets
Root `secrets.yaml` (user-maintained, gitignored, **never read**); `esphome/secrets.yaml` symlinks to it.
Keys: `wifi_ssid`, `wifi_password`, `api_key` (`head -c 32 /dev/urandom | base64`), `ota_password`.

## Build
```
cd esphome && ../work/venv/bin/esphome compile hydronic-s3.yaml
tools/m5.sh upload         # USB; then OTA: esphome upload hydronic-s3.yaml --device 10.22.10.73
```
`work/venv`: `uv venv work/venv && uv pip install -p work/venv/bin/python esphome pyserial`.

## Next steps
1. Flash, put on bus, read logs over full cycle: power-on → start → regulation (~75 °C outlet) →
   pause (85 °C) → restart (70 °C) → off. Vary EasyStart setpoint/mode.
2. 29-bit IDs: J1939 HTR **PGN 65133 (0x18FE6D45)**. 11-bit: compare Airtronic map.
3. Decode into component sensors (temps, state, pump, faults); display shows decoded values.
4. If temps not cyclic: UDS diagnostics.

## Safety
Combustion heater: never send frames until the passive decode is solid; keep independent power cut;
emulating EasyStart identity alongside the real one can trigger P000342 / lock the control box.
