import { join } from 'node:path';
import { homedir } from 'node:os';

// Keep control-command startup independent of audio, WebSocket and setup modules.
export function runtimeDir() {
  return process.platform === 'darwin'
    ? join(homedir(), 'Library', 'Caches', 'voxa')
    : process.env.XDG_RUNTIME_DIR ?? `/run/user/${process.getuid?.() ?? 0}`;
}
export function socketPath() { return join(runtimeDir(), 'voxa.sock'); }
