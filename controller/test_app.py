import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from http.server import ThreadingHTTPServer
from app import Supervisor, make_handler

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.s = Supervisor(str(Path(self.tmp.name) / 'history.sqlite'), 'http://unused', 'esp-test-token', control=True)
        self.calls = []
        self.uptime = 1000
        self.fail = False
        self.s.esp = self.esp

    def tearDown(self):
        self.tmp.cleanup()

    def esp(self, path, fields=None):
        self.calls.append((path, fields))
        if self.fail:
            raise OSError('network lost')
        if path == '/api/status':
            return {'uptime_us': self.uptime, 'ports': []}
        if path == '/api/xye':
            return {'valid': True, 'stale': False}
        if path.startswith('/api/controller'):
            return {'boot_id': 42, 'lease_active': True}
        if path == '/api/frames':
            return {'frames': [{'port': 0, 'first_us': 1, 'last_us': 2, 'hex': 'A0', 'result': 'truncated'}]}
        if path == '/api/xye/set':
            return {'accepted': True, 'command_id': 1}
        return {}

    def test_history_deduplicates_frames_and_survives_restart(self):
        self.s.poll(); self.s.poll()
        with self.s.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM frames').fetchone()[0], 1)
            self.assertEqual(db.execute('SELECT count(*) FROM samples').fetchone()[0], 2)
        restored = Supervisor(self.s.db_path, 'http://unused', '', demo=True)
        with restored.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM samples').fetchone()[0], 2)

    def test_network_loss_stops_heartbeat_and_rejects_commands(self):
        self.s.poll()
        self.calls.clear(); self.fail = True; self.s.poll()
        self.assertFalse(self.s.snapshot()['connected'])
        self.assertFalse(any('/heartbeat' in p for p, _ in self.calls))
        with self.assertRaises(ValueError):
            self.s.command({'kind': 'climate', 'mode': 'cool', 'fan': 'auto', 'temperature_f': 79})

    def test_no_command_replay_after_recovery(self):
        self.s.poll()
        result = self.s.command({'kind': 'climate', 'mode': 'cool', 'fan': 'auto', 'temperature_f': 79})
        self.assertEqual(result['command_id'], 1)
        self.fail = True; self.s.poll(); self.fail = False; self.s.poll()
        writes = [(p, f) for p, f in self.calls if p == '/api/xye/set']
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0][1]['boot_id'], 42)

    def test_invalid_controls_and_demo_are_rejected(self):
        self.s.poll()
        for body in ({'kind':'climate','mode':'cool','fan':'turbo','temperature_f':79},
                     {'kind':'climate','mode':'cool','fan':'auto','temperature_f':True},
                     {'kind':'capacity','hz':75}):
            with self.assertRaises(ValueError): self.s.command(body)
        demo = Supervisor(self.s.db_path, 'http://unused', '', demo=True, control=True)
        demo.poll()
        with self.assertRaises(ValueError): demo.command({'kind':'xye_polling','enabled':True})

    def test_storage_failure_stops_heartbeat(self):
        self.s.poll(); self.calls.clear()
        def broken_db(): raise OSError('disk full')
        self.s.db = broken_db
        self.s.poll()
        self.assertFalse(self.s.snapshot()['connected'])
        self.assertFalse(any('/heartbeat' in p for p, _ in self.calls))

    def test_indeterminate_command_is_not_retried(self):
        self.s.poll(); self.fail = True
        with self.assertRaisesRegex(ValueError, 'outcome unknown'):
            self.s.command({'kind':'climate','mode':'cool','fan':'auto','temperature_f':79})
        self.fail = False; self.s.poll()
        self.assertEqual(sum(p == '/api/xye/set' for p, _ in self.calls), 1)
        with self.s.db() as db:
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE kind='command_indeterminate'").fetchone()[0], 1)

    def test_inactive_lease_and_relay_fault_reject_commands(self):
        self.s.poll()
        self.s.state['device']['lease_active'] = False
        with self.assertRaises(ValueError): self.s.command({'kind':'xye_polling','enabled':True})
        self.s.state['device']['lease_active'] = True
        self.s.state['device']['relay_errors'] = 1
        with self.assertRaises(ValueError): self.s.command({'kind':'xye_polling','enabled':True})

    def test_http_authentication_and_command_route(self):
        self.s.poll()
        server = ThreadingHTTPServer(('127.0.0.1', 0), make_handler(self.s, 'browser-test-token'))
        worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
        base = 'http://127.0.0.1:' + str(server.server_port)
        try:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                urllib.request.urlopen(base + '/api/state')
            self.assertEqual(caught.exception.code, 401)
            request = urllib.request.Request(base + '/api/command',
                data=json.dumps({'kind':'climate','mode':'cool','fan':'auto','temperature_f':79}).encode(),
                headers={'Authorization':'Bearer browser-test-token', 'Content-Type':'application/json'})
            with urllib.request.urlopen(request) as response:
                self.assertEqual(response.status, 202)
                self.assertTrue(json.load(response)['accepted'])
        finally:
            server.shutdown(); server.server_close(); worker.join()

if __name__ == '__main__': unittest.main()
