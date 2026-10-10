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

At boot the ESP has no supervisory lease. Successful authenticated heartbeats
renew it for 10 seconds using ESP monotonic time. Container failure, network loss
or storage failure stops renewal. In the bench relay profile, raw forwarding
continues unchanged in both supervised and failover states. Monitoring profiles
report standalone monitoring, never claim to be forwarding.

Commands carry the current ESP boot ID and expire within two seconds, bounded by
the lease deadline. Old commands are not persisted as desired state or replayed
after reconnection. C3 confirmation is a later matching C0 mode/setpoint/fan
read-back, not just a successful socket write. A timeout is unverified, not proof
the unit ignored a write. Applied climate settings remain on the factory unit;
lease loss does not undo a thermostat setting. Future capacity policies must
explicitly expire locally and require fresh authorization on recovery.

The browser is a client of the container. Closing a browser does not stop a healthy
container's heartbeat. ESP reset, loss of power or relay-task failure cannot be
covered by software forwarding. A normally closed physical bypass is needed if
those failures must preserve the factory electrical connection.

## Configure and deploy

1. In ignored `include/LocalConfig.h`, configure the normal LAN Wi-Fi SSID/password
   and a random `HVAC_CONTROLLER_TOKEN` (at least 16 characters). Keep diagnostic
   AP access for recovery. Build `xye_queries` for the currently jumpered topology.
2. Copy `.env.example` to ignored `.env`. Set ESP_URL to the ESP's reachable LAN
   IP, ESP_TOKEN to the firmware token, and CONTROLLER_TOKEN to a different random
   browser key. The Dockhand host must have a route to that IP; 192.168.4.1 is only
   usable from a host attached to the diagnostic AP.
3. Leave CONTROL_ENABLED=false initially. Set BIND_ADDRESS to the intended private
   host interface if direct LAN access is wanted, or leave loopback for a reverse
   proxy. Do not publish this control service directly to the Internet.
4. Deploy this workspace as a Compose stack: `docker compose up -d --build`.
   Dockhand must receive the controller build context as well as compose.yaml.
   An existing stack needs the matching environment values. History uses a named
   volume. The container runs as UID 10001 with a read-only root filesystem.
5. Open port 8080 and enter CONTROLLER_TOKEN. Confirm current telemetry and
   heartbeat status. Set CONTROL_ENABLED=true only when ready to validate writes.

The application uses Python's standard library and runs without pip packages.
Its built-in HTTP server is intended for this private local deployment. Use a
trusted LAN or TLS reverse proxy because bearer tokens over plain HTTP are not
encrypted. No token is embedded in the HTML or persisted in browser storage.

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
validation. Container shutdown/restart, persistent volume ownership and actual
Dockhand networking also require testing on the deployment host.
