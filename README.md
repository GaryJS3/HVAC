# Senville SENDC-60HF ESP and local controller

PlatformIO / Arduino firmware for classic ESP32-WROOM-32 DevKit V1 (4 MB flash).
Stages 1 and 2 are implemented, with an opt-in XYE C0 query foundation.
The default build cannot transmit. Live captures have established S1/S2 reception
and XYE polling on this unit. New controller/relay work is described in
[the controller guide](docs/CONTROLLER.md); active relay operation remains bench-only.

## Build and flash

Install Python and PlatformIO Core, or the PlatformIO VS Code extension. From this directory:

```powershell
python -m pip install platformio==6.2.0
Copy-Item include/LocalConfig.example.h include/LocalConfig.h
python -m platformio run
python -m platformio device list
python -m platformio run -t upload --upload-port COM5
python -m platformio device monitor --port COM5 --baud 115200
```

Replace COM5 with the board's port. Edit `LocalConfig.h` to set the AP password
and optionally your Wi-Fi SSID/password. This file is ignored by Git. Without
configuration, the bench AP is `HVAC-Diagnostics`, password `change-this-first`.
Join it and open http://192.168.4.1/. Disable cellular/VPN routing if it interferes.
The AP remains available while station Wi-Fi connects asynchronously. UART
processing starts before Wi-Fi and runs in its own priority-3 task on core 1.
HTTP service runs in the Arduino loop at lower priority. UART buffers are 2048
bytes per port, event queues 64 entries, frame buffers 263 bytes, history 32 frames.
Do not expose this unauthenticated diagnostic API to the Internet.

In the default build UART0 remains USB diagnostics; XYE is uninitialized. BOOT/EN
can be used for manual download mode if automatic flashing fails. Future UART0
XYE support must remove all Serial logging before remapping RX34/TX25. ROM boot
output uses default UART0 pins, not GPIO25. The optional XYE build remaps UART0
after disabling software diagnostics; see its separate instructions below.

## Wiring and electrical prerequisites

| Interface | UART | RX | Reserved TX (not routed) | EN |
|---|---|---|---|---|
| XYE | 0, unused | 34 | 25 | 26 |
| ODU segment (port 0) | 1 | 35 | 27 | 32 |
| IDU segment (port 1) | 2 | 33 | 14 | 13 |

Modules: 3.3 V VCC, common ESP ground. On the user's verified boards, module
TXD is RO (chip pin 1), the output to ESP RX; module RXD is DI (chip pin 4),
the input from reserved ESP TX. These labels are opposite the original assumption.
GPIO34/35 are input-only without internal pull-ups.
Explicit RX assignments avoid UART1 flash pins. Monitoring profiles never assign
the S1/S2 TX pins. For initial testing disconnect module RXD/DI and hard-disable DE
after verifying the board's circuitry. EN LOW is a provisional assumption only.

| RJ45 pin | Signal | Module connection |
|---|---|---|
| 5 | IDU X | XYE A |
| 4 | IDU Y | XYE B |
| 8 | IDU E | communication ground only after compatibility checks |
| 2 | IDU S1 | IDU A (provisional) |
| 1 | IDU S2 | IDU B (provisional) |
| 6 | ODU S1 | ODU A (provisional) |
| 3 | ODU S2 | ODU B (provisional) |

These MAX3485 breakouts are non-isolated. Have a qualified person verify the
actual S1/S2 differential and conductor-to-ground voltages, ground potential
differences, transceiver common-mode ratings and transient protection before
connection. No S1/S2 ground wire is present: this does not establish compatibility
or isolation. Two modules tied to ESP ground can couple the two segments. Use
suitable isolated transceivers and isolated power when the measurements require
it. Never connect a possibly mains-referenced bus to USB/PC test equipment.
Protocol-compatible bytes do not prove electrical safety. Do not power this
board from an unverified HVAC connector.

### Verify EN on an isolated bench first

1. Disconnect all HVAC wires. Inspect the breakout schematic or trace continuity
   from EN to the MAX3485 DE and /RE pins. Verify header-to-chip connectivity;
   the user's boards have TXD = RO (pin 1), RXD = DI (pin 4).
2. With only 3.3 V bench power, confirm EN LOW makes DE LOW and /RE LOW if they
   are tied together. If it does not, do not attach this firmware to the bus.
3. Use a separate known-good RS485 adapter as sender on a bench pair. At EN LOW,
   confirm bytes reach RO/ESP RX while the breakout driver stays high impedance.
   A scope/logic analyzer and appropriate bench bias/load help verify this.
4. Only with HVAC disconnected, verify EN HIGH selects transmit and suppresses
   the receiver as the module design specifies. Restore LOW afterward.
5. Fit an external ~10 kOhm pull-down on each EN (26,32,13) after confirming LOW
   disables its driver. Firmware cannot hold EN safe before startup, during ROM
   boot, reset or power loss. Hard-tying DE LOW and leaving DI disconnected gives
   stronger receive-only protection for these milestones. Check for unwanted
   driver enable during reset/flash with a scope.

Do not add termination or bias blindly to an existing bus. Determine existing
termination, topology and idle behavior first. Keep tap stubs short.

## Bench simulator and host tests

The tests compile the real portable C++ parser and decoder, not a Python replica.

```powershell
python -m pip install ziglang==0.13.0 pyserial==3.5
python tools/run_tests.py
python tools/simulator.py --output build/stream.bin --corrupt --repeat 2
python tools/run_tests.py build/stream.bin
```

For a physical bench test, connect a known-good USB RS485 sender to one module's
A/B (HVAC entirely disconnected). Then run:

```powershell
python tools/simulator.py --port COM6 --repeat 100 --corrupt
```

COM6 is the separate sender, not the ESP USB console. Test each receiver in turn.
Confirm `bytes`, `valid_frames`, raw frame timestamps and telemetry update for
the corresponding port; corruption should increase `crc_failures` and parsing
should recover. Swap A/B on the bench if polarity is wrong. During resets and
flashing verify ESP-side DE remains disabled. The simulator uses published
captures; it is not an HVAC emulator, timing/proxy validation or a safe bus master.

## First live receive-only test

1. Complete electrical and EN checks above. Keep a straightforward way to
   restore the original factory cable; use a suitable bypass if it was split.
2. Leave the factory IDU-to-ODU bus intact and passively tap it with one verified
   receive-only interface. Attach a second tap only after checking its additional
   electrical loading and shared-ground consequences. Do not join two energized
   segments using software or an improvised relay.
3. If already separated, expect mainly ODU master polls on the ODU segment and
   no normal IDU replies on the disconnected IDU segment. This is a topology
   effect and interrupts normal HVAC communication. Restore the factory path
   for normal operation; this firmware provides no proxy or automatic bypass.
4. Flash with HVAC disconnected, verify EN disabled through reset, then connect
   the qualified receive-only tap. Open `/api/status` and `/api/frames`.
5. Observe valid CRC frames with source 1 (ODU) and 256 (IDU) on an intact bus,
   increasing counters and plausible measurements. Port names identify physical
   inputs; either can contain frames from both logical sources on a tapped bus.
6. If only noise/CRC errors arrive, disconnect and check voltage, polarity,
   baud, wiring and ground compatibility. Swap A/B only with the circuit safely
   disconnected. No bytes can mean idle bus, wrong wiring or wrong topology.
7. Save both endpoint JSON outputs with operating conditions for comparison to
   service measurements. All field mappings remain unverified on SENDC-60HF-IM.

## Diagnostics and limits

`GET /api/status`: per-port counters and telemetry. Every measurement has value
(null until available), valid, stale, updated_us and confidence. Stale means over
15 seconds since update; it never authorizes a command. `reference_confirmed`
means confirmed by the reference author on another machine, not this system.
Telemetry is separated by physical input so competing captures do not overwrite
one another. Fields not decoded remain in raw frames.

`GET /api/frames`: oldest-to-newest bounded history, raw hex, address/command,
length, CRC result and first/last timestamps. These are 64-bit monotonic
microseconds since boot at UART software drain, not UTC or wire-edge times.
Bytes drained in a batch share a timestamp; timing includes UART FIFO/task
latency. Reboots reset timestamps. Invalid/truncated candidates are retained.
History eviction is expected and counted separately from receive data loss.

UART framing/parity errors and FIFO/buffer overruns are counted from IDF events.
`known_dropped_bytes` counts buffered bytes deliberately flushed on overflow;
the hardware cannot report the exact number already lost in an overflowing
FIFO. `drop_events` and `queue_saturations` flag incomplete accounting. Event
queue saturation itself is best effort, not proof of zero event loss.
Parser truncations also contribute to `framing_errors`. Noise preceding a
preamble is counted by the parser internally. Raw history is not a permanent
capture archive. Network JSON allocations occur outside the receive task.

## Implemented versus future work

Implemented: dual RX hardware UARTs at 4800 8N1, fixed-buffer length/CRC parser,
sliding recovery and 50 ms idle truncation recovery, raw history, health counters,
typed core and performance telemetry, freshness, Wi-Fi AP/station HTTP diagnostics,
host C++ tests and bench sender, offline XYE C0 response validation/status fields.

The local controller adds SQLite history, a web dashboard and authenticated XYE
mode/fan/temperature requests with read-back verification. Writes have not yet
been validated live. `proxy_bench` provides experimental raw forwarding for a
disconnected bench; no capacity rewriting, independent watchdog, hardware bypass,
solar or zoning controller is implemented. The default firmware cannot transmit;
`xye_queries` keeps S1/S2 receive-only. See [the controller guide](docs/CONTROLLER.md)
for capabilities, deployment and failure behavior.

A future proxy needs arbitration, TX-complete handling, measured timing, hardware
fail-safe normally-bypassed relay and independent watchdog. A software watchdog
cannot make a dead ESP electrically transparent. Never alter protection/fault
messages, and never infer permission to increase demand from missing telemetry.

See [protocol notes](docs/PROTOCOL.md) and [validation record](docs/VALIDATION.md).

## Raw-byte capture and optional XYE status queries

The default build also emits every S1/S2 UART drain chunk on USB serial at
115200 baud. `RX timestamp_us port sequence length HEX` records include all
bytes, even unrecognized protocols. Port 0 = GPIO35 tap; port 1 = GPIO33 tap.
On the joined factory bus both taps see both logical sources. Every five seconds,
`HEALTH` reports cumulative frame/error counts and serial queue drops. A static
32-entry queue separates reception from a low-priority logger task. Serial writes
are limited to available buffer capacity. If logging falls behind, chunks are
dropped and counted; UART reception continues. UART0 serial logging is compiled
out of the XYE query build, preventing diagnostic output on X/Y.

```powershell
python tools/capture_serial.py --port COM5 --seconds 300 --output captures/session.log
python tools/analyze_capture.py captures/session.log
```

The capture tool saves original serial bytes plus UTC start metadata. The analyzer
reassembles each tap into a binary replay file and reports message types, CRC,
sequence gaps, provisional value ranges and software-observed reply intervals.
Close other serial monitors first. Capturing does not intentionally toggle reset
pins; flashing necessarily resets the ESP. Captures beginning/ending mid-frame
may include partial data. Health counts are cumulative since device boot.

`GET /api/bytes` retains the latest 16 UART drain chunks per S1/S2 input, up to
128 bytes each, regardless of preamble or CRC. Chunks include monotonic drain
timestamps and per-port sequences. Evictions mean history replacement, not UART
loss. Chunks are not frame boundaries. This allows inspection of unrecognized
protocols, wrong baud or polarity without discarding all non-A0 bytes.

The separate `xye_queries` environment enables C0 queries and authenticated C3
climate writes. C3 requires a fresh controller lease and explicit request.
It does not send anything on startup. It uses UART0 RX34/TX25 and disables USB
Serial diagnostics completely. ROM boot/download mode remains on GPIO1/3, so USB
flashing is retained. ESP logging is suppressed before UART0 is remapped.

Before using this build, verify XYE electrical compatibility and remove any
other XYE/CCM master from that pair. With power off, remove the hard ground tie
from **only the XYE EN**, connect EN to GPIO26, and fit a 10 kOhm EN-to-GND
pull-down. Both S1/S2 ENs are held LOW by the monitoring firmware; the installed
modules use their assigned ESP EN pins with external 10 kOhm pulldowns. On the verified modules, XYE
TXD/RO goes to GPIO34 and RXD/DI to GPIO25. Never drive GPIO26 high against a
hard ground tie. Keep factory IDU/ODU communication intact for normal operation.

```powershell
python -m platformio run -e xye_queries
python -m platformio run -e xye_queries -t upload --upload-port COM5
# Rejoin HVAC-Diagnostics after the upload resets the ESP.
curl.exe --noproxy "*" http://192.168.4.1/api/xye
# Deliberately arm status queries only after the wiring/master checks above:
curl.exe --noproxy "*" -H "Authorization: Bearer $env:ESP_TOKEN" -X POST http://192.168.4.1/api/xye/enable
curl.exe --noproxy "*" -H "Authorization: Bearer $env:ESP_TOKEN" -X POST http://192.168.4.1/api/xye/disable
```

The default `esp32dev` build rejects enable requests. The opt-in build scans
unicast addresses 0..63, at most one C0 per second, with a one-second reply timeout after TX
completion and at least 130 ms received-line idle time. Valid C0 replies must
have the expected source address, master destination 63 in both destination
fields, delimiters and checksum. A matching reply selects that address; after
three consecutive timeouts discovery resumes. C3 SET is only sent for an explicit
authenticated request with a fresh lease. There are no FOLLOW_ME or extended
queries, broadcasts, automatic setpoint controls or S1/S2 forwarding in this profile.
The XYE task may wait up to 60 ms for hardware TX completion before lowering EN;
the independent S1/S2 RX task continues at higher priority. TX errors disarm
polling. A disable request takes effect on the XYE task's next iteration and
does not truncate a query already being transmitted.

`GET /api/xye` reports compiled/ready/armed flags, query/reply/timeout counters,
validated address, last status and freshness, error/protection words, and recent
raw RX hex. State freshness is independent of the cached address. Disarm/reboot
does not issue a power or mode command. Reboot always returns to unarmed.
Read-only diagnostics remain available on the trusted diagnostic network.
Mutation endpoints require the configured controller bearer token. Live C0
polling at address 0/master 63 has been validated; new C3 writes are not yet
live-validated. No new firmware is automatically flashed by a container deployment.

## ESP registration and network setup

The ESP home page configures Wi-Fi, controller hostname/IP, port and device key.
Settings persist across restart; the diagnostic AP remains available. The ESP
registers with the controller and uploads telemetry every two seconds. Commands
return in those responses, so no ESP address is configured on the controller.
See [deployment and failover details](docs/CONTROLLER.md).
