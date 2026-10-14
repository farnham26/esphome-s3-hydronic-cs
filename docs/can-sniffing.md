# Sniffing the heater CAN bus

How the protocol in [`protocol.md`](protocol.md) was worked out ([`research.md`](research.md) has the evidence), and how to repeat or extend it with this
project's research tools. Read-only: listen, and read with UDS `22` / `19`. Writing to the heater is out of
scope (see the README's [Scope](../README.md#scope)).

## Bus facts

- 500 kbit/s, 11-bit IDs, no 29-bit / J1939 traffic.
- Nodes: the heater (`2C4`–`2C6`, `625`, UDS answers on `73C`) and the controller, normally the EasyStart
  (`054`–`057`, `060D`, `065`, config burst `05C`–`10A`). UDS requests go to `7A0`.
- The bus sleeps about 2 s after the last frame. Any frame wakes the heater (and the EasyStart).
- Frame and DID decodes: [`protocol.md`](protocol.md); fault codes: [`dtc.md`](dtc.md).

## Hardware and tap

Hardware, wiring and pins: README [Hardware](../README.md#hardware). Tap at the EasyStart connector or the
diagnostic plug, mid-bus, without an extra terminator.

**Acknowledge.** A CAN frame needs at least one other node to acknowledge it.
- With the EasyStart on the bus it acks, and the sniffer can stay **listen-only** (invisible).
- Without it, a listen-only sniffer leaves the heater repeating its first frame forever. The component therefore
  acks (normal mode) while the bus is awake and no EasyStart frame was seen for 60 s; otherwise it leaves
  listen-only only for its own requests (`set_listen_only_hook`).

## Research tools

Add the research package (`esphome/research.yaml`, included by `esphome/hydronic-s3.yaml`) and set
`research: true` on `hydronic_s3`. Then, at log level DEBUG (runtime select **Log Level**):

| Output | Content |
|---|---|
| `new std 2C4 [8] …` | first frame of each new ID |
| `2C4 B4 1F->20  …` | change log: one line per changed byte, with the whole frame |
| ID table (periodic) | per ID: count, mean period, age, last data |
| wake trace | the first frames after bus silence, with timestamps (who wakes whom) |
| UDS dump | every request on `7A0` and answer on `73C` |

Web UI group **Debug** (research package):

| Control | Use |
|---|---|
| **UDS Request** + **Send UDS Request** | send one request (hex), answer in **Diag Response** |
| **DID Scan From / To** + **Scan DID Range** | `22` over a DID range; answering DIDs go to **Diag Response** and the log |
| **Scan Identification DIDs** | the `F1xx` identification DIDs |
| **Poll All DIDs** | poll every answering DID instead of the sensor set (watch values move) |
| **Scan Local IDs** | KWP `21` probe (not supported by this heater) |
| **Scan Routine Results** | `31 03` (results only, never starts a routine) over the range |

A request sent while a poll sweep waits for an answer is deferred until that answer is in (the heater drops a
request that overlaps its pending one).

## Capturing

Log to a file, reconnecting on Wi-Fi drops:

```sh
cd esphome
until ../work/venv/bin/esphome logs hydronic-s3.yaml --device <ip> >> ../work/capture.log 2>&1; do
  echo "--- reconnect $(date +%T)" >> ../work/capture.log; done
```

Entity changes as a timestamped event stream (web server):

```sh
curl -sN http://<ip>/events | while IFS= read -r l; do
  case "$l" in data:*) echo "$(date +%T.%N | cut -c1-12) $l";; esac; done >> work/events.log
```

## Method

1. **One stimulus at a time**, and write down the time: a menu item on the EasyStart, a mode change, a pump
   unplugged. Then look for the bytes that changed at that moment (change log).
2. **Use the EasyStart as an oracle**: its menus send the requests the heater understands (e.g. its hour counter
   reads `22 FD17`, its "delete error memory" showed the DTC-clear sequence). Sniff listen-only while using them.
3. **Correlate with an independent measurement**: thermometer, multimeter, clamp meter, the display of the
   EasyStart. A candidate scaling must hold over the whole range, not one point.
4. **Wake traces** show who starts a conversation and what the controller must send (init burst, keep-alive).
5. **DID scans** list what exists; **Poll All DIDs** during a run shows which DIDs are live and which constant.
6. Record each finding with date, conditions and confidence in [`research.md`](research.md); move it to
   [`protocol.md`](protocol.md) and the decoders only once it is solid.

## Pitfalls

- **Our own traffic changes the system.** UDS polling keeps the heater awake; the component polls only while the
  heater is active. A controller frame (`054`, `060D`) on a sleeping bus wakes the EasyStart, which then sends
  "off". Never run your own controller next to the EasyStart.
- **Silence mid-run** raises P000307 (CAN communication error) in the heater: do not stop the sniffer/controller
  while it controls a run.
- **Fire specifics**: no SD card in the slot, SPI bus on SPI3 (IOMUX) — otherwise corrupted MCP2515 reads that
  look like protocol bytes. Check the counters (overflows, queue drops, REC/TEC) before trusting a capture.
- **Transcription**: frame bytes are B0…B7; multi-byte values were big-endian in UDS answers and little-endian
  in broadcast frames (e.g. `2C4` B4–B5 outlet temperature).

> **Safety.** A combustion heater. Keep an independent power cut, never leave experiments unattended, and keep
> to reads.
