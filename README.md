# Voxa

Voice dictation for **Omarchy and macOS**, powered by a native **C daemon and CLI**. Start recording with a shortcut, speak, then stop to insert text into the focused app.

Requires an internet connection and an **ElevenLabs API key with Speech to Text access**. Audio is streamed to ElevenLabs; transcription is not offline. No Node.js or npm is required.

| Platform | Hold to record | Toggle recording | Text insertion |
|---|---|---|---|
| Omarchy | F10 | F11 | Direct typing; clipboard untouched |
| macOS (experimental) | Command+Shift+R | Command+Shift+U | Paste, then best-effort clipboard restoration |

## Quick start: Omarchy

Requires Hyprland, PipeWire, a C11 compiler, make, pkg-config, json-c and **libcurl with WebSocket (`wss`) support**.

```sh
# Arch: install missing packages
sudo pacman -S base-devel curl json-c pipewire wtype

# Run from this repository
bash scripts/install.sh
voxa setup
voxa doctor
```

`voxa setup` runs in a terminal: enter your API key, choose language/microphone/punctuation and shortcuts, then enable the user service. Keys are hidden during input and saved with mode `600`. Existing config and keys are preserved. Shortcut changes back up `bindings.lua` and reject conflicting custom bindings.

To try it, focus a **disposable text editor**, hold F10, speak, then release. Keep the editor focused until text appears. F11 starts/stops the same recording; use one mode at a time. **Newlines may act as Enter**, including submitting a chat or terminal command.

The installer installs C directly, optionally enables the Omarchy OSD plugin, and upgrades an existing installation only while idle. [Migration and backups](docs/migration.md).

## Quick start: macOS (experimental)

Requires macOS 13+, Xcode Command Line Tools, json-c and a WebSocket-enabled libcurl. With Homebrew:

```sh
brew install curl json-c pkg-config
bash scripts/install-macos.sh
```

The installer builds `~/Applications/Voxa.app`. Its Swift shell captures audio in the app process, owns shortcuts and displays the OSD; the C daemon handles transcription and the CLI. No JavaScript runtime is bundled or launched.

1. In the Voxa menu bar, choose **Set ElevenLabs API Key…**.
2. Grant **Microphone** and **Accessibility** to Voxa.app in System Settings → Privacy & Security. Allow Automation for System Events when prompted for paste.
3. Choose **Enable / Retry Shortcuts**, then **Test microphone (speak for 2 seconds)**. Expect `mic OK (peak N)` with `N > 0`.
4. Focus a disposable editor and try Command+Shift+R.

Only the system default microphone is supported. Use **Record Hold Shortcut…** or **Record Toggle Shortcut…** in the Voxa menu bar to press and save a shortcut (F-key or a key with modifiers). Changes apply immediately and persist across restarts; `voxa settings` provides other terminal settings. The app registers itself as a login item. Logs: `~/Library/Logs/voxa.log`.

**The new C/macOS path has not been built or live-tested on this Linux development machine.** See [macOS notes](docs/macos.md) before relying on it.

## Commands

```sh
voxa settings                    # terminal settings menu
voxa status                      # idle / recording / committing
voxa start                       # opens mic and connects to ElevenLabs
voxa stop                        # finishes transcription and inserts text
voxa toggle                      # start or stop
voxa doctor                      # local dependency/config/service checks
voxa test-mic                    # records 2 seconds; prints byte count and peak
voxa test-scribe                 # records 3 seconds; prints transcript, no insertion
voxa test-paste 'hello สวัสดี'     # inserts into focused app — use a disposable field
voxa test-osd                    # display-only preview; no mic/API/insertion
```

`test-scribe` sends audio to ElevenLabs and uses quota. `test-paste` does not wait for you to focus another app. `test-osd` shares the real overlay; do not run during dictation. Empty transcripts are not inserted. Toggles during transcription are ignored.

## Configuration

Use `voxa settings` or edit `${XDG_CONFIG_HOME:-~/.config}/voxa/config.json` ([example](config.example.json)). Changes apply on the next recording.

| Setting | Default | Meaning |
|---|---|---|
| `language` | `"th"` | Primary language; `null` for automatic detection |
| `secondaryLanguages` | `["en"]` | Additional languages; use `[]` with auto detection |
| `audioDevice` | `"default"` | PipeWire source name/ID from `wpctl status`; macOS supports only `default` |
| `keyterms` | `[]` | Technical terms to bias transcription |
| `stopPunctuation` | `false` | Remove a final `.` or `。`; `true` keeps it |
| `debug` | `false` | Accepted for compatibility; C never logs transcript text |
| `pasteCommand` | `"wtype"` | Compatibility field; must remain `wtype`. macOS uses its native paste helper |

**API keys:** enter 1–5 comma-separated keys in settings. Voxa rotates sequentially, one key per recording. This does not increase quota for keys sharing an account. Keys are read from `voxa/env` each recording; a nonempty `ELEVENLABS_API_KEY` environment variable overrides that file. Never put credentials in `config.json`, shell history or the repository.

## Privacy and limits

- Microphone capture only while recording; no saved audio or transcript files.
- Logs contain state/errors/timings, not keys or transcript text. `test-scribe` explicitly prints its transcript.
- ElevenLabs data retention depends on your account policy.
- Omarchy uses `wtype` without reading/writing the clipboard.
- macOS snapshots readable clipboard types, pastes, then restores after 250 ms unless another copy changed it. Restoration is best-effort; slow apps may miss the paste and clipboard history tools may retain the temporary transcript.
- Recordings are limited to **60 seconds**; exceeding the limit cancels the session. Insertion has a two-second timeout. Focus is not restored automatically.
- Offline integration tests use mocks. They do not prove real ElevenLabs transcription quality or compatibility with every target app.

## Troubleshooting

**Omarchy:**

- Service/logs: `systemctl --user status voxa`, `journalctl --user -u voxa -f`.
- Microphone: `voxa test-mic`, `wpctl status`, `systemctl --user status pipewire wireplumber`.
- Insertion: check `WAYLAND_DISPLAY` and `command -v wtype`. The service needs the graphical session environment; inspect `systemctl --user show-environment`.
- Shortcuts: `hyprctl binds -j`; change them with `voxa settings`. Standalone modifier keys are not supported as hold shortcuts.
- OSD: check `omarchy-shell shell listPlugins` for `voxa.osd`. Display failure does not stop dictation.
- `libcurl requires WSS support`: install a libcurl build with WebSocket support and rebuild.

**macOS:** use the app's microphone test, not a permission previously granted to Terminal. See [macOS diagnostics](docs/macos.md).

## Uninstall

Omarchy (adjust `~/.config` if you use `XDG_CONFIG_HOME`):

```sh
systemctl --user disable --now voxa
rm -f ~/.config/systemd/user/voxa.service
rm -f ~/.config/systemd/user/voxa.service.d/{80-config,90-native}.conf
rm -f ~/.local/bin/voxa
rm -rf ~/.local/share/voxa
systemctl --user daemon-reload
# Remove the managed Voxa block from ~/.config/hypr/bindings.lua; hyprctl reload
# Optional: omarchy plugin disable voxa.osd
# Optional: remove ~/.config/omarchy/plugins/voxa.osd, then rescan plugins
# Optional: remove ~/.config/voxa (includes API keys) and ~/.local/share/voxa-backups
```

macOS: quit Voxa, disable its login item, remove `~/Applications/Voxa.app` and `~/.local/bin/voxa`. Optionally remove `~/.config/voxa` (includes keys).

## Development

```sh
make                             # C daemon + CLI → native/voxa
make test                        # offline tests; Python 3 standard library, Linux
make sanitize                    # AddressSanitizer + UndefinedBehaviorSanitizer tests
make clean all                   # restore release build
python3 native/bench-daemon.py    # isolated status-only benchmark; no mic/API
```

[C architecture and standalone usage](native/README.md). Python is only needed for tests/benchmarks, not installation or normal use. macOS UI and paste helper remain Swift; all former Node responsibilities are in C. Terminal settings replace the old GUI settings/shortcut recorder.
