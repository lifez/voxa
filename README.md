# Voxa for Omarchy and macOS

Hold F10 to record and release to transcribe, or press F11 once to record and again to transcribe. Voxa pastes via the Wayland clipboard on Omarchy and the system clipboard on macOS. Omarchy and the macOS app show a focus-free recording/transcribing OSD. There is no idle microphone capture, transcript persistence, or LLM. Logs omit transcripts unless `debug` is enabled. The clipboard remains set to the transcript.

## macOS (experimental)

Requires macOS 13+, Node.js 22+, Xcode Command Line Tools, and an ElevenLabs key with **Speech to Text** access. The app captures audio **in its own process** using AVAudioEngine (not an FFmpeg child); Node receives 16 kHz mono PCM via a private local socket. Install from a Terminal once:

```sh
bash scripts/install-macos.sh
~/.local/bin/voxa setup
```

The installer creates `~/Applications/Voxa.app`, removes the old launchd agent, opens the app and registers it as a login item. Grant **Microphone** and **Accessibility** to `~/Applications/Voxa.app` in System Settings → Privacy & Security. If shortcuts are not active yet, use **Enable / Retry Shortcuts** in the menu bar after granting Accessibility. Grant Automation to System Events when prompted for paste. Hold **Command+Shift+R** to record; **Command+Shift+T** toggles. Voxa consumes these shortcuts and shows a non-interactive OSD on the current screen while recording/transcribing; success and errors disappear after 1.3 seconds. Only the system default microphone is supported; change the default in macOS Sound settings. `voxa settings` is terminal-only.

**Verify the actual app path:** click Voxa in the menu bar → **Test microphone (speak for 2 seconds)**. The menu bar must show `mic OK (peak N)` with **N > 0**. A successful `voxa test-mic` from Terminal does *not* establish that the app path works. Then speak with the shortcut and check `~/Library/Logs/voxa.log` for `peak > 0` and `pasted N chars`. If silent, check Voxa's Microphone permission, macOS default input, and restart Voxa. Do not assume a permission granted to Terminal or an FFmpeg subprocess applies to the app. Login item status can be checked in System Settings → General → Login Items; enable Voxa manually if registration failed. This path has not been verified with live speech on this machine.

Uninstall: quit Voxa from the menu bar, disable its login item in System Settings, remove `~/Applications/Voxa.app` and `~/.local/bin/voxa`. Optionally remove `~/.config/voxa` (contains your API key).

## Dependencies / installation (Arch/Omarchy)

Requires Node.js 22+, `npm`, PipeWire (`pw-record`), `wl-clipboard`, `wtype`, and Hyprland. On Arch, install missing packages with `sudo pacman -S nodejs npm pipewire wl-clipboard wtype`.

```sh
bash scripts/install.sh
voxa setup                  # interactive wizard: key, shortcuts, language, microphone, service
voxa doctor                 # checks dependencies, config, bindings and service
voxa settings               # choose API key, Hold shortcut, Toggle shortcut, language or microphone
# Optional GUI/shortcut recorder dependencies: sudo pacman -S zenity python-gobject gtk4
voxa status
```

**ElevenLabs API key:** Enable **Speech to Text** access when creating the key. Enter it in `voxa setup` or choose **API key** in `voxa settings`. Leave the field blank to keep the existing key.

The installer installs the user service and enables the Omarchy Shell OSD plugin when available. Run `voxa setup` to save the key securely, enable the service, and manage shortcuts in `~/.config/hypr/bindings.lua` (backed up to `bindings.lua.voxa.bak`). Conflicting custom bindings are not overwritten. Later, use `voxa settings` to change individual options; `voxa settings --terminal` forces the terminal menu. The installer does not overwrite existing config or key files. Never put credentials in `config.json`, shell history, or this repository.

## Try each part

```sh
voxa test-mic             # 2 seconds; byte count, no saved audio
voxa test-scribe          # 3 seconds; reads key file, prints committed transcript, no paste
voxa test-paste 'hello สวัสดี'  # pastes into focused app; test with a disposable field!
voxa toggle; sleep 3; voxa toggle  # or: voxa start; sleep 3; voxa stop
journalctl --user -u voxa -f
```

Press F11 again to stop recording, or release F10 in hold mode. Empty transcripts are not pasted; errors are logged with `journalctl --user -u voxa -f`.

## Omarchy binding

`voxa setup` manages a marked block in your `~/.config/hypr/bindings.lua`. F10 holds to record; F11 toggles recording. Both shortcuts control the same recording, so use one mode at a time. Remove conflicting bindings first. Change keys with `voxa settings`: use the GUI shortcut recorder (requires GTK4/PyGObject) or type a shortcut manually. Standalone modifier keys may not fire reliable release events in hold mode; choose a non-modifier key. `hypr/voxa.lua` is a manual reference.

## Configuration

Use `voxa settings` or edit `~/.config/voxa/config.json` (see `config.example.json`). Changes apply on next recording; changing the API key restarts the service. Default is `language: "th"`, `secondaryLanguages: ["en"]`; for automatic detection set `language: null`, `secondaryLanguages: []` and compare results for Thai/English code switching. `keyterms` biases technical terms. `stopPunctuation: false` (default) removes a final full stop (`.` or `。`) from the transcript; set it to `true` to keep it. Internal punctuation and question marks are unchanged. This is also available in `voxa settings`. `pasteCommand` is retained for Linux compatibility (`wtype`); macOS uses `pbcopy` and `osascript` regardless of this setting. `debug: true` logs transcript text; leave false for privacy. ElevenLabs may retain request data per your account's policy.

On Omarchy, set `audioDevice` to a PipeWire source name or ID from `wpctl status` (or leave `default`). Focus must remain in the destination during transcription; Voxa does not restore focus.

## Troubleshooting (Omarchy)

- Microphone: run `voxa test-mic`, `wpctl status`, and `systemctl --user status pipewire wireplumber`.
- Paste: check `WAYLAND_DISPLAY` and `command -v wl-copy wtype`, then run `voxa test-paste` in a disposable focused field. Some terminals require Ctrl+Shift+V; Voxa uses Ctrl+V.
- OSD: check `omarchy-shell shell listPlugins` for an enabled `voxa.osd`; run `omarchy-shell shell rescanPlugins` if needed.
- Shortcuts: check `hyprctl binds -j`. The user systemd manager needs `WAYLAND_DISPLAY` and `PATH` imported (normally handled by Omarchy); inspect with `systemctl --user show-environment`.

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

Development: `npm ci && npm test`. `voxa doctor` checks local setup; use `voxa test-scribe` to test transcription with a valid ElevenLabs key.
