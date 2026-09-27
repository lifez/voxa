#!/usr/bin/env python3
"""Read-only live-daemon benchmark: status only, no mic/API/clipboard changes.
Run make first. Benchmarks the native CLI and the small C/Python prototypes.
"""
import argparse
import json
import math
import pathlib
import platform
import random
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def summary(values):
    values = sorted(values)
    return {'n': len(values), 'min_ms': min(values), 'median_ms': statistics.median(values),
            'p95_ms': values[math.ceil(len(values) * .95) - 1], 'max_ms': max(values)}


def measure(command):
    start = time.perf_counter_ns()
    result = subprocess.run(command + ['status'], capture_output=True, timeout=3, check=True)
    elapsed = (time.perf_counter_ns() - start) / 1e6
    if result.stdout.strip() not in (b'idle', b'recording', b'committing'):
        raise RuntimeError('Unexpected daemon response')
    return elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs', type=int, default=100)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error('--runs must be positive')
    with tempfile.TemporaryDirectory(prefix='voxa-control-') as directory:
        binary = str(pathlib.Path(directory) / 'control')
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'bench/control.c'), '-o', binary], check=True)
        commands = {'native': [str(ROOT / 'native/voxa')],
                    'python': [sys.executable, str(ROOT / 'bench/control.py')], 'c': [binary]}
        for command in commands.values():
            for _ in range(5):
                measure(command)
        values = {name: [] for name in commands}
        rng = random.Random(42)
        for _ in range(args.runs):
            names = list(commands)
            rng.shuffle(names)
            for name in names:
                values[name].append(measure(commands[name]))
        print(json.dumps({'platform': platform.platform(), 'python': platform.python_version(),
                          'method': 'warm filesystem cache; 5 warmups; shuffled rounds; subprocess start to exit; status only',
                          'results': {name: summary(samples) for name, samples in values.items()}}, indent=2))


if __name__ == '__main__':
    main()
