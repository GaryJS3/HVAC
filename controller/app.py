"""Local HVAC supervisor. Standard-library HTTP, SQLite history, no bus timing here."""
import argparse
from contextlib import contextmanager
import hmac
import json
import math
import os
import re
import secrets
from pathlib import Path
import sqlite3
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent

class Supervisor:
    def __init__(self, db_path, demo=False, control=False):
        self.db_path = db_path
        self.demo, self.control = demo, control and not demo
        self.lock, self.command_lock, self.stop = threading.Lock(), threading.Lock(), threading.Event()
        self.state = {'connected': False, 'demo': demo, 'control_enabled': self.control, 'error': 'Waiting for first sample'}
        self.boot = 'legacy-start'
        self.last_uptime = 0
        self.last_prune = 0
        self.pending = None
        self.next_id = secrets.randbits(31)
        Path(db_path).parent.mkdir(parents=True, exist_ok=True)
        with self.db() as db:
            db.executescript('''CREATE TABLE IF NOT EXISTS samples(id INTEGER PRIMARY KEY, ts REAL, payload TEXT);
CREATE TABLE IF NOT EXISTS frames(id INTEGER PRIMARY KEY, ts REAL, boot TEXT, port INTEGER, first_us INTEGER, last_us INTEGER, hex TEXT, result TEXT, UNIQUE(boot,port,first_us,last_us,hex));
CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, ts REAL, kind TEXT, payload TEXT);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
CREATE INDEX IF NOT EXISTS samples_ts ON samples(ts);
CREATE INDEX IF NOT EXISTS frames_ts ON frames(ts);''')

    @contextmanager
    def db(self):
        db = sqlite3.connect(self.db_path, timeout=5)
        try:
            db.execute('PRAGMA journal_mode=WAL')
            yield db
            db.commit()
        except Exception:
            db.rollback()
            raise
        finally:
            db.close()

    def event(self, kind, payload):
        with self.db() as db:
            db.execute('INSERT INTO events(ts,kind,payload) VALUES(?,?,?)', (time.time(), kind, json.dumps(payload)))

    def snapshot(self):
        with self.lock:
            result = json.loads(json.dumps(self.state))
        if time.time() - result.get('sampled_at', 0) > 8:
            result['connected'] = False
            result['error'] = 'Waiting for fresh ESP telemetry'
        return result

    def poll(self, payload=None):
        if not self.demo and payload is None:
            return
        now = time.time()
        try:
            if self.demo:
                fields = {'indoor_demand_hz': 50, 'actual_hz': 40 + 8 * math.sin(now / 25),
                          'target_hz': 48, 'indoor_ambient_c': 25.5, 'outdoor_ambient_c': 29,
                          'fan_rpm': 800, 'power_w': 2400 + 300 * math.sin(now / 25)}
                status = {'uptime_us': int(now * 1e6), 'proxy_enabled': False, 'ports': [{'port': 0,
                    'stale': False, 'valid_frames': 1200, 'crc_failures': 0,
                    'telemetry': {k: {'value': v, 'valid': True, 'stale': False, 'confidence': 'probable'} for k, v in fields.items()}}]}
                xye = {'valid': True, 'stale': False, 'address': 0, 'replies': 100, 'mode': 136, 'fan': 132, 'setpoint_raw': 79}
                device = {'boot_id': 'demo', 'state': 'demo_monitor', 'lease_active': False, 'proxy_enabled': False}
                frames = []
            else:
                if type(payload.get('boot_id')) is not int or not 0 <= payload['boot_id'] <= 0xffffffff or not isinstance(payload.get('status'), dict) or not isinstance(payload['status'].get('ports'), list) or not isinstance(payload.get('xye'), dict) or not isinstance(payload.get('device', {}), dict):
                    raise ValueError('Valid boot_id, status and XYE snapshot required')
                status, xye = payload['status'], payload['xye']
                if type(status.get('uptime_us')) is not int or status['uptime_us'] < 0:
                    raise ValueError('Monotonic uptime required')
                device = {**payload.get('device', {}), 'boot_id': payload['boot_id'], 'device_id':payload['device_id']}
                frames = payload.get('frames', [])
            if 'boot_id' in device:
                if self.boot != str(device['boot_id']):
                    self.pending = None
                self.boot = str(device['boot_id'])
            elif status['uptime_us'] < self.last_uptime:
                self.boot = str(now)
            self.last_uptime = status['uptime_us']
            sample = {'connected': True, 'demo': self.demo, 'control_enabled': self.control,
                      'sampled_at': now, 'status': status, 'xye': xye, 'device': device, 'error': None}
            # Persist first: storage failure stops heartbeat renewal and supervisory authority.
            with self.db() as db:
                db.execute('INSERT INTO samples(ts,payload) VALUES(?,?)', (now, json.dumps(sample)))
                db.executemany('INSERT OR IGNORE INTO frames(ts,boot,port,first_us,last_us,hex,result) VALUES(?,?,?,?,?,?,?)',
                    [(now, self.boot, f['port'], f['first_us'], f['last_us'], f['hex'], f['result']) for f in frames])
                if now - self.last_prune > 3600:
                    for table in ('samples', 'frames', 'events'):
                        db.execute(f'DELETE FROM {table} WHERE ts < ?', (now - 30 * 86400,))
                    self.last_prune = now
            with self.lock:
                was_connected = self.state.get('connected')
                self.state = sample
            if not was_connected:
                self.event('connected', {'demo': self.demo, 'boot': self.boot})
            return True
        except Exception as exc:
            with self.lock:
                self.state = {**self.state, 'connected': False, 'error': str(exc), 'failed_at': time.time()}
            # No command retry, no heartbeat on failure. ESP lease expires locally.
            return False

    def register(self, payload):
        if self.demo:
            raise ValueError('Demo cannot accept equipment telemetry')
        with self.command_lock:
            device_id = payload.get('device_id')
            if not isinstance(device_id,str) or not re.fullmatch(r'(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}',device_id):
                raise ValueError('Stable ESP MAC identity required')
            device_id = device_id.upper()
            payload = {**payload, 'device_id':device_id}
            try:
                with self.db() as db:
                    row = db.execute("SELECT value FROM settings WHERE key='device_id'").fetchone()
                if row and row[0] != device_id:
                    raise ValueError('Controller is already paired with another ESP')
            except (OSError, sqlite3.Error) as exc:
                with self.lock: self.state = {**self.state, 'connected':False, 'error':str(exc)}
                raise ValueError('History storage unavailable') from exc
            if not self.poll(payload):
                raise ValueError('Telemetry not saved; lease not renewed')
            with self.db() as db:
                db.execute("INSERT OR IGNORE INTO settings(key,value) VALUES('device_id',?)",(device_id,))
            command, self.pending = self.pending, None
            if command and command['expires'] > time.time() and command['boot_id'] == payload['boot_id']:
                self.event('command_delivered', command)
                return command
            if command:
                self.event('command_expired', command)
            return None

    def run(self):
        while not self.stop.is_set():
            self.poll()
            self.stop.wait(2)

    def command(self, body):
        if not self.control:
            raise ValueError('Control is disabled for this deployment')
        with self.command_lock:
            state = self.snapshot()
            if not state.get('connected') or time.time() - state.get('sampled_at', 0) > 5:
                raise ValueError('ESP data is stale or unavailable')
            if state['device'].get('proxy', {}).get('fault_active', bool(state['device'].get('relay_errors', 0))) and not (body.get('kind') == 'proxy' and body.get('enabled') is False):
                raise ValueError('ESP relay reports a transport fault')
            kind = body.get('kind')
            if self.pending and self.pending['expires'] > time.time():
                raise ValueError('A command is already pending')
            if kind in ('proxy','proxy_probe'):
                proxy = state['device'].get('proxy',{})
                if not proxy.get('compiled'):
                    raise ValueError('MTM firmware required')
                if kind == 'proxy':
                    if not isinstance(body.get('enabled'),bool):
                        raise ValueError('enabled must be boolean')
                    if body['enabled'] and body.get('segments_separated') is not True:
                        raise ValueError('Remove A-A/B-B jumpers and confirm separated segments first')
                    fields = {'enabled':int(body['enabled'])}
                else:
                    if proxy.get('armed') or proxy.get('arm_pending') or body.get('hvac_disconnected') is not True:
                        raise ValueError('Disarmed proxy and disconnected HVAC harness required')
                    source = body.get('source')
                    if type(source) is not int or source not in (0,1):
                        raise ValueError('source must be 0 or 1')
                    fields = {'source':source}
            elif kind == 'xye_polling':
                if not isinstance(body.get('enabled'), bool):
                    raise ValueError('enabled must be boolean')
                fields = {'enabled': int(body['enabled'])}
            elif kind == 'climate':
                if body.get('mode') not in ('off', 'cool', 'heat', 'dry', 'fan') or body.get('fan') not in ('auto', 'low', 'medium', 'high'):
                    raise ValueError('Unsupported mode or fan')
                temp = body.get('temperature_f')
                if isinstance(temp, bool) or not isinstance(temp, int) or not 60 <= temp <= 86:
                    raise ValueError('Whole Fahrenheit temperature must be 60..86')
                if not state.get('xye', {}).get('valid') or state['xye'].get('stale'):
                    raise ValueError('Fresh XYE status required')
                fields = {'mode': {'off':0,'fan':129,'dry':130,'heat':132,'cool':136}[body['mode']],
                          'fan': {'auto':128,'high':1,'medium':2,'low':4}[body['fan']], 'temp':temp}
            else:
                raise ValueError('Unsupported command')
            # Audit before send; if storage is broken, do not act.
            self.next_id = (self.next_id + 1) & 0x7fffffff
            item = {'id':self.next_id, 'kind':kind, 'boot_id':state['device']['boot_id'], 'expires':time.time()+5, **fields}
            self.event('command_requested', item)
            self.pending = item
            return {'accepted':True, 'command_id':item['id'] if kind in ('climate','proxy','proxy_probe') else None, 'expires_at':item['expires']}

def make_handler(supervisor, token, esp_token=''):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, code, value, content_type='application/json', headers=None):
            raw = json.dumps(value).encode() if content_type == 'application/json' else value
            self.send_response(code)
            for key, value in (headers or {}).items(): self.send_header(key, str(value))
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(raw)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.end_headers()
            self.wfile.write(raw)

        def authorized(self):
            return hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + token)

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            if url.path == '/healthz':
                return self.send(200, {'service': 'ok', 'esp_connected': supervisor.snapshot().get('connected', False)})
            if url.path == '/':
                return self.send(200, (ROOT / 'static/index.html').read_bytes(), 'text/html; charset=utf-8')
            if not self.authorized():
                return self.send(401, {'error': 'Access key required'})
            if url.path == '/api/state':
                return self.send(200, supervisor.snapshot())
            if url.path in ('/api/history', '/api/events', '/api/frames'):
                with supervisor.db() as db:
                    if url.path == '/api/history':
                        result = []
                        for ts, payload in db.execute('SELECT ts,payload FROM samples ORDER BY id DESC LIMIT 1800'):
                            sample = json.loads(payload)
                            values = {}
                            for port in sample.get('status', {}).get('ports', []):
                                for key, value in port.get('telemetry', {}).items():
                                    if value.get('valid') and not value.get('stale') and value.get('updated_us', 0) >= values.get(key, {}).get('updated_us', 0):
                                        values[key] = value
                            result.append({'ts': ts, 'values': {k: v['value'] for k, v in values.items()}})
                        result.reverse()
                    elif url.path == '/api/events':
                        result = [{'ts': ts, 'kind': kind, 'data': json.loads(payload)} for ts, kind, payload in db.execute('SELECT ts,kind,payload FROM events ORDER BY id DESC LIMIT 100')]
                    else:
                        result = [dict(zip(('ts','boot','port','first_us','last_us','hex','result'), row)) for row in db.execute('SELECT ts,boot,port,first_us,last_us,hex,result FROM frames ORDER BY id DESC LIMIT 100')]
                return self.send(200, result)
            self.send(404, {'error': 'Not found'})

        def do_POST(self):
            if self.path == '/api/devices/register':
                if len(esp_token) < 16 or not hmac.compare_digest(self.headers.get('Authorization',''), 'Bearer '+esp_token):
                    return self.send(401, {'error':'Device token required'})
                try:
                    length = int(self.headers.get('Content-Length','0'))
                    if not 0 < length <= 65536: raise ValueError('Invalid telemetry size')
                    payload = json.loads(self.rfile.read(length))
                    if not isinstance(payload,dict): raise ValueError('Object required')
                    command = supervisor.register(payload)
                    headers = {'X-HVAC-Lease':'1','X-HVAC-Boot':payload['boot_id']}
                    if command:
                        headers.update({'X-HVAC-Command':command['kind'],'X-HVAC-Id':command['id']})
                        for key in ('mode','fan','temp','enabled','source'):
                            if key in command: headers['X-HVAC-'+key.title()] = command[key]
                    return self.send(200, {'registered':True}, headers=headers)
                except (ValueError,KeyError,TypeError): return self.send(409,{'error':'Telemetry rejected; lease not renewed'})
                except sqlite3.Error: return self.send(503,{'error':'History storage unavailable; lease not renewed'})
            if not self.authorized():
                return self.send(401, {'error': 'Access key required'})
            if self.path != '/api/command':
                return self.send(404, {'error': 'Not found'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 2048:
                    raise ValueError('Invalid body size')
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError('Object body required')
                return self.send(202, supervisor.command(body))
            except (ValueError, KeyError, TypeError) as exc:
                return self.send(409, {'error': str(exc)})
    return Handler

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--demo', action='store_true')
    parser.add_argument('--port', type=int, default=int(os.getenv('PORT', '8080')))
    parser.add_argument('--bind', default=os.getenv('BIND', '127.0.0.1'))
    args = parser.parse_args()
    token = os.getenv('CONTROLLER_TOKEN', '')
    if len(token) < 16:
        parser.error('CONTROLLER_TOKEN must contain at least 16 characters')
    esp_token = os.getenv('ESP_TOKEN','')
    if not args.demo and len(esp_token) < 16:
        parser.error('ESP_TOKEN must contain at least 16 characters')
    supervisor = Supervisor(os.getenv('DB_PATH', str(ROOT / 'data/hvac.sqlite')),
        args.demo, os.getenv('CONTROL_ENABLED', 'false').lower() == 'true')
    worker = threading.Thread(target=supervisor.run, daemon=True)
    if args.demo: worker.start()
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(supervisor, token, esp_token))
    try:
        server.serve_forever()
    finally:
        supervisor.stop.set()
        if args.demo: worker.join(timeout=10)
        server.server_close()

if __name__ == '__main__':
    main()
