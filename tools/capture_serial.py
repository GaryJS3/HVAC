"""Record RX/HEALTH lines without toggling ESP reset pins; retain original bytes."""
import argparse
import datetime
import json
import pathlib
import time
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', default='COM5')
p.add_argument('--seconds', type=float, default=300)
p.add_argument('--output', required=True)
a = p.parse_args()
path = pathlib.Path(a.output)
path.parent.mkdir(parents=True, exist_ok=True)
s = serial.Serial(port=None, baudrate=115200, timeout=0.5)
s.dtr = False
s.rts = False
s.port = a.port
s.open()
start = time.monotonic()
total = 0
meta = {'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'port': a.port, 'baud': 115200, 'requested_seconds': a.seconds}
try:
    with path.open('wb') as f:
        while time.monotonic() - start < a.seconds:
            data = s.read(4096)
            if data:
                f.write(data)
                f.flush()
                total += len(data)
finally:
    s.close()
    meta.update(elapsed_seconds=time.monotonic()-start, bytes=total)
    path.with_suffix('.meta.json').write_text(json.dumps(meta, indent=2))
print(json.dumps(meta))
