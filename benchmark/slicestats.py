#!/usr/bin/env python3
"""Aggregate a srcslice output into dataflow statistics.

usage: slicestats.py SLICES.json [-j WORKERS] [-o RESULT.json] [--probe-pattern probe]

Reads the JSON that srcslice writes (one entry per declared variable, one field per line) in
parallel byte ranges, so the 41.7 GB full-kernel output takes about 15 s on 32 cores, and
reports what the forward slices say about the code base:

- flows between subsystems: for every pair of top-level directories (A, B), how many variables
  declared under A have a use under B (`flow_vars`) and how many use positions that is
  (`flow_pos`); the same one level down inside drivers/ (`flow_vars_drivers`)
- per subsystem: variables declared, and how many of them are used in another subsystem
  (`leave_any`, or `leave_proper` counting only the kernel-proper directories)
- reach: for every variable the number of distinct files among its definition and use
  positions; `reach_hist` is the histogram, `top` the variables with the widest reach, and
  `probe_top` the same restricted to variables declared in functions whose name contains the
  probe pattern (a driver's device-context struct, typically)

The result is JSON; `--summary` prints the headline numbers. Subsystem = the first path
component of a position (`drivers`, `fs`, ...), `(root)` for top-level files.
"""
import argparse
import heapq
import json
import os
import re
import sys
import time
from collections import Counter
from multiprocessing import Pool

POS = re.compile(rb'"([^":]+):\d+:\d+"')
STR = re.compile(rb'"([^"]*)"')
TOPN = 300
PROBE_TOPN = 3000
PROPER = {'arch', 'block', 'crypto', 'drivers', 'fs', 'include', 'init', 'io_uring', 'ipc',
          'kernel', 'lib', 'mm', 'net', 'security', 'sound', 'virt'}
PROBE = b'probe'


def subsystem(path):
    i = path.find(b'/')
    return (path[:i] if i > 0 else b'(root)').decode()


def driver_area(path):
    """drivers/<area> for a path under drivers/, else None."""
    if not path.startswith(b'drivers/'):
        return None
    j = path.find(b'/', 8)
    return (path[:j] if j > 0 else path).decode()


def new_entry():
    return {'file': b'', 'name': b'', 'function': b'', 'uses': 0, 'files': set(),
            'usesub': Counter(), 'usearea': Counter(), 'calls': 0}


def push_top(heap, limit, item):
    if len(heap) < limit:
        heapq.heappush(heap, item)
    elif item > heap[0]:
        heapq.heapreplace(heap, item)


def finish(entry, acc):
    acc['stats']['entries'] += 1
    dsub = subsystem(entry['file'])
    acc['decl_vars'][dsub] += 1
    reach = len(entry['files'])
    acc['reach_hist'][reach] += 1
    uses = entry['uses']
    acc['stats']['uses'] += uses
    usesub = entry['usesub']
    for usub, n in usesub.items():
        acc['flow_pos'][(dsub, usub)] += n
        acc['flow_vars'][(dsub, usub)] += 1
        if usub != dsub:
            acc['stats']['cross_pos'] += n
    if any(s != dsub for s in usesub):
        acc['stats']['cross_vars'] += 1
        acc['leave_any'][dsub] += 1
    if any(s != dsub and s in PROPER for s in usesub):
        acc['leave_proper'][dsub] += 1
        if dsub in PROPER:
            acc['stats']['cross_vars_proper'] += 1
    darea = driver_area(entry['file'])
    if darea is not None:
        acc['decl_vars_drivers'][darea] += 1
        for uarea in entry['usearea']:
            acc['flow_vars_drivers'][(darea, uarea)] += 1
        if any(u != darea for u in entry['usearea']):
            acc['leave_drivers'][darea] += 1
    item = (reach, uses, entry['name'].decode(errors='replace'), entry['function'].decode(errors='replace'),
            entry['file'].decode(errors='replace'), entry['calls'])
    push_top(acc['top'], TOPN, item)
    if PROBE in entry['function']:
        push_top(acc['probe_top'], PROBE_TOPN, item)


def process_range(args):
    path, start, end = args
    acc = {'flow_pos': Counter(), 'flow_vars': Counter(), 'decl_vars': Counter(), 'reach_hist': Counter(),
           'leave_any': Counter(), 'leave_proper': Counter(), 'flow_vars_drivers': Counter(),
           'decl_vars_drivers': Counter(), 'leave_drivers': Counter(), 'stats': Counter(),
           'top': [], 'probe_top': []}
    entry = None
    with open(path, 'rb') as f:
        f.seek(start)
        if start > 0:
            f.readline()                      # finish the partial line
        while True:
            pos = f.tell()
            if pos >= end:
                break
            line = f.readline()
            if not line:
                break
            # an entry starts with its key line at column 0 and ends with a line starting with }
            if line.startswith(b'"') and line.rstrip().endswith(b'":{'):
                entry = new_entry()
                continue
            if entry is None:
                continue
            if line.startswith(b'}'):
                finish(entry, acc)
                entry = None
                continue
            s = line.lstrip()
            if s.startswith(b'"file":'):
                m = STR.search(s, 7)
                entry['file'] = m.group(1) if m else b''
            elif s.startswith(b'"name":'):
                m = STR.search(s, 7)
                entry['name'] = m.group(1) if m else b''
            elif s.startswith(b'"function":'):
                m = STR.search(s, 11)
                entry['function'] = m.group(1) if m else b''
            elif s.startswith(b'"use":'):
                files, usesub, usearea = entry['files'], entry['usesub'], entry['usearea']
                n = 0
                for m in POS.finditer(s, 6):
                    p = m.group(1)
                    files.add(p)
                    usesub[subsystem(p)] += 1
                    area = driver_area(p)
                    if area is not None:
                        usearea[area] += 1
                    n += 1
                entry['uses'] = n
            elif s.startswith(b'"definition":'):
                for m in POS.finditer(s, 13):
                    entry['files'].add(m.group(1))
            elif s.startswith(b'"calls":'):
                entry['calls'] = s.count(b'"functionName"')
    # tuple keys do not survive JSON: join them with a tab
    for k in ('flow_pos', 'flow_vars', 'flow_vars_drivers'):
        acc[k] = {f'{a}\t{b}': n for (a, b), n in acc[k].items()}
    return acc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('slices', help='srcslice output (JSON)')
    ap.add_argument('-j', '--workers', type=int, default=os.cpu_count() or 1)
    ap.add_argument('-o', '--output', default=None, help='result JSON (default: <slices>.stats.json)')
    ap.add_argument('--probe-pattern', default='probe', help='substring of function names that count as probe functions')
    ap.add_argument('--summary', action='store_true', help='print the headline numbers')
    args = ap.parse_args()
    global PROBE
    PROBE = args.probe_pattern.encode()

    size = os.path.getsize(args.slices)
    chunks = args.workers * 4
    ranges = [(args.slices, size * i // chunks, size * (i + 1) // chunks) for i in range(chunks)]
    t0 = time.time()
    merged = {k: Counter() for k in ('flow_pos', 'flow_vars', 'decl_vars', 'reach_hist', 'leave_any', 'leave_proper',
                                     'flow_vars_drivers', 'decl_vars_drivers', 'leave_drivers', 'stats')}
    top, probe_top = [], []
    with Pool(args.workers) as pool:
        for i, r in enumerate(pool.imap_unordered(process_range, ranges)):
            for k in merged:
                merged[k].update({(int(a) if k == 'reach_hist' else a): n for a, n in r[k].items()})
            for item in r['top']:
                push_top(top, TOPN, tuple(item))
            for item in r['probe_top']:
                push_top(probe_top, PROBE_TOPN, tuple(item))
            print(f'\r{i + 1}/{chunks} chunks, {merged["stats"]["entries"]:,} entries, {time.time() - t0:.0f} s', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)
    result = {k: dict(v) for k, v in merged.items()}
    result['top'] = sorted(top, reverse=True)
    result['probe_top'] = sorted(probe_top, reverse=True)
    result['elapsed_s'] = time.time() - t0
    out = args.output or (args.slices + '.stats.json')
    with open(out, 'w') as f:
        json.dump(result, f)
    print(f'wrote {out} ({result["elapsed_s"]:.0f} s)', file=sys.stderr)

    if args.summary:
        st = result['stats']
        print(f"{st['entries']:,} variables, {st['uses']:,} use positions; "
              f"{st['cross_vars']:,} variables ({st['cross_vars'] / st['entries'] * 100:.1f} %) are used in another subsystem")
        hist = result['reach_hist']
        total = sum(hist.values())
        one = hist.get(1, 0)
        print(f"reach: {one / total * 100:.1f} % of variables stay in one file; {sum(n for r, n in hist.items() if r > 100):,} reach over 100 files")
        print('largest flows (variables):')
        for key, n in sorted(result['flow_vars'].items(), key=lambda kv: -kv[1])[:40]:
            a, b = key.split('\t')
            if a != b:
                print(f'  {a:12} -> {b:12} {n:10,}')
        print('widest reach:')
        for reach, uses, name, function, file, calls in result['top'][:10]:
            print(f'  {reach:5} files  {name} in {function or "(file scope)"}, {file}')


if __name__ == '__main__':
    main()
