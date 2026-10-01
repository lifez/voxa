"""Offline regression tests. Python standard library only; no real audio/API/desktop."""
import base64
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parent
BINARY = ROOT / "voxa-test"
PRODUCTION = ROOT / "voxa"


def until(fn, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if fn():
            return
        time.sleep(.02)
    raise AssertionError("condition timed out")


def exact(sock, count):
    data = b""
    while len(data) < count:
        more = sock.recv(count - len(data))
        if not more:
            raise EOFError
        data += more
    return data


def send_frame(sock, data, opcode=1, final=True):
    if isinstance(data, str):
        data = data.encode()
    n = len(data)
    header = bytes([(128 if final else 0) | opcode])
    header += bytes([n]) if n < 126 else b"\x7e" + struct.pack("!H", n)
    sock.sendall(header + data)


def read_frame(sock):
    a, b = exact(sock, 2)
    n = b & 127
    if n == 126:
        n = struct.unpack("!H", exact(sock, 2))[0]
    elif n == 127:
        n = struct.unpack("!Q", exact(sock, 8))[0]
    if n > 2**20:
        raise ValueError("oversized frame")
    mask = exact(sock, 4) if b & 128 else b""
    data = exact(sock, n)
    if mask:
        data = bytes(x ^ mask[i % 4] for i, x in enumerate(data))
    return a & 15, data


FAKE = r'''
import json, os, signal, sys, time
from pathlib import Path
root = Path(os.environ['TEST_ROOT'])
role = Path(sys.argv[0]).name
if role == 'pw-record':
    if os.environ.get('FAIL_MIC'): sys.exit(1)
    (root / 'mic').write_text(str(os.getpid()))
    running = True
    def stop(*args):
        global running
        running = False
    signal.signal(signal.SIGTERM, stop)
    try:
        while running:
            os.write(1, b'\1' * 6400)
            time.sleep(.02)
        os.write(1, b'\1' * 6400)
    except BrokenPipeError:
        pass
elif role == 'wtype':
    if os.environ.get('FAIL_TYPE'): sys.exit(4)
    (root / 'text').write_bytes(sys.stdin.buffer.read())
    (root / 'args').write_text(json.dumps(sys.argv[1:]))
elif role == 'wl-copy':
    (root / 'clipboard').touch()
    sys.exit(1)
elif role == 'omarchy-shell':
    with (root / 'osd').open('a') as f: f.write(json.dumps(sys.argv[1:]) + '\n')
    if os.environ.get('HANG_OSD') and 'summon' in sys.argv:
        time.sleep(30)
    if os.environ.get('FAIL_OSD'): sys.exit(1)
'''


class Fixture:
    def __init__(self, case, config=None, text="สวัสดี hello.", delay=0,
                 error=False, no_response=False, no_osd=False, extra_env=None):
        self.tmp = tempfile.TemporaryDirectory(prefix="voxa-")
        self.dir = Path(self.tmp.name)
        self.config = self.dir / "voxa"
        self.config.mkdir()
        (self.config / "config.json").write_text(json.dumps(config or {}))
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        for name in ["pw-record", "wtype", "wl-copy", "omarchy-shell"]:
            file = bin_dir / name
            file.write_text(f"#!{sys.executable}\n" + FAKE)
            file.chmod(0o700)
        self.env = {**os.environ, "HOME": str(self.dir), "XDG_CONFIG_HOME": str(self.dir),
                    "XDG_RUNTIME_DIR": str(self.dir), "TEST_ROOT": str(self.dir),
                    "ELEVENLABS_API_KEY": "test-key-1,test-key-2", "PATH": f"{bin_dir}:{os.environ['PATH']}"}
        self.env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
        self.env.update(extra_env or {})
        self.chunks, self.keys, self.errors = [], [], []
        self.clients = set()
        fixture = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                sock = self.request
                fixture.clients.add(sock)
                sock.settimeout(12)
                try:
                    request = b""
                    while not request.endswith(b"\r\n\r\n"):
                        request += exact(sock, 1)
                        if len(request) > 16000:
                            raise ValueError("large HTTP headers")
                    headers = dict(line.split(": ", 1) for line in request.decode().split("\r\n")[1:] if ": " in line)
                    headers = {k.lower(): v for k, v in headers.items()}
                    fixture.keys.append(headers["xi-api-key"])
                    accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
                    sock.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
                    if error:
                        send_frame(sock, '{"message_type":"authentication_error","message":"test-key-1 MUST_NOT_LOG"}')
                        return
                    time.sleep(delay)
                    send_frame(sock, '{"message_type":"session_started"}')
                    while True:
                        opcode, raw = read_frame(sock)
                        if opcode == 8:
                            return
                        if opcode != 1:
                            continue
                        event = json.loads(raw)
                        fixture.chunks.append(event)
                        if event["commit"] and not no_response:
                            response = json.dumps({"message_type": "committed_transcript", "text": text}, ensure_ascii=False).encode()
                            send_frame(sock, response[:10], final=False)
                            send_frame(sock, b"test", opcode=9)
                            send_frame(sock, response[10:], opcode=0)
                except (EOFError, ConnectionError, OSError):
                    pass
                except Exception as exc:
                    fixture.errors.append(exc)
                finally:
                    fixture.clients.discard(sock)

        class Server(socketserver.ThreadingTCPServer):
            daemon_threads = True

        self.server = Server(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.socket = self.dir / "daemon.sock"
        self.args = [str(BINARY), "--socket", str(self.socket), "--test-endpoint",
                     f"ws://127.0.0.1:{self.server.server_address[1]}/", *(["--no-osd"] if no_osd else []), "daemon"]
        self.logfile = (self.dir / "logs").open("w+")
        self.child = subprocess.Popen(self.args, env=self.env, stdout=subprocess.DEVNULL, stderr=self.logfile)
        case.addCleanup(self.cleanup)
        until(lambda: self.socket.exists() or self.child.poll() is not None)
        if self.child.poll() is not None:
            raise AssertionError(self.logs())

    def command(self, command):
        return subprocess.check_output([str(BINARY), "--socket", str(self.socket), command], env=self.env, timeout=3, text=True).strip()

    def logs(self):
        return (self.dir / "logs").read_text()

    def states(self):
        file = self.dir / "osd"
        if not file.exists():
            return []
        result = []
        for line in file.read_text().splitlines():
            args = json.loads(line)
            assert args[:2] == ["-q", "shell"] and args[3] == "voxa.osd"
            result.append("hide" if args[2] == "hide" else json.loads(args[4])["state"])
        return result

    def dictation(self):
        self.command("start")
        time.sleep(.2)
        self.command("stop")
        until(lambda: self.command("status") == "idle", 10)

    def stop(self):
        if self.child.poll() is None:
            self.child.terminate()
        try:
            self.child.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.wait()
            raise AssertionError("daemon failed to terminate")

    def cleanup(self):
        self.stop()
        for sock in list(self.clients):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)
        logs = self.logs()
        self.logfile.close()
        self.tmp.cleanup()
        assert not self.errors, self.errors
        assert not any(x in logs for x in ["AddressSanitizer", "LeakSanitizer", "runtime error:"]), logs


@unittest.skipUnless(sys.platform.startswith("linux"), "PipeWire/Omarchy integration mocks are Linux-only")
class NativeTests(unittest.TestCase):
    def test_streaming_rotation_and_delayed_ready(self):
        f = Fixture(self, delay=.3)
        self.assertEqual(f.socket.stat().st_mode & 0o777, 0o600)
        for _ in range(2):
            self.assertEqual(f.command("toggle"), "recording")
            until(lambda: (f.dir / "mic").exists())
            self.assertEqual(f.command("stop"), "committing")
            self.assertEqual(f.command("toggle"), "committing")
            until(lambda: f.command("status") == "idle")
            self.assertEqual((f.dir / "text").read_text(), "สวัสดี hello")
            self.assertEqual(json.loads((f.dir / "args").read_text()), ["-"])
            self.assertFalse((f.dir / "clipboard").exists())
            (f.dir / "mic").unlink()
        until(lambda: len(f.states()) == 6)
        self.assertEqual(f.states(), ["recording", "committing", "done"] * 2)
        self.assertEqual(f.keys, ["test-key-1", "test-key-2"])
        self.assertEqual(sum(c["commit"] for c in f.chunks), 2)
        for chunk in f.chunks:
            self.assertEqual(chunk["sample_rate"], 16000)
            pcm = base64.b64decode(chunk["audio_base_64"])
            self.assertTrue(pcm and len(pcm) % 2 == 0 and set(pcm) == {1})
            if not chunk["commit"]:
                self.assertEqual(len(pcm), 6400)
        self.assertIn("stop_to_done_ms=", f.logs())
        self.assertNotRegex(f.logs(), "test-key|สวัสดี|hello")

    def test_cancel_recording(self):
        f = Fixture(self)
        self.assertEqual(f.command("cancel"), "idle")
        self.assertEqual(f.command("toggle"), "recording")
        until(lambda: len(f.chunks) > 0)
        mic = int((f.dir / "mic").read_text())
        f.command("cancel")
        until(lambda: f.command("status") == "idle")
        until(lambda: f.states()[-1:] == ["hide"])
        self.assertFalse(any(chunk["commit"] for chunk in f.chunks))
        self.assertFalse((f.dir / "text").exists())
        self.assertEqual(f.states(), ["recording", "hide"])
        with self.assertRaises(ProcessLookupError):
            os.kill(mic, 0)
        self.assertEqual(f.command("cancel"), "idle")
        f.dictation()
        self.assertEqual((f.dir / "text").read_text(), "สวัสดี hello")

    def test_cancel_during_commit_is_ignored(self):
        f = Fixture(self, no_response=True)
        f.command("toggle")
        until(lambda: len(f.chunks) > 0)
        self.assertEqual(f.command("toggle"), "committing")
        self.assertEqual(f.command("cancel"), "committing")
        until(lambda: any(chunk["commit"] for chunk in f.chunks))
        self.assertFalse((f.dir / "text").exists())

    def test_thai_transcript_preserved_before_typing(self):
        # Test builds capture the insertion boundary; desktop tests check actual keys.
        text = "รู้สึกว่าตัว transcribe ที่ Linux จะทำงานได้ไม่ค่อยถูกต้องเท่าไหร่ น้ำ กำ ทำ"
        f = Fixture(self, text=text)
        f.dictation()
        self.assertEqual((f.dir / "text").read_text(), text)
        self.assertEqual(json.loads((f.dir / "args").read_text()), ["-"])
        self.assertFalse((f.dir / "clipboard").exists())

    def test_empty_transcript(self):
        f = Fixture(self, text=" \t\u00a0\u3000\ufeff")
        f.dictation()
        until(lambda: f.states()[-1:] == ["hide"])
        self.assertEqual(f.states(), ["recording", "committing", "hide"])
        self.assertFalse((f.dir / "text").exists())
        self.assertFalse((f.dir / "clipboard").exists())

    def test_config_reload(self):
        f = Fixture(self, config={"stopPunctuation": True}, text="Hello。")
        for expected in ["Hello。", "Hello"]:
            f.dictation()
            self.assertEqual((f.dir / "text").read_text(), expected)
            (f.config / "config.json").write_text('{"stopPunctuation":false}')

    def test_api_error_redacted(self):
        f = Fixture(self, error=True)
        f.command("start")
        until(lambda: f.command("status") == "idle")
        until(lambda: f.states()[-1:] == ["error"])
        self.assertNotRegex(f.logs(), "test-key|MUST_NOT_LOG")
        self.assertFalse((f.dir / "text").exists())

    def test_invalid_config(self):
        f = Fixture(self)
        for config in ['{', '{} garbage', '{"language":"invalid"}', '{"unknown":true}', '{"debug":1}', '{"audioDevice":null}', '{"keyterms":[2]}', '{"pasteCommand":"shell"}']:
            (f.config / "config.json").write_text(config)
            self.assertEqual(f.command("start"), "idle")
        self.assertFalse(f.keys)
        self.assertFalse((f.dir / "mic").exists())

    def test_clients_and_duplicate_daemon(self):
        f = Fixture(self)
        other = subprocess.run(f.args, env=f.env, capture_output=True, timeout=2)
        self.assertEqual(other.returncode, 1)
        with socket.socket(socket.AF_UNIX) as slow:
            slow.connect(str(f.socket))
            slow.sendall(b"sta")
            self.assertEqual(f.command("status"), "idle")
        for parts, expected in [([b"status\0junk\n"], b"unknown command\n"), ([b"sta", b"tus\n"], b"idle\n")]:
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(2)
                client.connect(str(f.socket))
                for part in parts:
                    client.sendall(part)
                    time.sleep(.02)
                client.shutdown(socket.SHUT_WR)
                self.assertEqual(client.recv(128), expected)

    def test_shutdown_reaps_mic(self):
        f = Fixture(self)
        f.command("start")
        until(lambda: (f.dir / "mic").exists())
        mic = int((f.dir / "mic").read_text())
        f.stop()
        self.assertFalse(f.socket.exists())
        self.assertEqual(f.states()[-1], "hide")
        def gone():
            try:
                os.kill(mic, 0)
                return False
            except ProcessLookupError:
                return True
        until(gone)

    def test_insertion_failure(self):
        f = Fixture(self, extra_env={"FAIL_TYPE": "1"})
        f.dictation()
        self.assertIn("text insertion failed", f.logs())
        self.assertFalse((f.dir / "clipboard").exists())

    def test_mic_failure(self):
        f = Fixture(self, extra_env={"FAIL_MIC": "1"})
        f.command("start")
        until(lambda: f.command("status") == "idle")
        self.assertIn("microphone exited unexpectedly", f.logs())

    def test_commit_timeout(self):
        f = Fixture(self, no_response=True)
        f.dictation()
        self.assertIn("Scribe commit timed out", f.logs())
        self.assertFalse((f.dir / "text").exists())

    def test_hung_osd(self):
        f = Fixture(self, extra_env={"HANG_OSD": "1"})
        start = time.monotonic()
        f.dictation()
        self.assertLess(time.monotonic() - start, 1.5)
        until(lambda: "committing" in f.states(), 3)
        f.stop()
        self.assertEqual(f.states()[-1], "hide")

    def test_failed_osd(self):
        f = Fixture(self, extra_env={"FAIL_OSD": "1"})
        f.dictation()
        self.assertTrue((f.dir / "text").exists())

    def test_no_osd(self):
        f = Fixture(self, no_osd=True)
        f.dictation()
        f.stop()
        self.assertEqual(f.states(), [])

    def test_native_keyboard_validation_and_timeout(self):
        with tempfile.TemporaryDirectory(prefix="voxa-wayland-") as directory:
            display = Path(directory) / "wayland-test"
            env = {**os.environ, "WAYLAND_DISPLAY": str(display), "XDG_RUNTIME_DIR": directory}
            # A connected compositor that never answers must not hang insertion.
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(str(display))
                server.listen(1)
                start = time.monotonic()
                result = subprocess.run([str(PRODUCTION), "test-paste", "hi"], env=env,
                                        capture_output=True, timeout=4)
                self.assertEqual(result.returncode, 1)
                self.assertLess(time.monotonic() - start, 3.5)
            for text in [b"\xff", b"\xe0\xb8", b"\x1b", "".join(chr(0x400 + i) for i in range(385)).encode()]:
                result = subprocess.run([os.fsencode(PRODUCTION), b"test-paste", text], env=env,
                                        capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 1)

    def test_cli_tests_use_native(self):
        f = Fixture(self)
        def run(*args):
            binary = BINARY if args[0] == "test-paste" else PRODUCTION
            return subprocess.run([str(binary), *args], env=f.env, capture_output=True, text=True, timeout=5)
        result = run("test-mic")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, r"peak [1-9]\d*")
        self.assertFalse(f.keys)
        text = '-M ctrl สวัสดี\nhello'
        self.assertEqual(run("test-paste", text).returncode, 0)
        self.assertEqual((f.dir / "text").read_text(), text)
        self.assertEqual(json.loads((f.dir / "args").read_text()), ["-"])
        self.assertFalse((f.dir / "clipboard").exists())
        self.assertEqual(run("test-paste").returncode, 2)
        self.assertEqual(run("settings", "--terminal").returncode, 1)
        self.assertEqual(run("settings", "--unknown").returncode, 2)

    def test_settings_secret_and_reload(self):
        f = Fixture(self, extra_env={"ELEVENLABS_API_KEY": ""})
        (f.config / "env").write_text("ELEVENLABS_API_KEY=old-key\n")
        master, slave = pty.openpty()
        proc = subprocess.Popen([str(PRODUCTION), "settings"], stdin=slave, stdout=slave, stderr=slave, env=f.env)
        os.close(slave)
        received = b""
        def expect(text):
            nonlocal received
            deadline = time.monotonic() + 4
            pending = b""
            while text not in pending and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    chunk = os.read(master, 8192)
                    pending += chunk
                    received += chunk
            self.assertIn(text, pending)
        try:
            expect(b"Choose")
            os.write(master, b"1\n")
            expect(b"blank keeps current")
            # Wait for terminal echo to be disabled before sending the secret.
            import termios
            until(lambda: not termios.tcgetattr(master)[3] & termios.ECHO)
            os.write(master, b"new-key-1, new-key-2\n")
            expect(b"Choose")
            os.write(master, b"4\n")
            expect(b"Keep final full stop?")
            os.write(master, b"yes\n")
            expect(b"Choose")
            os.write(master, b"0\n")
            self.assertEqual(proc.wait(timeout=3), 0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
        self.assertNotIn(b"new-key", received)
        self.assertEqual((f.config / "env").stat().st_mode & 0o777, 0o600)
        self.assertEqual((f.config / "env").read_text(), "ELEVENLABS_API_KEY=new-key-1,new-key-2\n")
        self.assertTrue(json.loads((f.config / "config.json").read_text())["stopPunctuation"])
        f.dictation()
        self.assertEqual(f.keys, ["new-key-1"])
        self.assertEqual((f.dir / "text").read_text(), "สวัสดี hello.")

    def test_production_endpoint_and_file_safety(self):
        result = subprocess.run([str(PRODUCTION), "--test-endpoint", "ws://127.0.0.1:1/", "daemon"], capture_output=True)
        self.assertEqual(result.returncode, 2)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "socket"
            path.write_text("keep")
            result = subprocess.run([str(BINARY), "--socket", str(path), "daemon"], capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(path.read_text(), "keep")
            env = {**os.environ, "XDG_RUNTIME_DIR": tmp}
            result = subprocess.run([str(PRODUCTION), "status"], env=env, capture_output=True, timeout=3)
            self.assertEqual(result.returncode, 1)


@unittest.skipUnless(sys.platform.startswith("linux"), "Omarchy shortcuts")
class BindingTests(unittest.TestCase):
    def test_cancel_bindings_and_conflicts(self):
        with tempfile.TemporaryDirectory(prefix="voxa-bindings-") as tmp:
            root = Path(tmp)
            path = root / "hypr/bindings.lua"
            path.parent.mkdir()
            old = '-- Keep user settings\no.bind("SUPER + X", "Other", "other")\n'
            managed = (old + "-- BEGIN VOXA (managed by voxa settings)\n"
                       'o.bind("F11", "Toggle Voxa", "~/.local/bin/voxa toggle")\n'
                       "-- END VOXA\n")
            for original, cancel, expected in [
                (old, "", "ESCAPE"),
                (managed, "Escape", "ESCAPE"),
                (old, "F12", "F12"),
                (managed, "CTRL + F12", "CTRL + F12"),
                (old, "F10", None),
                (old, "F11", None),
                (old, "CTRL++F12", None),
                (old + 'o.bind("F12", "Other", "other")\n', "F12", None),
                (old + 'o.bind("ESCAPE", "Other", "other")\n', "", None),
                (old + 'o.bind("Escape", "Other", "other")\n', "", None),
                (old + 'o.bind("F9", "Cancel", "~/.local/bin/voxa cancel")\n', "", None),
            ]:
                with self.subTest(cancel=cancel, original=original):
                    path.write_text(original)
                    master, slave = pty.openpty()
                    env = {**os.environ, "XDG_CONFIG_HOME": tmp}
                    env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
                    proc = subprocess.Popen([str(PRODUCTION), "settings"],
                                            stdin=slave, stdout=slave, stderr=slave, env=env)
                    os.close(slave)
                    try:
                        os.write(master, f"5\nF10\nF11\n{cancel}\n0\n".encode())
                        self.assertEqual(proc.wait(timeout=3), 0)
                    finally:
                        if proc.poll() is None:
                            proc.kill()
                            proc.wait()
                        os.close(master)
                    if expected is None:
                        self.assertEqual(path.read_text(), original)
                    else:
                        saved = path.read_text()
                        self.assertIn(old, saved)
                        options = ", { non_consuming = true }" if expected == "ESCAPE" else ""
                        self.assertIn(f'o.bind("{expected}", "Cancel Voxa", "~/.local/bin/voxa cancel"{options})', saved)
                        self.assertEqual(saved.count("voxa cancel"), 1)
                        self.assertEqual(Path(str(path) + ".voxa.bak").read_text(), original)


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux installer")
class InstallerTests(unittest.TestCase):
    def fixture(self, fail=False):
        temp = tempfile.TemporaryDirectory(prefix="voxa-install-")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        repo = root / "repo"
        for directory in ["scripts", "systemd", "native"]:
            (repo / directory).mkdir(parents=True)
        shutil.copy(ROOT.parent / "scripts/install.sh", repo / "scripts/install.sh")
        shutil.copy(ROOT.parent / "systemd/voxa.service", repo / "systemd/voxa.service")
        shutil.copy(ROOT.parent / "config.example.json", repo / "config.example.json")
        shutil.copy(PRODUCTION, repo / "native/voxa")
        home = root / "home"
        app = home / ".local/share/voxa"
        app.mkdir(parents=True)
        (app / "dist").mkdir()
        (app / "dist/index.js").write_text("legacy code")
        (app / "node_modules").mkdir()
        launcher = home / ".local/bin/voxa"
        launcher.parent.mkdir(parents=True)
        launcher.symlink_to(app / "dist/index.js")
        config = home / "custom-config"
        units = config / "systemd/user"
        (units / "voxa.service.d").mkdir(parents=True)
        (units / "voxa.service").write_text("old service")
        (units / "voxa.service.d/80-config.conf").write_text("old config override")
        (units / "voxa.service.d/90-native.conf").write_text("old native override")
        (config / "voxa").mkdir()
        (config / "voxa/env").write_text("ELEVENLABS_API_KEY=keep-secret\n")
        (config / "voxa/config.json").write_text('{"language":"en"}')
        mock = root / "bin"
        mock.mkdir()
        scripts = {
            "make": "exit 0", "cc": "exit 0", "pkg-config": "exit 0", "wayland-scanner": "exit 0",
            "pw-record": "exit 99", "wtype": "exit 99",
            "omarchy-shell": "exit 1",
            "node": "echo Node must not run >&2; exit 99",
            "npm": "echo npm must not run >&2; exit 99",
            "systemctl": '''
if [ "$2" = is-active ]; then exit 1; fi
if [ "$2" = daemon-reload ] && [ -n "${FAIL_INSTALL:-}" ] && [ ! -e "$HOME/failed-once" ]; then
  touch "$HOME/failed-once"
  exit 1
fi
exit 0
''',
        }
        for name, script in scripts.items():
            file = mock / name
            file.write_text("#!/bin/sh\n" + script + "\n")
            file.chmod(0o700)
        env = {**os.environ, "HOME": str(home), "XDG_CONFIG_HOME": str(config),
               "XDG_RUNTIME_DIR": str(root), "PATH": f"{mock}:{os.environ['PATH']}"}
        if fail:
            env["FAIL_INSTALL"] = "1"
        result = subprocess.run(["bash", str(repo / "scripts/install.sh")], env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertNotIn("must not run", result.stderr)
        self.assertEqual((config / "voxa/env").read_text(), "ELEVENLABS_API_KEY=keep-secret\n")
        self.assertEqual((config / "voxa/config.json").read_text(), '{"language":"en"}')
        return result, home, app, launcher, units

    def test_upgrade_without_node(self):
        result, home, app, launcher, units = self.fixture()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(launcher.resolve(), app / "voxa")
        self.assertFalse((app / "dist").exists())
        self.assertFalse((app / "node_modules").exists())
        self.assertFalse((units / "voxa.service.d/90-native.conf").exists())
        self.assertNotIn("node", (units / "voxa.service").read_text())
        self.assertIn("custom-config", (units / "voxa.service.d/80-config.conf").read_text())
        backups = list((home / ".local/share/voxa-backups").glob("install-*"))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / "dist/index.js").read_text(), "legacy code")

    def test_failed_install_restores_overrides_and_launcher(self):
        result, _, app, launcher, units = self.fixture(fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(launcher.resolve(), app / "dist/index.js")
        self.assertFalse((app / "voxa").exists())
        self.assertEqual((units / "voxa.service").read_text(), "old service")
        self.assertEqual((units / "voxa.service.d/80-config.conf").read_text(), "old config override")
        self.assertEqual((units / "voxa.service.d/90-native.conf").read_text(), "old native override")
        self.assertTrue((app / "node_modules").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
