import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:net';
import { spawn } from 'node:child_process';

test('app capture reads PCM from local socket and rejects non-default devices', async t => {
  if (process.platform !== 'darwin') return t.skip('macOS only');
  const home = mkdtempSync(join(tmpdir(), 'voxa-mic-'));
  try {
    const dir = join(home, 'Library/Caches/voxa');
    mkdirSync(dir, { recursive: true });
    const sockets = new Set();
    const server = createServer(sock => { sockets.add(sock); sock.on('close', () => sockets.delete(sock)); sock.write(Buffer.from([0x37, 0x0c])); });
    await new Promise(resolve => server.listen(join(dir, 'mic.sock'), resolve));
    try {
      const child = spawn(process.execPath, ['--input-type=module', '-e', `
        import { capture, audioPeak } from './dist/audio.js';
        assertDevice();
        function assertDevice() { try { capture('1', () => {}, () => {}); throw Error('accepted non-default'); } catch (e) { if (!e.message.includes('system default')) throw e; } }
        const mic = capture('default', b => { if (audioPeak(b) !== 3127) process.exit(2); mic.stop(); }, e => { console.error(e); process.exit(3); });
        await mic.waitClose();
      `], { env: { ...process.env, HOME: home, VOXA_MAC_APP: '1' }, timeout: 3000 });
      let stderr = '';
      child.stderr.on('data', b => { stderr += b; });
      const code = await new Promise(resolve => child.on('close', resolve));
      assert.equal(code, 0, stderr);
    } finally { for (const sock of sockets) sock.destroy(); server.close(); }
  } finally { rmSync(home, { recursive: true, force: true }); }
});
