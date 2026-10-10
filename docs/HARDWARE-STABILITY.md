# Hardware stabilization and acceptance

This is the bring-up record for the current three-module MAX3485 prototype.
It is not yet qualified for unattended service. Keep the stock bypass cable
available until a separate hardware bypass has been built and tested.

## Confirmed configuration

| Interface | ESP RX (module RO / silkscreen TXD) | ESP TX (module DI / silkscreen RXD) | EN |
|---|---|---|---|
| XYE | GPIO34 | GPIO25 | GPIO26 |
| ODU S1/S2 | GPIO35 | GPIO27 | GPIO32 |
| IDU S1/S2 | GPIO33 | GPIO14 | GPIO13 |

- All modules use 3.3 V and have a 10 kΩ EN-to-GND pulldown.
- GPIO35 now has a 10 kΩ pull-up to 3.3 V. With unchanged firmware, the
  combined framing counter stopped increasing during a 220-second observation.
- GPIO34 also lacks an internal pull-up. With the harness isolated and the board
  unpowered, inspect for an existing RO-to-3.3 V pull-up on the XYE module.
  If absent, add 10 kΩ from GPIO34/RO to 3.3 V. A power-off resistance reading
  alone may include semiconductor paths and is not definitive resistor evidence.
- GPIO33 supports internal pulls, but its actual idle level during EN HIGH still
  needs verification. An explicit external RX pull-up can be used if necessary.
- RX pull-ups are distinct from the EN pulldowns and differential A/B termination.

## Work sequence

1. Restore stock/bypass communication. Disconnect the complete experimental
   harness from the HVAC and unplug ESP USB power before changing board wiring.
2. Inspect/install the XYE RX pull-up; retain the ODU RX pull-up and all EN
   pulldowns. Secure solder joints/terminals and add strain relief. Keep TTL wiring
   short and separate it from mains wiring.
3. With modules isolated and unpowered, record A-B resistance for each module.
   Do not change onboard termination until the complete segment has been checked.
4. To measure unit-side termination, turn off power to both HVAC units, verify
   the communication conductors are de-energized, and separate the IDU/ODU pairs.
   Record each unit's pair separately, then the intended complete segment with its
   module connected and still unpowered. Never use resistance mode on a live pair.
   Restore the stock cable afterward if the HVAC needs to run during board work.
5. Keep the HVAC harness disconnected. Join only the two S1/S2 modules A-A/B-B,
   reconnect ESP USB power, and flash the prepared `mitm` profile. Network
   configuration persists; forwarding and XYE polling start disarmed.
6. Run both isolated module transmit probes and require exact-byte passes.
   Keep the ESP powered, remove both bench jumpers, and reconnect the separated
   HVAC segments before explicitly arming forwarding.
7. Enable XYE polling, verify status, and exercise a cooling start/stop/restoration
   cycle. Observe the new separate UART framing and parser truncation counters.
8. Perform extended operation, controller unavailability, and physical Wi-Fi-loss
   tests. Reboot/power-loss testing on the live bridge must wait for an operational
   physical bypass; current firmware resets disarmed and stops the bridge.

## Measurements to collect

| Measurement | XYE | IDU S1/S2 | ODU S1/S2 |
|---|---|---|---|
| Isolated module A-B, power off | Pending | Pending | Pending |
| Unit-side pair, power off and separated | Pending | Pending | Pending |
| Complete intended segment, power off | Pending | Pending | Pending |
| Actual module supply voltage | Pending | Pending | Pending |
| RX idle voltage with receiver disabled, isolated bench | Pending | Pending | Pending |
| Powered A/B differential and each conductor-to-reference voltage | Pending | Pending | Pending |

Two 120 Ω resistors in parallel measure approximately 60 Ω; one measures
approximately 120 Ω. These are clues, not automatic pass/fail criteria: bias
networks, electronics, polarity and cable resistance can affect readings. Record
values and topology before selecting termination. The earlier single 121 Ω
reading does not establish the termination on each final separated segment.
See [TI's RS-485 design guide](https://www.ti.com/lit/an/slla272d/slla272d.pdf).

The current breakouts are non-isolated and communicate across two HVAC units.
Actual conductor/reference voltages and grounding compatibility still need to be
documented; communication success alone does not qualify the electrical interface.
Select isolation, isolated power, protection and enclosure construction from those
measurements before designing the permanent board. Do not connect grounded USB
or scope equipment to a bus whose reference compatibility has not been established.

## Hardware bypass concept

Use a de-energized-to-stock, break-before-make arrangement that disconnects both
transceiver pairs when returning to stock. A four-pole changeover contact set can
provide the following logical connections (actual relay pin numbering is not yet
selected):

| Pole | Common | Normally closed (coil off) | Normally open (coil on) |
|---|---|---|---|
| 1 | IDU A | Linked to pole 3 NC | IDU module A |
| 2 | IDU B | Linked to pole 4 NC | IDU module B |
| 3 | ODU A | Linked to pole 1 NC | ODU module A |
| 4 | ODU B | Linked to pole 2 NC | ODU module B |

Coil off joins IDU A to ODU A and IDU B to ODU B, while isolating all four module
A/B terminals. Coil on routes each unit to its respective module. Keeping modules
permanently tapped onto the bypassed bus would leave their termination/loading
present and would not restore the original electrical topology.

This is a topology proposal, not a selected or validated relay circuit. Contact
voltage/isolation rating, low-current signal suitability, linked pole timing,
transient behavior and the treatment of the separate XYE interface need review
against the measured equipment interface. Do not wire a coil directly to an ESP
GPIO. A driver and suitable coil suppression are required.

An independent hardware watchdog should allow the coil to stay energized only
while the local forwarding service is healthy and armed. Controller/network loss
must not release the coil: local unmodified forwarding continues in that case.
ESP power loss, reset, a stopped forwarding task or disarm must release it.
The watchdog/coil circuitry and firmware control pin are not implemented yet.
Software alone cannot provide a bypass when the ESP has no power.

Validate continuity and transitions with HVAC disconnected before a live test.
Then test ESP power removal and watchdog timeout while observing both units; a
relay transition can interrupt a frame, so recovery must be demonstrated rather
than described as seamless.

## Acceptance record

| Check | Current evidence | Remaining |
|---|---|---|
| TX/RX/EN mapping | Both isolated probes passed | Repeat after flash/wiring work |
| ODU RX pull-up | Initial 220-second clean observation | Cooling cycle and extended run |
| XYE RX idle level | Not measured | Inspect pull-up and bench idle voltage |
| Error accounting | Separate counters built/tested | Flash and collect live values |
| Termination/reference/supply | Incomplete measurements | Complete table above |
| Controller loss | 42-second observed failover, automatic recovery | Repeat after final assembly |
| Physical Wi-Fi loss | Not tested | Disconnect/recover without ESP reset |
| ESP power/reset bypass | Stock cable only | Build and validate hardware bypass |
| Long-duration operation | Not completed | Record 24-hour error deltas and resets |

A permanent installation requires secured connectors, strain relief, an enclosure,
a documented power supply and measured voltage stability through Wi-Fi activity
and unit starts. Inspect for brownout/reset events and supply droop; UART counters
cannot prove power integrity. Keep capacity modifications disabled during this
qualification.

Component references:
[MAX3485 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX3483-MAX3491.pdf),
[ESP32 GPIO documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/gpio.html).
