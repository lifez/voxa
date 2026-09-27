import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { loadKeyFile, nextApiKey } from '../dist/config.js';

test('macOS daemon reloads the saved key on each recording', () => {
  const dir = mkdtempSync(join(tmpdir(), 'voxa-key-'));
  const oldDir = process.env.XDG_CONFIG_HOME;
  const oldKey = process.env.ELEVENLABS_API_KEY;
  try {
    process.env.XDG_CONFIG_HOME = dir;
    mkdirSync(join(dir, 'voxa'));
    const file = join(dir, 'voxa', 'env');
    writeFileSync(file, 'ELEVENLABS_API_KEY=first\n');
    loadKeyFile(true);
    assert.equal(process.env.ELEVENLABS_API_KEY, 'first');
    writeFileSync(file, 'ELEVENLABS_API_KEY=second,third\n');
    loadKeyFile(true);
    assert.equal(process.env.ELEVENLABS_API_KEY, 'second,third');
    assert.equal(nextApiKey(), 'second');
    assert.equal(nextApiKey(), 'third');
    assert.equal(nextApiKey(), 'second');
    writeFileSync(file, 'ELEVENLABS_API_KEY=new,other\n');
    loadKeyFile(true);
    assert.equal(nextApiKey(), 'new');
    process.env.ELEVENLABS_API_KEY = 'one,,two';
    assert.throws(() => nextApiKey(), /invalid ELEVENLABS_API_KEY/);
    rmSync(file);
    loadKeyFile(true);
    assert.equal(process.env.ELEVENLABS_API_KEY, undefined);
  } finally {
    if (oldDir === undefined) delete process.env.XDG_CONFIG_HOME; else process.env.XDG_CONFIG_HOME = oldDir;
    if (oldKey === undefined) delete process.env.ELEVENLABS_API_KEY; else process.env.ELEVENLABS_API_KEY = oldKey;
    rmSync(dir, { recursive: true, force: true });
  }
});
