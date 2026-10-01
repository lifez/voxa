"""Opt-in real Ghostty/Hyprland input checks. Briefly changes focus; no clipboard/API/audio."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent
CAPTURE = r'''
import os, select, sys, termios, time, tty
from pathlib import Path
root = Path(sys.argv[1])
old = termios.tcgetattr(0)
try:
    tty.setraw(0)
    kitty = sys.argv[2] == '1'
    if kitty: os.write(1, b'\x1b[>1u')
    ending = b'::VOXA-DONE::' + (b'' if kitty else b'\r')
    root.with_suffix('.ready').touch()
    data = b''
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if select.select([0], [], [], .1)[0]:
            data += os.read(0, 65536)
            if data.endswith(ending): break
    root.with_suffix('.bytes').write_bytes(data)
except Exception:
    import traceback
    root.with_suffix('.error').write_text(traceback.format_exc())
    raise
finally:
    termios.tcsetattr(0, termios.TCSANOW, old)
'''


def hypr(query):
    return json.loads(subprocess.check_output(['hyprctl', query, '-j']))


def focus(address):
    subprocess.run(['hyprctl', 'dispatch', 'hl.dsp.focus({ window = ' + json.dumps('address:' + address) + ' })'],
                   check=True, stdout=subprocess.DEVNULL)


def main():
    if not sys.platform.startswith('linux'):
        raise SystemExit('Linux/Hyprland/Ghostty only')
    monitors = hypr('monitors')
    if not any(monitor['dpmsStatus'] for monitor in monitors) or any('LOCK' in monitor.get('solitaryBlockedBy', []) for monitor in monitors):
        raise SystemExit('Wake and unlock the desktop before running this opt-in test')
    original = hypr('activewindow')['address']
    workspace = hypr('activeworkspace')['id']
    temporary_workspace = max(w['id'] for w in hypr('workspaces')) + 100
    thai = 'รู้สึกว่าตัว transcribe ที่ Linux จะทำงานได้ไม่ค่อยถูกต้องเท่าไหร่ น้ำ กำ ทำ'
    cases = [
        ('reported Thai sentence', [thai], False),
        ('English and mixed Unicode', [''.join(chr(c) for c in range(32, 127)) + ' สวัสดี 中文 日本語 😀 café e\u0301'], False),
        ('more than 256 distinct characters', [''.join(chr(c) for c in range(0x400, 0x540))], False),
        ('long text', [(thai + ' ') * 25], False),
        ('successive keymaps', ['ทำงาน', thai, 'hello น้ำ 😀'], False),
        ('unshifted control keys', [''.join(chr(c) for c in range(32, 96)) + '\t\nทำงาน'], False),
        ('Kitty keyboard protocol', [''.join(chr(c) for c in range(32, 127)) + ' น้ำ 😀'], True),
    ]
    with tempfile.TemporaryDirectory(prefix='voxa-keyboard-') as directory:
        root = Path(directory)
        script = root / 'capture.py'
        script.write_text(CAPTURE)
        for index, (name, parts, kitty) in enumerate(cases):
            capture = root / f'capture-{index}'
            title = f'voxa-keyboard-check-{root.name}-{index}'
            subprocess.run(['hyprctl', 'dispatch', 'hl.dsp.focus({ workspace = ' + json.dumps(str(temporary_workspace)) + ' })'],
                           check=True, stdout=subprocess.DEVNULL)
            with capture.with_suffix('.log').open('wb') as log:
                child = subprocess.Popen(['ghostty', '--gtk-single-instance=false', '--title=' + title,
                                          '-e', sys.executable, str(script), str(capture), str(int(kitty))],
                                         stdout=subprocess.DEVNULL, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while not capture.with_suffix('.ready').exists():
                    if child.poll() is not None or time.monotonic() >= deadline:
                        error = capture.with_suffix('.error')
                        raise AssertionError(error.read_text() if error.exists() else capture.with_suffix('.log').read_text())
                    time.sleep(.02)
                window = next(window for window in hypr('clients') if window['title'] == title)
                deadline = time.monotonic() + 3
                while True:
                    focus(window['address'])
                    time.sleep(.3)
                    if hypr('activewindow').get('address') == window['address']:
                        break
                    if time.monotonic() >= deadline:
                        raise AssertionError('Capture cannot keep focus; refusing to type elsewhere')
                ending = '::VOXA-DONE::' + ('' if kitty else '\n')
                for part in [*parts[:-1], parts[-1] + ending]:
                    assert hypr('activewindow').get('address') == window['address'], 'Focus changed; refusing to type'
                    assert not any('LOCK' in monitor.get('solitaryBlockedBy', []) for monitor in hypr('monitors')), 'Desktop locked; refusing to type'
                    subprocess.run([str(ROOT / 'voxa'), 'test-paste', part], check=True, timeout=20)
                child.wait(timeout=12)
                actual = capture.with_suffix('.bytes').read_bytes()
                expected = (''.join(parts).replace('\n', '\r') + ending.replace('\n', '\r')).encode()
                assert actual == expected, f'{name}: expected {len(expected)} bytes, got {len(actual)}: {actual[:100]!r}'
                print(f'PASS {name}: {len(actual)} bytes matched exactly')
            finally:
                try:
                    subprocess.run(['hyprctl', 'dispatch', 'hl.dsp.focus({ workspace = ' + json.dumps(str(workspace)) + ' })'],
                                   check=True, stdout=subprocess.DEVNULL)
                    focus(original)
                finally:
                    if child.poll() is None:
                        child.terminate()
                        try:
                            child.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait()


if __name__ == '__main__':
    main()
