#!/usr/bin/env python3
"""Compare idle Node/C daemons on isolated sockets; never record or use an API key.
Prerequisites: npm run build && make -C native
"""
import json
import math
import os
import pathlib
import platform
import random
import socket
import statistics
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def summary(a):
    a = sorted(a)
    return {'n': len(a), 'median_ms': statistics.median(a),
            'p95_ms': a[math.ceil(.95 * len(a)) - 1], 'min_ms': a[0], 'max_ms': a[-1]}


def status(path):
    start = time.perf_counter_ns()
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(1.5)
        sock.connect(str(path))
        sock.sendall(b'status\n')
        sock.shutdown(socket.SHUT_WR)
        data = b''
        while not data.endswith(b'\n'):
            chunk = sock.recv(64)
            if not chunk:
                raise RuntimeError('No complete response')
            data += chunk
        if data != b'idle\n':
            raise RuntimeError('Unexpected state')
    return (time.perf_counter_ns() - start) / 1e6


def cli(command, env):
    start = time.perf_counter_ns()
    result = subprocess.run(command + ['status'], env=env, capture_output=True, check=True, timeout=3)
    if result.stdout != b'idle\n':
        raise RuntimeError('Unexpected CLI output')
    return (time.perf_counter_ns() - start) / 1e6


def rss(pid):
    for line in pathlib.Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith('VmRSS:'):
            return int(line.split()[1])
    return None


def main():
    daemons = []
    with tempfile.TemporaryDirectory(prefix='voxa-daemon-bench-') as temp:
        mock_bin = pathlib.Path(temp) / 'bin'
        mock_bin.mkdir()
        mock_osd = mock_bin / 'omarchy-shell'
        mock_osd.write_text('#!/bin/sh\nexit 0\n')
        mock_osd.chmod(0o700)
        env = {**os.environ, 'XDG_RUNTIME_DIR': temp, 'XDG_CONFIG_HOME': temp,
               'PATH': str(mock_bin) + os.pathsep + os.environ.get('PATH', '')}
        env.pop('ELEVENLABS_API_KEY', None)
        commands = {'node': ['node', str(ROOT / 'dist/index.js')],
                    'c': [str(ROOT / 'native/voxa-c')]}
        paths = {'node': pathlib.Path(temp) / 'voxa.sock', 'c': pathlib.Path(temp) / 'voxa-c.sock'}
        try:
            for name, command in commands.items():
                process = subprocess.Popen(command + ['daemon'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                daemons.append(process)
                deadline = time.monotonic() + 5
                while not paths[name].exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'{name} daemon failed to start')
                    time.sleep(.01)
            functions = {
                'node_socket': lambda: status(paths['node']),
                'c_socket': lambda: status(paths['c']),
                'node_cli': lambda: cli(commands['node'], env),
                'c_cli': lambda: cli(commands['c'], env),
            }
            for function in functions.values():
                for _ in range(5):
                    function()
            values = {name: [] for name in functions}
            rng = random.Random(42)
            for _ in range(100):
                names = list(functions)
                rng.shuffle(names)
                for name in names:
                    values[name].append(functions[name]())
            print(json.dumps({
                'platform': platform.platform(),
                'node': subprocess.check_output(['node', '-v'], text=True).strip(),
                'method': 'isolated idle daemons; 5 warmups; 100 shuffled rounds; status only; no mic/API/clipboard',
                'results': {name: summary(a) for name, a in values.items()},
                'idle_rss_kib': {name: rss(process.pid) for name, process in zip(commands, daemons)},
                'rss_note': 'includes shared libraries; excludes recording worker and audio/paste processes; not PSS',
            }, indent=2))
        finally:
            for process in daemons:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
