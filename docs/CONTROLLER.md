# ESP and local controller

The ESP owns UART timing, monitoring and XYE transactions. The local controller
owns the dashboard, SQLite history and explicit high-level user commands. Bus
forwarding never depends on an HTTP request or the container's scheduling.

## Current capabilities

- Two receive-only S1/S2 inputs and optional XYE status polling.
- Authenticated heartbeat, a 10-second local lease and boot identity.
- C3 mode/fan/temperature requests, bounded pending work, no automatic retries,
  and C0 read-back status. Builders follow the user's working AC project. A 79→80→79°F Cool/Auto setpoint test passed with XYE read-back; other
  mode/fan combinations still need live validation. Temperature is the unit's raw Fahrenheit
  byte, restricted to whole 60..86 F. Fan supports auto/low/medium/high; turbo
  encoding is unverified and is not offered.
- Web dashboard, frequency/fan/power/temperature history, communication counters,
  raw frame archive and command audit trail. SQLite retains 30 days of records;
  chart API displays the latest 1,800 samples. Power remains provisional.
- `mitm` (and compatibility `proxy_bench`) adds explicitly armed, buffered
  forwarding to the opposite UART. Known-length frames are sent whole; noise and
  incomplete/damaged lengths drain after an 8 ms idle gap. Unknown commands and
  invalid CRC bytes are preserved exactly. An explicitly enabled cooling-demand
  ceiling can lower validated IDU core requests; it starts disabled.

The bench relay is not validated for installed HVAC. Its bounded queue, blocking
TX completion and receiver-disable periods must be measured for latency, gaps,
echo and simultaneous traffic. FIFO/queue errors are visible and indicate loss,
not successful failover. Forwarding must never be armed with A-A/B-B jumpers between
the two modules: that would feed forwarded traffic back into the same bus. The
MTM build starts disarmed, so it can be flashed with those jumpers still installed.
Keep the installed unit on the receive-only S1/S2 profile until bench validation.

## Failure and recovery contract

At boot the ESP has no supervisory lease. Successful authenticated outbound registration/telemetry exchanges
renew it for 10 seconds using ESP monotonic time. Container failure, network loss
or storage failure stops renewal. Once armed, MTM forwarding
continues in both supervised and failover states. An enabled demand ceiling
expires on lease loss; failover forwards original packets. It is disarmed on
every ESP reset and requires both transceiver bench tests again before arming. Monitoring profiles
report standalone monitoring, never claim to be forwarding.

Delivered commands are tied to the current ESP boot ID and have a two-second
local transmit deadline after a successful exchange. Old commands are not persisted as desired state or replayed
after reconnection. C3 confirmation is a later matching C0 mode/setpoint/fan
read-back, not just a successful socket write. A timeout is unverified, not proof
the unit ignored a write. Applied climate settings remain on the factory unit;
lease loss does not undo a thermostat setting. The cooling-demand ceiling expires locally and requires explicit re-enabling
after controller recovery.

The browser is a client of the container. Closing a browser does not stop a healthy
container's telemetry responses. ESP reset, loss of power or relay-task failure cannot be
covered by software forwarding. A normally closed physical bypass is needed if
those failures must preserve the factory electrical connection.

## Configure and deploy

1. Copy `.env.example` to ignored `.env`. Set ESP_TOKEN to a random device key
   of at least 16 characters and CONTROLLER_TOKEN to a different browser key.
   There is no ESP_URL or ESP IP setting in the controller.
2. Leave CONTROL_ENABLED=false initially. Set BIND_ADDRESS to the host's private
   LAN interface (or 0.0.0.0 on a trusted LAN). The default loopback binding is
   for local review/reverse proxy and cannot receive direct ESP LAN requests.
3. Deploy the repository as a Compose stack: `docker compose up -d`.
   No image build is required: both services use the official `python:3.12-slim`
   image, and the controller mounts the Git-synced `controller/` directory read-only.
   That directory must exist on the Docker host beside compose.yaml.
   History uses a named volume. A one-shot `init-history` service sets its directory
   owner to UID/GID 10001; successful exit is normal. The app runs as that non-root
   user with a read-only root filesystem. The user handles Dockhand Git syncing.
   Restart the controller service after syncing Python code changes; a running
   Python process does not reload its modules automatically.
4. Flash `xye_queries` for the currently jumpered topology. Open the ESP's
   HVAC-Diagnostics access point at http://192.168.4.1/. Enter Wi-Fi SSID/password,
   controller IP or DNS hostname, port (default 8080), and the matching ESP_TOKEN.
   Blank password/key fields preserve existing values; the open-network checkbox
   explicitly clears a saved Wi-Fi password. Saving restarts the ESP.
5. Existing configurations require the current device key to change settings.
   An initial unconfigured ESP permits setup only through its diagnostic AP.
   The page never returns saved passwords or keys. Settings persist in one NVS
   record across restarts. LocalConfig.h still supplies initial defaults and the
   diagnostic AP password. The diagnostic AP remains available for recovery.
6. The ESP initiates an authenticated POST to `/api/devices/register` every two
   seconds, containing stable MAC identity, boot ID, telemetry and recent frames.
   The controller pairs with the first ESP and persists that identity. This
   initial version handles one ESP per controller instance. To replace an ESP,
   remove only the `device_id` row from the SQLite settings table while stopped.
7. Open the controller on port 8080 and enter CONTROLLER_TOKEN. Confirm fresh
   telemetry. Set CONTROL_ENABLED=true when ready to validate climate writes.

Commands return in the next ESP registration response, never through an inbound
connection to the ESP. Only one command may wait at a time; it expires after five
seconds and is dropped on an ESP reboot. Delivery is attempted once. A lost HTTP
response can leave an unknown outcome; check live settings before retrying. The
ESP validates commands locally and reports rejection or subsequent XYE read-back.
No queued command survives a container restart. Success responses renew the local
10-second lease only after telemetry is saved. No response means no renewal.
Frame history deduplicates overlapping ESP ring snapshots. This is best-effort
archival: a network outage or ring overflow can lose frames; the ESP does not have
a durable capture spool.

The application uses Python's standard library and runs without pip packages.
Its built-in HTTP server is intended for this private local deployment. Use a
trusted LAN or TLS reverse proxy because bearer tokens over plain HTTP are not
encrypted. No token is embedded in the HTML. Successful browser sign-in saves the access key
in sessionStorage for that tab session, allowing page reloads. Sign out clears it;
a rejected key also clears the saved sign-in. It is not saved in localStorage.

## Local review and tests

PowerShell example for simulated review (never sends equipment commands):

```powershell
$env:CONTROLLER_TOKEN = 'demo-review-access-key'
python controller/app.py --demo --port 8089
```

Run `python -m unittest discover -s controller -p test_app.py` and
`python tools/run_tests.py`. Compile all firmware profiles with
`python -m platformio run -e esp32dev -e xye_queries -e proxy_bench`.

For a physical relay bench test, keep the HVAC disconnected. Send known and noisy
streams from each side separately and verify exact output bytes on the opposite
side; then test simultaneous bursts, saturation, reboot, expired heartbeat and
recovery. Measure wire timing externally. A successful compilation is not that
validation. Container startup/shutdown, persistent volume ownership and actual
Dockhand networking also require testing on the deployment host.

## Dockhand build failure: /root/.docker is read-only

The reported `Image hvac-controller Building` followed by
`mkdir /root/.docker: read-only file system` happens in the deployment/build
process before the app starts. The app service's `read_only: true` applies to the
running container, so removing it would not repair the builder's filesystem.

The default stack now uses a published Python image and has no `build:` section.
Resync main and redeploy with the existing environment keys. This avoids the
failing image-build step. The optional Dockerfile remains for environments with
a working builder. If a subsequent pull also fails writing Docker client config,
the Dockhand process itself needs a writable DOCKER_CONFIG directory; Compose
service environment settings cannot change that parent process.

## MTM bring-up sequence

1. Flash `python -m platformio run -e mitm -t upload --upload-port COM5`.
   Saved Wi-Fi/controller settings survive flashing. All direction pins start LOW;
   both S1/S2 drivers stay disabled until explicit arming or a requested bench probe.
2. Keep the ESP powered over USB. Disconnect the HVAC harness from the unit; put
   the unit back on its stock/bypass cable if it needs to keep operating. Leave
   just the two S1/S2 modules connected together with their A-A/B-B jumpers.
3. Sync main and restart the controller Python service for its new command routes.
   Under MTM forwarding → Transceiver bench test, confirm HVAC is disconnected.
   Run ODU module TX and IDU module TX separately. Each sends 24 diagnostic bytes
   from one module and requires an exact match at the other. This checks TX/RX,
   EN assignment and differential polarity; it is not a complete relay timing test.
4. Both tests must pass in this ESP boot before arming is accepted. Keep the ESP
   powered, remove A-A/B-B jumpers, then reconnect the HVAC harness with IDU and ODU
   on separate module segments. Confirm the separated-segments checkbox and arm.
   Arming waits for 100 ms quiet on both inputs and expires after two seconds if
   that is not possible. Watch both directional byte counters, temperatures,
   unit operation and all transport errors during the initial supervised test.
5. Record queue delay, assembly delay and TX duration. These are UART-drain/task
   metrics, not oscilloscope wire timing. Output is serialized, and waits at least
   8 ms after target RX activity. A bounded queue or UART fault can still lose bytes;
   errors/dropped-byte counters are explicit. Unknown/noisy data is never rewritten.
6. Disconnect controller networking while observing the ESP and HVAC locally.
   Forwarding must keep operating as failover_passthrough. Restore networking and
   confirm supervised_passthrough without replaying old commands.

Disarming completes any packet already being transmitted; buffered/queued data is
then discarded and counted rather than replayed on a later arm. Disarming or an
ESP reset stops the electrical bridge, so restore physical jumpers/stock bypass
when necessary. There is no power/reset hardware bypass in this prototype.
The probes prove the module path only. Full directional byte fidelity, burst and
latency measurements with an independent RS485 source/analyzer remain bench work
before claiming the transport is production validated.

## Transient fault recovery

The dashboard shows six temperature cards in Fahrenheit and Celsius, plus all
decoded fields separately for the IDU-side and ODU-side inputs. Each measurement
includes freshness, age at the received sample, and interpretation confidence.
Unavailable values remain labeled rather than displayed as zero; stale values
are retained with a last-known label. Coil temperatures, discharge temperature,
current, DC bus voltage and valve positions can also be selected in history.
Diagnostics expose every communication counter, the complete XYE status, and
the received snapshot for fields not yet given a dedicated display. Protocol
power and other provisional fields retain their qualification labels.

XYE polling stays armed after a transmit failure or missed reply. It backs off
1, 2, 4, 8, 16, then at most 30 seconds between recovery attempts; a validated
reply resets the backoff. Three consecutive response misses still invalidate
the address and restart discovery. Explicitly switching polling off stops retries.
An absent address during discovery uses a one-second retry delay, so a full scan
does not inherit a 30-second wait per address. Explicitly restarting polling
without a validated address starts discovery again at address zero.
`failure_streak` (saturated at 6) and `retry_delay_ms` expose recovery state;
error/timeout totals remain cumulative. UART TX completion allows 150 ms for the
33 ms query plus scheduling margin, and checks prior TX completion before sending.

Relay queue/idle/TX faults continue to count errors and lost bytes while the
worker keeps forwarding subsequent traffic. `proxy.fault_active` pauses climate
changes and re-arming for 10 seconds after the latest fault, then automatically
clears; historical totals alone do not latch controls off. Continued faults extend
the pause. Older firmware without this field retains the previous conservative
controller/UI behavior. Parser truncations and CRC failures do not stop transport.

Uncertain relay packets and climate writes are never automatically retransmitted:
they may already have reached the unit. Polling reads recover independently, and
the command remains failed/unconfirmed until the operator reviews current status.
Stale XYE/device telemetry still blocks climate changes. Controller/network loss
still leaves local forwarding active; these changes do not implement power bypass.

Deploy the controller update and restart its container. The firmware update must
be flashed with the HVAC on its stock bypass and the experimental harness isolated,
then both bench probes repeated before reconnecting and arming.


## Experimental cooling-demand ceiling

Use the separate **Cooling demand ceiling** form to enable/update a whole-number
20..120 Hz ceiling, or disable it. This is a software configuration range, not a
validated operating-frequency range or a claim about the compressor minimum.
For example, a factory 40 Hz request becomes 20 Hz with a 20 Hz ceiling; a 15 Hz
or zero request remains unchanged. Acknowledgment confirms configuration only.
Actual compressor Hz and factory protections remain independently controlled by
the unit; live effectiveness and stability have not yet been established.

The ESP rewrites only CRC-valid, exactly 20-byte IDU core frames arriving on the
IDU port: address 0100, type 20, payload length 12, byte 5 equal to 11, indoor mode
Cool (01), byte 8 zero, request byte 7 in 0..120. A recent (within 15 seconds)
CRC-valid ODU core frame must also report outdoor Cool (01). It changes byte 7
only when above the ceiling, then recomputes the two CRC bytes. Lower requests,
heating, defrost, dry, forced modes, startup flags, unknown layouts, corrupt or
truncated packets and all other fields pass unchanged. It never increases demand
or fabricates responses. Unknown bytes/protection semantics are not decoded by
this feature, so these guards do not constitute equipment qualification.

The policy starts disabled and is never persisted as desired state. Enabling
requires armed forwarding, a fresh controller lease and no active transport fault.
Lease expiry (10 seconds), transport fault, disarm or reboot disables the policy;
recovery does not silently re-enable it. Forwarding continues with original data.
A packet already selected/in flight may finish before a disable takes effect.

`device.proxy.ceiling` (also under local `/api/proxy`) reports configuration,
command acknowledgment, reason, modification count, last original/sent hex and
factory/forwarded demand from successfully transmitted IDU core frames. These
are software TX observations, not an independent wire capture. Raw received
frames remain original. The history selector compares factory demand, forwarded
demand and decoded actual compressor frequency. Configuration commands are
boot-bound, delivered once and audited, using `kind: demand_ceiling`, `enabled`
and (when enabling) `maximum_hz`.

Validation before live use: flash while bypassed/isolated, repeat both module
probes, reconnect separated segments and arm with the ceiling disabled. Compare
a baseline cooling ramp, then enable a conservative ceiling and compare original
and sent hex/CRC, ODU target/actual frequency, temperatures, power and errors.
Disable to verify original demand resumes. Finally test controller loss while
limited and require the policy to expire with forwarding continuing; restore
controller connectivity and require the ceiling to stay disabled.
