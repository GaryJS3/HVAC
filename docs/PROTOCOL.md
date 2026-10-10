# Protocol evidence and scope

References inspected on 2026-10-10:

- [MidATRIX S1/S2 README](https://github.com/MidATRIX/midea-s1s2-rs485-monitor),
  commit `656474563d80f921dd9e99c1d066463e075a7624`.
  Inspected README, example configuration and repository tree. The example
  references `components/midea_s1s2/midea_s1s2.cpp`, but that directory/source is
  absent from this checkout. Thus no available decoder implementation could be
  studied there; the parser follows the README's framing/CRC specification.
- [HomeOps XYE protocol](https://github.com/HomeOps/ESPHome-Midea-XYE/blob/main/esphome/components/midea_xye/PROTOCOL.md),
  commit `2c648a7a34e3fd3c80f68fe3ed640bfa27c73ab2`.
  Studied PROTOCOL.md, xye.h checksum, xye_send.cpp constructors/checksum update
  and xye_recv.cpp response validation. No ESPHome code is linked into firmware.

## S1/S2

Candidate 4800 8N1. `A0 address_hi address_lo command payload_length payload
trailer crc_lo crc_hi`. Total length is payload_length + 8 (8..263 bytes).
CRC-16/MODBUS starts at FFFF, reflected polynomial A001, covers everything
through the trailer. The trailer is not assumed constant. Address 0001 is
outdoor-originated and 0100 indoor-originated in the reference; unknown sources
and commands are accepted as raw CRC-valid frames. There is no assumed end byte.

The parser accepts all one-byte payload lengths; this is the complete legal
length range according to the reference. Sensor decoding requires exactly the
documented 12-byte payload, independently verified CRC and matching source/type.
Bad CRC advances one byte and scans for the next A0. An incomplete candidate is
reported after a 50 ms software-observed idle gap, then each suffix is examined
to recover complete frames concealed by a corrupted length. A0 inside payload
is allowed. This idle threshold is a diagnostic heuristic requiring hardware
validation. No claims of proxy timing transparency are made.

Published captures are in `samples/published.txt`. The IDU example includes one
extra trailing 00 after the CRC; tests deliberately preserve it as stream noise.
Generated extended/edge-case tests are synthetic, not additional real captures.
No SENDC-60HF captures were supplied.

## Telemetry

Field offsets are absolute frame indices, matching the reference README. IDU
20: mode 6, requested Hz 7 (probable), setpoint 11, blower enum 12 (probable),
ambient NTC 13 and coil NTC 14 (probable). ODU 20: actual Hz 6 (probable), coil
NTC 9, ambient NTC(10+15/256), discharge curve 11 (probable), current 12/1.875
(probable), mode 14. ODU 50: fan 11*8 RPM, EXV 12*2 (probable), DC voltage
14*2-29 (probable). ODU 51: target fan 5*8, target EXV 6*2 (probable), session
minutes 11, lifetime hours 12+256*13. ODU 52: signed PID step 9 and fan gear 13
(both probable). ODU 53: compressor state 9, power 11+256*12 (probable), target
Hz 13. Handshakes, keepalives and indoor acknowledgments do not update sensors.

NTC(V) = 1/(1/298.15 + ln(0.81*(255-V)/V)/4150)-273.15.
Discharge uses L=ln((255-V)/V), then
1/(0.002873+0.0002491*L+0.000000974*L^3)-273.15.
NTC endpoint values 0/255 become invalid/null instead of fabricated temperatures.
Unknown input-voltage calibration, averaged-frequency candidates, zones and
protection semantics are intentionally retained raw. Power is a probable system
measurement, not a verified compressor-only electrical consumption value.

## XYE status-query foundation

XYE uses AA/55 delimiters, 16-byte requests and 32-byte responses. Checksum is
FF minus the low-byte sum of **all other bytes including AA and 55**, excluding
the checksum position. It is not the S1/S2 CRC. C0/C4 query, C3 set, C6 follow-me
and 130 ms address polling slots are reference information. The opt-in query
build sends only C0 at most once per second, with a one-second reply timeout after
hardware TX completion. C0 decoding masks response direction bit
7 and reads device address from byte 4 without assuming zero. The setpoint byte
is preserved raw because the user's working project supports Fahrenheit values
that overlap the generic reference's bit-6 flag; Celsius is not assumed.
Error/protection words are preserved. Discovery scans unicast addresses
0..63 and requires a matching source and master destination fields. Live request
timing, address discovery and reply correlation have now been validated at unit
address 0/master 63 after the user secured loose XYE wires. New C3 writes and C0
read-back matching are implemented behind authenticated controller access, but
are not yet live-validated. See CONTROLLER.md for scope and expiry semantics.

The user supplied AC.zip from a previously working installation on this unit.
Its `xye_bridge.py` and `xye_hvac.py` use the same checksum/query layout, but
default the master address to 63 (rather than our original 0). The bridge's
reply window is one second. Firmware now uses master 63 in request bytes 3/5
and checks it in reply bytes 3/5, with the longer reply window. The unit address
default of 0 is a lead only; discovery still validates the actual reply address.
The archive's bridge used a Waveshare TCP/RS485 adapter, not ESP GPIO direction
control, so it does not establish wiring/polarity/timing for these MAX3485 modules.
