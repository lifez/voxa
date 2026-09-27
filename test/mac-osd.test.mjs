import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:net';
import { spawn } from 'node:child_process';

test('Mac App OSD sends ordered states and hide over its own socket', async t => {
  if (process.platform !== 'darwin') return t.skip('macOS only');
  const home = mkdtempSync(join(tmpdir(), 'voxa-osd-'));
  const dir = join(home, 'Library/Caches/voxa');
  mkdirSync(dir, { recursive: true });
  const messages = [];
  const server = createServer(socket => {
    let text = '';
    socket.on('data', data => { text += data; });
    socket.on('end', () => { messages.push(text); socket.end(); });
  });
  try {
    await new Promise(resolve => server.listen(join(dir, 'osd.sock'), resolve));
    const child = spawn(process.execPath, ['--input-type=module', '-e', `
      import { Osd } from './dist/osd.js';
      const osd = new Osd();
      osd.show('recording'); osd.show('committing'); osd.show('done'); osd.hide();
      await osd.queue;
    `], { env: { ...process.env, HOME: home, VOXA_MAC_APP: '1' }, timeout: 5000 });
    let stderr = '';
    child.stderr.on('data', data => { stderr += data; });
    const code = await new Promise(resolve => child.on('close', resolve));
    assert.equal(code, 0, stderr);
    assert.deepEqual(messages, ['recording\n', 'committing\n', 'done\n', 'hide\n']);
  } finally { server.close(); rmSync(home, { recursive: true, force: true }); }
});
