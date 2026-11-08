"""Optional Home Assistant MQTT discovery and thermostat bridge."""
import json
import logging
import math
import os
import re
import sqlite3
from threading import Event, Lock
from time import monotonic


LOG = logging.getLogger(__name__)
MODES = ['off', 'cool', 'heat', 'dry', 'fan_only']
FANS = ['auto', 'low', 'medium', 'high']
CLIMATE_FIELDS = {'mode': 'mode', 'fan_mode': 'fan', 'temperature': 'temperature_f'}
CEILING_FIELDS = {
    'ceiling_maximum_hz': ('maximum_hz', 'Cooling demand ceiling'),
    'ceiling_requested_hz': ('requested_hz', 'Factory requested demand'),
    'ceiling_forwarded_hz': ('forwarded_hz', 'Forwarded demand'),
}
MEASUREMENT_NAMES = {
    'indoor_demand_hz': 'Indoor requested demand',
    'actual_hz': 'Actual compressor frequency',
    'target_hz': 'Target compressor frequency',
    'power_w': 'Protocol power (provisional)',
}
UNITS = {
    '_c': ('°C', 'temperature'),
    '_hz': ('Hz', 'frequency'),
    '_a': ('A', 'current'),
    '_v': ('V', 'voltage'),
    '_w': ('W', 'power'),
    '_rpm': ('rpm', None),
    '_steps': ('steps', None),
    '_minutes': ('min', 'duration'),
    '_hours': ('h', 'duration'),
}


def number(value):
    return type(value) in (float, int) and math.isfinite(value)


def availability(key):
    return "{{ 'online' if value_json.get('" + key + "') is not none else 'offline' }}"


class HomeAssistantBridge:
    def __init__(self, supervisor, readback, client, identity, discovery_prefix):
        self.supervisor = supervisor
        self.readback = readback
        self.client = client
        self.identity = identity
        self.discovery_prefix = discovery_prefix
        self.base = f'hvac_controller/{identity}'
        self.status_topic = f'{self.base}/availability'
        self.state_topic = f'{self.base}/state'
        self.refresh = Event()
        self.lock = Lock()
        self.commands = {}
        self.command_ready_at = 0
        self.discovered = set()
        self.last_status = None
        self.next_state_at = 0

    def publish_config(self, platform, key, name, config):
        if key in self.discovered:
            return
        topic = f'{self.discovery_prefix}/{platform}/{self.identity}/{key}/config'
        payload = {
            'unique_id': f'{self.identity}_{key}', 'name': name,
            'device': {'identifiers': [self.identity], 'name': 'HVAC Controller'},
            **config,
        }
        if self.client.publish(topic, json.dumps(payload), qos=1, retain=True).rc == 0:
            self.discovered.add(key)

    def availability_config(self, template):
        return {
            'availability_mode': 'all',
            'availability': [
                {'topic': self.status_topic},
                {'topic': self.state_topic, 'value_template': template},
            ],
        }

    def sensor(self, key, name, *, binary=False):
        config = {
            'state_topic': self.state_topic,
            'value_template': "{{ value_json.get('" + key + "') }}",
            **self.availability_config(availability(key)),
        }
        if binary:
            self.publish_config('binary_sensor', key, name, config)
            return
        for suffix, (unit, device_class) in UNITS.items():
            if key.endswith(suffix):
                config['unit_of_measurement'] = unit
                if device_class:
                    config['device_class'] = device_class
                config['state_class'] = 'total_increasing' if device_class == 'duration' else 'measurement'
                break
        self.publish_config('sensor', key, name, config)

    def climate(self):
        config = {
            'modes': MODES, 'fan_modes': FANS, 'temperature_unit': 'F',
            'min_temp': 60, 'max_temp': 86, 'temp_step': 1,
            'optimistic': False, 'retain': False,
            **self.availability_config(availability('climate_ready')),
        }
        config['current_temperature_topic'] = self.state_topic
        config['current_temperature_template'] = "{{ value_json.get('indoor_f', 'None') }}"
        for field, key in CLIMATE_FIELDS.items():
            config[f'{field}_state_topic'] = self.state_topic
            config[f'{field}_state_template'] = "{{ value_json.get('" + key + "', 'None') }}"
            if self.supervisor.control:
                config[f'{field}_command_topic'] = f'{self.base}/set/{field}'
        if self.supervisor.control:
            config['temperature_command_template'] = '{{ value | float | round(0) | int }}'
        self.publish_config('climate', 'thermostat', None, config)

    def state_payload(self, snapshot):
        state = {}
        connected = snapshot.get('connected')
        indoor = None
        for port in snapshot.get('status', {}).get('ports', []):
            side = {0: 'odu', 1: 'idu'}.get(port.get('port'))
            if side is None:
                continue
            for field, measurement in port.get('telemetry', {}).items():
                if not re.fullmatch(r'[a-z0-9_]+', field) or not isinstance(measurement, dict):
                    continue
                key = f'{side}_{field}'
                label = MEASUREMENT_NAMES.get(field, field.removesuffix('_c').replace('_', ' ').capitalize())
                self.sensor(key, f'{side.upper()} {label}')
                value = measurement.get('value')
                if connected and measurement.get('valid') and not measurement.get('stale') and number(value):
                    state[key] = value
                    if field == 'indoor_ambient_c':
                        updated = measurement.get('updated_us', 0)
                        if indoor is None or updated >= indoor[0]:
                            indoor = (updated, value)
        if indoor is not None:
            state['indoor_f'] = round(indoor[1] * 9 / 5 + 32, 1)

        ceiling = snapshot.get('device', {}).get('proxy', {}).get('ceiling', {})
        if ceiling.get('supported'):
            self.sensor('ceiling_enabled', 'Cooling demand ceiling enabled', binary=True)
            if connected and type(ceiling.get('enabled')) is bool:
                state['ceiling_enabled'] = 'ON' if ceiling['enabled'] else 'OFF'
            for key, (field, name) in CEILING_FIELDS.items():
                self.sensor(key, name)
                value = ceiling.get(field)
                if connected and number(value) and (field == 'maximum_hz' or ceiling.get('fresh')):
                    state[key] = value

        xye = snapshot.get('xye', {})
        self.climate()
        if connected and xye.get('valid') and not xye.get('stale'):
            climate = self.readback(xye)
            if climate['mode'] is not None:
                state['mode'] = 'fan_only' if climate['mode'] == 'fan' else climate['mode']
                state['climate_ready'] = True
            if climate['fan'] is not None:
                state['fan'] = climate['fan']
            if type(climate['temperature_f']) is int and 60 <= climate['temperature_f'] <= 86:
                state['temperature_f'] = climate['temperature_f']
            for field in ('error_flags', 'protection_flags'):
                if number(xye.get(field)):
                    state[f'xye_{field}'] = xye[field]
        for field in ('error_flags', 'protection_flags'):
            self.sensor(f'xye_{field}', f"XYE {field.replace('_', ' ')}")
        return state

    def on_message(self, client, userdata, message):
        if message.retain or not self.supervisor.control:
            return
        field = message.topic.removeprefix(f'{self.base}/set/')
        if field not in CLIMATE_FIELDS or message.topic != f'{self.base}/set/{field}':
            return
        try:
            value = message.payload.decode('ascii')
            if field == 'temperature':
                if not re.fullmatch(r'[0-9]{2}(?:\.0+)?', value):
                    raise ValueError('Temperature must be a whole Fahrenheit value')
                value = int(value.split('.')[0])
                if not 60 <= value <= 86:
                    raise ValueError('Temperature must be 60–86 °F')
            elif field == 'mode':
                if value not in MODES:
                    raise ValueError('Unsupported HVAC mode')
                if value == 'fan_only':
                    value = 'fan'
            elif value not in FANS:
                raise ValueError('Unsupported fan mode')
        except ValueError as exc:
            LOG.warning('MQTT thermostat command rejected: %s', exc)
            return
        with self.lock:
            self.commands[CLIMATE_FIELDS[field]] = value
            # Home Assistant can send mode and setpoint in separate messages for one action.
            self.command_ready_at = monotonic() + 0.2

    def take_commands(self, *, discard=False):
        with self.lock:
            if not discard and monotonic() < self.command_ready_at:
                return {}
            commands, self.commands = self.commands, {}
        return commands

    def publish(self):
        if self.refresh.is_set():
            self.refresh.clear()
            self.discovered.clear()
            self.last_status = None
            self.next_state_at = 0
        if not self.client.is_connected():
            return
        commands = self.take_commands()
        if commands:
            try:
                self.supervisor.command({'kind': 'climate', **commands}, partial_climate=True)
            except (ValueError, sqlite3.Error) as exc:
                LOG.warning('MQTT thermostat command rejected: %s', exc)
        now = monotonic()
        if now < self.next_state_at:
            return
        self.next_state_at = now + 2
        snapshot = self.supervisor.snapshot()
        self.client.publish(self.state_topic, json.dumps(self.state_payload(snapshot)))
        status = 'online' if snapshot.get('connected') else 'offline'
        if status != self.last_status and self.client.publish(self.status_topic, status, qos=1, retain=True).rc == 0:
            self.last_status = status


def run_bridge(supervisor, readback):
    try:
        from paho.mqtt import client as mqtt

        while not supervisor.stop.is_set():
            device_id = supervisor.snapshot().get('device', {}).get('device_id')
            if device_id:
                break
            supervisor.stop.wait(1)
        else:
            return

        supervisor.mqtt_status = 'connecting'
        identity = 'hvac_' + device_id.replace(':', '').lower()
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=identity, protocol=mqtt.MQTTv5)
        bridge = HomeAssistantBridge(supervisor, readback, client, identity,
                                     os.getenv('MQTT_DISCOVERY_PREFIX', 'homeassistant'))
        tls = os.getenv('MQTT_TLS', '').lower() == 'true'
        if tls:
            client.tls_set()
        if os.getenv('MQTT_USERNAME'):
            client.username_pw_set(os.environ['MQTT_USERNAME'], os.getenv('MQTT_PASSWORD'))

        def on_connect(client, userdata, flags, reason_code, properties):
            if reason_code.is_failure:
                supervisor.mqtt_status = 'failed'
                LOG.warning('MQTT broker refused connection: %s', reason_code)
                return
            supervisor.mqtt_status = 'connected'
            bridge.refresh.set()
            if supervisor.control:
                client.subscribe(f'{bridge.base}/set/+', options=mqtt.SubscribeOptions(retainAsPublished=True))

        def on_disconnect(client, userdata, flags, reason_code, properties):
            bridge.take_commands(discard=True)
            if not supervisor.stop.is_set():
                supervisor.mqtt_status = 'failed'

        def on_connect_fail(client, userdata):
            supervisor.mqtt_status = 'failed'

        client.on_connect = on_connect
        client.on_connect_fail = on_connect_fail
        client.on_disconnect = on_disconnect
        client.on_message = bridge.on_message
        client.will_set(bridge.status_topic, 'offline', qos=1, retain=True)
        client.connect_async(os.environ['MQTT_HOST'], int(os.getenv('MQTT_PORT') or (8883 if tls else 1883)),
                             keepalive=15, clean_start=True)
        client.loop_start()
        try:
            while not supervisor.stop.is_set():
                bridge.publish()
                supervisor.stop.wait(0.25)
        finally:
            if client.is_connected():
                offline = client.publish(bridge.status_topic, 'offline', qos=1, retain=True)
                if offline.rc == 0:
                    offline.wait_for_publish(timeout=2)
            client.disconnect()
            client.loop_stop()
    except (ImportError, OSError, RuntimeError, ValueError) as exc:
        supervisor.mqtt_status = 'failed'
        LOG.error('MQTT bridge stopped: %s', exc)
