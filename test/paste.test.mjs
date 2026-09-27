import test from 'node:test';
import assert from 'node:assert/strict';
import { createPaster } from '../dist/paste.js';

test('Linux types Unicode and option-like text through stdin without clipboard access', async () => {
  const calls = [];
  const paste = createPaster('linux', async (...args) => { calls.push(args); });
  const text = '-M ctrl สวัสดี hello\nsecond line';
  await paste(text);
  assert.deepEqual(calls, [['wtype', ['-'], text]]);
});

test('macOS sends text only to native paste-and-restore helper', async () => {
  const calls = [];
  const paste = createPaster('darwin', async (...args) => { calls.push(args); });
  await paste('สวัสดี "hello"');
  assert.equal(calls.length, 1);
  assert.ok(calls[0][0].endsWith('/dist/voxa-paste'));
  assert.deepEqual(calls[0].slice(1), [[], 'สวัสดี "hello"']);
});

test('empty transcripts do not type or change clipboard on either platform', async () => {
  for (const platform of ['linux', 'darwin']) {
    await createPaster(platform, async () => { assert.fail('must not run'); })(' \n\t');
  }
});

test('insertion errors propagate without destructive clipboard fallback', async () => {
  for (const platform of ['linux', 'darwin']) {
    let calls = 0;
    const paste = createPaster(platform, async () => { calls++; throw Error('insertion failed'); });
    await assert.rejects(paste('hello'), /insertion failed/);
    assert.equal(calls, 1);
  }
});
