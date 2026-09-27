import { test } from 'node:test';
import { strict as assert } from 'node:assert';
import { formatTranscript } from '../dist/scribe.js';

test('stop punctuation defaults to off without removing internal punctuation', () => {
  assert.equal(formatTranscript('Hello, world.', false), 'Hello, world');
  assert.equal(formatTranscript('สวัสดี。', false), 'สวัสดี');
  assert.equal(formatTranscript('Hello.  ', false), 'Hello  ');
  assert.equal(formatTranscript('Really?', false), 'Really?');
  assert.equal(formatTranscript('v1.2 is ready.', false), 'v1.2 is ready');
  assert.equal(formatTranscript('Hello, world.', true), 'Hello, world.');
});
