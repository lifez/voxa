#!/usr/bin/env python3
"""Exercise the native vocabulary dialog against an isolated config on macOS."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def apple(script):
    return subprocess.check_output(["osascript", "-e", script], text=True,
                                   stderr=subprocess.DEVNULL).strip()


with tempfile.TemporaryDirectory(prefix="voxa-vocabulary-ui-") as work:
    binary = Path(work) / "VocabularyUITest"
    subprocess.run(["swiftc", "-D", "VOCABULARY_UI_TEST", str(ROOT / "mac/Voxa.swift"),
                    "-o", str(binary)], check=True)
    config = Path(work) / "voxa/config.json"
    config.parent.mkdir()
    config.write_text(json.dumps({"language": "th", "keyterms": ["existing"]}))

    def dialog(action):
        process = subprocess.Popen([str(binary)], env={**os.environ, "XDG_CONFIG_HOME": work})
        target = f'first application process whose unix id is {process.pid}'
        try:
            for _ in range(100):
                try:
                    apple(f'tell application "System Events" to get window 1 of ({target})')
                    break
                except subprocess.CalledProcessError:
                    if process.poll() is not None:
                        raise RuntimeError("Dialog exited before opening")
                    time.sleep(0.1)
            else:
                raise RuntimeError("Dialog did not open")
            apple(f'tell application "System Events"\ntell ({target})\n{action}\nend tell\nend tell')
            process.wait(timeout=10)
            assert process.returncode == 0
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)

    dialog('set value of text area 1 of scroll area 1 of window 1 to "Jitta' + '\nสวัสดี"\nclick button "Save" of window 1')
    assert json.loads(config.read_text()) == {"language": "th", "keyterms": ["Jitta", "สวัสดี"]}
    before = config.read_bytes()
    dialog('set value of text area 1 of scroll area 1 of window 1 to "discard me"\nclick button "Cancel" of window 1')
    assert config.read_bytes() == before
    dialog('set value of text area 1 of scroll area 1 of window 1 to "duplicate\nduplicate"\n'
           'click button "Save" of window 1\ndelay 0.2\n'
           'if not (exists static text 1 of window 1 whose value contains "Could not save") then error "Missing validation error"\n'
           'if value of text area 1 of scroll area 1 of window 1 is not "duplicate\nduplicate" then error "Lost unsaved text"\n'
           'set value of text area 1 of scroll area 1 of window 1 to "corrected"\n'
           'click button "Save" of window 1')
    assert json.loads(config.read_text())["keyterms"] == ["corrected"]
    dialog('set value of text area 1 of scroll area 1 of window 1 to ""\nclick button "Save" of window 1')
    assert json.loads(config.read_text())["keyterms"] == []
    print("Native vocabulary UI: save, Thai text, cancel, validation retry, and remove-all passed")
