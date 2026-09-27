import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdtemp, mkdir, writeFile, readFile, rm, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { connect } from 'node:net';
import { once } from 'node:events';
import { WebSocketServer } from 'ws';
const exec = promisify(execFile);
const root = dirname(fileURLToPath(import.meta.url));
const binary = join(root, 'voxa-c-test');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(fn, timeout = 5000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { if (await fn()) return; await delay(15); }
  throw Error('condition timed out');
}
async function fixture(t, options = {}) {
  const dir = await mkdtemp(join(tmpdir(), 'voxa-native-'));
  const bin = join(dir, 'bin'), config = join(dir, 'voxa');
  await mkdir(bin); await mkdir(config);
  await writeFile(join(config, 'config.json'), JSON.stringify(options.config ?? {}));
  const script = body => `#!${process.execPath}\n${body}\n`;
  await writeFile(join(bin, 'pw-record'), script(`
    const fs = require('node:fs');
    fs.writeFileSync(process.env.TEST_MIC, String(process.pid));
    const pcm = Buffer.alloc(6400, 1);
    const tick = setInterval(() => process.stdout.write(pcm), 20);
    process.on('SIGTERM', () => { clearInterval(tick); process.stdout.end(pcm, () => process.exit(0)); });
  `), { mode: 0o700 });
  await writeFile(join(bin, 'wl-copy'), script(`
    require('node:fs').writeFileSync(process.env.TEST_COPY, 'clipboard touched');
    process.exit(1);
  `), { mode: 0o700 });
  await writeFile(join(bin, 'wtype'), script(`
    const fs = require('node:fs');
    let text = '';
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', chunk => text += chunk);
    process.stdin.on('end', () => {
      fs.writeFileSync(process.env.TEST_TEXT, text);
      fs.writeFileSync(process.env.TEST_PASTE, JSON.stringify(process.argv.slice(2)));
    });
  `), { mode: 0o700 });
  await writeFile(join(bin, 'omarchy-shell'), script(`
    const fs = require('node:fs');
    const args = process.argv.slice(2);
    fs.appendFileSync(process.env.TEST_OSD, JSON.stringify(args) + '\\n');
    if (${!!options.hangOsd} && args.includes('summon')) setInterval(() => {}, 1000);
    else process.exit(${options.failOsd ? 1 : 0});
  `), { mode: 0o700 });
  const env = { ...process.env, TEST_OSD: join(dir, 'osd'), ELEVENLABS_API_KEY: 'test-key-1,test-key-2', XDG_CONFIG_HOME: dir,
    PATH: `${bin}:${process.env.PATH}`, TEST_COPY: join(dir, 'copy'), TEST_TEXT: join(dir, 'text'), TEST_PASTE: join(dir, 'paste'), TEST_MIC: join(dir, 'mic') };
  const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
  await once(server, 'listening');
  const chunks = [], keys = [];
  server.on('connection', (ws, request) => {
    keys.push(request.headers['xi-api-key']);
    if (options.error) { ws.send(JSON.stringify({ message_type: 'authentication_error', message: 'test-key-1 MUST_NOT_LOG' })); return; }
    setTimeout(() => { if (ws.readyState === 1) ws.send('{"message_type":"session_started"}'); }, options.readyDelay ?? 0);
    ws.on('message', raw => {
      const event = JSON.parse(raw); chunks.push(event);
      if (event.commit && !options.noCommitResponse) {
        const response = JSON.stringify({ message_type: 'committed_transcript', text: options.text ?? 'สวัสดี hello.' });
        // Exercise fragmented WebSocket messages, a control frame in between,
        // and final transcript delivery separate from session_started.
        ws.send(response.slice(0, 10), { fin: false });
        ws.ping('test');
        ws.send(response.slice(10), { fin: true });
      }
    });
  });
  const socket = join(dir, 'daemon.sock');
  const args = ['--socket', socket, '--test-endpoint', `ws://127.0.0.1:${server.address().port}/`, ...(options.noOsd ? ['--no-osd'] : []), 'daemon'];
  const child = spawn(binary, args, { env, stdio: ['ignore', 'ignore', 'pipe'] });
  let logs = ''; child.stderr.on('data', b => logs += b);
  const exited = once(child, 'exit');
  t.after(async () => {
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGTERM');
    await Promise.race([exited, delay(3000).then(() => { if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL'); })]);
    for (const ws of server.clients) ws.terminate();
    await new Promise(resolve => server.close(resolve));
    await rm(dir, { recursive: true, force: true });
    assert.doesNotMatch(logs, /AddressSanitizer|LeakSanitizer|runtime error:/);
  });
  await until(async () => { try { return (await stat(socket)).isSocket(); } catch { if (child.exitCode !== null) throw Error(logs); return false; } });
  const command = async cmd => (await exec(binary, ['--socket', socket, cmd], { env, timeout: 3000 })).stdout.trim();
  const osdStates = async () => {
    try {
      return (await readFile(env.TEST_OSD, 'utf8')).trim().split('\n').map(line => {
        const args = JSON.parse(line);
        assert.deepEqual(args.slice(0, 2), ['-q', 'shell']);
        assert.equal(args[3], 'voxa.osd');
        return args[2] === 'hide' ? 'hide' : JSON.parse(args[4]).state;
      });
    } catch (e) { if (e.code === 'ENOENT') return []; throw e; }
  };
  return { dir, bin, env, socket, args, child, exited, server, chunks, keys, command, osdStates, logs: () => logs };
}

test('native streams PCM, commits after delayed session readiness, types formatted UTF-8, rotates keys', async t => {
  const f = await fixture(t, { readyDelay: 200 });
  assert.equal((await stat(f.socket)).mode & 0o777, 0o600);
  assert.equal(await f.command('status'), 'idle');
  for (let i = 0; i < 2; i++) {
    assert.equal(await f.command('toggle'), 'recording');
    await until(async () => { try { return !!(await readFile(f.env.TEST_MIC)); } catch { return false; } });
    await delay(60);
    assert.equal(await f.command('stop'), 'committing');
    // Ignore presses while committing: no second session is created.
    assert.equal(await f.command('toggle'), 'committing');
    await until(async () => await f.command('status') === 'idle');
    assert.equal(await readFile(f.env.TEST_TEXT, 'utf8'), 'สวัสดี hello');
    assert.deepEqual(JSON.parse(await readFile(f.env.TEST_PASTE, 'utf8')), ['-']);
    await assert.rejects(readFile(f.env.TEST_COPY), { code: 'ENOENT' });
    await rm(f.env.TEST_MIC);
  }
  await until(async () => (await f.osdStates()).length === 6);
  assert.deepEqual(await f.osdStates(), ['recording', 'committing', 'done', 'recording', 'committing', 'done']);
  assert.deepEqual(f.keys, ['test-key-1', 'test-key-2']);
  assert.equal(f.chunks.filter(c => c.commit).length, 2);
  for (const c of f.chunks) {
    assert.equal(c.message_type, 'input_audio_chunk'); assert.equal(c.sample_rate, 16000);
    const pcm = Buffer.from(c.audio_base_64, 'base64');
    assert.ok(pcm.length > 0 && pcm.length % 2 === 0); assert.ok(pcm.every(b => b === 1));
    if (!c.commit) assert.equal(pcm.length, 6400);
  }
  assert.match(f.logs(), /stop_to_done_ms=/);
  assert.doesNotMatch(f.logs(), /test-key|สวัสดี|hello/);
});

test('empty transcript never touches clipboard or paste', async t => {
  const f = await fixture(t, { text: ' \t\u00a0\u3000\ufeff' });
  await f.command('start'); await delay(150); await f.command('stop');
  await until(async () => await f.command('status') === 'idle');
  await assert.rejects(readFile(f.env.TEST_COPY), { code: 'ENOENT' });
  await assert.rejects(readFile(f.env.TEST_PASTE), { code: 'ENOENT' });
  assert.match(f.logs(), /empty transcript/);
  await until(async () => (await f.osdStates()).at(-1) === 'hide');
  assert.deepEqual(await f.osdStates(), ['recording', 'committing', 'hide']);
});

test('punctuation config and reload apply next session', async t => {
  const f = await fixture(t, { config: { stopPunctuation: true }, text: 'Hello。' });
  for (const expected of ['Hello。', 'Hello']) {
    await f.command('start'); await delay(150); await f.command('stop');
    await until(async () => await f.command('status') === 'idle');
    assert.equal(await readFile(f.env.TEST_TEXT, 'utf8'), expected);
    await writeFile(join(f.dir, 'voxa/config.json'), '{"stopPunctuation":false}');
  }
});

test('API errors return idle without leaking server error body or key', async t => {
  const f = await fixture(t, { error: true });
  await f.command('start');
  await until(async () => await f.command('status') === 'idle');
  assert.match(f.logs(), /session failed/);
  assert.doesNotMatch(f.logs(), /MUST_NOT_LOG|test-key/);
  await until(async () => (await f.osdStates()).at(-1) === 'error');
  assert.deepEqual(await f.osdStates(), ['recording', 'error']);
  await assert.rejects(readFile(f.env.TEST_PASTE), { code: 'ENOENT' });
});

test('reject invalid config before microphone/network access', async t => {
  const f = await fixture(t, { config: { language: 'invalid' } });
  assert.equal(await f.command('start'), 'idle');
  assert.equal(f.keys.length, 0);
  await assert.rejects(readFile(f.env.TEST_MIC), { code: 'ENOENT' });
  assert.match(f.logs(), /invalid config/);
  await until(async () => (await f.osdStates()).at(-1) === 'error');
});

test('live daemon cannot be replaced; malformed and partial clients do not block status', async t => {
  const f = await fixture(t);
  await assert.rejects(exec(binary, f.args, { env: f.env, timeout: 2000 }), e => e.code === 1);
  assert.equal(await f.command('status'), 'idle');
  const slow = connect(f.socket); await once(slow, 'connect'); slow.write('sta');
  assert.equal(await f.command('status'), 'idle');
  slow.destroy();
  const bad = connect(f.socket); await once(bad, 'connect'); bad.end('status\0junk\n');
  let response = ''; bad.on('data', b => response += b); await once(bad, 'close');
  assert.equal(response, 'unknown command\n');
  const partial = connect(f.socket); await once(partial, 'connect');
  let answer = ''; partial.on('data', b => answer += b);
  partial.write('sta'); await delay(20); partial.end('tus\n'); await once(partial, 'close');
  assert.equal(answer, 'idle\n');
});

test('SIGTERM stops an active recording, removes socket and leaves no microphone', async t => {
  const f = await fixture(t, { noCommitResponse: true });
  await f.command('start');
  await until(async () => { try { return !!(await readFile(f.env.TEST_MIC)); } catch { return false; } });
  const mic = Number(await readFile(f.env.TEST_MIC, 'utf8'));
  f.child.kill('SIGTERM'); await f.exited;
  assert.equal((await f.osdStates()).at(-1), 'hide');
  await assert.rejects(stat(f.socket), { code: 'ENOENT' });
  await until(() => { try { process.kill(mic, 0); return false; } catch (e) { return e.code === 'ESRCH'; } });
});

test('insertion failure never falls back to clipboard and session recovers', async t => {
  const f = await fixture(t);
  await writeFile(join(f.bin, 'wtype'), `#!${process.execPath}\nprocess.exit(4);\n`, { mode: 0o700 });
  await f.command('start'); await delay(150); await f.command('stop');
  await until(async () => await f.command('status') === 'idle');
  await assert.rejects(readFile(f.env.TEST_PASTE), { code: 'ENOENT' });
  await assert.rejects(readFile(f.env.TEST_COPY), { code: 'ENOENT' });
  assert.match(f.logs(), /text insertion failed/);
});

test('failed microphone recovers without clipboard changes', async t => {
  const f = await fixture(t);
  await writeFile(join(f.bin, 'pw-record'), `#!${process.execPath}\nprocess.exit(1);\n`, { mode: 0o700 });
  await f.command('start');
  await until(async () => await f.command('status') === 'idle');
  assert.match(f.logs(), /microphone exited unexpectedly/);
  await assert.rejects(readFile(f.env.TEST_PASTE), { code: 'ENOENT' });
});

test('missing final transcript times out without paste', { timeout: 15000 }, async t => {
  const f = await fixture(t, { noCommitResponse: true });
  await f.command('start'); await delay(150); await f.command('stop');
  await until(async () => await f.command('status') === 'idle', 10000);
  assert.match(f.logs(), /Scribe commit timed out/);
  await assert.rejects(readFile(f.env.TEST_PASTE), { code: 'ENOENT' });
});

test('rejects malformed, unknown-field and wrong-type config on reload', async t => {
  const f = await fixture(t);
  for (const config of ['{', '{} garbage', '{"unknown":true}', '{"audioDevice":null}', '{"debug":1}', '{"keyterms":[2]}', '{"pasteCommand":"shell"}']) {
    await writeFile(join(f.dir, 'voxa/config.json'), config);
    assert.equal(await f.command('start'), 'idle');
  }
  assert.equal(f.keys.length, 0);
  await assert.rejects(readFile(f.env.TEST_MIC), { code: 'ENOENT' });
});

test('hung OSD never blocks recording, insertion or control, and has bounded shutdown', async t => {
  const f = await fixture(t, { hangOsd: true });
  await f.command('start'); await delay(150); await f.command('stop');
  await until(async () => await f.command('status') === 'idle', 1200);
  assert.equal(await readFile(f.env.TEST_TEXT, 'utf8'), 'สวัสดี hello');
  // A killed first display command must not stop later queued states.
  await until(async () => (await f.osdStates()).includes('committing'), 2500);
  const started = Date.now(); f.child.kill('SIGTERM'); await f.exited;
  assert.ok(Date.now() - started < 2000);
  assert.equal((await f.osdStates()).at(-1), 'hide');
});

test('unavailable OSD backend is best-effort', async t => {
  const f = await fixture(t, { failOsd: true });
  await f.command('start'); await delay(150); await f.command('stop');
  await until(async () => await f.command('status') === 'idle');
  assert.equal(await readFile(f.env.TEST_TEXT, 'utf8'), 'สวัสดี hello');
});

test('--no-osd suppresses all display commands including shutdown', async t => {
  const f = await fixture(t, { noOsd: true });
  await f.command('start'); await delay(150); await f.command('stop');
  await until(async () => await f.command('status') === 'idle');
  f.child.kill('SIGTERM'); await f.exited;
  assert.deepEqual(await f.osdStates(), []);
});

test('production binary rejects test endpoint override and preserves non-socket files', async t => {
  await assert.rejects(exec(join(root, 'voxa-c'), ['--test-endpoint', 'ws://127.0.0.1:1/', 'daemon']), e => e.code === 2);
  const dir = await mkdtemp(join(tmpdir(), 'voxa-native-file-')); t.after(() => rm(dir, { recursive: true, force: true }));
  const path = join(dir, 'socket'); await writeFile(path, 'keep');
  await assert.rejects(exec(binary, ['--socket', path, 'daemon']), e => e.code === 1);
  assert.equal(await readFile(path, 'utf8'), 'keep');
});
