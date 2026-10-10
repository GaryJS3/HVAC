# Validation

The ESP32 build and portable C++ tests are run during project creation. Final
results are recorded below after execution. Hardware is unavailable: EN polarity,
RX/TX labeling, common-mode/electrical suitability, actual bus baud/polarity,
timing and field mappings on SENDC-60HF-IM remain unverified.

Host coverage: published ODU/IDU frames and extra trailing byte, standard Modbus
CRC vector, timestamps, core/extended decoding, invalid checksum, resynchronization,
corrupted length hiding a frame, maximum length with A0 payload, unknown commands,
thermistor endpoints, XYE response checksum/address/direction/setpoint flag,
stale-data logic and immutable transmission prohibition. Simulator replay exercises
the same C++ parser through a binary file. Hardware UART overflow, Wi-Fi under
load, reset EN behavior and physical bus compatibility require bench testing.

Executed successfully on 2026-10-10:

- `python -m platformio run` with espressif32 6.12.0 / Arduino ESP32 2.0.17.
- `python tools/run_tests.py`: compiled portable C++ with Zig 0.13.0,
  `-Wall -Wextra -Werror`; all assertions passed, including independent parser
  instances and 100,000 deterministic random input bytes followed by recovery.
- `python tools/simulator.py --output build/stream.bin --corrupt --repeat 2`
  and `python tools/run_tests.py build/stream.bin`: replay produced four valid
  captured frames and two expected CRC-failure records, then passed all checks.

Zig's first invocation emitted warnings in its bundled libunwind Windows runtime;
project source compiled without warnings. No build errors remain. No device was
flashed, and live HTTP responsiveness or physical UART counters have not been
verified on an ESP32.

## Subsequent bench session, 2026-10-10

Default firmware flashed on COM5, ESP32-D0WD-V3 revision 3.1. Boot confirmed;
after Wi-Fi reconnect, root, status and frame endpoints returned HTTP 200.
User measured EN directly connected to chip /RE and DE, and grounded EN on all
modules. User established module TXD = RO/pin1, RXD = DI/pin4 (opposite the
initial naming assumption); README wiring corrected. After swapping data wires,
GPIO35 received bytes but no recognized A0 frames. GPIO33 remained silent.
This is not protocol validation; raw-byte diagnostics were added to investigate.

Both `esp32dev` and `xye_queries` builds pass. C++ tests additionally cover raw
chunk splitting, ring wrap/eviction/sequence order, C0 request bytes/checksum and
address bounds. The expected-query checksum in the new test was corrected from
74 to 7A after an initial assertion failure; the final suite passes. Optional
XYE C0 polling has not been enabled or physically tested. Its UART direction,
reply timing and state machine still need bench validation. Only the default
receive-only build is uploaded at this stage.

User subsequently verified cable segments were swapped from the initial plan:
ODU is the UART1 module on RX35/TX27/EN32; IDU is UART2 on RX33/TX14/EN13.
Firmware assignments and diagnostic labels corrected to match, retaining physical
port numbering (0 = ODU, 1 = IDU). Reversing A/B on the GPIO35 module yielded
15 CRC-valid outdoor-originated frames (20,21,50..53,91) before a later change
to unrecognized 10-byte traffic. Captures are saved under `captures/`.

XYE query build was flashed and explicitly armed after user confirmed prior XYE
coexistence with the factory controller on this unit. First scan using master 0
and 100 ms timeout: 73 queries, no valid replies, three raw RX bytes CCDBD5;
polling was disarmed. User then supplied the previously working AC.zip project.
Its query builder and checksum were inspected as data without running its code;
only whitelisted XYE configuration keys were inspected from its environment file.
Master 63 and one-second response timeout adopted, matching bridge defaults.
Tests pass for the exact unit-0/master-63 query
`AAC0003F803F000000000000003F0355`, reply destination matching, and raw
Fahrenheit-like setpoint preservation. Build/upload results are reported in chat;
live success with these changes is not yet established.

### Five-minute passive capture, 2026-10-10

After unsuccessful additional XYE scans, the default receive-only build was
compiled and flashed on COM5. XYE polling is disabled. A separate low-priority
USB serial logger emits timestamped RX chunks at 115200 baud; its bounded queue
reports drops without blocking UART reception. Both firmware environments build
and the host parser tests pass.

With the factory S1/S2 connection restored by A-A/B-B jumpers, both modules tap
the same bus. The 300.312-second host capture contains 41,420 bytes and 2,071
CRC-valid frames per receiver. The two reconstructed streams are identical;
there are zero CRC failures, unframed bytes, serial sequence gaps or reported
queue drops/UART overruns. Both binary streams also replay successfully through
the C++ host parser. These are duplicate observations, not 4,142 unique frames.

Observed traffic repeats a 24-frame cycle approximately every 3.479 seconds.
Matched ODU/IDU replies have approximately 94 ms median receive timestamp
separation. Timestamps reflect UART chunk reception, not electrical bus edges.
This steady-state capture does not validate transitions, faults or every decoded
field. The HTML report includes raw frames, charts, uncertainty and model-specific
differences. Artifacts: `captures/2026-10-10-passive-five-minutes.*`.

### XYE restored after securing IDU wires, 2026-10-10

User found loose XYE wires at the IDU and confirmed reconnection. The existing
xye_queries build was flashed on COM5 and armed over HTTP. Address 0 validated
using master address 63. The saved snapshot shows 46 queries and 46 valid replies,
zero timeouts and TX errors, one invalid candidate, and fresh decoded status.
Reported error and protection flags are zero. Some stray receive bytes are
visible, so this establishes successful polling, not a noise-free physical link.
Snapshot: captures/2026-10-10-xye-reconnected.json. Read-only C0 polling remains
armed at approximately one query per second; no mode or setpoint writes were sent.

### Local controller and ESP supervisory foundation, 2026-10-10

Eight Python integration tests pass: HTTP authentication, accepted command
routing, invalid controls, demo isolation, persistent history/frame deduplication,
network/storage failures stopping heartbeat renewal, inactive leases/relay faults,
and no replay of completed or indeterminate commands after recovery. Portable
C++ tests pass for exact C3 layout/checksum, bounds and lease expiry/revocation,
alongside the existing parser/decoder suite. An initially mistyped expected C3
checksum in the test fixture was corrected using an independent sum calculation.

All esp32dev, xye_queries and proxy_bench firmware profiles compile. Browser review
verified access-key login, live telemetry from the existing ESP, rendered history
charts and disabled controls for older firmware. A separate demo was verified
with clearly marked simulated data and disabled equipment writes.

No new firmware was flashed, no climate write was sent, and no installed S1/S2
relay was enabled. Physical relay timing/byte fidelity, new C3 read-back on this
ESP implementation, and lease expiry on actual hardware remain unvalidated.
Docker is unavailable on the development computer, so the Docker image and Compose
stack have not been run; Python application execution was tested directly.

### Outbound ESP registration and saved network setup, 2026-10-10

Replaced controller-to-ESP polling with authenticated ESP-initiated registration,
telemetry upload and response-carried commands. Added persistent ESP setup page,
MAC pairing, stale-data rejection, five-second pending command expiry and
at-most-once delivery. Unit coverage includes restart persistence, duplicate
frame handling, authentication separation, reboot, expiry and storage failure.
Actual LAN registration and climate writes still require deployment validation.

All 11 registration/controller tests and the portable C++ suite pass. All three
firmware profiles compile. The embedded setup page was reviewed in the browser
using stub network status. This revision has not been flashed; saved NVS settings
and real outbound ESP registration remain hardware/deployment checks.

### Dockhand deployment without an image build, 2026-10-10

Removed the default Compose build after Dockhand failed creating /root/.docker
before app startup. The stack pulls official Python images, mounts synced source
read-only and initializes the history volume for the non-root app UID. Docker
Compose's official CLI validates the configuration; the controller's 11 tests
still pass. This computer has no Docker daemon, so image pulls, Linux volume
ownership and actual container startup remain deployment-host checks.
