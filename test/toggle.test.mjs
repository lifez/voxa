import { test } from 'node:test';
import { strict as assert } from 'node:assert';
import { Daemon } from '../dist/daemon.js';

test('toggle starts, stops, and ignores presses while committing', () => {
  const daemon = new Daemon();
  const calls = [];
  daemon.start = () => { calls.push('start'); daemon.state = 'recording'; };
  daemon.stop = () => { calls.push('stop'); daemon.state = 'committing'; };

  daemon.toggle();
  assert.equal(daemon.state, 'recording');
  daemon.toggle();
  assert.equal(daemon.state, 'committing');
  daemon.toggle();
  assert.deepEqual(calls, ['start', 'stop']);
  daemon.state = 'idle';
  daemon.toggle();
  assert.deepEqual(calls, ['start', 'stop', 'start']);
});
