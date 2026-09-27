#!/usr/bin/env node
import { connect } from 'node:net';
import { Daemon, socketPath } from './daemon.js';
import { capture, audioPeak } from './audio.js';
import { paste } from './paste.js';
import { Scribe } from './scribe.js';
import { loadConfig, loadKeyFile } from './config.js';
import { setup, settings, doctor } from './setup.js';

const [command, ...args] = process.argv.slice(2);
if (command === 'setup') setup().catch(e => { console.error(`voxa: ${e.message}`); process.exitCode = 1; });
else if (command === 'settings') settings(args[0] !== '--terminal').catch(e => { console.error(`voxa: ${e.message}`); process.exitCode = 1; });
else if (command === 'doctor') doctor().catch(e => { console.error(`voxa: ${e.message}`); process.exitCode = 1; });
else if (command === 'daemon') { loadKeyFile(); new Daemon().serve(); }
else if (command === 'test-mic') {
  let bytes = 0, peak = 0;
  const mic = capture(loadConfig().audioDevice, b => { bytes += b.length; peak = Math.max(peak, audioPeak(b)); }, e => { console.error(e.message); process.exitCode = 1; });
  setTimeout(() => { mic.stop(); console.log(`Captured ${bytes} bytes of 16k mono PCM in 2 seconds; peak ${peak}`); if (!bytes || !peak) process.exitCode = 1; }, 2000);
} else if (command === 'test-scribe') {
  try {
    loadKeyFile();
    const scribe = new Scribe(loadConfig(), e => console.error(e.message));
    const mic = capture(loadConfig().audioDevice, b => scribe.add(b), e => console.error(e.message));
    console.log('Speak now (3 seconds)...');
    setTimeout(async () => {
      mic.stop();
      await mic.waitClose();
      try { console.log('Transcript:', await scribe.stop()); }
      catch (e) { console.error((e as Error).message); process.exitCode = 1; }
    }, 3000);
  } catch (e) { console.error((e as Error).message); process.exitCode = 1; }
} else if (command === 'test-paste') {
  paste(args.join(' ')).catch(e => { console.error(e.message); process.exitCode = 1; });
} else if (['start', 'stop', 'toggle', 'status'].includes(command ?? '')) {
  const sock = connect(socketPath());
  sock.setTimeout(1500, () => sock.destroy(Error('daemon timed out')));
  sock.on('connect', () => sock.end(command + '\n'));
  sock.on('data', b => process.stdout.write(b));
  sock.on('error', e => { console.error(`voxa: ${e.message} (is the user service running?)`); process.exitCode = 1; });
} else { console.error('Usage: voxa setup|settings [--terminal]|doctor|daemon|start|stop|toggle|status|test-mic|test-scribe|test-paste TEXT'); process.exitCode = 2; }
