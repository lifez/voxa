// Explicit opt-in: briefly opens the default microphone; discards all audio.
// Measures first PCM delivery, NOT speech onset, quality, or end-to-end latency.
import { spawn } from 'node:child_process';
import { performance } from 'node:perf_hooks';
if (!process.argv.includes('--allow-mic') || process.platform !== 'linux') {
  console.error('Linux only. Usage: node bench/mic-latency.mjs --allow-mic');
  process.exit(2);
}
function trial(latency) {
  return new Promise((resolve, reject) => {
    const start = performance.now();
    const child = spawn('pw-record', ['--rate', '16000', '--channels', '1', '--format', 's16', '--raw', '--latency', `${latency}ms`, '-'], { stdio: ['ignore', 'pipe', 'ignore'] });
    let first, stop, frames = 0;
    const timer = setTimeout(() => child.kill('SIGTERM'), 2000);
    child.stdout.on('data', data => {
      frames += data.length;
      if (first === undefined) {
        first = performance.now() - start;
        stop = performance.now();
        child.kill('SIGTERM');
      }
    });
    child.on('error', e => { clearTimeout(timer); reject(e); });
    child.on('close', () => {
      clearTimeout(timer);
      if (first === undefined) reject(Error('No PCM before timeout'));
      else resolve({ first, drain: performance.now() - stop, bytes: frames });
    });
  });
}
const values = { 20: [], 50: [], 100: [] };
// Rotate order to reduce ordering bias; 10 trials per setting, no warmups.
for (let round = 0; round < 10; round++) {
  const order = [20, 50, 100];
  for (let i = 0; i < 3; i++) {
    const latency = order[(round + i) % 3];
    values[latency].push(await trial(latency));
  }
}
function summary(a) {
  a.sort((x, y) => x - y);
  return { n: a.length, median_ms: (a[4] + a[5]) / 2, p95_ms: a[9], min_ms: a[0], max_ms: a[9] };
}
console.log(JSON.stringify(Object.fromEntries(Object.entries(values).map(([latency, trials]) => [latency, {
  first_pcm: summary(trials.map(t => t.first)),
  stop_to_close: summary(trials.map(t => t.drain)),
}])), null, 2));
