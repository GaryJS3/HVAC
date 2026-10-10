"""Reassemble logged UART chunks and summarize CRC-valid S1/S2 traffic."""
import argparse
import collections
import json
import pathlib
import statistics

def crc(data):
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xa001 if value & 1 else 0)
    return value

def analyze(path):
    streams = [bytearray(), bytearray()]
    timestamps = [[], []]
    gaps = [0, 0]
    last_sequence = [None, None]
    health = []
    for line in path.read_text(errors='replace').splitlines():
        fields = line.split()
        if fields and fields[0] == 'HEALTH':
            health.append(line)
        if len(fields) != 6 or fields[0] != 'RX':
            continue
        try:
            stamp, port, sequence, length = map(int, fields[1:5])
            data = bytes.fromhex(fields[5])
        except ValueError:
            continue
        if port not in (0, 1) or len(data) != length:
            raise ValueError('Malformed RX record')
        if last_sequence[port] is not None:
            gaps[port] += sequence - last_sequence[port] - 1
        last_sequence[port] = sequence
        streams[port].extend(data)
        timestamps[port].extend([stamp] * length)
    result = {'ports': [], 'health_first': health[0] if health else None,
              'health_last': health[-1] if health else None,
              'timing_basis': 'UART software drain timestamps, not wire edges'}
    for port, data in enumerate(streams):
        path.with_suffix(f'.port{port}.bin').write_bytes(data)
        frames = []
        index = failures = noise = 0
        while index < len(data):
            if data[index] != 0xa0:
                noise += 1; index += 1; continue
            if index + 5 > len(data):
                break
            length = data[index + 4] + 8
            if index + length > len(data):
                # Search suffixes so a corrupt length cannot conceal later frames.
                index += 1; noise += 1; continue
            frame = bytes(data[index:index + length])
            if crc(frame[:-2]) != int.from_bytes(frame[-2:], 'little'):
                failures += 1; index += 1; continue
            frames.append((timestamps[port][index + length - 1], frame))
            index += length
        counts = collections.Counter(f'{int.from_bytes(f[1:3], "big"):04X}_{f[3]:02X}' for _, f in frames)
        ranges = collections.defaultdict(list)
        replies = []
        pending = None
        for stamp, f in frames:
            address = int.from_bytes(f[1:3], 'big')
            if address == 1:
                pending = (stamp, f[3])
            elif address == 256 and pending and pending[1] == f[3]:
                replies.append((stamp - pending[0]) / 1000)
                pending = None
            if len(f) != 20 or f[4] != 12:
                continue
            if address == 1 and f[3] == 0x20:
                ranges['actual_hz_probable'].append(f[6])
            if address == 1 and f[3] == 0x53:
                ranges['target_hz_reference'].append(f[13])
                ranges['system_power_w_probable'].append(f[11] + 256 * f[12])
                ranges['compressor_state_raw'].append(f[9])
            if address == 256 and f[3] == 0x20:
                ranges['requested_hz_probable'].append(f[7])
                ranges['indoor_setpoint_c_reference'].append(f[11])
                ranges['indoor_mode_raw'].append(f[6])
        result['ports'].append({'port': port, 'rx_gpio': 35 if port == 0 else 33,
            'bytes': len(data), 'valid_frames': len(frames), 'crc_failed_candidates': failures,
            'unframed_bytes': noise, 'chunk_sequence_gaps': gaps[port],
            'messages': dict(sorted(counts.items())),
            'ranges': {name: {'min': min(v), 'max': max(v)} for name, v in ranges.items()},
            'matched_reply_intervals_ms': {'count': len(replies), 'min': min(replies),
                'median': statistics.median(replies), 'max': max(replies)} if replies else None})
    path.with_suffix('.analysis.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=pathlib.Path)
    analyze(p.parse_args().capture)
