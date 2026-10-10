"""Local HVAC supervisor. Standard-library HTTP, SQLite history, no bus timing here."""
import argparse
from contextlib import contextmanager
import hmac
import json
import math
import os
from pathlib import Path
import sqlite3
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent

class Supervisor:
    def __init__(self, db_path, esp_url, esp_token, demo=False, control=False):
        self.db_path, self.esp_url, self.esp_token = db_path, esp_url.rstrip('/'), esp_token
        self.demo, self.control = demo, control and not demo
        self.lock, self.command_lock, self.stop = threading.Lock(), threading.Lock(), threading.Event()
        self.state = {'connected': False, 'demo': demo, 'control_enabled': self.control, 'error': 'Waiting for first sample'}
        self.boot = 'legacy-start'
        self.last_uptime = 0
        self.last_prune = 0
        Path(db_path).parent.mkdir(parents=True, exist_ok=True)
        with self.db() as db:
            db.executescript('''CREATE TABLE IF NOT EXISTS samples(id INTEGER PRIMARY KEY, ts REAL, payload TEXT);
CREATE TABLE IF NOT EXISTS frames(id INTEGER PRIMARY KEY, ts REAL, boot TEXT, port INTEGER, first_us INTEGER, last_us INTEGER, hex TEXT, result TEXT, UNIQUE(boot,port,first_us,last_us,hex));
CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, ts REAL, kind TEXT, payload TEXT);
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

    def esp(self, path, fields=None):
        request = urllib.request.Request(self.esp_url + path,
            data=None if fields is None else urllib.parse.urlencode(fields).encode(),
            headers={'Authorization': 'Bearer ' + self.esp_token})
        with urllib.request.urlopen(request, timeout=2) as response:
            return json.load(response)

    def event(self, kind, payload):
        with self.db() as db:
            db.execute('INSERT INTO events(ts,kind,payload) VALUES(?,?,?)', (time.time(), kind, json.dumps(payload)))

    def snapshot(self):
        with self.lock:
            return json.loads(json.dumps(self.state))

    def poll(self):
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
                status, xye = self.esp('/api/status'), self.esp('/api/xye')
                try:
                    device = self.esp('/api/controller')
                except urllib.error.HTTPError as exc:
                    if exc.code != 404:
                        raise
                    device = {'state': 'legacy_monitor', 'lease_active': False, 'proxy_enabled': False}
                frames = self.esp('/api/frames').get('frames', [])
            if 'boot_id' in device:
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
            if not self.demo and 'boot_id' in device:
                sample['device'] = self.esp('/api/controller/heartbeat', {})
            with self.lock:
                was_connected = self.state.get('connected')
                self.state = sample
            if not was_connected:
                self.event('connected', {'demo': self.demo, 'boot': self.boot})
        except Exception as exc:
            with self.lock:
                self.state = {**self.state, 'connected': False, 'error': str(exc), 'failed_at': time.time()}
            # No command retry, no heartbeat on failure. ESP lease expires locally.

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
            if not state.get('device', {}).get('lease_active'):
                raise ValueError('ESP controller lease is inactive')
            if state['device'].get('relay_errors', 0):
                raise ValueError('ESP relay reports a transport fault')
            kind = body.get('kind')
            if kind == 'xye_polling':
                if not isinstance(body.get('enabled'), bool):
                    raise ValueError('enabled must be boolean')
                path, fields = '/api/xye/' + ('enable' if body['enabled'] else 'disable'), {}
            elif kind == 'climate':
                if body.get('mode') not in ('off', 'cool', 'heat', 'dry', 'fan') or body.get('fan') not in ('auto', 'low', 'medium', 'high'):
                    raise ValueError('Unsupported mode or fan')
                temp = body.get('temperature_f')
                if isinstance(temp, bool) or not isinstance(temp, int) or not 60 <= temp <= 86:
                    raise ValueError('Whole Fahrenheit temperature must be 60..86')
                if not state.get('xye', {}).get('valid') or state['xye'].get('stale'):
                    raise ValueError('Fresh XYE status required')
                path = '/api/xye/set'
                fields = {k: body[k] for k in ('mode', 'fan', 'temperature_f')}
                fields['boot_id'] = state['device']['boot_id']
            else:
                raise ValueError('Unsupported command')
            # Audit before send; if storage is broken, do not act.
            self.event('command_requested', body)
            try:
                result = self.esp(path, fields)
            except Exception as exc:
                self.event('command_indeterminate', {'error': str(exc), 'request': body})
                raise ValueError('Command failed or outcome unknown; not retried') from exc
            self.event('command_accepted', result)
            return result

def make_handler(supervisor, token):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, code, value, content_type='application/json'):
            raw = json.dumps(value).encode() if content_type == 'application/json' else value
            self.send_response(code)
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
            except (ValueError, KeyError) as exc:
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
    esp_url = os.getenv('ESP_URL', 'http://192.168.4.1')
    if urllib.parse.urlsplit(esp_url).scheme not in ('http', 'https'):
        parser.error('ESP_URL must be HTTP(S)')
    supervisor = Supervisor(os.getenv('DB_PATH', str(ROOT / 'data/hvac.sqlite')),
        esp_url, os.getenv('ESP_TOKEN', ''), args.demo, os.getenv('CONTROL_ENABLED', 'false').lower() == 'true')
    worker = threading.Thread(target=supervisor.run, daemon=True)
    worker.start()
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(supervisor, token))
    try:
        server.serve_forever()
    finally:
        supervisor.stop.set()
        worker.join(timeout=10)
        if not args.demo:
            try:
                supervisor.esp('/api/controller/release', {})
            except Exception:
                pass
        server.server_close()

if __name__ == '__main__':
    main()
