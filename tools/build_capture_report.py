"""Build an offline HTML review of the recorded five-minute passive capture."""
import collections
import json
import math
import pathlib
import statistics

root = pathlib.Path(__file__).resolve().parents[1]
capture = root / 'captures/2026-10-10-passive-five-minutes.log'
analysis = json.loads(capture.with_suffix('.analysis.json').read_text())
meta = json.loads(capture.with_suffix('.meta.json').read_text())
frames = []
streams = [[], []]
for line in capture.read_text().splitlines():
    fields = line.split()
    if len(fields) == 6 and fields[0] == 'RX':
        stamp, port = int(fields[1]), int(fields[2])
        data = bytes.fromhex(fields[5])
        streams[port].append(data)
        if port == 0:
            assert len(data) == 20 and data[0] == 0xa0
            frames.append({'us': stamp, 'source': 'ODU' if data[1:3] == b'\0\1' else 'IDU',
                           'cmd': f'{data[3]:02X}', 'hex': data.hex().upper()})
assert streams[0] == streams[1]
first_us = frames[0]['us']
for frame in frames:
    frame['seconds'] = round((frame['us'] - first_us) / 1e6, 6)

handshakes = [f['us'] for f in frames if f['source'] == 'ODU' and f['cmd'] == '21']
periods = [(b-a)/1000 for a, b in zip(handshakes, handshakes[1:])]
start = next(i for i, f in enumerate(frames) if f['source'] == 'ODU' and f['cmd'] == '21')
end = next(i for i in range(start+1, len(frames)) if frames[i]['source'] == 'ODU' and frames[i]['cmd'] == '21')
sequence = [f[ 'source'] + ' ' + f['cmd'] for f in frames[start:end]]

def ntc(value):
    return 1 / (1/298.15 + math.log(.81*(255-value)/value)/4150) - 273.15

series = collections.defaultdict(list)
examples = {}
for frame in frames:
    b = bytes.fromhex(frame['hex'])
    examples.setdefault(frame['source']+'_'+frame['cmd'], frame['hex'])
    values = {}
    if frame['source'] == 'IDU' and frame['cmd'] == '20':
        values = {'Indoor ambient °C': ntc(b[13]), 'Indoor coil °C (probable)': ntc(b[14]),
                  'Requested Hz (probable)': b[7], 'Setpoint °C': b[11]}
    if frame['source'] == 'ODU' and frame['cmd'] == '20':
        values = {'Outdoor ambient °C': ntc(b[10]+b[15]/256), 'Outdoor coil °C': ntc(b[9]),
                  'Actual Hz (probable)': b[6], 'Current A (probable)': b[12]/1.875}
    if frame['source'] == 'ODU' and frame['cmd'] == '53':
        values = {'Target Hz': b[13], 'System power W (probable)': b[11]+256*b[12]}
    if frame['source'] == 'ODU' and frame['cmd'] == '50':
        values = {'Outdoor fan RPM': b[11]*8, 'EXV steps (probable)': b[12]*2,
                  'DC bus V (probable)': b[14]*2-29}
    for name, value in values.items():
        series[name].append([frame['seconds'], round(value, 3)])

payload = {'frames': frames, 'analysis': analysis, 'meta': meta, 'series': series,
           'examples': examples, 'sequence': sequence,
           'cycle': {'min': min(periods), 'mean': statistics.mean(periods), 'max': max(periods)}}
template = r'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Senville S1/S2 capture review</title>
<style>
:root{color-scheme:light;--ink:#172c38;--muted:#516674;--teal:#087f78;--line:#d6e2e5;--paper:#f3f7f7}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.6 system-ui,sans-serif}
main{max-width:1180px;margin:auto;padding:42px 28px 80px}header{padding:12px 0 26px;border-bottom:3px solid var(--teal)}
.eyebrow{font-size:12px;letter-spacing:.16em;text-transform:uppercase;color:var(--teal);font-weight:750}
h1{font-size:clamp(30px,5vw,50px);line-height:1.12;letter-spacing:-.04em;margin:14px 0}h2{font-size:25px;margin:0 0 15px}h3{font-size:17px}
p{max-width:92ch}.muted,small{color:var(--muted)}.cards{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin:25px 0}
.card,section{background:white;border:1px solid var(--line);border-radius:12px;padding:24px}.card strong{display:block;font-size:30px;line-height:1.2}.card span{font-size:13px;color:var(--muted)}section{margin-top:20px}
.pill{display:inline-block;padding:3px 10px;background:#ddf3ec;color:#096046;border-radius:20px;font-size:12px;font-weight:700}
.note{border-left:4px solid #d2912e;padding:12px 18px;background:#fff8e9;border-radius:3px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:24px}
table{border-collapse:collapse;width:100%;font-size:14px}th{text-align:left;background:#edf4f4;color:#365460;font-weight:650}td,th{padding:10px 12px;border-bottom:1px solid var(--line);vertical-align:top}
.scroll{overflow:auto}code{font:12px/1.5 ui-monospace,Consolas,monospace;overflow-wrap:anywhere}.bytes{display:flex;flex-wrap:wrap;gap:6px}.bytes div{padding:12px;background:#e8f2f3;border-radius:6px;text-align:center;flex:1;min-width:95px}.bytes b{display:block;font-size:13px}.bytes small{font-size:11px}
select,input,button{font:inherit;padding:8px 11px;border:1px solid #b8cccf;border-radius:6px;background:white;color:var(--ink)}button{cursor:pointer}button:disabled{opacity:.4}.controls{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:15px 0}svg{width:100%;height:auto;display:block}.chart{background:#f7fafa;border-radius:8px;padding:12px}.seq{font:12px/1.8 ui-monospace,monospace;word-spacing:4px}a{color:#08756f}.widehex{white-space:nowrap;font-size:11px}.footer{font-size:13px;margin-top:25px;color:var(--muted)}
@media(max-width:720px){main{padding:24px 14px}.cards{grid-template-columns:1fr 1fr}.grid{grid-template-columns:1fr}section{padding:18px}td,th{padding:8px}}
@media print{body{background:white}main{max-width:none;padding:0}section{break-inside:avoid}.controls,.explorer{display:none}.cards{grid-template-columns:repeat(4,1fr)}}
</style></head><body><main>
<header><div class="eyebrow">Senville SENDC-60HF · Passive observation · 10 October 2026</div>
<h1>IDU ↔ ODU communication<br>Five-minute capture review</h1>
<p>Both ESP32 receivers captured the same factory conversation. Every captured byte formed a CRC-valid S1/S2 frame, and the two saved streams match byte for byte.</p>
<span class="pill">Receive-only capture</span> <span class="pill">XYE disabled</span>
<p class="muted" id="session"></p></header>
<div class="cards"><div class="card"><strong>2,071</strong><span>frames per input · 4,142 observed copies</span></div><div class="card"><strong>41,420</strong><span>raw bus bytes per input</span></div><div class="card"><strong>0</strong><span>CRC failures, sequence gaps or log drops</span></div><div class="card"><strong>3.479 s</strong><span>mean repeating exchange cycle</span></div></div>
<section><h2>What was connected</h2><p>The original IDU–ODU path was electrically rejoined with jumpers. Both RS485 modules remained attached as passive taps on that shared pair. Port names describe their intended cable segments; in this topology each input hears both logical sources.</p>
<div class="grid"><div><h3>Port 0 · GPIO35 · ODU-side tap</h3><p>UART1 receiver. Module group: RX35 / reserved TX27 / EN32.</p></div><div><h3>Port 1 · GPIO33 · IDU-side tap</h3><p>UART2 receiver. Module group: RX33 / reserved TX14 / EN13.</p></div></div>
<p>No software forwarding or bus transmission was active. UART0 stayed on USB at 115200 baud; S1/S2 receivers used 4800 baud, 8N1. The capture tool recorded serial output for 300 seconds; individual log chunks are UART drain batches, not guaranteed frame boundaries.</p>
<div class="note">Both onboard 120 Ω terminators remained installed at the user's request. This clean sample demonstrates reception under these conditions; it does not establish termination margin, electrical isolation, or reliability across all operating states.</div></section>
<section><h2>Protocol observations</h2><div class="bytes"><div><b>A0</b><small>start marker</small></div><div><b>00 01 / 01 00</b><small>ODU / IDU source</small></div><div><b>command</b><small>20, 21, 50–53, 91</small></div><div><b>0C</b><small>12 payload bytes</small></div><div><b>payload + trailer</b><small>13 bytes</small></div><div><b>CRC low / high</b><small>MODBUS CRC-16</small></div></div>
<p>All observed frames were 20 bytes long: a five-byte header, twelve payload bytes, one pre-CRC trailer byte, and a two-byte little-endian CRC. CRC covers the header, payload and trailer using initialization FFFF and reflected polynomial A001. There were no unknown command types in this sample.</p>
<div class="grid"><div><h3>Observed exchange cycle</h3><p id="cycle"></p><p class="seq" id="sequence"></p></div><div><h3>Response timing</h3><p id="timing"></p><p class="muted">These intervals compare last-byte software drain timestamps. They include frame reception, FIFO behavior and task latency; they are not exact wire turnaround times or a proxy latency budget.</p></div></div>
<h3>Message counts — one input, without double-counting the second tap</h3><div class="scroll"><table><thead><tr><th>Command</th><th>ODU frames</th><th>IDU frames</th><th>Interpretation from reference</th></tr></thead><tbody id="messages"></tbody></table></div>
<p>The non-core IDU replies for 50–53 and 91 contain zero payloads in this sample. Their associated outdoor messages contain the extended data. Slight count differences at cycle boundaries are expected because recording starts and stops within the repeating sequence.</p></section>
<section><h2>Decoded values and trends</h2><p>Requested, target and probable actual compressor frequency stayed at 20 Hz. Indoor mode byte 01 is consistent with cooling, the setpoint remained 25 °C, and compressor state byte 06 is consistent with running. This is a steady-state sample; it does not validate behavior during ramping, defrost, faults or shutdown.</p>
<div class="controls"><label for="metric">Plot</label><select id="metric"></select></div><div class="chart"><svg id="trend" viewBox="0 0 1000 250" role="img" aria-label="Selected measurement over the capture"></svg></div>
<div class="scroll"><table><thead><tr><th>Measurement</th><th>Minimum</th><th>Maximum</th></tr></thead><tbody id="ranges"></tbody></table></div>
<p class="note">“Probable” mappings come from reverse engineering on another system. Other mappings were confirmed by that reference author, but have not all been independently calibrated on this machine. The 764–781 W field is a probable system-power measurement, not proven compressor-only consumption. Temperature formulas and all raw bytes are retained for review.</p></section>
<section><h2>Model differences worth investigating</h2><p>Some bytes described as constants in the published reference differ here. For example, IDU core byte 15 is 64 hex (100 decimal), while the reference describes 19 hex. ODU command 52 begins its payload with 28 hex here, while the reference describes 22 hex. These are observed differences, not decoded functions or evidence of corruption: the frames pass CRC.</p>
<p>Do not use these differences to infer control meanings. Future captures should compare raw offsets while changing one operating condition at a time, retaining the factory protective behavior.</p>
<h3>Capture integrity</h3><p>The portable C++ parser independently replayed both saved binary streams and accepted 2,071 valid frames from each. The Python analyzer found zero CRC-failed candidates, zero unframed bytes, and no serial sequence gaps. Firmware health records reported zero CRC failures, framing errors, FIFO overruns and logger drops throughout the recorded window.</p>
<p class="muted">The serial file contains 271,802 bytes, including hex encoding and health records. Cumulative device counters include traffic received before the capture file opened, so their final totals exceed the file's frame count. This is expected.</p></section>
<section class="explorer"><h2>Explore the captured frames</h2><p>Each row represents one received frame on port 0. Port 1 captured an identical ordered byte stream, so duplicate rows are omitted. Times are relative to the first retained port-0 frame; original monotonic timestamps remain available below.</p>
<div class="controls"><select id="source"><option value="">Both sources</option><option>ODU</option><option>IDU</option></select><select id="command"><option value="">All commands</option></select><input id="search" placeholder="Search raw hex" aria-label="Search raw hexadecimal data"><button id="prev">Previous</button><button id="next">Next</button><span id="page"></span></div>
<div class="scroll"><table><thead><tr><th>Time (s)</th><th>Drain (µs)</th><th>Source</th><th>Command</th><th>CRC</th><th>Raw frame (hex)</th></tr></thead><tbody id="frames"></tbody></table></div></section>
<section><h2>Useful next captures</h2><p>Use the current setup to collect a baseline through a normal thermostat setpoint change, a compressor ramp, and a complete normal stop/start. Record the exact action and time alongside the log. Compare indoor request, outdoor target, probable actual frequency, coil temperatures and protection-related raw bytes. Capture defrost only when it occurs naturally; avoid forcing undocumented states.</p><p>XYE remains a separate unresolved issue: previous status-query tests produced no valid response. This capture establishes the S1/S2 receive path and does not validate XYE, active forwarding, frequency limits or modified demand frames.</p>
<h3>Files and references</h3><p><a href="2026-10-10-passive-five-minutes.log">Original serial log</a> · <a href="2026-10-10-passive-five-minutes.analysis.json">Machine-readable analysis</a> · <a href="2026-10-10-passive-five-minutes.meta.json">Capture metadata</a> · <a href="2026-10-10-passive-five-minutes.port0.bin">Port 0 binary</a> · <a href="2026-10-10-passive-five-minutes.port1.bin">Port 1 binary</a></p>
<p><a href="https://github.com/MidATRIX/midea-s1s2-rs485-monitor">S1/S2 protocol reference</a> · <a href="../docs/PROTOCOL.md">Local protocol notes</a> · <a href="../README.md">Firmware instructions</a></p><p class="muted">The HTML itself is self-contained and works offline. Supporting-file links work when the report stays beside its capture files. No external scripts, fonts, services or network requests are used.</p></section>
<p class="footer">Capture review generated from saved data. Confidence labels describe protocol interpretation; they do not certify electrical safety or a control strategy.</p>
</main><script id="data" type="application/json">__DATA__</script><script>
const d=JSON.parse(document.getElementById('data').textContent), el=id=>document.getElementById(id);
const local=new Intl.DateTimeFormat('en-US',{dateStyle:'medium',timeStyle:'long',timeZone:'America/New_York'}).format(new Date(d.meta.started_utc));
el('session').textContent=`Started ${local} (America/New_York) · ${d.meta.elapsed_seconds.toFixed(2)} seconds · ESP32 classic WROOM-32`;
el('cycle').textContent=`${d.sequence.length} frames per repeating cycle. ODU 21-to-21 periods: ${(d.cycle.min/1000).toFixed(3)}–${(d.cycle.max/1000).toFixed(3)} seconds; mean ${(d.cycle.mean/1000).toFixed(6)} seconds.`;
el('sequence').textContent=d.sequence.join(' → ');
const r=d.analysis.ports[0].matched_reply_intervals_ms;
el('timing').textContent=`${r.count.toLocaleString()} matching ODU→IDU command pairs: ${r.min.toFixed(1)}–${r.max.toFixed(1)} ms, median ${r.median.toFixed(1)} ms on port 0. Port 1 median: ${d.analysis.ports[1].matched_reply_intervals_ms.median.toFixed(1)} ms.`;
const meaning={'20':'Core status / demand','21':'Handshake','50':'Performance A','51':'Performance B','52':'Performance C','53':'Performance D','91':'Keepalive'};
for(const cmd of Object.keys(meaning)){const tr=document.createElement('tr');[cmd,d.analysis.ports[0].messages['0001_'+cmd],d.analysis.ports[0].messages['0100_'+cmd],meaning[cmd]].forEach(v=>{const td=document.createElement('td');td.textContent=v;tr.append(td)});el('messages').append(tr);el('command').add(new Option(cmd,cmd))}
for(const [name,values] of Object.entries(d.series)){el('metric').add(new Option(name,name));const v=values.map(p=>p[1]),tr=document.createElement('tr');[name,Math.min(...v).toFixed(3),Math.max(...v).toFixed(3)].forEach(x=>{const td=document.createElement('td');td.textContent=x;tr.append(td)});el('ranges').append(tr)}
function draw(){const name=el('metric').value,values=d.series[name],v=values.map(p=>p[1]);let lo=Math.min(...v),hi=Math.max(...v),pad=(hi-lo)*.15||Math.max(Math.abs(lo)*.03,.2);lo-=pad;hi+=pad;const X=s=>65+s/300*910,Y=v=>205-(v-lo)/(hi-lo)*175;
let svg='';for(let i=0;i<5;i++){let y=30+i*175/4,val=hi-i*(hi-lo)/4;svg+=`<line x1="65" x2="975" y1="${y}" y2="${y}" stroke="#dce7e8"/><text x="55" y="${y+4}" text-anchor="end" font-size="12" fill="#516674">${val.toFixed(2)}</text>`}for(let t=0;t<=300;t+=60)svg+=`<text x="${X(t)}" y="230" text-anchor="middle" font-size="12" fill="#516674">${t}s</text>`;svg+=`<polyline fill="none" stroke="#087f78" stroke-width="2.5" points="${values.map(p=>X(p[0])+','+Y(p[1])).join(' ')}"/>`;el('trend').innerHTML=svg}
el('metric').addEventListener('change',draw);draw();let page=0,filtered=[];
function render(reset=false){if(reset)page=0;const q=el('search').value.toUpperCase().replace(/\s/g,'');filtered=d.frames.filter(f=>(!el('source').value||f.source===el('source').value)&&(!el('command').value||f.cmd===el('command').value)&&f.hex.includes(q));const pages=Math.max(1,Math.ceil(filtered.length/25));page=Math.min(page,pages-1);el('frames').replaceChildren();for(const f of filtered.slice(page*25,(page+1)*25)){let tr=document.createElement('tr');[f.seconds.toFixed(3),f.us,f.source,f.cmd,'Valid',f.hex].forEach((v,i)=>{let td=document.createElement('td');td.textContent=v;if(i===5)td.className='widehex';tr.append(td)});el('frames').append(tr)}el('page').textContent=`${filtered.length} frames · page ${page+1}/${pages}`;el('prev').disabled=page===0;el('next').disabled=page===pages-1}
['source','command','search'].forEach(id=>el(id).addEventListener('input',()=>render(true)));el('prev').onclick=()=>{page--;render()};el('next').onclick=()=>{page++;render()};render();
</script></body></html>'''
out = capture.with_suffix('.html')
out.write_text(template.replace('__DATA__', json.dumps(payload, separators=(',', ':'))), encoding='utf-8')
print(out)
