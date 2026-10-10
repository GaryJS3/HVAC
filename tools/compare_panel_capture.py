"""Compare saved panel history with the setpoint experiment, retaining clock uncertainty."""
import csv
import datetime as dt
import html
import json
from pathlib import Path
import statistics
import sys

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1])
capture = root / 'captures/2026-10-10-setpoint-ramp.log'
meta = json.loads(capture.with_suffix('.meta.json').read_text())
analysis = json.loads(capture.with_suffix('.analysis.json').read_text())
rows = list(csv.DictReader(source.open(encoding='utf-8-sig')))
assert len({r['entity_id'] for r in rows}) == 1
panel = [(dt.datetime.fromisoformat(r['last_changed'].replace('Z', '+00:00')), float(r['state'])) for r in rows]
assert all(a[0] <= b[0] for a, b in zip(panel, panel[1:]))
edt = dt.timezone(dt.timedelta(hours=-4))
clock = lambda t: t.astimezone(edt).strftime('%H:%M:%S')
def window(start, end):
    values = [v for t, v in panel if start <= t.astimezone(edt).strftime('%H:%M:%S') < end]
    return {'samples': len(values), 'min_w': min(values), 'median_w': statistics.median(values), 'max_w': max(values)}
summary = {'source': str(source), 'samples': len(panel),
           'start_edt': clock(panel[0][0]), 'end_edt': clock(panel[-1][0]),
           'baseline_0200_0210': window('02:00:00', '02:10:00'),
           'high_output_0212_021330': window('02:12:00', '02:13:30'),
           'residual_0215_0220': window('02:15:00', '02:20:00')}
peak = max(panel, key=lambda x: x[1])
summary['peak'] = {'edt': clock(peak[0]), 'watts': peak[1]}
summary['panel_shutdown_drop'] = next({'edt': clock(t), 'watts': v} for t, v in panel if clock(t) >= '02:13:00' and v < 2000)
summary['panel_residual_start'] = next({'edt': clock(t), 'watts': v} for t, v in panel if clock(t) >= '02:13:00' and v < 300)
start = dt.datetime.fromisoformat(meta['started_utc'])
first = None
previous = {}
changes = []
for line in capture.read_text().splitlines():
    s = line.split()
    if len(s) != 6 or s[0] != 'RX':
        continue
    us, port = int(s[1]), int(s[2])
    if first is None:
        first = us
    b = bytes.fromhex(s[5])
    if port != 1 or len(b) != 20:
        continue
    values = ({'setpoint_c_reference': b[11], 'demand_hz_probable': b[7]}
              if b[1:4] == bytes([1, 0, 32]) else
              {'actual_hz_probable': b[6]} if b[1:4] == bytes([0, 1, 32]) else
              {'target_hz_reference': b[13], 'compressor_state_raw': b[9]}
              if b[1:4] == bytes([0, 1, 83]) else {})
    for name, value in values.items():
        if previous.get(name) != value:
            changes.append({'approx_host_edt': clock(start + dt.timedelta(microseconds=us-first)),
                            'seconds_from_first_rx': (us-first)/1e6, 'field': name, 'value': value})
        previous[name] = value
summary['protocol_changes'] = changes
summary['clock_warning'] = 'Panel transitions precede host-anchored protocol transitions by roughly 30–35 seconds. No synchronization or correction was applied. Do not infer exact cross-device latency.'
summary['capture_analysis'] = analysis
capture.with_suffix('.power-comparison.json').write_text(json.dumps(summary, indent=2))
def table(headers, body):
    return '<table><thead><tr>'+''.join('<th>'+html.escape(str(x))+'</th>' for x in headers)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+html.escape(str(x))+'</td>' for x in row)+'</tr>' for row in body)+'</tbody></table>'
power_table = table(['Panel period (EDT)', 'Minimum W', 'Median W', 'Maximum W'],
    [(label, *(f'{summary[key][v]:,.1f}' for v in ('min_w', 'median_w', 'max_w')))
     for label, key in [('02:00–02:10 baseline', 'baseline_0200_0210'), ('02:12–02:13:30 high output', 'high_output_0212_021330'), ('02:15–02:20 residual', 'residual_0215_0220')]])
transitions = table(['Approximate host time (EDT)', 'Protocol field', 'Value'],
                    [(c['approx_host_edt'], c['field'], c['value']) for c in changes])
report = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Setpoint experiment and panel power</title>
<style>body{font:16px/1.6 system-ui,sans-serif;background:#f3f7f7;color:#172c38;margin:0}main{max-width:1000px;margin:auto;padding:32px 24px}h1{line-height:1.2}section{background:white;border:1px solid #d6e2e5;border-radius:10px;padding:24px;margin:24px 0}table{border-collapse:collapse;width:100%;font-size:14px}td,th{text-align:left;padding:9px;border-bottom:1px solid #ddd}th{background:#eef4f4}.note{border-left:4px solid #a66b00;padding:12px;background:#fff8e9}a{color:#087f78}</style><main>
<h1>Setpoint experiment and panel power</h1><p>October 10, 2026. Local times are EDT (UTC−4). Passive S1/S2 capture: 02:09:56 to approximately 02:19:57. Panel history: 400 readings from 02:00:00 to 02:19:57.</p>
<section><h2>What this establishes</h2><p>Lowering the thermostat produced a candidate indoor demand of 75. Outdoor target and candidate actual frequency rose from 20 to 62 Hz. Raising the thermostat produced zero demand, then zero target and actual frequency. This supports the demand interpretation and shows that the outdoor target can remain below the indoor request. It does not prove that modified requests will be accepted.</p>
<p>Panel power rose from approximately 1.2 kW to a peak of '''+f'{peak[1]:,.0f} W at {clock(peak[0])}'+'''. The sharp decline appears at 02:13:30, and readings reach approximately 230 W at 02:14:30.</p>
<p class="note">'''+html.escape(summary['clock_warning'])+''' The host mapping anchors the first serial RX timestamp to capture-open time. Logger buffering and clock differences have not been independently measured.</p></section>
<section><h2>External panel measurements</h2>'''+power_table+'''<p>The final plateau is stable near 230 W. This alone cannot distinguish a real standby load from monitor error or other included loads. No baseline subtraction or calibration correction was applied.</p></section>
<section><h2>Power field remains uncalibrated</h2><p>The provisional protocol power field ranges from 0 to 3,030 W, versus a panel peak near 4,806 W for IDU and ODU combined. That mismatch does not establish which measurement is wrong: the protocol field may have different scope or scaling. Treat it as a raw candidate rather than calibrated total power.</p></section>
<section><h2>Capture integrity</h2><p>Port 1 contains 4,142 valid frames with no sequence gaps and is used for the transition table. Port 0 contains 4,141 valid frames and one sequence gap. Both show zero CRC-failed candidates and unframed bytes. Firmware health reports zero logger drops and UART overruns. The missing port-0 record is retained as a limitation, rather than assuming both copies were complete.</p></section>
<section><h2>Protocol transitions</h2><p>These are approximate host-clock times, not synchronized panel times. Setpoint bytes descend to 16 and rise to 26, consistent with the user’s 60°F and 80°F settings using integer Celsius representation.</p>'''+transitions+'''</section>
<p>Sources: user-supplied history.csv (entity sensor.pwrmon3_critload_main_hvac_watts_2), saved serial RX data, capture metadata, and user action notes. The source CSV is unchanged.</p>
<p><a href="2026-10-10-setpoint-ramp.log">Original serial capture</a> · <a href="2026-10-10-setpoint-ramp.power-comparison.json">Comparison data</a> · <a href="2026-10-10-setpoint-ramp-notes.md">Action notes</a></p></main></html>'''
capture.with_suffix('.html').write_text(report, encoding='utf-8')
print(json.dumps({k: v for k, v in summary.items() if k not in ('protocol_changes', 'capture_analysis')}, indent=2))
