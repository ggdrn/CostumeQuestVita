#!/usr/bin/env python3
"""Summarize a particle trace capture without treating overlapping calls as frame time."""
import argparse
from collections import defaultdict
from pathlib import Path
import re


def analyze(text):
    groups = defaultdict(list)
    names = {}
    totals = {}
    version = None
    for line in text.splitlines():
        if match := re.search(r'particle trace v(\d+):', line):
            version = int(match[1])
        if match := re.search(r'particle asset: resource=(\w+) name=(.*)', line):
            names[match[1]] = match[2]
        if 'particle trace: ' in line:
            values = dict(re.findall(r'(\w+)=([^\s]+)', line))
            groups[(values['kind'], values['resource'], values['data'])].append(values)
        if 'particle trace totals:' in line:
            totals = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
    return version, groups, names, totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    version, groups, names, totals = analyze(args.log.read_text(errors='replace'))
    if version is None:
        parser.error('This log has no particle trace version marker.')
    print(f'Particle trace v{version}; resource identifiers apply only to this capture.')
    print('Latest cumulative counters:', totals)
    print('Detailed calls >= 1 ms, capped at 256; smaller calls appear only in counters.')
    print('Times below sum call durations; they are not exclusive frame times.\n')
    print('kind resource data calls slow>=50ms total_ms min_ms max_ms reset_age_ms unknown name')
    ranked = sorted(groups.items(), key=lambda item: -sum(int(r['us']) for r in item[1]))
    for key, rows in ranked:
        times = [int(r['us']) / 1000 for r in rows]
        ages = [int(r['reset_age_ms']) for r in rows if int(r['reset_age_ms']) >= 0]
        age_range = f'{min(ages)}..{max(ages)}' if ages else 'untracked'
        unknown = sum(r['generation'] == '0' for r in rows)
        slow = sum(t >= 50 for t in times)
        print(*key, len(rows), slow, f'{sum(times):.3f}', f'{min(times):.3f}',
              f'{max(times):.3f}', age_range, unknown, names.get(key[1], '(opaque)'))
    if totals.get('untracked_resets', 0):
        print('\nWARNING: identity table filled; some resets were not tracked. '
              'Generation 0 does not imply the same instance lifetime.')
    if totals.get('detail_records', 0) >= 256:
        print('WARNING: detail limit reached; grouping is a partial sample.')


if __name__ == '__main__':
    main()
