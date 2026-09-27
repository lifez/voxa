#!/usr/bin/env python3
"""Experimental Python control client; never installed automatically."""
import os
import socket
import sys


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('status', 'start', 'stop', 'toggle'):
        print('Usage: control.py status|start|stop|toggle', file=sys.stderr)
        return 2
    directory = (os.path.expanduser('~/Library/Caches/voxa') if sys.platform == 'darwin'
                 else os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))
    try:
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(1.5)
            sock.connect(os.path.join(directory, 'voxa.sock'))
            sock.sendall((sys.argv[1] + '\n').encode())
            sock.shutdown(socket.SHUT_WR)
            received = False
            while data := sock.recv(256):
                received = True
                sys.stdout.buffer.write(data)
            return 0 if received else 1
    except OSError as error:
        print(f'voxa: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
