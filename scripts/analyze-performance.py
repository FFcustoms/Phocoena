#!/usr/bin/env python3
"""Analyze an explicitly supplied PC log. Never discovers or accesses an SD card.

Speed uses the same first/last heartbeats inside guest seconds 20..60 as the
35%, 52% and 57.1% comparisons. Inclusive scopes overlap; sampled scopes are
estimates, not exact frame accounting or GPU utilization.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)', line))


def interval(first, last):
    wall = last['wall_ms'] - first['wall_ms']
    guest = last['guest_ms'] - first['guest_ms']
    if wall <= 0 or guest < 0:
        return None
    scopes = {}
    for name, current in last['scopes'].items():
        previous = first['scopes'].get(name)
        if not previous or current['sample_scale'] != previous['sample_scale']:
            continue
        values = {k: current[k] - previous[k] for k in ('calls', 'ms', 'cpu_calls', 'cpu_ms')}
        if any(v < -0.002 for v in values.values()):
            continue
        values['sample_scale'] = current['sample_scale']
        values['wall_fraction_pct_estimate'] = 100 * values['ms'] * current['sample_scale'] / wall
        values['cpu_fraction_pct_estimate'] = (
            100 * values['cpu_ms'] * current['sample_scale'] / wall
            if values['calls'] > 0 and values['cpu_calls'] == values['calls'] else None)
        scopes[name] = values
    return dict(guest_start_ms=first['guest_ms'], guest_end_ms=last['guest_ms'], wall_ms=wall,
                speed_pct=100 * guest / wall, scopes=scopes, threads=last['threads'])


def analyze(text):
    runs = []
    run = None
    sample = None
    for line in text.splitlines():
        if 'INFO: Boot stage 1:' in line:
            run = dict(game=line.split('INFO: Boot stage 1:', 1)[1].strip(),
                       worker=None, samples=[], clean_join=False)
            runs.append(run)
            sample = None
        if run is None:
            continue
        if 'Vulkan boot: submission worker=' in line:
            run['worker'] = fields(line).get('worker') == '1'
        match = re.search(r'Boot progress: (\S+) wall_ms=(\d+).*?guest_ms=([\d.]+)', line)
        if match:
            sample = dict(phase=match[1], wall_ms=int(match[2]), guest_ms=float(match[3]),
                          scopes={}, threads={})
            run['samples'].append(sample)
            run['clean_join'] |= match[1] == 'after-join'
        match = re.search(r'Performance scope: (\S+) metric=(\w+)', line)
        if match and sample and match[1] == sample['phase']:
            data = fields(line)
            sample['scopes'][match[2]] = dict(
                calls=int(data['boot_calls']), ms=float(data['boot_ms']),
                cpu_calls=int(data.get('boot_cpu_calls', 0)), cpu_ms=float(data.get('boot_cpu_ms', 0)),
                sample_scale=int(data.get('sample_scale', 1)))
        if sample and 'Performance threads:' in line:
            cpu = re.search(r'CPU_id=(\d+) core=(\d+) busy_pct=([-\d.]+)', line)
            gpu = re.search(r'GPU_id=(\d+) core=(\d+) busy_pct=([-\d.]+)', line)
            for role, match in (('cpu', cpu), ('graphics', gpu)):
                if match:
                    sample['threads'][role] = dict(id=int(match[1]), core=int(match[2]), busy_pct=float(match[3]))
        if sample and 'Performance worker:' in line:
            data = fields(line)
            sample['threads']['worker'] = dict(id=int(data['id']), core=int(data['core']),
                                               busy_pct=float(data['busy_pct']),
                                               age_ms=int(data['sample_age_ms']))
    for run in runs:
        samples = run.pop('samples')
        window = [s for s in samples if s['phase'] == 'heartbeat' and 20000 <= s['guest_ms'] <= 60000]
        run['window'] = interval(window[0], window[-1]) if len(window) >= 2 else None
        run['intervals'] = [value for a, b in zip(window, window[1:]) if (value := interval(a, b))]
        if run['intervals']:
            rates = [i['speed_pct'] for i in run['intervals']]
            run['sampled_range_pct'] = [min(rates), max(rates)]
            ordered = sorted(run['intervals'], key=lambda i: i['speed_pct'])
            count = max(1, len(ordered) // 4)
            # References to full interval records let callers inspect what grew
            # in slow scenes; no automatic bottleneck claim from inclusive sums.
            run['slowest_quartile'] = ordered[:count]
            run['fastest_quartile'] = ordered[-count:]
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='Explicit PC-resident dolphin.log')
    parser.add_argument('--output', type=Path, help='Optional JSON report path on PC')
    args = parser.parse_args()
    data = args.log.read_bytes()
    report = dict(file=str(args.log.resolve()), sha256=hashlib.sha256(data).hexdigest(),
                  method='First/last heartbeats with 20000 <= guest_ms <= 60000; 100 * delta guest / delta wall. No interpolation.',
                  limits='Scopes overlap across nested calls and threads. Sample_scale=64 means raw probabilistic samples; scaled fractions are noisy estimates. CPU role includes JIT/HLE/core services. No physical GPU utilization or pure JIT execution measurement. Similar guest times do not prove identical scene/input.',
                  runs=analyze(data.decode(errors='replace')))
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    for index, run in enumerate(report['runs'], 1):
        if not run['window']:
            print(f'Run {index}: insufficient samples in guest 20..60s window')
            continue
        low, high = run['sampled_range_pct']
        print(f"Run {index}: worker={run['worker']} speed={run['window']['speed_pct']:.1f}% range={low:.1f}..{high:.1f}%")
    if not args.output:
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
