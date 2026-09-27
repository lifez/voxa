# Experimental C daemon (Linux only)

A separate implementation; **the Node daemon and original service unit are retained**. Building does not replace the running daemon. The opt-in migration below switches the service and launcher with a backup and rollback. Standalone default socket is `$XDG_RUNTIME_DIR/voxa-c.sock` (fallback `/run/user/$UID/voxa-c.sock`), not Node's `voxa.sock`.

## Build / run

Requires a C11 compiler, make, pkg-config, libcurl **with `ws`/`wss` support**, json-c, `pw-record` and `wtype`. Verified locally with libcurl 8.22.0 and json-c 0.19. On Arch, development headers ship in `curl` and `json-c`; a compiler is available through `base-devel`. No automatic package installation is performed.

```sh
make -C native
./native/voxa-c daemon                 # foreground; Ctrl+C to stop
# In another terminal:
./native/voxa-c status                 # read-only
```

Commands: `start`, `stop`, `toggle`, `status`, `test-osd` (display-only preview); optionally specify `--socket /absolute/private/directory/voxa-c.sock` on **both** daemon and client. The socket's parent directory must already exist, belong to your UID and not be writable by other users. The socket is mode 0600; peers are checked with `SO_PEERCRED`. A persistent `.lock` file prevents competing native daemons. Do not point it at the existing Node socket during evaluation.

**Live `start` opens the microphone and connects to ElevenLabs, using quota and sending audio to that service. `stop` commits and types into whichever app is focused at completion.** Use a disposable text editor, not a shell/chat submission field; newlines can act as Enter. Keep focus in the destination. Before migration, F10/F11 still operate Node; after migration they use C through the existing launcher path. Never start both recordings simultaneously.

Configuration and API keys use the existing files:

- `${XDG_CONFIG_HOME:-$HOME/.config}/voxa/config.json`
- `ELEVENLABS_API_KEY` from the environment takes precedence; otherwise read `voxa/env` on each recording.
- Same config fields/defaults, language/keyterms query parameters, sequential rotation of 1–5 keys and stop-punctuation formatting. Rotation is process-local, independent of Node.
- Invalid config or keys leave the daemon idle and emit a generic error; `start` returning `recording` means the worker was launched, **not** that microphone or API setup succeeded.

## Switch an existing Linux installation

```sh
bash scripts/use-native.sh
```

This builds/tests a release binary, requires the current daemon to be idle, backs up the launcher and any previous native override, and installs:

- `~/.local/share/voxa/native/voxa-c`
- A native command router at `~/.local/bin/voxa`; setup/settings/doctor and other legacy utilities still run the **preserved installed Node version**.
- `~/.config/systemd/user/voxa.service.d/90-native.conf`, overriding only `ExecStart` and using the existing `%t/voxa.sock`.

It restarts `voxa.service`, checks the executable and `idle` response, and rolls back on deployment failure. Keys, original Node files, original service unit and keybindings are not changed. Existing environment settings and service restart policy are inherited. The script prints the exact rollback command:

```sh
bash scripts/use-native.sh --rollback /path/to/backup-before-native-TIMESTAMP.SUFFIX
```

Finish any dictation before switching/rolling back. A later run of the original Node installer can replace the launcher; rerun this migration to restore native routing. Building C alone does not update an installed binary.

## Implemented

- Omarchy OSD using the existing `voxa.osd` plugin: recording, committing, done, error; empty transcript hides it. Calls are serialized in a bounded parent-side queue, with a 1.5s helper timeout. Backend failure never blocks audio/transcription/typing. Graceful daemon shutdown clears pending states and hides the OSD. `--no-osd` disables display commands.
- Persistent control server with responsive `idle` / `recording` / `committing` states and bounded client reads; ignores toggles while committing.
- One forked worker per dictation, microphone only during dictation, nonblocking audio/control I/O, concurrent PipeWire startup and TLS/WebSocket connection using libcurl multi.
- PCM16 little-endian, 16kHz mono, requested PipeWire latency 100ms, 200ms stream frames with a final audio tail for manual commit.
- Bounded 12-second audio queue, 1MiB inbound message limit, fragmented WebSocket reassembly, TLS peer/hostname verification, generic error logging without server text/key/transcript contents.
- 60-second recording limit, 8-second connection timeout / 10-second session deadline, up to 250ms mic drain, 8-second final transcript timeout after commit send, bounded subprocess cleanup.
- **Linux text insertion uses `wtype -` with transcript on stdin**, matching the updated Node backend. No clipboard writes/reads, no text in process arguments, no clipboard fallback. Insertion command is bounded to two seconds; very slow apps or unusually long outputs may be interrupted.
- Per-session monotonic timing: first PCM, session ready, mic drain, commit→transcript, stop→transcript, typing, stop→done; total audio byte count. No audio files or transcript persistence.

## Deliberate limitations / not yet verified

- **Experimental.** No real ElevenLabs transcription or real target-app typing was exercised during implementation/deployment; integration tests use a local mock server, synthetic PCM and fake `wtype`. Read-only service checks are not an end-to-end dictation test.
- **No macOS implementation or native setup/settings/doctor UI.** Use the existing Node tools for settings; C emits state/errors/timings in stderr. `debug` is accepted for config compatibility but never logs transcript text in C.
- OSD requires an enabled `voxa.osd` Omarchy plugin. No shell/plugin files are replaced by the migration. The existing plugin controls appearance, focus-free behavior and 1.3s done/error auto-hide. Run `voxa test-osd` after migration to preview without mic/API/insertion; its exit status does not confirm display success (the backend is best-effort).
- No proof yet of reduced end-to-end speech latency or equivalent transcription quality. Idle/CLI benchmarks do not measure microphone, worker startup, network or API processing. libcurl connection reuse/preconnect is not enabled.
- Test binaries can override the endpoint **only to loopback WS**; production `voxa-c` rejects `--test-endpoint`. Never use real API credentials with `voxa-c-test`.
- Config reload/key-file refresh happens on `start`, not a service-wide shared configuration. If the environment contains a key, editing the key file does not override that environment.

## Tests

```sh
npm ci                           # if Node dependencies are not already installed
make -C native test              # production + loopback-only test binary, 15 tests
make -C native sanitize          # ASan + UBSan; rebuilds binaries with instrumentation
make -C native clean all         # restore optimized build before benchmarking
npm test                         # existing Node regression tests
```

OSD tests additionally verify ordered states, empty/error handling, shutdown hide, a failing/hung display backend, and `--no-osd`; a fake `omarchy-shell` prevents desktop side effects. Tests exercise real native sockets, worker/mic subprocess lifecycle, PCM framing, delayed session readiness/stop before ready, fragmented response plus ping, Unicode/punctuation, config reload, key rotation, empty transcript, API error redaction, insertion/microphone failures, commit timeout, malformed/partial clients, duplicate-daemon rejection, permissions, shutdown cleanup and preservation of non-socket paths. Executables for microphone/insertion are test-local mocks. They do not access the real mic, clipboard, focused app or external API. Sanitizers do not establish full memory safety; they cover the exercised paths only.

## Reproducible idle benchmark

```sh
npm run build
make -C native clean all
python3 native/bench-daemon.py
```

This starts isolated Node/C daemons with temporary runtime/config directories, removes the API key from their environment, sends **only `status`**, then terminates them. It does not use or restart your running service. Five warmups, 100 shuffled rounds per path, warm filesystem cache. RSS is an idle snapshot including shared libraries, excluding the recording worker and children—not PSS.

Measured on this machine (2026-09-27):

| Path | Node median / P95 | C median / P95 |
|---|---:|---:|
| Direct socket round-trip | 0.101 / 0.125 ms | 0.042 / 0.057 ms |
| CLI start → response/exit | 21.69 / 23.20 ms | 2.79 / 2.97 ms |
| Idle RSS | 64,508 KiB | 9,108 KiB |

Node here is the **lazy-import optimized** version. C CLI is this full daemon binary linked with libcurl/json-c, so its startup is not the same as the earlier minimal `bench/control.c` (~0.64ms). The daemon's own socket saving is only about **0.06ms**; most observed CLI savings are process/runtime startup. Do not infer that a ~400ms remote transcription becomes ~50ms.

Raw results: [`../docs/benchmarks/native-daemon-2026-09-27.json`](../docs/benchmarks/native-daemon-2026-09-27.json).
