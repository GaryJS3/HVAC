# ESP and local controller

The ESP owns UART timing, monitoring and XYE transactions. The local controller
owns the dashboard, SQLite history and explicit high-level user commands. Bus
forwarding never depends on an HTTP request or the container's scheduling.

## Current capabilities

- Two receive-only S1/S2 inputs and optional XYE status polling.
- Authenticated heartbeat, a 10-second local lease and boot identity.
- C3 mode/fan/temperature requests, bounded pending work, no automatic retries,
  and C0 read-back status. Builders follow the user's working AC project. New
  writes still require live validation. Temperature is the unit's raw Fahrenheit
  byte, restricted to whole 60..86 F. Fan supports auto/low/medium/high; turbo
  encoding is unverified and is not offered.
- Web dashboard, frequency/fan/power/temperature history, communication counters,
  raw frame archive and command audit trail. SQLite retains 30 days of records;
  chart API displays the latest 1,800 samples. Power remains provisional.
- Separate `proxy_bench` build relays raw incoming chunks to the other UART,
  including unknown commands and invalid CRCs. No capacity rewriting exists.

The bench relay is not validated for installed HVAC. Its bounded queue, blocking
TX completion and receiver-disable periods must be measured for latency, gaps,
echo and simultaneous traffic. FIFO/queue errors are visible and indicate loss,
not successful failover. This profile must never run with A-A/B-B jumpers between
the two modules: that would feed forwarded traffic back into the same bus.
Keep the installed unit on the receive-only S1/S2 profile until bench validation.

## Failure and recovery contract

At boot the ESP has no supervisory lease. Successful authenticated outbound registration/telemetry exchanges
renew it for 10 seconds using ESP monotonic time. Container failure, network loss
or storage failure stops renewal. In the bench relay profile, raw forwarding
continues unchanged in both supervised and failover states. Monitoring profiles
report standalone monitoring, never claim to be forwarding.

Delivered commands are tied to the current ESP boot ID and have a two-second
local transmit deadline after a successful exchange. Old commands are not persisted as desired state or replayed
after reconnection. C3 confirmation is a later matching C0 mode/setpoint/fan
read-back, not just a successful socket write. A timeout is unverified, not proof
the unit ignored a write. Applied climate settings remain on the factory unit;
lease loss does not undo a thermostat setting. Future capacity policies must
explicitly expire locally and require fresh authorization on recovery.

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
