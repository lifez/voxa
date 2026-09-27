import { test } from 'node:test';
import { strict as assert } from 'node:assert';
import { validate, defaults } from '../dist/config.js';

test('defaults and optional language detection', () => {
  assert.equal(validate({}).language, 'th');
  assert.equal(validate({ language: null, secondaryLanguages: [] }).language, null);
  assert.deepEqual(defaults.secondaryLanguages, ['en']);
  assert.equal(validate({}).stopPunctuation, false);
  assert.equal(validate({ stopPunctuation: true }).stopPunctuation, true);
});
test('rejects unknown fields and invalid types', () => {
  assert.throws(() => validate({ ELEVENLABS_API_KEY: 'secret' }), /unknown/);
  assert.throws(() => validate({ keyterms: 'oops' }), /string array/);
  assert.throws(() => validate({ pasteCommand: 'sh' }), /invalid/);
  assert.throws(() => validate({ stopPunctuation: 'false' }), /invalid/);
});
