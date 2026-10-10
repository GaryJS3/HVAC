import copy
import json
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from http.server import ThreadingHTTPServer
from app import Supervisor, make_handler

CLIMATE = {'kind':'climate','mode':'cool','fan':'auto','temperature_f':79}
class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.s = Supervisor(str(Path(self.tmp.name)/'history.sqlite'), control=True)
        self.payload = {'device_id':'AA:BB:CC:DD:EE:FF', 'boot_id':42, 'status':{'uptime_us':1000,'ports':[]},
            'xye':{'valid':True,'stale':False}, 'device':{'lease_active':True},
            'frames':[{'port':0,'first_us':1,'last_us':2,'hex':'A0','result':'truncated'}]}
    def tearDown(self): self.tmp.cleanup()
    def test_history_deduplicates_frames_and_survives_restart(self):
        self.s.register(self.payload); self.s.register(self.payload)
        restored=Supervisor(self.s.db_path)
        with restored.db() as db:
            self.assertEqual(db.execute('SELECT count(*) FROM frames').fetchone()[0],1)
            self.assertEqual(db.execute('SELECT count(*) FROM samples').fetchone()[0],2)
    def test_device_pairing_survives_restart(self):
        self.s.register(self.payload)
        restored=Supervisor(self.s.db_path)
        with self.assertRaises(ValueError): restored.register({**self.payload,'device_id':'11:22:33:44:55:66'})
        self.assertIsNone(restored.register(self.payload))
    def test_command_is_delivered_once_in_next_registration(self):
        self.s.register(self.payload)
        result=self.s.command(CLIMATE)
        command=self.s.register(self.payload)
        self.assertEqual(command['id'],result['command_id'])
        self.assertEqual((command['mode'],command['fan'],command['temp']),(136,128,79))
        self.assertIsNone(self.s.register(self.payload))
    def test_expired_command_never_replayed(self):
        self.s.register(self.payload); self.s.command(CLIMATE)
        self.s.pending['expires']=time.time()-1
        self.assertIsNone(self.s.register(self.payload))
        self.assertIsNone(self.s.register(self.payload))
    def test_reboot_clears_pending_command(self):
        self.s.register(self.payload); self.s.command(CLIMATE)
        self.payload['boot_id']=43
        self.assertIsNone(self.s.register(self.payload))
    def test_stale_data_disables_control(self):
        self.s.register(self.payload); self.s.state['sampled_at']=time.time()-11
        self.assertFalse(self.s.snapshot()['connected'])
        with self.assertRaises(ValueError): self.s.command(CLIMATE)
    def test_storage_failure_withholds_lease_and_command(self):
        self.s.register(self.payload); self.s.command(CLIMATE)
        def broken(): raise OSError('disk full')
        self.s.db=broken
        with self.assertRaises(ValueError): self.s.register(self.payload)
        self.assertFalse(self.s.snapshot()['connected'])
    def test_invalid_control_demo_and_relay_fault(self):
        self.s.register(self.payload)
        for body in ({**CLIMATE,'fan':'turbo'},{**CLIMATE,'temperature_f':True},{'kind':'capacity','hz':75}):
            with self.assertRaises(ValueError): self.s.command(body)
        self.s.state['device']['relay_errors']=1
        with self.assertRaises(ValueError): self.s.command(CLIMATE)
        demo=Supervisor(self.s.db_path,demo=True,control=True); demo.poll()
        with self.assertRaises(ValueError): demo.command(CLIMATE)
    def test_pending_command_blocks_second_write(self):
        self.s.register(self.payload); self.s.command(CLIMATE)
        with self.assertRaises(ValueError): self.s.command(CLIMATE)
    def test_ceiling_requires_support_lease_armed_and_valid_range(self):
        body={'kind':'demand_ceiling','enabled':True,'maximum_hz':20}
        self.s.register(self.payload)
        with self.assertRaises(ValueError): self.s.command(body)
        self.payload['device']['proxy']={'compiled':True,'armed':True,'ceiling':{'supported':True}}
        self.s.register(self.payload)
        for value in (True,19,121,20.5,'20'):
            with self.assertRaises(ValueError): self.s.command({**body,'maximum_hz':value})
        for key in ('armed','lease_active'):
            target=self.payload['device']['proxy'] if key=='armed' else self.payload['device']
            target[key]=False; self.s.register(self.payload)
            with self.assertRaises(ValueError): self.s.command(body)
            target[key]=True
        self.s.register(self.payload)
        result=self.s.command(body); sent=self.s.register(self.payload)
        self.assertEqual(sent['id'],result['command_id'])
        self.assertEqual((sent['enabled'],sent['maximum_hz']),(1,20))
        self.assertIsNone(self.s.register(self.payload))
    def test_ceiling_can_be_disabled_during_relay_fault(self):
        self.payload['device']['proxy']={'compiled':True,'armed':False,'fault_active':True,'ceiling':{'supported':True}}
        self.s.register(self.payload)
        self.s.command({'kind':'demand_ceiling','enabled':False})
        self.assertEqual(self.s.register(self.payload)['enabled'],0)
    def test_historical_relay_error_does_not_latch_controls(self):
        self.payload['device']['relay_errors']=3
        self.payload['device']['proxy']={'fault_active':False}
        self.s.register(self.payload)
        self.s.command(CLIMATE)
        self.s.register(self.payload)
        self.payload['device']['proxy']['fault_active']=True
        self.s.register(self.payload)
        with self.assertRaises(ValueError): self.s.command(CLIMATE)
    def test_invalid_registration_does_not_renew(self):
        for payload in ({}, {**self.payload,'boot_id':True}, {**self.payload,'status':None}):
            with self.assertRaises(ValueError): self.s.register(payload)
    def test_proxy_requires_firmware_and_topology_confirmation(self):
        self.s.register(self.payload)
        with self.assertRaises(ValueError): self.s.command({'kind':'proxy','enabled':True,'segments_separated':True})
        self.payload['device']['proxy']={'compiled':True,'armed':False,'probes_passed':3}
        self.s.register(self.payload)
        with self.assertRaises(ValueError): self.s.command({'kind':'proxy','enabled':True})
        result=self.s.command({'kind':'proxy','enabled':True,'segments_separated':True})
        delivered=self.s.register(self.payload)
        self.assertEqual(delivered['id'],result['command_id'])
        self.assertEqual(delivered['enabled'],1)
    def test_probe_requires_disconnected_hvac_and_disarmed_relay(self):
        self.payload['device']['proxy']={'compiled':True,'armed':False}
        self.s.register(self.payload)
        with self.assertRaises(ValueError): self.s.command({'kind':'proxy_probe','source':0})
        with self.assertRaises(ValueError): self.s.command({'kind':'proxy_probe','source':True,'hvac_disconnected':True})
        self.s.command({'kind':'proxy_probe','source':1,'hvac_disconnected':True})
        self.assertEqual(self.s.register(self.payload)['source'],1)
        self.payload['device']['proxy']['armed']=True;self.s.register(self.payload)
        with self.assertRaises(ValueError): self.s.command({'kind':'proxy_probe','source':0,'hvac_disconnected':True})
    def test_proxy_disarm_is_allowed_during_transport_fault(self):
        self.payload['device']={'relay_errors':1,'proxy':{'compiled':True,'armed':True}}
        self.s.register(self.payload)
        with self.assertRaises(ValueError): self.s.command(CLIMATE)
        self.s.command({'kind':'proxy','enabled':False})
        self.assertEqual(self.s.register(self.payload)['enabled'],0)
    def test_http_device_auth_and_response_command(self):
        server=ThreadingHTTPServer(('127.0.0.1',0),make_handler(self.s,'browser-test-token','device-test-token'))
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        base='http://127.0.0.1:'+str(server.server_port)
        def post(path,body,key):
            return urllib.request.urlopen(urllib.request.Request(base+path,data=json.dumps(body).encode(),headers={'Authorization':'Bearer '+key,'Content-Type':'application/json'}))
        try:
            with self.assertRaises(urllib.error.HTTPError) as caught: post('/api/devices/register',self.payload,'browser-test-token')
            self.assertEqual(caught.exception.code,401)
            with post('/api/devices/register',self.payload,'device-test-token') as response:
                self.assertEqual(response.headers['X-HVAC-Lease'],'1')
            with post('/api/command',CLIMATE,'browser-test-token') as response: self.assertEqual(response.status,202)
            with post('/api/devices/register',self.payload,'device-test-token') as response:
                self.assertEqual(response.headers['X-HVAC-Command'],'climate')
                self.assertEqual(response.headers['X-HVAC-Temp'],'79')
            with post('/api/devices/register',self.payload,'device-test-token') as response: self.assertIsNone(response.headers.get('X-HVAC-Command'))
            self.payload['device']['proxy']={'armed':True,'ceiling':{'supported':True}}
            with post('/api/devices/register',self.payload,'device-test-token'): pass
            with post('/api/command',{'kind':'demand_ceiling','enabled':True,'maximum_hz':20},'browser-test-token') as response: self.assertEqual(response.status,202)
            with post('/api/devices/register',self.payload,'device-test-token') as response:
                self.assertEqual(response.headers['X-HVAC-Command'],'demand_ceiling')
                self.assertEqual(response.headers['X-HVAC-Maximum-Hz'],'20')
            self.payload['device']['proxy']['ceiling'].update(fresh=True,forwarded_hz=20)
            with post('/api/devices/register',self.payload,'device-test-token'): pass
            req=urllib.request.Request(base+'/api/history',headers={'Authorization':'Bearer browser-test-token'})
            with urllib.request.urlopen(req) as response:
                self.assertEqual(json.load(response)[-1]['values']['forwarded_demand_hz'],20)
            with self.assertRaises(urllib.error.HTTPError): post('/api/devices/register',{},'device-test-token')
        finally: server.shutdown();server.server_close();thread.join()
if __name__=='__main__': unittest.main()
