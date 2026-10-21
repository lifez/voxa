"""Offline settings and real config/request-builder tests; no credentials or audio."""
import json
import os
from pathlib import Path
import pty
import select
import socket
import sys
import threading
import subprocess
import tempfile
import time
import unittest
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent


class VocabularyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="voxa-vocabulary-")
        self.addCleanup(self.tmp.cleanup)
        self.home = Path(self.tmp.name)
        self.config = self.home / "voxa/config.json"
        self.config.parent.mkdir()
        self.env = {**os.environ, "HOME": str(self.home),
                    "XDG_CONFIG_HOME": str(self.home), "ELEVENLABS_API_KEY": ""}

    def request(self, config):
        self.config.write_text(json.dumps(config, ensure_ascii=False))
        return subprocess.run([str(ROOT / "voxa-config-test")], env=self.env,
                              capture_output=True, text=True, timeout=3)

    def test_real_request_builder(self):
        baseline = self.request({})
        self.assertEqual(baseline.returncode, 0)
        self.assertEqual(baseline.stdout, self.request({"keyterms": []}).stdout)
        url = urlsplit(baseline.stdout.strip())
        self.assertEqual((url.scheme, url.netloc, url.path),
                         ("wss", "api.elevenlabs.io", "/v1/speech-to-text/realtime"))
        original = parse_qs(url.query)
        self.assertNotIn("keyterms", original)
        self.assertEqual(original["model_id"], ["scribe_v2_realtime"])
        words = ["สวัสดี", "Jitta Wealth", "C++", "A&B=é?", "😀" * 20]
        result = self.request({"keyterms": words})
        self.assertEqual(result.returncode, 0)
        query = parse_qs(urlsplit(result.stdout.strip()).query)
        self.assertEqual(query.pop("keyterms"), words)
        self.assertEqual(query, original)
        self.assertEqual(result.stderr, "")
        self.assertEqual(self.request({"keyterms": [str(i) for i in range(50)]}).returncode, 0)

    def test_config_limits_and_no_diagnostic_content(self):
        for words in [[""], [" \t\u3000"], ["x" * 21], ["😀" * 21],
                      [str(i) for i in range(51)], ["same", "same"], [2],
                      ["escape\x1b"], ["embedded\0nul"]]:
            with self.subTest(words=words):
                result = self.request({"keyterms": words})
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertEqual(result.stderr, "Invalid configuration\n")

    def editor(self, actions):
        master, slave = pty.openpty()
        proc = subprocess.Popen([str(ROOT / "voxa"), "settings"],
                                stdin=slave, stdout=slave, stderr=slave, env=self.env)
        os.close(slave)
        output = b""
        pending = b""
        def expect(expected):
            nonlocal pending, output
            expected = expected.encode()
            deadline = time.monotonic() + 4
            while expected not in pending and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    try:
                        chunk = os.read(master, 8192)
                    except OSError:
                        break
                    pending += chunk
                    output += chunk
            self.assertIn(expected, pending)
            pending = pending.split(expected, 1)[1]
        try:
            for prompt, answer in actions:
                expect(prompt)
                if answer is not None:
                    os.write(master, (answer + "\n").encode())
            self.assertEqual(proc.wait(timeout=3), 0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
        return output.decode()

    def test_add_remove_validation_and_restart(self):
        self.config.write_text('{"stopPunctuation":true}')
        output = self.editor([
            ("Choose", "6"), ("a) Add", "a"), ("Word or phrase", "  สวัสดี  "),
            ("a) Add", "a"), ("Word or phrase", "สวัสดี"),
            ("already in the list", None), ("a) Add", "a"), ("Word or phrase", "\u3000 "),
            ("nonempty", None), ("a) Add", "a"), ("Word or phrase", "x" * 21),
            ("at most 20", None), ("a) Add", "a"), ("Word or phrase", "Jitta Wealth"),
            ("a) Add", "r"), ("Number to remove", "3"), ("Choose a number", None),
            ("a) Add", "r"), ("Number to remove", "1"),
            ("a) Add", "b"), ("Choose", "0")])
        self.assertIn("not guaranteed replacements", output)
        self.assertIn("ElevenLabs with transcription", output)
        self.assertIn("1) สวัสดี", output)
        config = json.loads(self.config.read_text())
        self.assertEqual(config["keyterms"], ["Jitta Wealth"])
        self.assertTrue(config["stopPunctuation"])
        self.assertEqual(self.config.stat().st_mode & 0o777, 0o600)
        output = self.editor([("Choose", "6"), ("1) Jitta Wealth", None),
                              ("a) Add", "r"), ("Number to remove", "1"),
                              ("(none)", None), ("a) Add", "b"), ("Choose", "0")])
        self.assertEqual(json.loads(self.config.read_text())["keyterms"], [])

    def test_session_request_and_provider_rejection(self):
        from test_native import Fixture, until
        for http_error in (True, False):
            with self.subTest(http_error=http_error):
                f = Fixture(self, config={"keyterms": ["PRIVATE_TERM", "สวัสดี"]},
                            no_osd=True, error=not http_error, http_error=http_error)
                if sys.platform == "darwin":
                    # Task-owned synthetic mic socket, never the installed app or microphone.
                    mic_path = f.dir / "Library/Caches/voxa/mic.sock"
                    mic_path.parent.mkdir(parents=True)
                    server = socket.socket(socket.AF_UNIX)
                    server.bind(str(mic_path))
                    server.listen(1)
                    self.addCleanup(server.close)
                    def feed(server=server):
                        conn, _ = server.accept()
                        with conn:
                            try:
                                while True:
                                    conn.sendall(b"\0" * 640)
                                    time.sleep(.01)
                            except OSError:
                                pass
                    thread = threading.Thread(target=feed, daemon=True)
                    thread.start()
                f.command("start")
                # The running request owns the old config, despite later saved edits.
                (f.config / "config.json").write_text('{"keyterms":["next recording"]}')
                until(lambda: len(f.requests) == 1)
                until(lambda: f.command("status") == "idle")
                query = parse_qs(urlsplit(f.requests[0]).query)
                self.assertEqual(query["keyterms"], ["PRIVATE_TERM", "สวัสดี"])
                self.assertEqual(query["model_id"], ["scribe_v2_realtime"])
                self.assertFalse((f.dir / "text").exists())
                logs = f.logs()
                self.assertNotIn("PRIVATE_TERM", logs)
                self.assertNotIn("test-key", logs)
                self.assertNotIn("MUST_NOT_LOG", logs)
                self.assertIn("failed", logs)
                if sys.platform == "darwin":
                    thread.join(2)
                    self.assertFalse(thread.is_alive())

    def test_editor_count_limit(self):
        self.config.write_text(json.dumps({"keyterms": [str(i) for i in range(50)]}))
        self.editor([("Choose", "6"), ("a) Add", "a"), ("Word or phrase", "extra"),
                     ("at most 50", None), ("a) Add", "b"), ("Choose", "0")])
        self.assertEqual(len(json.loads(self.config.read_text())["keyterms"]), 50)


if __name__ == "__main__":
    unittest.main()
