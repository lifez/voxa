import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
const exec = promisify(execFile);

test('lazy CLI and prototype clients preserve control protocol and report missing daemon', { skip: process.platform !== 'linux' }, async () => {
  const dir = await mkdtemp(join(tmpdir(), 'voxa-cli-'));
  const clients = [[process.execPath, ['dist/index.js']]];
  let server;
  try {
    // Prototypes are optional: npm test still works without a compiler/Python.
    try {
      await exec('cc', ['-O2', '-Wall', '-Wextra', '-Werror', 'bench/control.c', '-o', join(dir, 'control')]);
      clients.push([join(dir, 'control'), []]);
    } catch (e) { if (e.code !== 'ENOENT') throw e; }
    try { await exec('python3', ['--version']); clients.push(['python3', ['bench/control.py']]); }
    catch (e) { if (e.code !== 'ENOENT') throw e; }
    const received = [];
    server = createServer(sock => {
      let input = '';
      sock.on('data', data => {
        input += data;
        if (input.endsWith('\n')) { received.push(input); sock.end('idle\n'); }
      });
    });
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(join(dir, 'voxa.sock'), resolve); });
    const options = { env: { ...process.env, XDG_RUNTIME_DIR: dir }, timeout: 4000 };
    for (const [binary, args] of clients) {
      for (const command of ['status', 'start', 'stop', 'toggle']) {
        const result = await exec(binary, [...args, command], options);
        assert.equal(result.stdout, 'idle\n');
        assert.equal(received.at(-1), command + '\n');
      }
      await assert.rejects(exec(binary, [...args, 'invalid'], options), e => e.code === 2);
    }
    await new Promise(resolve => server.close(resolve));
    for (const [binary, args] of clients) {
      await assert.rejects(exec(binary, [...args, 'status'], options), e => e.code === 1);
    }
  } finally {
    server?.close();
    await rm(dir, { recursive: true, force: true });
  }
});
