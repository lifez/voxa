# Voxa for Omarchy and macOS

Hold F10 to record and release to transcribe, or press F11 once to record and again to transcribe. Both modes paste via Wayland clipboard. Omarchy shows a focus-free, bottom-center recording/transcribing OSD (not a notification). No idle microphone capture, transcript persistence, or LLM. The daemon only opens the microphone and WebSocket on `start`. Logs omit transcripts unless `debug` is enabled. Clipboard remains set to the transcript.

## macOS (experimental)

Requires macOS with Node.js 22+, `ffmpeg` (`brew install ffmpeg`), Xcode Command Line Tools (`xcode-select --install`), and an ElevenLabs key with **Speech to Text** access. On a Mac, run:

```sh
bash scripts/install-macos.sh
~/.local/bin/voxa setup
~/.local/bin/voxa test-mic
~/.local/bin/voxa test-scribe
~/.local/bin/voxa test-paste 'hello'  # focus a disposable text field first
~/.local/bin/voxa doctor
```

The installer builds a native keyboard listener at `~/Library/Application Support/voxa/bin/voxa-keys` and installs a per-user launchd agent (`com.voxa.daemon`). Grant **Accessibility** permission to `voxa-keys` in System Settings → Privacy & Security → Accessibility; grant Automation access for `osascript`/System Events when macOS prompts for paste. Restart after granting permissions: `launchctl kickstart -k gui/$(id -u)/com.voxa.daemon`. Inspect `~/Library/Logs/voxa-error.log` and `~/Library/Logs/voxa.log` if keys do not work. If macOS treats F10/F11 as media keys, enable standard function keys in Keyboard settings or use Fn+F10/F11. F10 holds to record; F11 toggles. These macOS shortcuts are **fixed for now**; the Omarchy shortcut editor and OSD are not available on macOS. Key events also reach the focused app. The clipboard remains set to the transcript, as on Linux.

Microphone recording uses FFmpeg's AVFoundation default audio device, converts to 16 kHz mono PCM, and may prompt for Microphone permission. To choose another device, run `ffmpeg -f avfoundation -list_devices true -i ""` and enter its **audio device index** in `voxa settings`. On macOS `voxa settings` is terminal-only. The first paste may prompt for permission to control System Events; without it, the transcript is copied to the clipboard but cannot be pasted automatically. The native listener is compiled locally and currently has no macOS release/signing process; permissions may need to be granted again after reinstalling. This path has not been exercised on macOS in CI.

Uninstall: `launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.voxa.daemon.plist`, then remove that plist, `~/Library/Application Support/voxa`, and `~/.local/bin/voxa`. Optionally remove `~/.config/voxa` (contains your API key).

## Dependencies / installation (Arch/Omarchy)

Arch/Omarchy: `node` (>=22), `npm`, PipeWire (`pw-record`), `wl-clipboard`, `wtype`, Hyprland. If missing: `sudo pacman -S nodejs npm pipewire wl-clipboard wtype`. Check with `command -v pw-record wl-copy wtype node npm`. `pw-record` is supplied by PipeWire on this machine; audio is raw signed 16-bit mono 16 kHz (PipeWire resamples as needed).

```sh
bash scripts/install.sh
voxa setup                  # interactive wizard: key, shortcuts, language, microphone, service
voxa doctor                 # checks dependencies, config, bindings and service
voxa settings               # choose API key, Hold shortcut, Toggle shortcut, language or microphone
# Optional GUI/shortcut recorder dependencies: sudo pacman -S zenity python-gobject gtk4
voxa status
```

**ElevenLabs API key:** When creating the key in ElevenLabs, enable **Speech to Text** access for that key. Voxa uses ElevenLabs Scribe for transcription; a key without this permission will not work. Paste the key into the API key prompt in `voxa setup` (or choose **API key** in `voxa settings`). Leave the field blank to keep the existing key. `voxa settings` does not require a key to edit other options.

The installer copies built files to `~/.local/share/voxa`, links `~/.local/bin/voxa`, installs the user service, and enables the `voxa.osd` user-owned Omarchy Shell panel plugin when Omarchy Shell is running. Run `voxa setup` after installation; it updates managed bindings in `~/.config/hypr/bindings.lua` (backed up to `bindings.lua.voxa.bak`), saves the key with permissions 0600 and enables the service. Existing manual F10/F11 Voxa bindings from the original example are migrated; conflicting custom bindings cause an error instead of being overwritten. `voxa setup` guides you through the settings on first install; `voxa settings` opens a menu so you can edit just one option and return to the menu. Each option saves independently. The menu shows separate **Hold shortcut** and **Toggle shortcut** options with their current keys (and whether the API key is set); changing one leaves the other untouched. The shortcut recorder also shows the key being replaced. `voxa settings --terminal` forces the terminal menu. Choose either shortcut in the GUI, click **Record shortcut**, press the key combination, then click **Use shortcut**. If your compositor intercepts that key, choose **Type manually instead**. Closing a GUI prompt cancels only the current option and returns to the menu; earlier saved options remain saved. `test-scribe` reads the same key file if your shell does not export the key; the systemd service reads it through `EnvironmentFile`. The installer does not overwrite existing config or env files. Never put credentials in `config.json`, shell history, or this repository. Systemd user manager must have `WAYLAND_DISPLAY` and `PATH` imported (Omarchy's graphical session normally does); inspect with `systemctl --user show-environment`.

## Try each part

```sh
voxa test-mic             # 2 seconds; byte count, no saved audio
voxa test-scribe          # 3 seconds; reads key file, prints committed transcript, no paste
voxa test-paste 'hello สวัสดี'  # pastes into focused app; test with a disposable field!
voxa toggle; sleep 3; voxa toggle  # or: voxa start; sleep 3; voxa stop
journalctl --user -u voxa -f
```

`start`, `stop`, `toggle`, `status` communicate over a mode-0600 Unix socket in `$XDG_RUNTIME_DIR`; `toggle` atomically starts when idle, stops when recording, and does nothing while transcribing. Repeated starts and idle stops are harmless. Stop-to-transcript latency is logged. Missing mic/network/key/paste commands are logged and return to idle. Empty transcripts are not pasted. Recording stops on the second press (or release in hold mode) even if the network is slow; an 8-second commit timeout and a 60-second recording safety cutoff prevent getting stuck. Large audio chunks are sent as 200ms frames with a final committed frame.

## Omarchy binding

`voxa setup` manages a marked block in **user** `~/.config/hypr/bindings.lua` (never packaged defaults). F10 is hold-to-record/release-to-transcribe; F11 is press-once-to-record/press-again-to-transcribe. Both shortcuts control the same recording; use one mode at a time. Remove old Scribe or other conflicting bindings first. To change keys later use `voxa settings`; on a graphical session `voxa setup` also offers the key recorder. GTK4/PyGObject is needed for key capture; if unavailable, type the shortcut instead. `hypr/voxa.lua` remains as a manual reference. Standalone modifier keys may not fire reliable release events in hold mode; choose a non-modifier key. Verify key symbols with `wev` and check `hyprctl binds -j`. When Hyprland is running the editor reloads it and restores the prior file if `hyprctl configerrors` reports errors.

## Configuration

Use `voxa settings` or edit `~/.config/voxa/config.json` (see `config.example.json`). Changes apply on next recording; changing the API key restarts the service. Default is `language: "th"`, `secondaryLanguages: ["en"]`; for automatic detection set `language: null`, `secondaryLanguages: []` and compare results for Thai/English code switching. `keyterms` biases technical terms. `stopPunctuation: false` (default) removes a final full stop (`.` or `。`) from the transcript; set it to `true` to keep it. Internal punctuation and question marks are unchanged. This is also available in `voxa settings`. `pasteCommand` is retained for Linux compatibility (`wtype`); macOS uses `pbcopy` and `osascript` regardless of this setting. `debug: true` logs transcript text; leave false for privacy. ElevenLabs may retain request data per your account's policy.

For microphones, `wpctl status` or `pactl list short sources` lists sources; set `audioDevice` to the PipeWire node name or ID (passed to `pw-record --target`), or leave `default`. Check the default source with `pactl info`. For PipeWire problems run `voxa test-mic`, `wpctl status`, and `systemctl --user status pipewire wireplumber`. For paste problems check `WAYLAND_DISPLAY`, `command -v wl-copy wtype`, `wl-paste`, and test `voxa test-paste` in a focused browser field. The OSD is supplied by `~/.config/omarchy/plugins/voxa.osd/`, uses Omarchy Shell's native layer surface and never takes keyboard focus; if it does not appear, check `omarchy-shell shell listPlugins` for an enabled `voxa.osd` and run `omarchy-shell shell rescanPlugins`. Some terminal apps need Ctrl+Shift+V rather than Ctrl+V; this V1 uses Ctrl+V, so verify your terminal's paste binding. Focus must remain in the destination during transcription; the utility does not restore or force focus.

## Upgrading from Scribe dictation

Stop and disable the old service before starting Voxa to avoid two daemons on the same key. The installer copies the old `config.json` and private `env` into `~/.config/voxa/` only if the new files do not already exist; it does not delete the originals. Remove the old Scribe binding lines from `~/.config/hypr/bindings.lua`; `voxa setup` will create the Voxa bindings. After installing and confirming Voxa works, remove the old service and plugin:

```sh
systemctl --user disable --now scribe-dictation
bash scripts/install.sh
# Remove old Scribe bindings from ~/.config/hypr/bindings.lua
voxa setup
omarchy plugin disable scribe.osd  # if the old plugin is enabled
rm -f ~/.config/systemd/user/scribe-dictation.service ~/.local/bin/scribe-dictation
rm -rf ~/.local/share/scribe-dictation ~/.config/omarchy/plugins/scribe.osd
omarchy-shell shell rescanPlugins  # if Omarchy Shell is running
systemctl --user daemon-reload
# Optional after confirming migration: rm -rf ~/.config/scribe-dictation (contains your key)
```

## Uninstall

```sh
systemctl --user disable --now voxa
rm -f ~/.config/systemd/user/voxa.service ~/.local/bin/voxa
rm -rf ~/.local/share/voxa
omarchy plugin disable voxa.osd  # optional if Omarchy Shell is running
rm -rf ~/.config/omarchy/plugins/voxa.osd
omarchy-shell shell rescanPlugins  # optional if Omarchy Shell is running
systemctl --user daemon-reload
# Remove the managed Voxa block from ~/.config/hypr/bindings.lua; hyprctl reload
# Optional: rm -rf ~/.config/voxa (includes your private key)
```

Development: `npm ci && npm test`. `voxa doctor` performs local checks but does not send audio or verify ElevenLabs credentials; use `voxa test-scribe` for a live test. The user service expects the key at `~/.config/voxa/env` (default XDG config directory). Requires a valid ElevenLabs key and real voice for live STT; automated tests only cover configuration. There is intentionally no automatic clipboard restoration: restoring too quickly can race the receiving application.
