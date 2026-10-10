"""Bench-only serial sender or binary stream generator. Never connect to HVAC."""
import argparse
import pathlib
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', help='Write stream for host_tests instead of serial')
p.add_argument('--port', help='Bench USB RS485 adapter COM port')
p.add_argument('--repeat', type=int, default=10)
p.add_argument('--corrupt', action='store_true')
a = p.parse_args()
if bool(a.output) == bool(a.port):
    p.error('choose exactly one of --output or --port')
lines = (pathlib.Path(__file__).resolve().parents[1] / 'samples/published.txt').read_text().splitlines()
frames = [bytes.fromhex(line) for line in lines if line and not line.startswith('#')]
if a.corrupt:
    b = bytearray(frames[0]); b[9] ^= 1; frames.insert(0, bytes(b))
if a.output:
    pathlib.Path(a.output).write_bytes(b''.join(frames) * a.repeat)
else:
    import serial
    with serial.Serial(a.port, 4800, timeout=1) as s:
        for _ in range(a.repeat):
            for frame in frames:
                s.write(frame); s.flush(); time.sleep(0.15)
