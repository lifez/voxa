import { test } from 'node:test';
import { strict as assert } from 'node:assert';
import { audioPeak } from '../dist/audio.js';

test('audio peak distinguishes silence from signed PCM speech', () => {
  assert.equal(audioPeak(Buffer.alloc(4)), 0);
  const samples = Buffer.alloc(6);
  samples.writeInt16LE(-1200, 0);
  samples.writeInt16LE(900, 2);
  samples.writeInt16LE(0, 4);
  assert.equal(audioPeak(samples), 1200);
});
