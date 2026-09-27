# Native Voxa

The C daemon and CLI are the only backend. Build with `make` from the repository root (C11, pkg-config, json-c, libcurl with `ws`/`wss` support). Runtime dependencies: PipeWire's `pw-record` and `wtype` on Linux; Voxa.app and its Swift paste helper on macOS.

## Standalone evaluation

The default control socket is `$XDG_RUNTIME_DIR/voxa.sock` on Linux (fallback `/run/user/$UID/voxa.sock`), or `~/Library/Caches/voxa/voxa.sock` on macOS. This is the same socket the installed service uses. For isolated evaluation, specify a different path on **both** server and client:

```sh
mkdir -m 700 /tmp/voxa-evaluation-$$
# Substitute the directory printed/created above in both terminals:
native/voxa --socket /tmp/voxa-evaluation-PID/control.sock --no-osd daemon
native/voxa --socket /tmp/voxa-evaluation-PID/control.sock status
```

The parent directory must be user-owned and not writable by others. Socket permissions are `600`; peer UID is checked. A persistent `.lock` file prevents competing daemons. Do not run simultaneous recordings through different daemons.

`start` opens the mic and sends audio to ElevenLabs; `stop` commits and inserts into the app focused at completion. A `recording` reply acknowledges worker startup, not successful mic/API initialization. See the root README for installation, configuration and commands.

## Implementation

- `main.c`: bounded nonblocking Unix control server; `idle` / `recording` / `committing` state machine; one forked worker per recording; responsive controls during commit.
- `session.c`: concurrent microphone capture and TLS/WebSocket connection via libcurl multi; PCM16 LE, 16 kHz mono; 200 ms frames and a final audio tail for manual commit; fragmented response handling.
- `platform.c`: descriptor lifecycle, peer credentials, runtime paths, microphone and insertion adapters. Linux uses `pw-record` / `wtype`; macOS connects to the Swift app's private PCM socket and invokes the bundled paste helper.
- `config.c`: strict JSON config, sequential key rotation, URL encoding, Unicode whitespace and punctuation handling. Config/key files reload per recording.
- `settings.c`: secure terminal setup/settings, conflict-safe managed bindings, doctor and live diagnostics. API key entry disables echo and atomic saves use mode `600`.
- `osd.c`: bounded asynchronous display queue. Linux calls the Omarchy plugin; macOS sends states to the app's OSD socket. `--no-osd` disables display calls.

Bounds: 12 seconds buffered audio, 1 MiB response, 60 seconds recording, 10 seconds session startup, 250 ms microphone drain, eight seconds commit response, two seconds insertion. Failure cancels the session without a clipboard fallback. TLS verification is always enabled. Production rejects endpoint overrides. Logs omit API keys, server error bodies and transcript text, including with `debug` enabled.

## Tests and measurements

```sh
make test
make sanitize
make clean all
python3 native/bench-daemon.py
```

Tests use Python's standard library for a loopback WebSocket server, fake audio/typing/OSD helpers and PTY interaction with C settings. They do not access the real desktop, microphone or external API. The test-only binary accepts only `ws://127.0.0.1:PORT/...` overrides; never use real credentials with it.

Sanitizers cover exercised paths, not a proof of memory safety. Linux integration tests are skipped on macOS; that platform needs a native build and manual permission/audio/paste testing. CLI/idle benchmarks do not establish end-to-end speech latency or transcription quality.

Historical results are retained under `docs/benchmarks/`; their old implementation paths and commands refer to the commit at which they were measured, not the current build.
