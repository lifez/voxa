# macOS native backend (experimental)

The backend and CLI are C. Swift remains responsible for AppKit UI, global shortcuts, in-process AVAudioEngine capture and the pasteboard helper. Node.js is not used.

## Build requirements

- macOS 13+ and Xcode Command Line Tools (`swiftc`, C compiler, make).
- pkg-config, json-c, libcurl with WebSocket (`wss`) support. The system libcurl may not provide this; the installer prefers Homebrew curl/json-c when Homebrew is available.
- `brew install curl json-c pkg-config`, then `bash scripts/install-macos.sh`.

The installed binary uses these shared libraries: keep them installed. This is a local build, not a self-contained redistributable app bundle. Quit Voxa before reinstalling; back up the old bundle if you need rollback.

## Architecture

`Voxa.app/Contents/Resources/bin/voxa` runs the daemon and handles shortcut commands. It reads audio from `~/Library/Caches/voxa/mic.sock` and sends OSD states to `osd.sock`. The app owns microphone capture so macOS permission applies to the app rather than a subprocess. The control socket is `voxa.sock` in the same private directory.

`Resources/bin/voxa-paste` snapshots readable pasteboard types, pastes using System Events, and restores after a 250 ms grace period if the clipboard was not changed by another copy. This is best-effort; clipboard history can capture the temporary transcript.

## Manual validation required

This port was implemented on Linux. **It has not been compiled or live-tested on macOS.** Linux mocked tests do not validate these integrations.

On a Mac:

1. Build the app; inspect any C/Swift compiler or linker errors.
2. Open Voxa, set a Speech to Text API key and grant Microphone/Accessibility to Voxa.app. Allow Automation for System Events when prompted.
3. Use **Enable / Retry Shortcuts**, then the app's microphone test. Expect a nonzero peak; `voxa test-mic` uses the same socket and requires the app to be running.
4. In a disposable editor, try hold and toggle recording, Thai/English text, empty speech, and repeated sessions. Confirm recording/transcribing/done/error OSD states without focus changes.
5. Check clipboard restoration, copying during the paste grace period, and insertion in your actual target apps. Avoid terminal/chat submission fields because newlines may submit content.
6. Quit during recording/transcription, reopen, and verify the microphone indicator turns off and no stale daemon or socket prevents restart.
7. Verify login-item registration in System Settings → General → Login Items.

Logs are in `~/Library/Logs/voxa.log`. Look for `inserted N UTF-8 bytes` and session timing/errors, not transcript content. `voxa doctor` checks common prerequisites but is not a full app/TCC diagnostic.

Only the system default microphone is supported. Change it in macOS Sound settings. Command+Shift+R holds; Command+Shift+U toggles by default. Record custom shortcuts in the Voxa menu bar; terminal settings do not change macOS shortcuts.
