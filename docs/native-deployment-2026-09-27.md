# Native daemon + OSD deployment — 2026-09-27

## Completed

- Added native parent-side asynchronous OSD queue: `recording`, `committing`, `done`, `error`, `hide`; bounded backlog and 1.5s command timeout. Empty transcript and graceful shutdown hide the overlay. Display failure does not block recording/typing.
- Reused the enabled `voxa.osd` plugin without changing its QML, shell configuration or keybindings.
- 15 native integration tests passed in optimized and ASan/UBSan builds. Microphone/API/typing/display are mocked in these tests.
- Exercised the **real** desktop display through `native/voxa-c test-osd` before switching the service: one `voxa-osd` layer during recording/committing/done/error; zero layers after done/error auto-hide and shutdown. Active window address remained unchanged at every sample. No microphone/API/text insertion was used by the preview.
- Switched the existing `voxa.service` to the optimized native binary. Verified `ActiveState=active`, `SubState=running`, `/proc/MainPID/exe` resolving to the installed C binary, and `voxa status` returning `idle`.

## Installed paths

- Binary: `~/.local/share/voxa/native/voxa-c`
- Launcher/router: `~/.local/bin/voxa`
- Override: `~/.config/systemd/user/voxa.service.d/90-native.conf`
- Shared control socket: `/run/user/1000/voxa.sock`
- Backup: `/home/phawin/.local/share/voxa/backup-before-native-20260927-193256.HkmSaJ`

Original Node code/dependencies, original service unit and API-key/config files are preserved. F10/F11 still invoke the same launcher path; its `start/stop/toggle/status/daemon/test-osd` commands now use C. Other commands use the preserved installed Node version.

## Rollback

Finish any dictation first, then from the repository:

```sh
bash scripts/use-native.sh --rollback /home/phawin/.local/share/voxa/backup-before-native-20260927-193256.HkmSaJ
```

This restores the launcher symlink, removes the new override (or restores a backed-up previous override), reloads systemd, restarts the previous daemon and checks `idle`. It does not delete native source or Node files.

## Still unverified

Real speech → ElevenLabs → insertion into a real app has **not** been tested after switching; service status and display checks do not establish transcription quality or end-to-end latency. Test with a disposable text editor focused, using F10/F11. The C backend directly types through `wtype -`, without touching the clipboard; transcript newlines can act as Enter. Logs contain timings and generic errors, not transcript content.

```sh
voxa status
journalctl --user -u voxa -f
```

The OSD preview remains available via `voxa test-osd` (display-only; do not use it during a dictation because it shares the live OSD).
