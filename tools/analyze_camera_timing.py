"""Inspect the PS5 original-camera capture without assuming the engine tick rate."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import statistics
import struct

RECORD = struct.Struct('<Q4I6f2d16f')
PREDICTED_RECORD = struct.Struct('<Q4I6f2d16f2IfI')
WALKING_RECORD = struct.Struct('<Q4I6f2d16f2IfI9fI')
EFFECTS_RECORD = struct.Struct('<Q4I6f2d16f2IfI9fI5I14fI')


def distribution(values):
    values = sorted(x for x in values if math.isfinite(x))
    if not values:
        return None
    return {'count': len(values), 'min': values[0], 'median': statistics.median(values),
            'p95': values[min(len(values)-1, math.ceil(len(values)*.95)-1)], 'max': values[-1]}


def analyze(path):
    data = Path(path).read_bytes()
    if len(data) < 8:
        raise ValueError('Truncated camera header')
    layout = {b'DCT1':RECORD, b'DCT2':PREDICTED_RECORD, b'DCT3':WALKING_RECORD,
              b'DCT4':EFFECTS_RECORD}.get(data[:4])
    if layout is None or struct.unpack_from('<I', data, 4)[0] != layout.size:
        raise ValueError('Unsupported camera capture format or record size')
    if (len(data)-8) % layout.size:
        raise ValueError('Truncated camera record; wait for the capture to finish')
    records = list(layout.iter_unpack(data[8:]))
    if not records:
        raise ValueError('No camera samples')
    owners = Counter((r[1], r[2]) for r in records)
    owner = owners.most_common(1)[0][0]
    rows = [r for r in records if (r[1], r[2]) == owner]
    periods, client_steps, player_steps, rotation, translation, phase_error = [], [], [], [], [], []
    boundary_speeds, within_speeds = [], []
    transitions = []
    for row in rows:
        phase_error.append((row[3]+row[5])-(row[4]+row[6]))
    for a, b in zip(rows, rows[1:]):
        dt = (b[0]-a[0])/1e9
        # Multiple original camera queries can occur inside one rendered frame.
        if dt < .001:
            continue
        periods.append(dt*1000)
        client_steps.append((b[3]+b[5])-(a[3]+a[5]))
        player_steps.append((b[4]+b[6])-(a[4]+a[6]))
        pa, pb = a[13:29], b[13:29]
        dot = sum(pa[r*4+c]*pb[r*4+c] for r in range(3) for c in range(3))
        angle = math.degrees(math.acos(max(-1, min(1, (dot-1)*.5))))
        distance = math.sqrt(sum((pa[i]-pb[i])**2 for i in (12,13,14)))
        rotation.append(angle)
        translation.append(distance)
        if math.isfinite(angle):
            (boundary_speeds if b[4] != a[4] else within_speeds).append(angle/dt)
        transitions.append({'host_ms':dt*1000, 'tick_before':a[4], 'tick_after':b[4],
                            'phase_before':a[6], 'phase_after':b[6],
                            'rotation_degrees':angle, 'distance':distance})
    stages = None
    if layout in (WALKING_RECORD, EFFECTS_RECORD):
        fixed, moving, partial = 0, 0, 0
        effects, base_steps, final_steps = [], [], []
        for a,b in zip(rows, rows[1:]):
            if not (a[42] and b[42]):
                continue
            base_steps.append(math.dist(a[39:42],b[39:42]))
            final_steps.append(math.dist(a[25:28],b[25:28]))
            effects.append(math.dist([a[25+i]-a[39+i] for i in range(3)],
                                     [b[25+i]-b[39+i] for i in range(3)]))
            if a[4] != b[4] or abs(a[6]-b[6]) < .05 or math.dist(b[33:36],b[36:39]) < .01:
                continue
            moving += 1
            if math.dist(a[33:36],b[33:36]) < .001:
                fixed += 1
            else:
                partial += 1
        stages = {'moving_pairs_inside_tick':moving, 'unchanged_entity_position':fixed,
                  'changed_entity_position':partial, 'before_effects_steps':distribution(base_steps),
                  'after_effects_steps':distribution(final_steps), 'effect_offset_changes':distribution(effects)}
    walking_detail = None
    if layout == EFFECTS_RECORD:
        head = [r for r in rows if r[43] & 1]
        smoothed = [r for r in rows if r[43] & 2]
        walking_detail = {'head_samples':len(head), 'smoothed_samples':len(smoothed),
            'after_walking_samples':sum(bool(r[62]) for r in rows),
            'test_enabled':sum(bool(r[43] & 4) for r in head),
            'test_applied':sum(bool(r[43] & 8) for r in head),
            'relative_test_enabled':sum(bool(r[43] & 16) for r in head),
            'relative_test_applied':sum(bool(r[43] & 32) for r in head),
            'rejection_codes':dict(Counter(r[44] for r in head)),
            'smoothing_elapsed':distribution([r[49] for r in smoothed])}
        for name, start, flag in [('raw_head_steps',50,1), ('filtered_head_steps',53,1),
                                  ('smoothed_head_steps',56,2)]:
            walking_detail[name] = distribution([math.dist(a[start:start+3],b[start:start+3])
                for a,b in zip(rows,rows[1:]) if a[43] & flag and b[43] & flag])
        walking_detail['after_walking_steps'] = distribution([math.dist(a[59:62],b[59:62])
            for a,b in zip(rows,rows[1:]) if a[62] and b[62]])
    return {
        'records': len(records), 'camera_owners': {f'{c:08X}/{p:08X}':n for (c,p),n in owners.items()},
        'format': data[:4].decode(),
        'walking_stages': stages,
        'walking_detail': walking_detail,
        'selected_player_copies': dict(Counter(f'{r[29]:08X}' for r in rows)) if layout != RECORD else None,
        'root_phase': distribution([r[31] for r in rows]) if layout != RECORD else None,
        'selected_camera': f'{owner[0]:08X}/{owner[1]:08X}',
        'capture_seconds': (rows[-1][0]-rows[0][0])/1e9,
        'engine_step_authored_units': distribution([r[7] for r in rows]),
        'time_scale': distribution([r[8] for r in rows]),
        'client_phase': distribution([r[5] for r in rows]),
        'player_phase': distribution([r[6] for r in rows]),
        'requested_phase': distribution([r[9] for r in rows]),
        'client_minus_player_time_ticks': distribution(phase_error),
        'camera_query_intervals_ms': distribution(periods),
        'client_time_advance_ticks': distribution(client_steps),
        'player_time_advance_ticks': distribution(player_steps),
        'camera_rotation_degrees': distribution(rotation),
        'camera_translation': distribution(translation),
        'rotation_degrees_per_second_at_tick_boundary': distribution(boundary_speeds),
        'rotation_degrees_per_second_inside_tick': distribution(within_speeds),
        'largest_rotation_steps': sorted(transitions, key=lambda x:x['rotation_degrees'], reverse=True)[:8],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture')
    args = parser.parse_args()
    print(json.dumps(analyze(args.capture), indent=2))
